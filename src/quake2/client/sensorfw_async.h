/*
 * sensorfw_async.h -- non-blocking variant of sensorfw_client.{h,c}.
 *
 * Same daemon, same protocol, same on-wire sample layout as
 * sensorfw_client.c -- see docs/sensorfw.md. The only thing that
 * differs is WHEN control comes back to the caller.
 *
 *
 * WHY THIS FILE EXISTS
 *
 * sensorfw_client.c routes every D-Bus call through one helper,
 * sensorfw_call_sync(), which uses
 * dbus_connection_send_with_reply_and_block(). Session setup
 * (availableSensorPlugins, loadPlugin, requestSensor) plus
 * setInterval/start/stop are therefore synchronous round trips to
 * sensorfwd, capped at SENSORFW_CALL_TIMEOUT_MS = 2000 ms each. The
 * socket handshake adds one more: the tag read at
 * sensorfw_client.c:434 has no timeout at all, and O_NONBLOCK is
 * only set afterwards.
 *
 * In a command-line utility that is fine -- sensorfw_demo.c has
 * nothing else to do. In a game it is not. start()/stop() sit behind
 * a user-facing gate, so they get hit whenever the player presses
 * the button that enables aiming; a 2 s stall there is a freeze, and
 * a missing or wedged sensorfwd turns startup into a multi-second
 * hang. No amount of lazy opening or hysteresis fixes that, because
 * those are mitigations FOR a blocking design rather than an
 * alternative to one.
 *
 * So this fork inverts the rule:
 *
 *     A function called from the frame may send a request and it may
 *     collect a reply that has already arrived. It may never wait.
 *
 * Everything else follows from that. The linear open/start/stop
 * sequence becomes a state machine advanced once per frame by
 * sensorfw_async_pump(); D-Bus replies arrive through
 * dbus_pending_call_set_notify() callbacks; the sample socket is put
 * into O_NONBLOCK *before* connect() so even the handshake is
 * polled rather than waited on.
 *
 *
 * WHAT IS DELIBERATELY DIFFERENT FROM sensorfw_client.c
 *
 *  - Self-contained. It does not include or link against
 *    sensorfw_client.{h,c}; it carries its own copy of the sensor
 *    table and the wire struct. A fork that shares half its
 *    definitions with the thing it forked from is worse to reason
 *    about than one that stands alone.
 *  - Own per-state deadlines rather than relying on the timeout
 *    argument of send_with_reply(). libdbus's own timeouts are
 *    driven by DBusTimeout objects, which normally need
 *    dbus_connection_set_timeout_functions() and a real main-loop
 *    integration; whether they fire under a bare
 *    dbus_connection_read_write_dispatch( bus, 0 ) is exactly the
 *    kind of question that cannot be settled without the device.
 *    Keeping our own deadline makes the answer irrelevant.
 *  - Notify callbacks only *store* the reply. Every transition
 *    happens in pump(). See the long comment on that rule in the .c.
 *  - Failure is reported through state + last_error rather than a
 *    false return, because by the time a call fails there is no
 *    call site left to return to.
 *
 * DO NOT "restore parity" with sensorfw_client.c by folding these
 * back into synchronous calls. The blocking behaviour is the defect
 * this file exists to avoid.
 *
 *
 * WHAT IS INTENTIONALLY THE SAME
 *
 * The sample-stream parsing (sensorfw_async_pump's inner loop) is a
 * near-literal copy of sensorfw_client_pump(), including the
 * SENSORFW_MAX_BATCH sanity cap and the 24-byte four-int32 wire
 * layout that contradicts upstream. That code is the part which was
 * empirically verified on hardware; forking it was not the point,
 * and it should keep tracking sensorfw_client.c if the layout is
 * ever corrected there.
 *
 *
 * ONE REMAINING SYNCHRONOUS CALL
 *
 * dbus_bus_get() connects to the bus socket and performs a
 * synchronous Hello. libdbus offers no asynchronous equivalent, and
 * hand-rolling one means implementing SASL EXTERNAL authentication.
 * It is left synchronous and confined to sensorfw_async_create(),
 * which the caller is expected to invoke at init time or from an
 * explicit user action -- never per frame. Note that this call talks
 * to dbus-daemon, not to sensorfwd: the peer is always running and
 * always answers, unlike the sensor daemon it is used to reach.
 *
 *
 * TYPICAL USE
 *
 *   sensorfw_async_t *g = sensorfw_async_create( SENSORFW_ASYNC_GYROSCOPE );
 *   sensorfw_async_set_interval_ms( g, 20 );          // ~50 Hz
 *   ...
 *   // once per frame, unconditionally:
 *   sensorfw_async_set_wanted( g, aiming_gate_is_open );
 *   sensorfw_async_pump( g, on_sample, NULL );
 *   ...
 *   sensorfw_async_destroy( g );
 *
 * pump() is cheap when there is nothing to do and never blocks, so
 * there is no reason to guard the call.
 */

