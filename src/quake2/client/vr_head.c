/*
 * vr_head.c -- см. vr_head.h.
 *
 * Что здесь есть и почему именно так:
 *
 * 1. Два независимых клиента sensorfwd (акселерометр и гироскоп) — у
 *    демона на каждый сенсор своя сессия и свой сокет. Используется
 *    НЕблокирующий sensorfw_async: set_wanted() + pump() раз в кадр.
 *    Синхронный sensorfw_client брать нельзя — у него каждый вызов
 *    D-Bus до 2 с с блокировкой, а гейт сенсоров живёт за пользовательским
 *    действием, то есть такой вызов оказался бы прямо в кадре.
 *
 * 2. Ориентация считается в «сенсорном времени», а не в кадровом: на
 *    каждый семпл гироскопа свой dt из его timestamp_us. Брать последний
 *    семпл за кадр нельзя — гироскоп меряет скорость, и выброшенные
 *    семплы дают ошибку, пропорциональную угловому ускорению, то есть
 *    ровно на быстрых поворотах головы (research/vr_sensors.md §3.4).
 *
 * 3. Маппинг осей идёт от RFBO_GetRotation() — того самого transform,
 *    которым повёрнут выводимый на экран квад. Это единственная величина,
 *    гарантированно согласованная с тем, что видит глаз: в ней уже сведены
 *    и системная ориентация, и тип панели, и текущий дисплей (§4.4, §4.6).
 *
 * 4. Камера не трогается: ни cl.viewangles, ни cl.refdef.viewangles.
 *    Это этап 3 плана, сюда он намеренно не входит.
 *
 * Про язык сообщений: комментарии русские, а выводимые строки — латиница.
 * Не вкусовщина: шрифт движка (conchars) индексируется байтом, кириллица в
 * UTF-8 займёт два байта и превратится в мусор и в консоли, и в оверлее.
 * Калибровкой пользуются глядя ровно в этот вывод, так что он обязан быть
 * читаемым.
 */

#include "client.h"
#include "vr_head.h"

#if defined(AURORA_VR)

#include "sensorfw_async.h"
#include "vr_math.h"
#include "refresh/r_fbo.h"

#include <math.h>
#include <string.h>

/* --- cvar'ы ------------------------------------------------------------ */

static cvar_t *vr_enabled;    /* гейт сессий сенсоров */
static cvar_t *vr_debug;      /* 0 выкл, 1 оверлей, 2 + сырые показания */
static cvar_t *vr_log;        /* строка в консоль раз в секунду */
static cvar_t *vr_interval_ms;

static cvar_t *vr_gyro_scale; /* °/с на единицу сырого значения гироскопа */
static cvar_t *vr_gyro_bias_x;
static cvar_t *vr_gyro_bias_y;
static cvar_t *vr_gyro_bias_z;

static cvar_t *vr_kp;
static cvar_t *vr_ki;
static cvar_t *vr_accel_gate;
static cvar_t *vr_gate_dps;
static cvar_t *vr_zupt;
static cvar_t *vr_zupt_dps;

/* --- состояние --------------------------------------------------------- */

typedef enum
{
	VRCAL_NONE,
	VRCAL_BIAS,   /* сбор смещения в покое */
	VRCAL_ROTATE, /* интегрирование известного угла */
	VRCAL_TILT    /* кросс-проверка акселерометром */
} vr_cal_mode_t;

/* Сколько результатов «поворот на 90°» помним, чтобы считать разброс. */
#define VR_CAL_HISTORY 8

typedef struct
{
	bool              inited;    /* VR_Init прошёл (в dedicated его нет) */
	sensorfw_async_t *accel;
	sensorfw_async_t *gyro;
	bool              created;   /* клиенты созданы (dbus_bus_get сделан) */
	int               interval_applied;

	vrm_filter_t      f;
	vrm_params_t      params;

	int               rotation;  /* RFBO_GetRotation() на текущем кадре */

	uint64_t          gyro_ts;   /* timestamp предыдущего семпла, мкс */
	float             yaw_offset;

	float             raw_gyro[3];  /* последние сырые значения, для отладки */
	float             raw_accel[3];

	/* Оценка фактической частоты: нужна, чтобы понять, уважил ли демон
	   setInterval (§3.4 — он имеет право прижать к своей сетке). */
	int               gyro_n, accel_n;
	float             gyro_hz, accel_hz;
	int               hz_last_ms;

	/* Калибровка (§3.3). */
	vr_cal_mode_t     cal_mode;
	int               cal_start_ms;
	int               cal_dur_ms;
	int               cal_n;
	double            cal_sum[3];   /* сырые ед.        */
	double            cal_sumsq[3]; /* сырые ед.^2      */
	double            cal_int[3];   /* сырые ед. * с    */
	float             cal_pitch_gyro;
	float             cal_pitch_accel0;
	int               cal_hist_n;
	float             cal_hist[VR_CAL_HISTORY];
} vr_state_t;

