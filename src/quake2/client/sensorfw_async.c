/*
 * sensorfw_async.c -- non-blocking session machine for sensorfwd.
 *
 * See sensorfw_async.h for why this exists and how it differs from
 * sensorfw_client.c, and docs/sensorfw.md for the protocol itself.
 *
 * Build: cc -c sensorfw_async.c $(pkg-config --cflags dbus-1)
 * Link:  $(pkg-config --libs dbus-1)
 */

/*
 * АДАПТАЦИЯ ДЛЯ ПОРТА: файл скопирован из gameport/examples/sensors/
 * без изменений логики. Добавлен только гард AURORA_VR — клиент
 * sensorfwd нужен исключительно VR-коду, и без VR не должно быть ни
 * зависимости от dbus-1, ни лишнего кода в бинарнике (правило порта:
 * весь код порта под своим дефайном, см. aurora_keepalive.c).
 *
 * Разбор потока семплов и раскладка кадра проверены на железе —
 * при синхронизации с gameport менять их нельзя.
 */
#if defined(AURORA_VR)

/* clock_gettime()/CLOCK_MONOTONIC under a strict -std=c11. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "sensorfw_async.h"

#include <dbus/dbus.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SENSORFW_SERVICE       "com.nokia.SensorService"
#define SENSORFW_OBJECT_PATH   "/SensorManager"
#define SENSORFW_MANAGER_IFACE "local.SensorManager"
#define SENSORFW_SOCKET_PATH   "/run/sensord.sock"

#define SENSORFW_DEFAULT_TIMEOUT_MS 2000

/* Same sanity cap as sensorfw_client.c: with the default bufferSize
 * sensord never batches anything like this many, so a larger count
 * means a desynced stream, not a busy one. */
#define SENSORFW_MAX_BATCH 64

/* Backoff ladder for FAILED, in milliseconds, and the number of
 * consecutive failures after which we stop retrying altogether. */
static const int SENSORFW_BACKOFF_MS[] = { 1000, 2000, 4000, 8000, 16000, 30000 };
#define SENSORFW_BACKOFF_STEPS ( (int)( sizeof( SENSORFW_BACKOFF_MS ) / sizeof( SENSORFW_BACKOFF_MS[0] ) ) )
#define SENSORFW_MAX_FAILURES  5

/*
 * Wire layout -- copied verbatim from sensorfw_client.c on purpose.
 * It does NOT match upstream sailfishos/sensorfw, where TimedXyzData
 * is { quint64; float x,y,z; }. What actually arrives is four raw
 * int32 after the timestamp, 24 bytes with no padding. Re-verify
 * with sensorfw_rawdump.c before trusting it on a new build.
 */
typedef struct
{
	uint64_t timestamp_us;
	int32_t  x;
	int32_t  y;
	int32_t  z;
	int32_t  reserved; /* session-constant, meaning unknown, ignored */
} sensorfw_wire_xyz_t;

_Static_assert( sizeof( sensorfw_wire_xyz_t ) == 24, "TimedXyzData wire layout mismatch" );

typedef struct
{
	sensorfw_async_sensor_t type;
	const char              *id;
	const char              *iface;
} sensorfw_type_entry_t;

static const sensorfw_type_entry_t SENSORFW_TYPES[] =
{
	{ SENSORFW_ASYNC_ACCELEROMETER, "accelerometersensor", "local.AccelerometerSensor" },
	{ SENSORFW_ASYNC_GYROSCOPE,     "gyroscopesensor",     "local.GyroscopeSensor" },
};

/*
 * One outstanding D-Bus call.
 *
 * `done` and `reply` are written by the notify callback and read by
 * pump(). Both run on the same thread -- the callback fires from
 * inside dbus_connection_read_write_dispatch(), which pump() calls
 * itself -- so no locking is involved. What matters is not
 * concurrency but re-entrancy: see the comment on notify_cb().
 */
typedef struct
{
	DBusPendingCall *pc;
	DBusMessage     *reply;
	bool             done;
} sensorfw_pending_t;

struct sensorfw_async
{
	DBusConnection *bus;

	sensorfw_async_sensor_t type;
	const char             *sensor_id;
	const char             *iface;
	char                    object_path[128];

	sensorfw_async_state_t  state;
	int64_t                 deadline_ms;   /* 0 = no deadline in this state */
	bool                    wanted;
	int                     interval_ms;
	bool                    interval_dirty;
	int                     timeout_ms;

	sensorfw_pending_t      call;

	/* Session + socket */
	int32_t                 session_id;
	bool                    have_session;
	int                     fd;
	size_t                  id_sent;       /* bytes of the 4-byte session id written */

	/* Probe result, cached for the lifetime of the process: whether
	 * this sensor is present at all. */
	bool                    probed;
	bool                    present;