#ifndef SENSORFW_ASYNC_H
#define SENSORFW_ASYNC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sensorfw_async sensorfw_async_t;

/*
 * Sensor types whose D-Bus interface name and whose on-wire sample
 * layout are both confirmed on real hardware. Same restriction, and
 * same reasoning, as sensorfw_sensor_type_t in sensorfw_client.h:
 * an arbitrary plugin id string could name a sensor this code would
 * silently decode as garbage.
 */
typedef enum
{
	SENSORFW_ASYNC_ACCELEROMETER,
	SENSORFW_ASYNC_GYROSCOPE,
	SENSORFW_ASYNC_TYPE_COUNT /* not a sensor -- array-sizing helper */
} sensorfw_async_sensor_t;

/*
 * Session state. Only the four marked STABLE are places the machine
 * will sit indefinitely; the rest are transient and are left as soon
 * as a reply or a socket event arrives, or as soon as the state's
 * deadline expires.
 *
 * A gate flip (sensorfw_async_set_wanted) is only recorded, never
 * acted on mid-sequence: the machine runs to the next stable state
 * and turns around there. Aborting a pending requestSensor() would
 * lose the sessionId that is about to arrive in its reply, and with
 * it the ability to ever releaseSensor() -- one wasted round trip is
 * the cheaper of the two.
 */
typedef enum
{
	SENSORFW_ASYNC_STATE_IDLE,          /* STABLE-ish: nothing open, nothing wanted */
	SENSORFW_ASYNC_STATE_PROBE_SENT,    /* availableSensorPlugins() outstanding */
	SENSORFW_ASYNC_STATE_LOAD_SENT,     /* loadPlugin() outstanding */
	SENSORFW_ASYNC_STATE_REQ_SENT,      /* requestSensor() outstanding */
	SENSORFW_ASYNC_STATE_CONNECTING,    /* connect() returned EINPROGRESS */
	SENSORFW_ASYNC_STATE_SENDING_ID,    /* writing the 4-byte session id */
	SENSORFW_ASYNC_STATE_AWAIT_TAG,     /* waiting for the 1-byte handshake tag */
	SENSORFW_ASYNC_STATE_INTERVAL_SENT, /* setInterval() outstanding */
	SENSORFW_ASYNC_STATE_START_SENT,    /* start() outstanding */
	SENSORFW_ASYNC_STATE_STREAMING,     /* STABLE: samples flowing */
	SENSORFW_ASYNC_STATE_STOP_SENT,     /* stop() outstanding, session kept */
	SENSORFW_ASYNC_STATE_STOPPED,       /* STABLE: session and socket alive, flow stopped */
	SENSORFW_ASYNC_STATE_RELEASE_SENT,  /* releaseSensor() outstanding, tearing down */
	SENSORFW_ASYNC_STATE_FAILED,        /* STABLE: backoff ticking, will retry */
	SENSORFW_ASYNC_STATE_DISABLED       /* STABLE: given up until explicitly retried */
} sensorfw_async_state_t;