static vr_state_t vr;

/* --- маппинг осей ------------------------------------------------------ */

/*
 * Вектор в осях устройства -> вектор в осях камеры Quake 2. Источник
 * истины — RFBO_GetRotation(), все четыре строки таблицы §4.4 лежат в
 * vr_math.h. Значение кэшируется раз в кадр (VR_Frame), чтобы семплы
 * одного кадра не разъехались по разным строкам таблицы, если система
 * решит повернуть экран прямо между ними.
 */
static void VR_MapDeviceToCamera( const float d[3], float out[3] )
{
	VRM_MapAxes( vr.rotation, d, out );
}

/* --- колбэки сенсоров -------------------------------------------------- */

static void VR_AccelSample( void *ud, uint64_t ts_us, float x, float y, float z )
{
	float raw[3], cam[3];

	(void)ud;
	(void)ts_us;

	raw[0] = x;
	raw[1] = y;
	raw[2] = z;

	vr.raw_accel[0] = x;
	vr.raw_accel[1] = y;
	vr.raw_accel[2] = z;
	vr.accel_n++;

	VR_MapDeviceToCamera( raw, cam );
	VRM_FeedAccel( &vr.f, cam );
}

static void VR_GyroSample( void *ud, uint64_t ts_us, float x, float y, float z )
{
	float raw[3], corr[3], w_cam[3];
	float dt, scale;
	int i;

	(void)ud;

	raw[0] = x;
	raw[1] = y;
	raw[2] = z;

	vr.raw_gyro[0] = x;
	vr.raw_gyro[1] = y;
	vr.raw_gyro[2] = z;
	vr.gyro_n++;

	if ( vr.gyro_ts == 0 ) /* первый семпл: dt неизвестен */
	{
		vr.gyro_ts = ts_us;
		return;
	}

	dt = (float)( ts_us - vr.gyro_ts ) * 1e-6f;
	vr.gyro_ts = ts_us;

	/* Разрывы: пауза приложения в фоне, перезапуск демона, переполнение
	   буфера. Интегрировать такой интервал нельзя — он не про движение. */
	if ( dt <= 0.0f || dt > 0.1f )
		return;

	/* Смещение (bias) снимается в СЫРЫХ осях устройства: именно в них его
	   меряет vr_calibrate, и именно они не зависят от поворота экрана. */
	if ( vr.cal_mode == VRCAL_BIAS )
	{
		for ( i = 0; i < 3; i++ )
		{
			vr.cal_sum[i] += raw[i];
			vr.cal_sumsq[i] += (double)raw[i] * (double)raw[i];
		}
		vr.cal_n++;
	}

	corr[0] = raw[0] - vr_gyro_bias_x->value;
	corr[1] = raw[1] - vr_gyro_bias_y->value;
	corr[2] = raw[2] - vr_gyro_bias_z->value;

	if ( vr.cal_mode == VRCAL_ROTATE )
	{
		for ( i = 0; i < 3; i++ )
			vr.cal_int[i] += (double)corr[i] * (double)dt;
		vr.cal_n++;
	}

	VR_MapDeviceToCamera( corr, w_cam );

	scale = vr_gyro_scale->value; /* сырые ед. -> °/с */
	for ( i = 0; i < 3; i++ )
		w_cam[i] *= scale;

	/* Кросс-проверка (§3.3 шаг 2): PITCH растёт как +w_left (§4.4). */
	if ( vr.cal_mode == VRCAL_TILT )
		vr.cal_pitch_gyro += w_cam[1] * dt;

	for ( i = 0; i < 3; i++ )
		w_cam[i] = VRM_DEG2RAD( w_cam[i] );

	VRM_Step( &vr.f, &vr.params, w_cam, dt );
}