	/* Sample framing -- identical to sensorfw_client.c */
	unsigned char           hdr_buf[4];
	size_t                  hdr_have;
	unsigned char           payload_buf[SENSORFW_MAX_BATCH * sizeof( sensorfw_wire_xyz_t )];
	size_t                  payload_cap;
	size_t                  payload_have;
	uint32_t                pending_count;

	/* Failure bookkeeping */
	char                    last_error[192];
	int                     fail_count;
	int64_t                 retry_at_ms;
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int64_t now_ms( void )
{
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const sensorfw_type_entry_t *type_entry( sensorfw_async_sensor_t type )
{
	for( size_t i = 0; i < sizeof( SENSORFW_TYPES ) / sizeof( SENSORFW_TYPES[0] ); ++i )
	{
		if( SENSORFW_TYPES[i].type == type )
			return &SENSORFW_TYPES[i];
	}
	return NULL;
}

const char *sensorfw_async_sensor_name( sensorfw_async_sensor_t type )
{
	const sensorfw_type_entry_t *e = type_entry( type );
	return e ? e->id : "(unknown)";
}

const char *sensorfw_async_state_name( sensorfw_async_state_t state )
{
	switch( state )
	{
		case SENSORFW_ASYNC_STATE_IDLE:          return "IDLE";
		case SENSORFW_ASYNC_STATE_PROBE_SENT:    return "PROBE_SENT";
		case SENSORFW_ASYNC_STATE_LOAD_SENT:     return "LOAD_SENT";
		case SENSORFW_ASYNC_STATE_REQ_SENT:      return "REQ_SENT";
		case SENSORFW_ASYNC_STATE_CONNECTING:    return "CONNECTING";
		case SENSORFW_ASYNC_STATE_SENDING_ID:    return "SENDING_ID";
		case SENSORFW_ASYNC_STATE_AWAIT_TAG:     return "AWAIT_TAG";
		case SENSORFW_ASYNC_STATE_INTERVAL_SENT: return "INTERVAL_SENT";
		case SENSORFW_ASYNC_STATE_START_SENT:    return "START_SENT";
		case SENSORFW_ASYNC_STATE_STREAMING:     return "STREAMING";
		case SENSORFW_ASYNC_STATE_STOP_SENT:     return "STOP_SENT";
		case SENSORFW_ASYNC_STATE_STOPPED:       return "STOPPED";
		case SENSORFW_ASYNC_STATE_RELEASE_SENT:  return "RELEASE_SENT";
		case SENSORFW_ASYNC_STATE_FAILED:        return "FAILED";
		case SENSORFW_ASYNC_STATE_DISABLED:      return "DISABLED";
	}
	return "(bad state)";
}

static void set_error( sensorfw_async_t *c, const char *fmt, ... )
{
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( c->last_error, sizeof( c->last_error ), fmt, ap );
	va_end( ap );
}

/* ------------------------------------------------------------------ */
/* Pending-call plumbing                                               */
/* ------------------------------------------------------------------ */

/*
 * The notify callback does exactly two things: steal the reply and
 * set a flag. It performs no transition, sends no follow-up call and
 * touches nothing outside the pending slot.
 *
 * That restraint is load-bearing, for three independent reasons:
 *
 *  1. It runs *inside* dbus_connection_read_write_dispatch(), i.e.
 *     inside libdbus's own dispatch with the connection lock held.
 *     Issuing the next dbus_connection_send_with_reply() from here
 *     is asking for re-entrancy trouble in someone else's library.
 *  2. It would otherwise become a second, hidden entry point into
 *     the host application's state, reached from the middle of
 *     libdbus at an unpredictable point in the frame.
 *  3. A machine advanced by exactly one function exactly once per
 *     frame can be driven end to end by a test harness feeding it
 *     synthetic replies. A machine smeared across callbacks cannot.
 */
static void notify_cb( DBusPendingCall *pc, void *user_data )
{
	sensorfw_pending_t *p = (sensorfw_pending_t *)user_data;

	if( p->pc != pc )
		return; /* stale callback for a call we already abandoned */

	p->reply = dbus_pending_call_steal_reply( pc );
	p->done  = true;
}

static void pending_clear( sensorfw_pending_t *p )
{
	if( p->reply )
	{
		dbus_message_unref( p->reply );
		p->reply = NULL;
	}
	if( p->pc )
	{
		dbus_pending_call_unref( p->pc );
		p->pc = NULL;
	}
	p->done = false;
}

static void pending_cancel( sensorfw_pending_t *p )
{
	if( p->pc )
		dbus_pending_call_cancel( p->pc );
	pending_clear( p );
}

/*
 * Send a method call without waiting for it. Returns false only if
 * the message could not be constructed or handed to libdbus at all.
 */
static bool call_async( sensorfw_async_t *c, const char *path, const char *iface,
                        const char *method, int first_arg_type, ... )
{
	pending_clear( &c->call );

	DBusMessage *msg = dbus_message_new_method_call( SENSORFW_SERVICE, path, iface, method );
	if( !msg )
	{
		set_error( c, "%s: out of memory building the message", method );
		return false;
	}

	if( first_arg_type != DBUS_TYPE_INVALID )
	{
		va_list ap;
		va_start( ap, first_arg_type );
		dbus_bool_t ok = dbus_message_append_args_valist( msg, first_arg_type, ap );
		va_end( ap );
		if( !ok )
		{
			dbus_message_unref( msg );
			set_error( c, "%s: could not append arguments", method );
			return false;
		}
	}

	DBusPendingCall *pc = NULL;
	dbus_bool_t sent = dbus_connection_send_with_reply( c->bus, msg, &pc, c->timeout_ms );
	dbus_message_unref( msg );

	if( !sent || !pc )
	{
		set_error( c, "%s: send_with_reply failed (disconnected?)", method );
		return false;
	}

	c->call.pc    = pc;
	c->call.reply = NULL;
	c->call.done  = false;

	if( !dbus_pending_call_set_notify( pc, notify_cb, &c->call, NULL ) )
	{
		pending_cancel( &c->call );
		set_error( c, "%s: set_notify failed", method );
		return false;
	}

	/*
	 * Documented libdbus race, and a classic source of "hangs
	 * sometimes at startup": if the reply arrived between
	 * send_with_reply() and set_notify(), the callback is never
	 * invoked. Collect it here instead.
	 */
	if( dbus_pending_call_get_completed( pc ) )
		notify_cb( pc, &c->call );

	/* Nudge the outgoing queue; this does not wait for a reply. */
	dbus_connection_flush( c->bus );
	return true;
}

/* ------------------------------------------------------------------ */
/* State transitions                                                   */
/* ------------------------------------------------------------------ */

static void close_socket( sensorfw_async_t *c )
{
	if( c->fd >= 0 )
	{
		close( c->fd );
		c->fd = -1;
	}
	c->id_sent      = 0;
	c->hdr_have     = 0;
	c->payload_cap  = 0;
	c->payload_have = 0;
	c->pending_count = 0;
}

static void enter( sensorfw_async_t *c, sensorfw_async_state_t state )
{
	c->state = state;

	switch( state )
	{
		/* Transient states get the deadline; stable ones do not. */
		case SENSORFW_ASYNC_STATE_PROBE_SENT:
		case SENSORFW_ASYNC_STATE_LOAD_SENT:
		case SENSORFW_ASYNC_STATE_REQ_SENT:
		case SENSORFW_ASYNC_STATE_CONNECTING:
		case SENSORFW_ASYNC_STATE_SENDING_ID:
		case SENSORFW_ASYNC_STATE_AWAIT_TAG:
		case SENSORFW_ASYNC_STATE_INTERVAL_SENT:
		case SENSORFW_ASYNC_STATE_START_SENT:
		case SENSORFW_ASYNC_STATE_STOP_SENT:
		case SENSORFW_ASYNC_STATE_RELEASE_SENT:
			c->deadline_ms = now_ms() + c->timeout_ms;
			break;
		default:
			c->deadline_ms = 0;
			break;
	}
}

/*
 * Give up on the current attempt. Everything session-shaped is torn
 * down; the backoff decides when IDLE is re-entered.
 */
static void fail( sensorfw_async_t *c, const char *fmt, ... )
{
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( c->last_error, sizeof( c->last_error ), fmt, ap );
	va_end( ap );

	pending_cancel( &c->call );
	close_socket( c );
	c->have_session = false;

	c->fail_count++;

	if( c->fail_count >= SENSORFW_MAX_FAILURES )
	{
		fprintf( stderr, "sensorfw_async: giving up on %s after %d attempts: %s\n",
		         c->sensor_id, c->fail_count, c->last_error );
		enter( c, SENSORFW_ASYNC_STATE_DISABLED );
		return;
	}

	int step = c->fail_count - 1;
	if( step >= SENSORFW_BACKOFF_STEPS )
		step = SENSORFW_BACKOFF_STEPS - 1;
	c->retry_at_ms = now_ms() + SENSORFW_BACKOFF_MS[step];

	fprintf( stderr, "sensorfw_async: %s failed in %s (attempt %d): %s -- retrying in %d ms\n",
	         c->sensor_id, sensorfw_async_state_name( c->state ), c->fail_count,
	         c->last_error, SENSORFW_BACKOFF_MS[step] );

	enter( c, SENSORFW_ASYNC_STATE_FAILED );
}

/*
 * requestSensor() is the one call that creates a resource on the
 * daemon's side. If its reply is lost we may well have a session on
 * sensorfwd whose id we will never learn and therefore can never
 * release. Retrying only multiplies that, so this is a one-shot
 * refusal rather than a backoff.
 *
 * Worth being clear that this is not a cost introduced by going
 * asynchronous: the blocking version leaked exactly the same way
 * when its 2 s timeout expired. It just never said so out loud. The
 * leak is bounded either way -- sensorfwd tracks sessions by pid and
 * reclaims them when our socket drops.
 */
static void fail_permanently( sensorfw_async_t *c, const char *why )
{
	set_error( c, "%s", why );
	pending_cancel( &c->call );
	close_socket( c );
	c->have_session = false;

	fprintf( stderr, "sensorfw_async: %s disabled: %s\n", c->sensor_id, why );
	enter( c, SENSORFW_ASYNC_STATE_DISABLED );
}

/* ------------------------------------------------------------------ */
/* Reply handling                                                      */
/* ------------------------------------------------------------------ */

/*
 * Take the reply the callback stored, if any. Returns:
 *   1  reply present in *out (caller owns it, must unref)
 *   0  still waiting
 *  -1  the call came back as an error, or the deadline expired --
 *      the machine has already been moved to FAILED/DISABLED.
 */
static int take_reply( sensorfw_async_t *c, DBusMessage **out )
{
	*out = NULL;

	if( !c->call.done )
	{
		if( c->deadline_ms && now_ms() > c->deadline_ms )
		{
			if( c->state == SENSORFW_ASYNC_STATE_REQ_SENT )
				fail_permanently( c, "requestSensor() timed out; session id unknowable, not retrying" );
			else
				fail( c, "no reply in %s within %d ms",
				      sensorfw_async_state_name( c->state ), c->timeout_ms );
			return -1;
		}
		return 0;
	}

	DBusMessage *reply = c->call.reply;
	c->call.reply = NULL;

	if( !reply )
	{
		fail( c, "%s: reply lost", sensorfw_async_state_name( c->state ) );
		return -1;
	}

	if( dbus_message_get_type( reply ) == DBUS_MESSAGE_TYPE_ERROR )
	{
		const char *name = dbus_message_get_error_name( reply );
		fail( c, "%s: %s", sensorfw_async_state_name( c->state ), name ? name : "(unnamed D-Bus error)" );
		dbus_message_unref( reply );
		return -1;
	}

	*out = reply;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Socket steps                                                        */
/* ------------------------------------------------------------------ */

/*
 * O_NONBLOCK goes on BEFORE connect(), the inverse of
 * sensorfw_client.c, which connects, writes, performs a blocking
 * one-byte read and only then switches the fd. Here even the
 * handshake is polled.
 */
static bool socket_begin( sensorfw_async_t *c )
{
	int fd = socket( AF_UNIX, SOCK_STREAM, 0 );
	if( fd < 0 )
	{
		fail( c, "socket(): %s", strerror( errno ) );
		return false;
	}

	int flags = fcntl( fd, F_GETFL, 0 );
	if( flags < 0 || fcntl( fd, F_SETFL, flags | O_NONBLOCK ) < 0 )
	{
		close( fd );
		fail( c, "fcntl(O_NONBLOCK): %s", strerror( errno ) );
		return false;
	}

	struct sockaddr_un addr;
	memset( &addr, 0, sizeof( addr ) );
	addr.sun_family = AF_UNIX;
	snprintf( addr.sun_path, sizeof( addr.sun_path ), "%s", SENSORFW_SOCKET_PATH );

	c->fd      = fd;
	c->id_sent = 0;

	if( connect( fd, (struct sockaddr *)&addr, sizeof( addr ) ) == 0 )
	{
		enter( c, SENSORFW_ASYNC_STATE_SENDING_ID );
		return true;
	}

	if( errno == EINPROGRESS || errno == EAGAIN )
	{
		enter( c, SENSORFW_ASYNC_STATE_CONNECTING );
		return true;
	}

	fail( c, "connect(%s): %s", SENSORFW_SOCKET_PATH, strerror( errno ) );
	return false;
}

static void step_connecting( sensorfw_async_t *c )
{
	int       err = 0;
	socklen_t len = sizeof( err );

	if( getsockopt( c->fd, SOL_SOCKET, SO_ERROR, &err, &len ) < 0 )
	{
		fail( c, "getsockopt(SO_ERROR): %s", strerror( errno ) );
		return;
	}

	if( err == EINPROGRESS || err == EALREADY )
	{
		if( c->deadline_ms && now_ms() > c->deadline_ms )
			fail( c, "connect(%s) did not complete within %d ms", SENSORFW_SOCKET_PATH, c->timeout_ms );
		return;
	}

	if( err != 0 )
	{
		fail( c, "connect(%s): %s", SENSORFW_SOCKET_PATH, strerror( err ) );
		return;
	}

	enter( c, SENSORFW_ASYNC_STATE_SENDING_ID );
}

static void step_sending_id( sensorfw_async_t *c )
{
	unsigned char buf[sizeof( int32_t )];
	memcpy( buf, &c->session_id, sizeof( buf ) );

	while( c->id_sent < sizeof( buf ) )
	{
		ssize_t n = write( c->fd, buf + c->id_sent, sizeof( buf ) - c->id_sent );
		if( n > 0 )
		{
			c->id_sent += (size_t)n;
			continue;
		}
		if( n < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK ) )
		{
			if( c->deadline_ms && now_ms() > c->deadline_ms )
				fail( c, "session id not written within %d ms", c->timeout_ms );
			return;
		}
		fail( c, "write(session id): %s", n == 0 ? "short write" : strerror( errno ) );
		return;
	}

	enter( c, SENSORFW_ASYNC_STATE_AWAIT_TAG );
}

static void step_await_tag( sensorfw_async_t *c )
{
	char    tag;
	ssize_t n = read( c->fd, &tag, 1 );

	if( n == 1 )
	{
		c->interval_dirty = true; /* always send the interval once per session */
		enter( c, SENSORFW_ASYNC_STATE_INTERVAL_SENT );
		if( !call_async( c, c->object_path, c->iface, "setInterval",
		                 DBUS_TYPE_INT32, &c->session_id,
		                 DBUS_TYPE_INT32, &c->interval_ms,
		                 DBUS_TYPE_INVALID ) )
			fail( c, "%s", c->last_error );
		return;
	}

	if( n < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK ) )
	{
		if( c->deadline_ms && now_ms() > c->deadline_ms )
			fail( c, "handshake tag not received within %d ms", c->timeout_ms );
		return;
	}

	fail( c, "read(handshake tag): %s", n == 0 ? "closed by peer" : strerror( errno ) );
}

/* ------------------------------------------------------------------ */
/* Sample draining -- deliberately the same code as                    */
/* sensorfw_client_pump(), see the header.                             */
/* ------------------------------------------------------------------ */

static bool drain_samples( sensorfw_async_t *c, sensorfw_async_sample_cb cb, void *user_data )
{
	for( ;; )
	{
		if( c->payload_cap == 0 )
		{
			ssize_t n = recv( c->fd, c->hdr_buf + c->hdr_have,
			                  sizeof( c->hdr_buf ) - c->hdr_have, 0 );
			if( n == 0 )
				return false; /* sensord closed the session */
			if( n < 0 )
			{
				if( errno == EAGAIN || errno == EWOULDBLOCK )
					return true;
				set_error( c, "recv(header): %s", strerror( errno ) );
				return false;
			}
			c->hdr_have += (size_t)n;
			if( c->hdr_have < sizeof( c->hdr_buf ) )
				continue;

			uint32_t count;
			memcpy( &count, c->hdr_buf, sizeof( count ) );
			c->hdr_have = 0;

			if( count == 0 || count > SENSORFW_MAX_BATCH )
			{
				set_error( c, "implausible batch count %u -- protocol desync", (unsigned)count );
				return false;
			}

			c->pending_count = count;
			c->payload_cap   = (size_t)count * sizeof( sensorfw_wire_xyz_t );
			c->payload_have  = 0;
			continue;
		}

		ssize_t n = recv( c->fd, c->payload_buf + c->payload_have,
		                  c->payload_cap - c->payload_have, 0 );
		if( n == 0 )
			return false;
		if( n < 0 )
		{
			if( errno == EAGAIN || errno == EWOULDBLOCK )
				return true;
			set_error( c, "recv(payload): %s", strerror( errno ) );
			return false;
		}
		c->payload_have += (size_t)n;
		if( c->payload_have < c->payload_cap )
			continue;

		if( cb )
		{
			for( uint32_t i = 0; i < c->pending_count; ++i )
			{
				sensorfw_wire_xyz_t s;
				memcpy( &s, c->payload_buf + (size_t)i * sizeof( s ), sizeof( s ) );
				cb( user_data, s.timestamp_us, (float)s.x, (float)s.y, (float)s.z );
			}
		}

		c->payload_cap   = 0;
		c->payload_have  = 0;
		c->pending_count = 0;
	}
}

/* ------------------------------------------------------------------ */
/* Probe reply parsing                                                 */
/* ------------------------------------------------------------------ */

static bool reply_lists_sensor( DBusMessage *reply, const char *id )
{
	DBusMessageIter iter, sub;

	if( !dbus_message_iter_init( reply, &iter ) ||
	    dbus_message_iter_get_arg_type( &iter ) != DBUS_TYPE_ARRAY )
		return false;

	dbus_message_iter_recurse( &iter, &sub );
	while( dbus_message_iter_get_arg_type( &sub ) == DBUS_TYPE_STRING )
	{
		const char *s = NULL;
		dbus_message_iter_get_basic( &sub, &s );
		if( s && strcmp( s, id ) == 0 )
			return true;
		dbus_message_iter_next( &sub );
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

sensorfw_async_t *sensorfw_async_create( sensorfw_async_sensor_t type )
{
	const sensorfw_type_entry_t *entry = type_entry( type );
	if( !entry )
		return NULL;

	DBusError err;
	dbus_error_init( &err );

	/*
	 * The one synchronous call in this file. It talks to
	 * dbus-daemon, not to sensorfwd -- see the header. Keep it out
	 * of the frame path.
	 */
	DBusConnection *bus = dbus_bus_get( DBUS_BUS_SYSTEM, &err );
	if( !bus )
	{
		fprintf( stderr, "sensorfw_async: system bus unreachable: %s\n",
		         dbus_error_is_set( &err ) ? err.message : "(no detail)" );
		dbus_error_free( &err );
		return NULL;
	}
	dbus_error_free( &err );

	/* We drive dispatch ourselves; libdbus must not tear the process
	 * down if the bus goes away. */
	dbus_connection_set_exit_on_disconnect( bus, FALSE );

	sensorfw_async_t *c = calloc( 1, sizeof( *c ) );
	if( !c )
	{
		dbus_connection_unref( bus );
		return NULL;
	}

	c->bus        = bus;
	c->type       = type;
	c->sensor_id  = entry->id;
	c->iface      = entry->iface;
	c->fd         = -1;
	c->interval_ms = 20;
	c->timeout_ms  = SENSORFW_DEFAULT_TIMEOUT_MS;
	c->state      = SENSORFW_ASYNC_STATE_IDLE;
	snprintf( c->object_path, sizeof( c->object_path ), "%s/%s", SENSORFW_OBJECT_PATH, entry->id );

	return c;
}

void sensorfw_async_destroy( sensorfw_async_t *c )
{
	if( !c )
		return;

	/*
	 * Best-effort release: send it, flush, do not wait. sensorfwd
	 * reclaims sessions by pid when the socket drops, so a reply we
	 * never read costs nothing.
	 */
	if( c->have_session )
	{
		DBusMessage *msg = dbus_message_new_method_call( SENSORFW_SERVICE, SENSORFW_OBJECT_PATH,
		                                                 SENSORFW_MANAGER_IFACE, "releaseSensor" );
		if( msg )
		{
			dbus_int64_t pid = (dbus_int64_t)getpid();
			if( dbus_message_append_args( msg,
			                              DBUS_TYPE_STRING, &c->sensor_id,
			                              DBUS_TYPE_INT32,  &c->session_id,
			                              DBUS_TYPE_INT64,  &pid,
			                              DBUS_TYPE_INVALID ) )
			{
				dbus_connection_send( c->bus, msg, NULL );
				dbus_connection_flush( c->bus );
			}
			dbus_message_unref( msg );
		}
	}

	pending_cancel( &c->call );
	close_socket( c );

	if( c->bus )
		dbus_connection_unref( c->bus );

	free( c );
}

void sensorfw_async_set_wanted( sensorfw_async_t *c, bool wanted )
{
	if( c )
		c->wanted = wanted;
}

void sensorfw_async_set_interval_ms( sensorfw_async_t *c, int interval_ms )
{
	if( !c || interval_ms <= 0 || interval_ms == c->interval_ms )
		return;
	c->interval_ms    = interval_ms;
	c->interval_dirty = true;
}

void sensorfw_async_set_timeout_ms( sensorfw_async_t *c, int timeout_ms )
{
	if( c && timeout_ms > 0 )
		c->timeout_ms = timeout_ms;
}

sensorfw_async_state_t sensorfw_async_state( const sensorfw_async_t *c )
{
	return c ? c->state : SENSORFW_ASYNC_STATE_DISABLED;
}

const char *sensorfw_async_last_error( const sensorfw_async_t *c )
{
	return c ? c->last_error : "";
}

int sensorfw_async_fail_count( const sensorfw_async_t *c )
{
	return c ? c->fail_count : 0;
}

int sensorfw_async_retry_in_ms( const sensorfw_async_t *c )
{
	if( !c || c->state != SENSORFW_ASYNC_STATE_FAILED )
		return 0;
	int64_t left = c->retry_at_ms - now_ms();
	return left > 0 ? (int)left : 0;
}

void sensorfw_async_retry( sensorfw_async_t *c )
{
	if( !c )
		return;
	c->fail_count  = 0;
	c->retry_at_ms = 0;
	c->last_error[0] = '\0';
	/* A fresh probe too: the sensor may have appeared since. */
	c->probed  = false;
	c->present = false;
	enter( c, SENSORFW_ASYNC_STATE_IDLE );
}

int sensorfw_async_fd( const sensorfw_async_t *c )
{
	return c ? c->fd : -1;
}

/* ------------------------------------------------------------------ */
/* The machine                                                         */
/* ------------------------------------------------------------------ */

bool sensorfw_async_pump( sensorfw_async_t *c, sensorfw_async_sample_cb cb, void *user_data )
{
	if( !c )
		return false;

	/*
	 * Zero timeout: moves queued I/O and fires any notify callbacks
	 * whose replies have landed, then returns immediately.
	 */
	dbus_connection_read_write_dispatch( c->bus, 0 );

	DBusMessage *reply = NULL;
	dbus_int64_t pid   = (dbus_int64_t)getpid();

	switch( c->state )
	{
		case SENSORFW_ASYNC_STATE_IDLE:
			if( !c->wanted )
				break;
			if( c->probed && !c->present )
				break; /* known absent -- nothing to do, and nothing to log again */

			if( !c->probed )
			{
				enter( c, SENSORFW_ASYNC_STATE_PROBE_SENT );
				if( !call_async( c, SENSORFW_OBJECT_PATH, SENSORFW_MANAGER_IFACE,
				                 "availableSensorPlugins", DBUS_TYPE_INVALID ) )
					fail( c, "%s", c->last_error );
			}
			else
			{
				enter( c, SENSORFW_ASYNC_STATE_LOAD_SENT );
				if( !call_async( c, SENSORFW_OBJECT_PATH, SENSORFW_MANAGER_IFACE, "loadPlugin",
				                 DBUS_TYPE_STRING, &c->sensor_id, DBUS_TYPE_INVALID ) )
					fail( c, "%s", c->last_error );
			}
			break;

		case SENSORFW_ASYNC_STATE_PROBE_SENT:
		{
			int r = take_reply( c, &reply );
			if( r <= 0 )
				break;

			c->probed  = true;
			c->present = reply_lists_sensor( reply, c->sensor_id );
			dbus_message_unref( reply );
			pending_clear( &c->call );

			if( !c->present )
			{
				/*
				 * Absent hardware is not a failure to retry -- it
				 * will not appear later. Say it once and stop.
				 */
				set_error( c, "%s is not present on this device", c->sensor_id );
				fprintf( stderr, "sensorfw_async: %s\n", c->last_error );
				enter( c, SENSORFW_ASYNC_STATE_DISABLED );
				break;
			}

			enter( c, SENSORFW_ASYNC_STATE_LOAD_SENT );
			if( !call_async( c, SENSORFW_OBJECT_PATH, SENSORFW_MANAGER_IFACE, "loadPlugin",
			                 DBUS_TYPE_STRING, &c->sensor_id, DBUS_TYPE_INVALID ) )
				fail( c, "%s", c->last_error );
			break;
		}

		case SENSORFW_ASYNC_STATE_LOAD_SENT:
		{
			int r = take_reply( c, &reply );
			if( r <= 0 )
				break;
			dbus_message_unref( reply );
			pending_clear( &c->call );

			enter( c, SENSORFW_ASYNC_STATE_REQ_SENT );
			if( !call_async( c, SENSORFW_OBJECT_PATH, SENSORFW_MANAGER_IFACE, "requestSensor",
			                 DBUS_TYPE_STRING, &c->sensor_id,
			                 DBUS_TYPE_INT64,  &pid,
			                 DBUS_TYPE_INVALID ) )
				fail( c, "%s", c->last_error );
			break;
		}

		case SENSORFW_ASYNC_STATE_REQ_SENT:
		{
			int r = take_reply( c, &reply );
			if( r <= 0 )
				break;

			dbus_int32_t sid = -1;
			DBusError    err;
			dbus_error_init( &err );
			if( !dbus_message_get_args( reply, &err, DBUS_TYPE_INT32, &sid, DBUS_TYPE_INVALID ) )
			{
				dbus_message_unref( reply );
				pending_clear( &c->call );
				fail( c, "requestSensor(): malformed reply: %s",
				      dbus_error_is_set( &err ) ? err.message : "(no detail)" );
				dbus_error_free( &err );
				break;
			}
			dbus_error_free( &err );
			dbus_message_unref( reply );
			pending_clear( &c->call );

			if( sid < 0 )
			{
				fail( c, "requestSensor() returned session id %d", (int)sid );
				break;
			}

			c->session_id   = (int32_t)sid;
			c->have_session = true;
			socket_begin( c );
			break;
		}

		case SENSORFW_ASYNC_STATE_CONNECTING:
			step_connecting( c );
			break;

		case SENSORFW_ASYNC_STATE_SENDING_ID:
			step_sending_id( c );
			break;

		case SENSORFW_ASYNC_STATE_AWAIT_TAG:
			step_await_tag( c );
			break;

		case SENSORFW_ASYNC_STATE_INTERVAL_SENT:
		{
			int r = take_reply( c, &reply );
			if( r <= 0 )
				break;
			dbus_message_unref( reply );
			pending_clear( &c->call );
			c->interval_dirty = false;

			enter( c, SENSORFW_ASYNC_STATE_START_SENT );
			if( !call_async( c, c->object_path, c->iface, "start",
			                 DBUS_TYPE_INT32, &c->session_id, DBUS_TYPE_INVALID ) )
				fail( c, "%s", c->last_error );
			break;
		}

		case SENSORFW_ASYNC_STATE_START_SENT:
		{
			int r = take_reply( c, &reply );
			if( r <= 0 )
				break;
			dbus_message_unref( reply );
			pending_clear( &c->call );

			c->fail_count = 0; /* a full success resets the ladder */
			enter( c, SENSORFW_ASYNC_STATE_STREAMING );
			break;
		}

		case SENSORFW_ASYNC_STATE_STREAMING:
			if( !drain_samples( c, cb, user_data ) )
			{
				fail( c, "sample stream lost: %s",
				      c->last_error[0] ? c->last_error : "session closed" );
				break;
			}

			if( c->interval_dirty )
			{
				c->interval_dirty = false;
				enter( c, SENSORFW_ASYNC_STATE_INTERVAL_SENT );
				if( !call_async( c, c->object_path, c->iface, "setInterval",
				                 DBUS_TYPE_INT32, &c->session_id,
				                 DBUS_TYPE_INT32, &c->interval_ms,
				                 DBUS_TYPE_INVALID ) )
					fail( c, "%s", c->last_error );
				break;
			}

			if( !c->wanted )
			{
				enter( c, SENSORFW_ASYNC_STATE_STOP_SENT );
				if( !call_async( c, c->object_path, c->iface, "stop",
				                 DBUS_TYPE_INT32, &c->session_id, DBUS_TYPE_INVALID ) )
					fail( c, "%s", c->last_error );
			}
			break;

		case SENSORFW_ASYNC_STATE_STOP_SENT:
		{
			int r = take_reply( c, &reply );
			if( r <= 0 )
				break;
			dbus_message_unref( reply );
			pending_clear( &c->call );
			enter( c, SENSORFW_ASYNC_STATE_STOPPED );
			break;
		}

		case SENSORFW_ASYNC_STATE_STOPPED:
			/*
			 * Session and socket stay alive here on purpose: coming
			 * back is one start() rather than the whole sequence.
			 * Drain anything still queued so a resumed stream does
			 * not begin with stale samples.
			 */
			if( !drain_samples( c, NULL, NULL ) )
			{
				fail( c, "socket lost while stopped: %s",
				      c->last_error[0] ? c->last_error : "session closed" );
				break;
			}
			if( c->wanted )
			{
				enter( c, SENSORFW_ASYNC_STATE_START_SENT );
				if( !call_async( c, c->object_path, c->iface, "start",
				                 DBUS_TYPE_INT32, &c->session_id, DBUS_TYPE_INVALID ) )
					fail( c, "%s", c->last_error );
			}
			break;

		case SENSORFW_ASYNC_STATE_RELEASE_SENT:
		{
			int r = take_reply( c, &reply );
			if( r == 0 )
				break;
			if( r > 0 )
			{
				dbus_message_unref( reply );
				pending_clear( &c->call );
			}
			close_socket( c );
			c->have_session = false;
			enter( c, SENSORFW_ASYNC_STATE_IDLE );
			break;
		}

		case SENSORFW_ASYNC_STATE_FAILED:
			if( now_ms() >= c->retry_at_ms )
				enter( c, SENSORFW_ASYNC_STATE_IDLE );
			break;

		case SENSORFW_ASYNC_STATE_DISABLED:
			break;
	}

	return c->state != SENSORFW_ASYNC_STATE_FAILED &&
	       c->state != SENSORFW_ASYNC_STATE_DISABLED;
}

#else /* !AURORA_VR */

/* Пустая единица трансляции запрещена ISO C — один typedef вместо неё.
   Заглушек API здесь нет намеренно: единственный потребитель модуля —
   vr_head.c, и он сам собирается в no-op при выключенном AURORA_VR. */
typedef int sensorfw_async_disabled_translation_unit_t;

#endif /* AURORA_VR */