/*
 * One parsed sample. timestamp_us is sensorfwd's monotonic clock in
 * microseconds -- deltas only, not wall clock.
 *
 * x/y/z are raw int32 driver counts widened to float for
 * convenience, NOT SI units. For the accelerometer they look like
 * milli-g (device flat -> z near 1000). The gyroscope scale is not
 * established anywhere; measure it, do not assume it.
 */
typedef void (*sensorfw_async_sample_cb)( void *user_data, uint64_t timestamp_us,
                                          float x, float y, float z );

/*
 * Create the client. Performs the one synchronous step
 * (dbus_bus_get, see the header comment) and nothing else: no
 * plugin is loaded, no session requested, no socket opened until
 * the gate is opened with set_wanted() and pump() is called.
 *
 * Returns NULL only if the system bus itself is unreachable.
 */
sensorfw_async_t *sensorfw_async_create( sensorfw_async_sensor_t type );

/*
 * Tear everything down. Sends releaseSensor() best-effort but does
 * not wait for its reply -- sensorfwd tracks the session by pid and
 * reclaims it when our socket closes, so a dropped reply here leaks
 * nothing.
 */
void sensorfw_async_destroy( sensorfw_async_t *c );

/*
 * The gate. true means "samples are wanted"; false means "they are
 * not, stop the flow when convenient". Cheap and idempotent -- call
 * it every frame with whatever the current answer is.
 *
 * Note this gates the SESSION, not the consumption of samples. A
 * caller that wants instant on/off should keep its own injection
 * gate and let this one lag; see docs/sensorfw.md.
 */
void sensorfw_async_set_wanted( sensorfw_async_t *c, bool wanted );

/*
 * Requested sampling interval. Best-effort: sensord may clamp to the
 * nearest rate it supports. Safe at any time; if the session is
 * already streaming the new value is sent on the next pump().
 */
void sensorfw_async_set_interval_ms( sensorfw_async_t *c, int interval_ms );

/*
 * Per-state deadline in milliseconds, default 2000 (matching
 * SENSORFW_CALL_TIMEOUT_MS in sensorfw_client.c). Applies to every
 * outstanding D-Bus call and to the socket handshake.
 */
void sensorfw_async_set_timeout_ms( sensorfw_async_t *c, int timeout_ms );

/*
 * Advance the state machine and drain whatever samples are waiting.
 * Call once per frame, unconditionally. Never blocks.
 *
 * cb is invoked once per sample, and only while STREAMING.
 *
 * The return value is a liveness hint, not an error channel:
 * false means the machine is in FAILED or DISABLED. Transient
 * failures are retried on their own with backoff, so a caller that
 * simply ignores the result still behaves correctly -- use
 * sensorfw_async_state() and sensorfw_async_last_error() when the
 * distinction matters.
 */
bool sensorfw_async_pump( sensorfw_async_t *c, sensorfw_async_sample_cb cb, void *user_data );

/* Current state, its name for logging, and the reason for the last
 * failure ("" if there has not been one). */
sensorfw_async_state_t sensorfw_async_state( const sensorfw_async_t *c );
const char *sensorfw_async_state_name( sensorfw_async_state_t state );
const char *sensorfw_async_last_error( const sensorfw_async_t *c );

/* Consecutive failures so far, and milliseconds until the next retry
 * (0 when not waiting). For a status command. */
int sensorfw_async_fail_count( const sensorfw_async_t *c );
int sensorfw_async_retry_in_ms( const sensorfw_async_t *c );

/*
 * Leave DISABLED and try again from scratch, resetting the backoff.
 * The only way out of DISABLED -- which is the point: the machine
 * gives up permanently rather than retrying forever, so re-arming it
 * has to be an explicit decision.
 */
void sensorfw_async_retry( sensorfw_async_t *c );

/*
 * Underlying sample-socket fd, or -1. Diagnostics only -- never read
 * from it directly, pump() owns the framing state.
 */
int sensorfw_async_fd( const sensorfw_async_t *c );

/* sensorfwd's plugin id for this type, e.g. "gyroscopesensor". */
const char *sensorfw_async_sensor_name( sensorfw_async_sensor_t type );

#ifdef __cplusplus
}
#endif

#endif /* SENSORFW_ASYNC_H */