/* --- вспомогательное --------------------------------------------------- */

static float VR_NormalizeAngle( float a )
{
	while ( a > 180.0f )
		a -= 360.0f;
	while ( a < -180.0f )
		a += 360.0f;
	return a;
}

/* Наклон по акселерометру, в осях камеры. Используется калибровкой и
   отладкой; вернёт false, пока акселерометр не дал ни одного семпла. */
static bool VR_AccelAngles( float *pitch, float *roll )
{
	if ( !vr.f.have_accel )
		return false;
	VRM_AccelToPitchRoll( vr.f.accel, pitch, roll );
	return true;
}

static char *VR_SensorLine( sensorfw_async_t *c, char *label )
{
	const char *err;

	if ( c == NULL )
		return va( "%s: not created", label );

	err = sensorfw_async_last_error( c );
	return va( "%s: %s fail=%d retry=%dms %s", label,
			sensorfw_async_state_name( sensorfw_async_state( c ) ),
			sensorfw_async_fail_count( c ),
			sensorfw_async_retry_in_ms( c ),
			( err != NULL && err[0] != '\0' ) ? err : "-" );
}

/* --- команды ----------------------------------------------------------- */

static void VR_Recenter_f( void )
{
	float a[3];

	VRM_QuatToQ2Angles( vr.f.q, a );
	vr.yaw_offset = a[1];
	Com_Printf( "vr: recentered, yaw_offset = %.1f\n", vr.yaw_offset );
}

/* Ручной выход из DISABLED: автомат сдаётся навсегда после 5 неудач
   подряд, и вернуть его может только явное решение (см. sensorfw_async.h). */
static void VR_Retry_f( void )
{
	if ( vr.gyro != NULL )
		sensorfw_async_retry( vr.gyro );
	if ( vr.accel != NULL )
		sensorfw_async_retry( vr.accel );
	Com_Printf( "vr: sensor state machines re-armed\n" );
}

static void VR_Status_f( void )
{
	float a[3], ap = 0.0f, ar = 0.0f;

	VRM_QuatToQ2Angles( vr.f.q, a );

	Com_Printf( "---- vr_status ----\n" );
	Com_Printf( "vr_enabled %d, clients %s\n", (int)vr_enabled->value,
			vr.created ? "created" : "not created" );
	Com_Printf( "%s\n", VR_SensorLine( vr.gyro, "gyro " ) );
	Com_Printf( "%s\n", VR_SensorLine( vr.accel, "accel" ) );
	Com_Printf( "rate: gyro %.0f Hz, accel %.0f Hz (requested %d ms)\n",
			vr.gyro_hz, vr.accel_hz, (int)vr_interval_ms->value );
	Com_Printf( "RFBO_GetRotation() = %d  (axis-table row, report 4.4)\n", vr.rotation );
	Com_Printf( "head angles: pitch %.1f yaw %.1f roll %.1f (yaw_offset %.1f)\n",
			a[0], VR_NormalizeAngle( a[1] - vr.yaw_offset ), a[2], vr.yaw_offset );
	if ( VR_AccelAngles( &ap, &ar ) )
		Com_Printf( "from accel: pitch %.1f roll %.1f, |a| %.0f, g_ref %.0f\n",
				ap, ar, vr.f.accel_norm, vr.f.gravity_ref );
	Com_Printf( "filter bias estimate: %.3f %.3f %.3f dps (camera axes)\n",
			VRM_RAD2DEG( vr.f.integral[0] ), VRM_RAD2DEG( vr.f.integral[1] ),
			VRM_RAD2DEG( vr.f.integral[2] ) );
	Com_Printf( "vr_gyro_scale %.6g, bias %.2f %.2f %.2f\n",
			vr_gyro_scale->value, vr_gyro_bias_x->value,
			vr_gyro_bias_y->value, vr_gyro_bias_z->value );
}

static void VR_CalHelp( void )
{
	Com_Printf( "vr_calibrate <mode> - gyro calibration (research/vr_sensors.md 3.3)\n" );
	Com_Printf( "  bias [sec]  put the device down, do not touch: measures bias at rest\n" );
	Com_Printf( "  rotate      start integrating, then turn smoothly by exactly 90 deg\n" );
	Com_Printf( "  stop [deg]  finish rotate/tilt: prints vr_gyro_scale and axis leakage\n" );
	Com_Printf( "  tilt        start cross-check: tilt slowly, then stop\n" );
	Com_Printf( "  reset       reset the filter and the measurement history\n" );
}

static void VR_CalFinishBias( void )
{
	double mean[3], var[3];
	int i;

	if ( vr.cal_n < 10 )
	{
		Com_Printf( "vr_calibrate: too few samples (%d) - is the sensor streaming?\n", vr.cal_n );
		vr.cal_mode = VRCAL_NONE;
		return;
	}

	for ( i = 0; i < 3; i++ )
	{
		mean[i] = vr.cal_sum[i] / vr.cal_n;
		var[i] = vr.cal_sumsq[i] / vr.cal_n - mean[i] * mean[i];
		if ( var[i] < 0.0 )
			var[i] = 0.0;
	}

	Cvar_Set( "vr_gyro_bias_x", va( "%.4f", (float)mean[0] ) );
	Cvar_Set( "vr_gyro_bias_y", va( "%.4f", (float)mean[1] ) );
	Cvar_Set( "vr_gyro_bias_z", va( "%.4f", (float)mean[2] ) );

	Com_Printf( "vr_calibrate bias: %d samples\n", vr.cal_n );
	for ( i = 0; i < 3; i++ )
		Com_Printf( "  axis %c: bias %+.3f, sigma %.3f%s\n", 'x' + i,
				(float)mean[i], (float)sqrt( var[i] ),
				( sqrt( var[i] ) > fabs( mean[i] ) && fabs( mean[i] ) > 0.5 )
					? "  (noise comparable to bias: needs a softer filter)" : "" );
	Com_Printf( "  written to vr_gyro_bias_x/y/z\n" );

	vr.cal_mode = VRCAL_NONE;
}

static void VR_CalFinishRotate( float degrees )
{
	int i, dom = 0;
	double amax = 0.0, scale;
	float sum, mean, lo, hi;

	for ( i = 0; i < 3; i++ )
	{
		if ( fabs( vr.cal_int[i] ) > amax )
		{
			amax = fabs( vr.cal_int[i] );
			dom = i;
		}
	}

	if ( amax < 1e-3 || vr.cal_n < 5 )
	{
		Com_Printf( "vr_calibrate: integral near zero (%d samples) - was there a rotation?\n", vr.cal_n );
		vr.cal_mode = VRCAL_NONE;
		return;
	}

	scale = degrees / amax;

	Com_Printf( "vr_calibrate rotate: %d samples, angle %.1f deg\n", vr.cal_n, degrees );
	Com_Printf( "  integrals: x %+.1f y %+.1f z %+.1f (raw units * s)\n",
			(float)vr.cal_int[0], (float)vr.cal_int[1], (float)vr.cal_int[2] );
	Com_Printf( "  dominant axis %c, leakage into the others: ", 'x' + dom );
	for ( i = 0; i < 3; i++ )
	{
		if ( i == dom )
			continue;
		Com_Printf( "%c %.1f%%  ", 'x' + i, (float)( 100.0 * fabs( vr.cal_int[i] ) / amax ) );
	}
	Com_Printf( "\n" );
	Com_Printf( "  vr_gyro_scale = %.6g  (0.001 = mdeg/s, 0.0573 = mrad/s)\n", (float)scale );

	/* История замеров: §3.3 требует 5 повторов в обе стороны с разбросом
	   меньше 3 %, а считать это в уме на телефоне неудобно. */
	if ( vr.cal_hist_n < VR_CAL_HISTORY )
		vr.cal_hist[vr.cal_hist_n++] = (float)scale;

	sum = 0.0f;
	lo = vr.cal_hist[0];
	hi = vr.cal_hist[0];
	for ( i = 0; i < vr.cal_hist_n; i++ )
	{
		sum += vr.cal_hist[i];
		if ( vr.cal_hist[i] < lo )
			lo = vr.cal_hist[i];
		if ( vr.cal_hist[i] > hi )
			hi = vr.cal_hist[i];
	}
	mean = sum / vr.cal_hist_n;
	Com_Printf( "  runs %d, mean %.6g, spread %.1f%%%s\n", vr.cal_hist_n, mean,
			( mean != 0.0f ) ? 100.0f * ( hi - lo ) / mean : 0.0f,
			( vr.cal_hist_n >= 3 && mean != 0.0f && 100.0f * ( hi - lo ) / mean < 3.0f )
				? "  - converged, safe to apply" : "" );

	vr.cal_mode = VRCAL_NONE;
}

static void VR_CalFinishTilt( void )
{
	float ap = 0.0f, ar = 0.0f, d_accel, d_gyro;

	if ( !VR_AccelAngles( &ap, &ar ) )
	{
		Com_Printf( "vr_calibrate: accelerometer silent, cross-check impossible\n" );
		vr.cal_mode = VRCAL_NONE;
		return;
	}

	d_accel = ap - vr.cal_pitch_accel0;
	d_gyro = vr.cal_pitch_gyro;

	Com_Printf( "vr_calibrate tilt: gyro %+.1f deg, accel %+.1f deg, mismatch %+.1f deg\n",
			d_gyro, d_accel, d_gyro - d_accel );
	if ( fabs( d_accel ) < 10.0f )
		Com_Printf( "  tilt below 10 deg is too small, repeat with ~60 deg\n" );
	else if ( fabs( d_gyro - d_accel ) < 3.0f )
		Com_Printf( "  match: gyro scale, sign and axes are correct\n" );
	else if ( d_accel != 0.0f && d_gyro / d_accel < 0.0f )
		Com_Printf( "  opposite sign: wrong axis or wrong sign in vrm_axis_maps\n" );
	else if ( d_accel != 0.0f && fabs( d_gyro ) > 1e-3f )
		Com_Printf( "  scale is off by %.2fx: vr_gyro_scale = %.6g\n",
				d_gyro / d_accel, vr_gyro_scale->value * d_accel / d_gyro );
	else
		Com_Printf( "  gyro produced no angle at all - check vr_gyro_scale and vr_status\n" );

	vr.cal_mode = VRCAL_NONE;
}

static void VR_CalReset( void )
{
	int i;

	vr.cal_mode = VRCAL_NONE;
	vr.cal_n = 0;
	vr.cal_hist_n = 0;
	vr.cal_pitch_gyro = 0.0f;
	for ( i = 0; i < 3; i++ )
	{
		vr.cal_sum[i] = 0.0;
		vr.cal_sumsq[i] = 0.0;
		vr.cal_int[i] = 0.0;
	}
}

static void VR_Calibrate_f( void )
{
	char *mode;

	if ( Cmd_Argc() < 2 )
	{
		VR_CalHelp();
		return;
	}

	mode = Cmd_Argv( 1 );

	if ( !Q_stricmp( mode, "bias" ) )
	{
		VR_CalReset();
		vr.cal_mode = VRCAL_BIAS;
		vr.cal_start_ms = Sys_Milliseconds();
		vr.cal_dur_ms = ( Cmd_Argc() > 2 ) ? (int)( atof( Cmd_Argv( 2 ) ) * 1000.0 ) : 10000;
		if ( vr.cal_dur_ms < 1000 )
			vr.cal_dur_ms = 1000;
		Com_Printf( "vr_calibrate bias: do not touch the device for %.0f s...\n",
				vr.cal_dur_ms / 1000.0f );
	}
	else if ( !Q_stricmp( mode, "rotate" ) )
	{
		VR_CalReset();
		vr.cal_mode = VRCAL_ROTATE;
		vr.cal_start_ms = Sys_Milliseconds();
		Com_Printf( "vr_calibrate rotate: turn smoothly by exactly 90 deg around one axis, then vr_calibrate stop\n" );
	}
	else if ( !Q_stricmp( mode, "tilt" ) )
	{
		float ap = 0.0f, ar = 0.0f;

		if ( !VR_AccelAngles( &ap, &ar ) )
		{
			Com_Printf( "vr_calibrate: no accelerometer data yet\n" );
			return;
		}
		VR_CalReset();
		vr.cal_mode = VRCAL_TILT;
		vr.cal_start_ms = Sys_Milliseconds();
		vr.cal_pitch_accel0 = ap;
		Com_Printf( "vr_calibrate tilt: tilt slowly (~20 dps) by ~60 deg, then vr_calibrate stop\n" );
	}
	else if ( !Q_stricmp( mode, "stop" ) )
	{
		if ( vr.cal_mode == VRCAL_ROTATE )
			VR_CalFinishRotate( ( Cmd_Argc() > 2 ) ? (float)atof( Cmd_Argv( 2 ) ) : 90.0f );
		else if ( vr.cal_mode == VRCAL_TILT )
			VR_CalFinishTilt();
		else if ( vr.cal_mode == VRCAL_BIAS )
			VR_CalFinishBias();
		else
			Com_Printf( "vr_calibrate: nothing to stop\n" );
	}
	else if ( !Q_stricmp( mode, "reset" ) )
	{
		VR_CalReset();
		VRM_Reset( &vr.f );
		vr.yaw_offset = 0.0f;
		vr.gyro_ts = 0;
		Com_Printf( "vr_calibrate: filter and measurement history reset\n" );
	}
	else
	{
		VR_CalHelp();
	}
}

/* --- жизненный цикл клиентов ------------------------------------------- */

/*
 * Создание клиентов делает один синхронный dbus_bus_get() (асинхронного
 * у libdbus нет). Собеседник там — dbus-daemon, а не sensorfwd, то есть
 * он всегда запущен и всегда отвечает; но в кадре этому всё равно не
 * место. Поэтому создаём ровно дважды: на старте, если vr_enabled уже
 * стоял в конфиге, и в момент, когда пользователь сам переключил cvar.
 */
static void VR_CreateClients( void )
{
	if ( vr.created )
		return;

	vr.created = true;
	vr.accel = sensorfw_async_create( SENSORFW_ASYNC_ACCELEROMETER );
	vr.gyro = sensorfw_async_create( SENSORFW_ASYNC_GYROSCOPE );

	if ( vr.accel == NULL || vr.gyro == NULL )
	{
		Com_Printf( "vr: system D-Bus unreachable, sensors will not work\n" );
		return;
	}

	vr.interval_applied = 0; /* вынудить set_interval_ms на ближайшем кадре */
	Com_Printf( "vr: sensorfwd clients created (presence is resolved by the state machine)\n" );
}

static void VR_DestroyClients( void )
{
	if ( vr.gyro != NULL )
	{
		sensorfw_async_destroy( vr.gyro );
		vr.gyro = NULL;
	}
	if ( vr.accel != NULL )
	{
		sensorfw_async_destroy( vr.accel );
		vr.accel = NULL;
	}
	vr.created = false;
}

/* --- публичный API ----------------------------------------------------- */

void VR_Init( void )
{
	memset( &vr, 0, sizeof( vr ) );
	VRM_Reset( &vr.f );
	VRM_ParamsDefault( &vr.params );

	vr_enabled = Cvar_Get( "vr_enabled", "0", CVAR_ARCHIVE );
	vr_debug = Cvar_Get( "vr_debug", "0", 0 );
	vr_log = Cvar_Get( "vr_log", "0", 0 );
	/* 5 мс = 200 Гц: ниже 100 Гц интеграция «ступенчатая» — при 50 Гц и
	   повороте 200 °/с один семпл это 4° скачка ориентации (§3.4).
	   Значение best-effort, демон вправе прижать к своей сетке. */
	vr_interval_ms = Cvar_Get( "vr_interval_ms", "5", CVAR_ARCHIVE );

	/* Главная неизвестная величина всего проекта. 0.001 — гипотеза
	   «милли-градусы в секунду» по симметрии с акселерометром; проверяется
	   командой vr_calibrate (§3.3). */
	vr_gyro_scale = Cvar_Get( "vr_gyro_scale", "0.001", CVAR_ARCHIVE );
	vr_gyro_bias_x = Cvar_Get( "vr_gyro_bias_x", "0", CVAR_ARCHIVE );
	vr_gyro_bias_y = Cvar_Get( "vr_gyro_bias_y", "0", CVAR_ARCHIVE );
	vr_gyro_bias_z = Cvar_Get( "vr_gyro_bias_z", "0", CVAR_ARCHIVE );

	vr_kp = Cvar_Get( "vr_kp", "0.5", CVAR_ARCHIVE );
	vr_ki = Cvar_Get( "vr_ki", "0.02", CVAR_ARCHIVE );
	vr_accel_gate = Cvar_Get( "vr_accel_gate", "0.06", CVAR_ARCHIVE );
	vr_gate_dps = Cvar_Get( "vr_gate_dps", "40", CVAR_ARCHIVE );
	vr_zupt = Cvar_Get( "vr_zupt", "1", CVAR_ARCHIVE );
	vr_zupt_dps = Cvar_Get( "vr_zupt_dps", "2", CVAR_ARCHIVE );

	Cmd_AddCommand( "vr_recenter", VR_Recenter_f );
	Cmd_AddCommand( "vr_calibrate", VR_Calibrate_f );
	Cmd_AddCommand( "vr_status", VR_Status_f );
	Cmd_AddCommand( "vr_retry", VR_Retry_f );

	vr.hz_last_ms = Sys_Milliseconds();
	vr.inited = true;

	if ( vr_enabled->value )
		VR_CreateClients();

	vr_enabled->modified = false;
}

void VR_Shutdown( void )
{
	/* В dedicated-сборке CL_Init не выполняется, а CL_Shutdown вызывается —
	   снимать незарегистрированные команды нельзя (ругнётся в консоль). */
	if ( !vr.inited )
		return;

	vr.inited = false;
	VR_DestroyClients();
	Cmd_RemoveCommand( "vr_recenter" );
	Cmd_RemoveCommand( "vr_calibrate" );
	Cmd_RemoveCommand( "vr_status" );
	Cmd_RemoveCommand( "vr_retry" );
}

void VR_Frame( void )
{
	bool gate;
	int now, interval;

	if ( !vr.inited )
		return;

	/* Переключение cvar'а — это явное действие пользователя, а не
	   покадровый путь: создать клиентов здесь можно (см. VR_CreateClients). */
	if ( vr_enabled->modified )
	{
		vr_enabled->modified = false;
		if ( vr_enabled->value && !vr.created )
			VR_CreateClients();
	}

	if ( !vr.created || vr.gyro == NULL || vr.accel == NULL )
		return;

	/* Параметры фильтра берём раз в кадр: дёргать cvar'ы на каждый семпл
	   при 200 Гц — лишняя работа, а меняются они только руками. */
	vr.params.kp = vr_kp->value;
	vr.params.ki = vr_ki->value;
	vr.params.accel_gate = vr_accel_gate->value;
	vr.params.gate_dps = vr_gate_dps->value;
	vr.params.zupt = ( vr_zupt->value != 0.0f );
	vr.params.zupt_dps = vr_zupt_dps->value;

	/* Строка таблицы осей фиксируется на кадр (см. VR_MapDeviceToCamera). */
	vr.rotation = RFBO_GetRotation() & 3;

	interval = (int)vr_interval_ms->value;
	if ( interval < 1 )
		interval = 1;
	if ( interval != vr.interval_applied )
	{
		vr.interval_applied = interval;
		sensorfw_async_set_interval_ms( vr.gyro, interval );
		sensorfw_async_set_interval_ms( vr.accel, interval );
	}

	gate = ( vr_enabled->value != 0.0f );

	/* set_wanted + pump раз в кадр, безусловно: pump двигает автомат,
	   в том числе backoff и корректный teardown при закрытом гейте. */
	sensorfw_async_set_wanted( vr.gyro, gate );
	sensorfw_async_set_wanted( vr.accel, gate );
	sensorfw_async_pump( vr.accel, VR_AccelSample, NULL );
	sensorfw_async_pump( vr.gyro, VR_GyroSample, NULL );

	if ( !gate )
	{
		/* Гейт закрыт: следующий семпл после повторного включения придёт
		   с чужим timestamp — dt по нему считать нельзя. */
		vr.gyro_ts = 0;
		return;
	}

	now = Sys_Milliseconds();

	if ( now - vr.hz_last_ms >= 1000 )
	{
		float secs = ( now - vr.hz_last_ms ) * 0.001f;

		vr.gyro_hz = vr.gyro_n / secs;
		vr.accel_hz = vr.accel_n / secs;
		vr.gyro_n = 0;
		vr.accel_n = 0;
		vr.hz_last_ms = now;

		if ( vr_log->value )
		{
			float a[3];

			VRM_QuatToQ2Angles( vr.f.q, a );
			Com_Printf( "vr: rot=%d pitch %+6.1f yaw %+6.1f roll %+6.1f | gyro %.0f Hz accel %.0f Hz | raw w=(%.0f %.0f %.0f) a=(%.0f %.0f %.0f)\n",
					vr.rotation, a[0], VR_NormalizeAngle( a[1] - vr.yaw_offset ), a[2],
					vr.gyro_hz, vr.accel_hz,
					vr.raw_gyro[0], vr.raw_gyro[1], vr.raw_gyro[2],
					vr.raw_accel[0], vr.raw_accel[1], vr.raw_accel[2] );
		}
	}

	if ( vr.cal_mode == VRCAL_BIAS && now - vr.cal_start_ms >= vr.cal_dur_ms )
		VR_CalFinishBias();
}

bool VR_Active( void )
{
	return vr.gyro != NULL && vr.accel != NULL
	    && sensorfw_async_state( vr.gyro ) == SENSORFW_ASYNC_STATE_STREAMING;
}

void VR_GetHeadAngles( float angles[3] )
{
	VRM_QuatToQ2Angles( vr.f.q, angles );
	angles[1] = VR_NormalizeAngle( angles[1] - vr.yaw_offset );
}

void VR_DrawDebug( void )
{
	float a[3], ap = 0.0f, ar = 0.0f;
	float scale;
	int y, step;

	if ( !vr.inited || vr_debug->value == 0.0f )
		return;

	VRM_QuatToQ2Angles( vr.f.q, a );

	/* Масштаб меню: в сплит-скрине 2D жмётся вдвое по горизонтали, и
	   обычный однопиксельный шрифт в шлеме нечитаем в принципе. */
	scale = SCR_GetMenuScale();
	step = (int)( 8 * scale );
	y = step;

	DrawStringScaled( step, y, va( "VR rot=%d  %s", vr.rotation,
			VR_Active() ? "STREAMING" : "no data" ), scale );
	y += step;
	DrawStringScaled( step, y, va( "pitch %+6.1f yaw %+6.1f roll %+6.1f",
			a[0], VR_NormalizeAngle( a[1] - vr.yaw_offset ), a[2] ), scale );
	y += step;
	DrawStringScaled( step, y, va( "gyro %3.0fHz  accel %3.0fHz  |w| %5.1f",
			vr.gyro_hz, vr.accel_hz, vr.f.gyro_speed_dps ), scale );
	y += step;

	if ( VR_AccelAngles( &ap, &ar ) )
	{
		DrawStringScaled( step, y, va( "accel pitch %+6.1f roll %+6.1f  trust %d",
				ap, ar, VRM_AccelTrustworthy( &vr.f, &vr.params ) ), scale );
		y += step;
	}

	DrawStringScaled( step, y, va( "bias est %+5.2f %+5.2f %+5.2f dps  still %.1f",
			VRM_RAD2DEG( vr.f.integral[0] ), VRM_RAD2DEG( vr.f.integral[1] ),
			VRM_RAD2DEG( vr.f.integral[2] ), vr.f.still_time ), scale );
	y += step;

	DrawStringScaled( step, y, VR_SensorLine( vr.gyro, "gyro " ), scale );
	y += step;
	DrawStringScaled( step, y, VR_SensorLine( vr.accel, "accel" ), scale );
	y += step;

	if ( vr_debug->value >= 2.0f )
	{
		DrawStringScaled( step, y, va( "raw w %7.0f %7.0f %7.0f",
				vr.raw_gyro[0], vr.raw_gyro[1], vr.raw_gyro[2] ), scale );
		y += step;
		DrawStringScaled( step, y, va( "raw a %7.0f %7.0f %7.0f  |a| %.0f g %.0f",
				vr.raw_accel[0], vr.raw_accel[1], vr.raw_accel[2],
				vr.f.accel_norm, vr.f.gravity_ref ), scale );
		y += step;
	}

	if ( vr.cal_mode != VRCAL_NONE )
	{
		static char *names[] = { "", "bias", "rotate", "tilt" };

		DrawStringScaled( step, y, va( "CALIBRATE %s: %d samples, %.1f s",
				names[vr.cal_mode], vr.cal_n,
				( Sys_Milliseconds() - vr.cal_start_ms ) * 0.001f ), scale );
	}
}

#else /* !AURORA_VR — сборка без VR: заглушки */

void VR_Init( void ) {}
void VR_Shutdown( void ) {}
void VR_Frame( void ) {}
bool VR_Active( void ) { return false; }
void VR_GetHeadAngles( float angles[3] ) { angles[0] = angles[1] = angles[2] = 0.0f; }
void VR_DrawDebug( void ) {}

#endif /* AURORA_VR */
