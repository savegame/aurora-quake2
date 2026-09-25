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
 * 3. Фильтр ведёт ориентацию КОРПУСА, в сырых осях устройства. Маппинг
 *    осей в камеру идёт от RFBO_GetRotation() — того самого transform,
 *    которым повёрнут выводимый на экран квад (§4.4, §4.6), — и
 *    применяется только на выходе, при получении углов. Раньше маппинг
 *    стоял на входе фильтра, и смена rotation на устройстве давала скачок
 *    yaw -160 / roll +89 с секундами переходного процесса: для фильтра
 *    смена строки таблицы выглядела как поворот корпуса на 180°.
 *
 * 4. Камера: голова идёт в cl.viewangles (§6.1), поэтому стрельба и
 *    движение совпадают с направлением взгляда. YAW — дельтой между
 *    кадрами (стик/тач/мышь доворачивают тело поверх головы, сервер
 *    свободно выставляет углы при телепорте/респауне через delta_angles),
 *    PITCH и ROLL — абсолютно относительно горизонта (см.
 *    VR_ApplyHeadToView).
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

static cvar_t *vr_mode;       /* VR-режим целиком: стерео + сенсоры + камера */
static cvar_t *vr_stereo_prev;/* gl_stereo до включения vr_mode, -1 = не сохранён */
static cvar_t *vr_enabled;    /* гейт сессий сенсоров без стерео и камеры */
static cvar_t *vr_debug;      /* 0 выкл, 1 оверлей, 2 + сырые показания */
static cvar_t *vr_log;        /* строка в консоль раз в секунду */
static cvar_t *vr_interval_ms;

static cvar_t *vr_aim_mode;   /* способ прицеливания, см. vr_aim_mode_t */
static cvar_t *vr_recenter_ease_ms; /* плавность возврата pitch при рецентре */

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

/* Больше этого yaw головы за один кадр измениться не может физически
   (90° за 16 мс — это 5600 °/с). Такой скачок — разрыв ветви углов Эйлера
   при переходе взгляда через зенит/надир, а не движение. */
#define VR_YAW_JUMP_DEG 90.0f

/* Тот же предел, что у CL_ClampPitch (cl_input.c) и PM_ClampAngles
   (pmove.c): выше зенита и ниже надира игра не смотрит в принципе. */
#define VR_PITCH_LIMIT 89.0f

/* Значения vr_aim_mode. Cvar числовой намеренно: следующие способы
   (прицел отдельно от камеры и т.п.) добавятся сюда новым значением, а
   неизвестное значение работает как VR_AIM_GYRO — то есть как раньше. */
typedef enum
{
	VR_AIM_GYRO = 0,      /* поворот тела стиком по yaw, наклон — только головой */
	VR_AIM_GYRO_STICK = 1 /* стик поворачивает и по pitch, голова доцеливает */
} vr_aim_mode_t;

typedef struct
{
	bool              inited;    /* VR_Init прошёл (в dedicated его нет) */
	sensorfw_async_t *accel;
	sensorfw_async_t *gyro;
	bool              created;   /* клиенты созданы (dbus_bus_get сделан) */
	int               interval_applied;

	vrm_filter_t      f;         /* ориентация КОРПУСА, оси устройства */
	vrm_params_t      params;
	bool              seeded;    /* кватернион выставлен по гравитации */

	int               rotation;  /* RFBO_GetRotation() на текущем кадре */

	uint64_t          gyro_ts;   /* timestamp предыдущего семпла, мкс */
	float             yaw_offset;

	/* VR-режим */
	bool              mode_applied;  /* vr_mode, применённый к gl_stereo */
	bool              lock_valid;    /* выбор внутри пары rotation зафиксирован */
	int               lock_transform;

	/* Камера */
	bool              cam_valid;     /* cam_prev_yaw — база для дельты */
	float             cam_prev_yaw;
	bool              cam_driving;   /* на прошлом кадре голова писала углы */

	/* Прицеливание */
	int               aim_mode;      /* vr_aim_mode, снятый на кадре */
	float             aim_pitch;     /* смещение pitch, накопленное стиком */
	bool              ease_active;   /* идёт плавный возврат смещения к нулю */
	float             ease_from;     /* смещение в момент рецентра */
	int               ease_start_ms;

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
 * vr_math.h. Значение кэшируется раз в кадр (VR_Frame), чтобы семплы и
 * углы одного кадра не разъехались по разным строкам таблицы.
 *
 * В фильтр мапленые векторы больше НЕ идут (см. п.3 в шапке): здесь
 * остались только калибровка tilt и отладочный вывод наклона.
 */
static void VR_MapDeviceToCamera( const float d[3], float out[3] )
{
	VRM_MapAxes( vr.rotation, d, out );
}

/* Углы камеры по текущему rotation, без рецентра. */
static void VR_CameraAngles( float a[3] )
{
	VRM_DeviceQuatToQ2Angles( vr.f.q, vr.rotation, a );
}

/* --- колбэки сенсоров -------------------------------------------------- */

static void VR_AccelSample( void *ud, uint64_t ts_us, float x, float y, float z )
{
	float raw[3];

	(void)ud;
	(void)ts_us;

	raw[0] = x;
	raw[1] = y;
	raw[2] = z;

	vr.raw_accel[0] = x;
	vr.raw_accel[1] = y;
	vr.raw_accel[2] = z;
	vr.accel_n++;

	/* Первый семпл после старта или паузы: горизонт берём сразу из
	   гравитации. Иначе Махони с Kp 0.5 тянул бы его секундами из
	   единичного кватерниона — а pitch головы в камере абсолютный, и
	   игрок видел бы, как горизонт медленно «приезжает». */
	if ( !vr.seeded )
	{
		VRM_Reset( &vr.f );
		if ( VRM_SeedFromAccel( &vr.f, raw ) )
		{
			vr.seeded = true;
			vr.cam_valid = false;
		}
	}

	VRM_FeedAccel( &vr.f, raw );
}

static void VR_GyroSample( void *ud, uint64_t ts_us, float x, float y, float z )
{
	float raw[3], corr[3], w[3];
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

	scale = vr_gyro_scale->value; /* сырые ед. -> °/с */
	for ( i = 0; i < 3; i++ )
		w[i] = corr[i] * scale;

	/* Кросс-проверка (§3.3 шаг 2): PITCH растёт как +w_left (§4.4), а
	   «влево» — понятие камеры, поэтому здесь маппинг нужен. */
	if ( vr.cal_mode == VRCAL_TILT )
	{
		float w_cam[3];

		VR_MapDeviceToCamera( w, w_cam );
		vr.cal_pitch_gyro += w_cam[1] * dt;
	}

	for ( i = 0; i < 3; i++ )
		w[i] = VRM_DEG2RAD( w[i] );

	/* Без затравки не интегрируем: кватернион ещё не знает, где верх. */
	if ( vr.seeded )
		VRM_Step( &vr.f, &vr.params, w, dt );
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

/* Значение vr_aim_mode, приведённое к известным. Незнакомое число (чужой
   конфиг, будущий режим в старой сборке) работает как 0: лучше привычное
   управление, чем непредсказуемое. */
static int VR_AimModeValue( void )
{
	return ( (int)vr_aim_mode->value == VR_AIM_GYRO_STICK )
			? VR_AIM_GYRO_STICK : VR_AIM_GYRO;
}

/* Наклон по акселерометру, в осях камеры. Используется калибровкой и
   отладкой; вернёт false, пока акселерометр не дал ни одного семпла. */
static bool VR_AccelAngles( float *pitch, float *roll )
{
	float g_cam[3];

	if ( !vr.f.have_accel )
		return false;
	VR_MapDeviceToCamera( vr.f.accel, g_cam );
	VRM_AccelToPitchRoll( g_cam, pitch, roll );
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

/*
 * Рецентр: «куда смотрю сейчас — там yaw головы 0».
 *
 * Камеру он НЕ поворачивает: yaw в cl.viewangles идёт дельтой
 * (VR_ApplyHeadToView), и направление тела в игре от абсолютного yaw
 * головы не зависит. Нулевая точка нужна vr_status/vr_debug и будущему
 * раздельному yaw тела (§6.4). База дельты сбрасывается, чтобы смена
 * yaw_offset не прошла в камеру скачком.
 *
 * Накопленное стиком смещение pitch (vr_aim_mode 1) здесь сбрасывается —
 * в отличие от yaw, это видимый поворот камеры. Так и задумано: рецентр
 * зовут именно тогда, когда «прямо перед собой» в шлеме перестало
 * совпадать с «прямо» в игре, и по pitch расхождение даёт как раз это
 * смещение. После рецентра взгляд в игре снова равен наклону головы, то
 * есть горизонт на месте — а набрать смещение заново стиком стоит секунды.
 */
static void VR_Recenter_f( void )
{
	float a[3];

	VR_CameraAngles( a );
	vr.yaw_offset = a[1];
	/* Набранный стиком наклон возвращаем к наклону головы не скачком:
	   в шлеме мгновенный поворот камеры на десятки градусов — это рывок
	   всей картины перед глазами, от которого укачивает. Разгон и
	   торможение (smoothstep) читаются как «камера сама доводится».
	   vr_recenter_ease_ms 0 возвращает прежнее мгновенное поведение. */
	if ( vr.aim_pitch != 0.0f && vr_recenter_ease_ms->value > 0.0f )
	{
		vr.ease_from = vr.aim_pitch;
		vr.ease_start_ms = Sys_Milliseconds();
		vr.ease_active = true;
	}
	else
	{
		vr.aim_pitch = 0.0f;
		vr.ease_active = false;
	}
	vr.cam_valid = false;
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

	VR_CameraAngles( a );

	Com_Printf( "---- vr_status ----\n" );
	Com_Printf( "vr_mode %d (applied %d), gl_stereo %d, saved gl_stereo %d\n",
			(int)vr_mode->value, (int)vr.mode_applied,
			(int)Cvar_VariableValue( "gl_stereo" ), (int)vr_stereo_prev->value );
	Com_Printf( "vr_enabled %d, clients %s\n", (int)vr_enabled->value,
			vr.created ? "created" : "not created" );
	Com_Printf( "%s\n", VR_SensorLine( vr.gyro, "gyro " ) );
	Com_Printf( "%s\n", VR_SensorLine( vr.accel, "accel" ) );
	Com_Printf( "rate: gyro %.0f Hz, accel %.0f Hz (requested %d ms)\n",
			vr.gyro_hz, vr.accel_hz, (int)vr_interval_ms->value );
	Com_Printf( "RFBO_GetRotation() = %d  (axis-table row, report 4.4), lock %s %d\n",
			vr.rotation, vr.lock_valid ? "on" : "off", vr.lock_transform );
	Com_Printf( "head angles: pitch %.1f yaw %.1f roll %.1f (yaw_offset %.1f)\n",
			a[0], VR_NormalizeAngle( a[1] - vr.yaw_offset ), a[2], vr.yaw_offset );
	Com_Printf( "camera: %s\n", vr.cam_driving ? "driven by head" : "not driven" );
	Com_Printf( "vr_aim_mode %d (%s), stick pitch offset %+.1f\n",
			(int)vr_aim_mode->value,
			( vr.aim_mode == VR_AIM_GYRO_STICK ) ? "gyro + right stick" : "gyro only",
			vr.aim_pitch );
	if ( VR_AccelAngles( &ap, &ar ) )
		Com_Printf( "from accel: pitch %.1f roll %.1f, |a| %.0f, g_ref %.0f\n",
				ap, ar, vr.f.accel_norm, vr.f.gravity_ref );
	Com_Printf( "filter bias estimate: %.3f %.3f %.3f dps (device axes)\n",
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
	vr.cal_pitch_gyro = 0.0f;
	/* cal_hist_n здесь НЕ трогаем: историю замеров копит серия повторных
	   rotate (§3.3 требует 5 заходов с разбросом < 3 %), а каждый заход
	   начинается с VR_CalReset. Обнуление здесь делало серию невозможной —
	   runs всегда оставался 1. Сбрасывает историю только vr_calibrate reset. */
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
		vr.cal_hist_n = 0;
		VRM_Reset( &vr.f );
		vr.seeded = false;
		vr.cam_valid = false;
		vr.yaw_offset = 0.0f;
		vr.aim_pitch = 0.0f;
		vr.ease_active = false;
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
 * место. Поэтому создаём ровно дважды: на старте, если vr_enabled/vr_mode
 * уже стояли, и в момент, когда пользователь сам переключил cvar.
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

/* --- VR-режим ---------------------------------------------------------- */

/*
 * Применение vr_mode. Вызывается только на смене значения, поэтому ручная
 * правка gl_stereo во время VR не перетирается каждый кадр.
 *
 * Прежний gl_stereo хранится в АРХИВНОМ vr_stereo_prev, а не в статике:
 * vr_mode тоже архивный, и после выхода из игры во включённом VR в
 * user.cfg окажутся gl_stereo 2 и vr_mode 1. Статика на следующем старте
 * «запомнила» бы 2 как прежнее значение, и выключение VR оставило бы
 * сплит навсегда. -1 — «ничего не сохранено».
 */
static void VR_ApplyMode( bool on )
{
	if ( on )
	{
		if ( vr_stereo_prev->value < 0.0f )
			Cvar_SetValue( "vr_stereo_prev", Cvar_VariableValue( "gl_stereo" ) );
		Cvar_SetValue( "gl_stereo", 2 ); /* STEREO_SPLIT_HORIZONTAL, §2.2 */

		VR_CreateClients();

		/* Фиксируем то, что пользователь видит в момент включения: с этим
		   поворотом он и вставит телефон в держатель. */
		vr.lock_transform = RFBO_GetRotation();
		vr.lock_valid = true;
		vr.cam_valid = false;
		/* Смещение pitch — часть сеанса в шлеме: новый сеанс начинается
		   с «взгляд в игре = наклон головы». */
		vr.aim_pitch = 0.0f;
		vr.ease_active = false;
		Com_Printf( "vr: mode on (gl_stereo 2, screen rotation locked at %d)\n", vr.lock_transform );
	}
	else
	{
		if ( vr_stereo_prev->value >= 0.0f )
		{
			Cvar_SetValue( "gl_stereo", vr_stereo_prev->value );
			Cvar_Set( "vr_stereo_prev", "-1" );
		}

		vr.lock_valid = false;
		vr.mode_applied = false; /* до R_AuroraUpdateTransform: иначе лок вернёт старый поворот */
#if defined(AURORA_FBO)
		/* Пока VR был включён, системные повороты внутри пары
		   игнорировались — догоняем фактическую ориентацию. */
		R_AuroraUpdateTransform();
#endif
		Com_Printf( "vr: mode off (gl_stereo %d)\n", (int)Cvar_VariableValue( "gl_stereo" ) );
	}

	vr.mode_applied = on;
}

/*
 * Нужно ли фиксировать выбор внутри пары rotation — да.
 *
 * R_AuroraComputeTransform для ландшафтной игры выдаёт только 1/3 на
 * портретной панели и только 0/2 на ландшафтной: пара определяется типом
 * панели, а системная ориентация выбирает внутри неё. Систему в шлеме
 * обмануть легко: голова к плечу градусов на 60 — и акселерометр
 * композитора видит «портрет», которому на портретной панели
 * соответствует другой элемент той же пары. Картинка в шлеме
 * переворачивается вверх ногами, глаза меняются местами.
 *
 * Поэтому внутри пары держим то, что было при включении. Смену пары
 * пропускаем: она означает другой дисплей или другой тип панели
 * (SDL_WINDOWEVENT_DISPLAY_CHANGED), и там без пересчёта сломаются и
 * вывод, и размер FBO, и маппинг осей. Тип панели здесь не выводится —
 * пару даёт само значение transform (младший бит).
 */
int VR_LockTransform( int transform )
{
	if ( !vr.inited || !vr.mode_applied )
		return transform;

	if ( !vr.lock_valid || ( ( transform ^ vr.lock_transform ) & 1 ) != 0 )
	{
		if ( vr.lock_valid )
			Com_Printf( "vr: display changed, rotation pair %d -> %d, relocked\n",
					vr.lock_transform, transform );
		vr.lock_transform = transform;
		vr.lock_valid = true;
	}
	else if ( transform != vr.lock_transform )
	{
		Com_Printf( "vr: system flip to %d ignored, keeping %d\n", transform, vr.lock_transform );
	}

	return vr.lock_transform;
}

bool VR_ModeEnabled( void )
{
	return vr.inited && vr.mode_applied;
}

/* Умолчание для рецентра: Y — единственная лицевая кнопка, не занятая в
   platform.cfg. Ставим только если она пуста, чтобы не перетирать бинд
   пользователя. В platform.cfg не кладём: он общий и для сборки без VR,
   где vr_recenter не существует. */
static void VR_DefaultBinds( void )
{
	if ( keybindings[K_GAMEPAD_Y] == NULL || keybindings[K_GAMEPAD_Y][0] == '\0' )
		Key_SetBinding( K_GAMEPAD_Y, "vr_recenter" );
}

/* --- публичный API ----------------------------------------------------- */

void VR_Init( void )
{
	memset( &vr, 0, sizeof( vr ) );
	VRM_Reset( &vr.f );
	VRM_ParamsDefault( &vr.params );

	vr_mode = Cvar_Get( "vr_mode", "0", CVAR_ARCHIVE );
	vr_stereo_prev = Cvar_Get( "vr_stereo_prev", "-1", CVAR_ARCHIVE );
	vr_enabled = Cvar_Get( "vr_enabled", "0", CVAR_ARCHIVE );
	vr_debug = Cvar_Get( "vr_debug", "0", 0 );
	vr_log = Cvar_Get( "vr_log", "0", 0 );
	/* 5 мс = 200 Гц: ниже 100 Гц интеграция «ступенчатая» — при 50 Гц и
	   повороте 200 °/с один семпл это 4° скачка ориентации (§3.4).
	   Значение best-effort, демон вправе прижать к своей сетке. */
	vr_interval_ms = Cvar_Get( "vr_interval_ms", "5", CVAR_ARCHIVE );

	/* Способ прицеливания: 0 — только гироскоп (как было), 1 — гироскоп
	   плюс наклон правым стиком. Архивный, задаётся и из лаунчера
	   (env AURORA_VR_AIM, misc.c — там же, где vr_mode), и через
	   +set vr_aim_mode, и меняется прямо в игре. */
	vr_aim_mode = Cvar_Get( "vr_aim_mode", "0", CVAR_ARCHIVE );
	/* Длительность доводки pitch после vr_recenter, мс. 0 — мгновенно. */
	vr_recenter_ease_ms = Cvar_Get( "vr_recenter_ease_ms", "250", CVAR_ARCHIVE );

	/* Сырые единицы гироскопа sensorfwd — милли-градусы в секунду, это не
	   гипотеза, а цепочка из исходников: hybris-адаптер sensorfw пишет
	   rad/s из Android HAL как x * RADIANS_TO_DEGREES * 1000, а QtSensors
	   (sensorfwgyroscope.cpp) обратно умножает на MILLI = 0.001 и получает
	   °/с. Заводская калибровка живёт ниже, в вендорском HAL, поэтому
	   множитель фиксированный. Cvar оставлен на случай устройства с иным
	   адаптером; vr_calibrate rotate его перепроверяет. */
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

	VR_DefaultBinds();
	VR_LensInit();

	vr.hz_last_ms = Sys_Milliseconds();
	vr.aim_mode = VR_AimModeValue();
	vr.inited = true;

	if ( vr_enabled->value )
		VR_CreateClients();

	/* Рендер к этому моменту уже поднят (R_initialize в CL_Init идёт
	   раньше), так что применить режим можно сразу: первый же кадр
	   выйдет в стерео, а не моргнёт моно-кадром. */
	if ( vr_mode->value )
		VR_ApplyMode( true );
	else if ( vr_stereo_prev->value >= 0.0f )
		VR_ApplyMode( false ); /* VR выключили до старта (лаунчер): вернуть gl_stereo */

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
	VR_LensShutdown();
}

void VR_Frame( void )
{
	bool gate, mode;
	int now, interval, aim;

	if ( !vr.inited )
		return;

	/* Смена способа прицеливания на лету. Накопленное стиком смещение
	   pitch относится к прежнему режиму: в «только гироскопе» оно не
	   используется, и при возврате всплыло бы скачком камеры. */
	aim = VR_AimModeValue();
	if ( aim != vr.aim_mode )
	{
		vr.aim_mode = aim;
		vr.aim_pitch = 0.0f;
		vr.ease_active = false;
	}

	/* Переключение режима из консоли: сравнение со значением, а не флаг
	   modified, — флаг могли сбросить посторонние, а значение надёжно. */
	mode = ( vr_mode->value != 0.0f );
	if ( mode != vr.mode_applied )
		VR_ApplyMode( mode );

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

	gate = ( vr_enabled->value != 0.0f ) || mode;

	/* set_wanted + pump раз в кадр, безусловно: pump двигает автомат,
	   в том числе backoff и корректный teardown при закрытом гейте. */
	sensorfw_async_set_wanted( vr.gyro, gate );
	sensorfw_async_set_wanted( vr.accel, gate );
	sensorfw_async_pump( vr.accel, VR_AccelSample, NULL );
	sensorfw_async_pump( vr.gyro, VR_GyroSample, NULL );

	if ( !gate )
	{
		/* Гейт закрыт: следующий семпл после повторного включения придёт
		   с чужим timestamp — dt по нему считать нельзя. И ориентация к
		   тому времени устареет: телефон могли как угодно перевернуть,
		   поэтому горизонт заново берётся затравкой. */
		vr.gyro_ts = 0;
		vr.seeded = false;
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

			VR_CameraAngles( a );
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
	VR_CameraAngles( angles );
	angles[1] = VR_NormalizeAngle( angles[1] - vr.yaw_offset );
}

/* delta_angles сервера в диапазоне ±180 — как в CL_ClampPitch. */
static float VR_ServerDelta( int axis )
{
	float d = SHORT2ANGLE( cl.frame.playerstate.pmove.delta_angles[axis] );

	if ( d > 180.0f )
		d -= 360.0f;
	return d;
}

/*
 * Голова -> cl.viewangles.
 *
 * YAW — дельтой. Абсолютный yaw головы вписывать нельзя: он затёр бы
 * доворот тела стиком/тачем/мышью, а yaw тела в игре складывается ещё и
 * из delta_angles сервера (cmd.angles + delta_angles, pmove.c
 * PM_ClampAngles). Сервер пишет delta_angles при респауне и телепорте,
 * клиентский cl.viewangles при этом не трогает (cl_input.c: IN_CenterView
 * и CL_ClampPitch только читают их) — поэтому прибавка дельты к
 * cl.viewangles с сервером не воюет.
 *
 * PITCH и ROLL — абсолютно относительно горизонта. Горизонт в шлеме
 * обязан совпадать с реальным, а дельта накапливала бы ошибку на каждом
 * упоре: CL_ClampPitch зажимает pitch ±89, и «лишний» наклон головы за
 * пределом терялся бы, после чего горизонт навсегда уезжал. При
 * абсолютной записи зажим срабатывает кадр за кадром и ничего не копит.
 * Вычитание delta_angles нужно, чтобы видимый pitch (cmd + delta) равнялся
 * pitch головы при любом значении, которое выставил сервер.
 *
 * При vr_aim_mode 1 к абсолютному pitch головы прибавляется смещение,
 * накопленное правым стиком (VR_AimPitch): голова по-прежнему задаёт
 * горизонт и точное доцеливание, а стик даёт грубый наклон, не задирая
 * шею. Дельтой pitch не делаем и здесь — по той же причине, по которой
 * абсолютен он сам.
 *
 * ROLL проходит до рендера без отдельного канала: ANGLE2SHORT по всем
 * трём углам (CL_FinishMove) -> PM_ClampAngles (цикл по трём, зажим только
 * PITCH) -> cl.predicted_angles -> cl.refdef.viewangles (CL_CalcViewValues)
 * -> матрица вида. Без предсказания (cl_predict 0) путь тот же:
 * CL_PredictMovement копирует viewangles + delta_angles по всем трём.
 */
/*
 * Управляет ли голова камерой прямо сейчас. В меню, консоли, до входа в
 * игру, без данных сенсоров и во время калибровки линз (мишень обязана
 * стоять в центре) — нет.
 *
 * Условие вынесено отдельно, потому что его же спрашивает ввод: пока
 * голова камерой не управляет, наклон стиком обязан идти в cl.viewangles
 * обычным путём, иначе в тех же меню и без сенсоров смотреть вверх-вниз
 * стало бы нечем.
 */
static bool VR_HeadDrives( void )
{
	return vr.inited && vr.mode_applied && vr.seeded && VR_Active()
	    && cls.state == ca_active && cls.key_dest == key_game
	    && !VR_LensCalibrating();
}

/*
 * Смещение pitch от стика поверх абсолютного pitch головы (vr_aim_mode 1).
 *
 * Сумма зажимается тем же ±89, что CL_ClampPitch и PM_ClampAngles, —
 * дальше игра всё равно не смотрит. На упоре смещение подтягивается к
 * границе, но ТОЛЬКО когда это уменьшает его модуль. Два разных случая,
 * и одно правило закрывает оба:
 *
 *  - игрок держит стик в упоре: без подтягивания смещение росло бы
 *    неограниченно, и обратный ход стика первые секунды не давал бы
 *    никакого движения картинки («залипание»). С ним отклик мгновенный;
 *  - игрок сам задрал голову за 89° при нулевом смещении: тут
 *    подтягивание, наоборот, СОЗДАЛО бы смещение из ниоткуда, и вернув
 *    голову к горизонту игрок увидел бы уехавший взгляд. В этом случае
 *    смещение не трогаем — хватает зажима выходного угла, ровно как в
 *    режиме «только гироскоп».
 *
 * Смещение при этом остаётся «добавкой к голове»: вернув голову в
 * исходное положение, игрок получает ровно тот угол, который набрал
 * стиком, а не что-то новое.
 */
/* Шаг плавного возврата смещения к нулю после рецентра. */
static void VR_AimEaseStep( void )
{
	float dur, t;

	if ( !vr.ease_active )
		return;

	dur = vr_recenter_ease_ms->value;
	if ( dur <= 0.0f )
	{
		vr.aim_pitch = 0.0f;
		vr.ease_active = false;
		return;
	}

	t = (float)( Sys_Milliseconds() - vr.ease_start_ms ) / dur;
	if ( t >= 1.0f )
	{
		vr.aim_pitch = 0.0f;
		vr.ease_active = false;
		return;
	}
	if ( t < 0.0f ) /* Sys_Milliseconds мог перещёлкнуть — не застреваем */
		t = 0.0f;

	/* smoothstep: плавный старт и плавная остановка. */
	vr.aim_pitch = vr.ease_from * ( 1.0f - t * t * ( 3.0f - 2.0f * t ) );
}

static float VR_AimPitch( float head_pitch )
{
	float sum;
	float want;

	VR_AimEaseStep();
	sum = head_pitch + vr.aim_pitch;

	if ( sum > VR_PITCH_LIMIT )
		sum = VR_PITCH_LIMIT;
	else if ( sum < -VR_PITCH_LIMIT )
		sum = -VR_PITCH_LIMIT;
	else
		return sum;

	want = sum - head_pitch;
	if ( fabs( want ) < fabs( vr.aim_pitch ) )
		vr.aim_pitch = want;

	return sum;
}

bool VR_AimPitchTakesStick( void )
{
	return vr.aim_mode == VR_AIM_GYRO_STICK && VR_HeadDrives();
}

void VR_AimPitchAdd( float delta )
{
	/* Игрок тронул стик — доводка больше не нужна, управление у него. */
	vr.ease_active = false;

	/* Зажим — в VR_AimPitch, на кадре применения: он зависит от текущего
	   наклона головы, которого здесь ещё нет. */
	vr.aim_pitch += delta;
}

void VR_ApplyHeadToView( void )
{
	float head[3], d, pitch;
	bool drive;

	if ( !vr.inited )
		return;

	/* База дельты yaw сбрасывается, чтобы повороты головы за время без
	   управления не прилетели в камеру разом при возврате. */
	drive = VR_HeadDrives();

	if ( !drive )
	{
		if ( vr.cam_driving && !vr.mode_applied )
		{
			/* VR выключили: roll головы остался бы в cl.viewangles
			   навсегда — в обычной игре его больше никто не пишет. */
			cl.viewangles[ROLL] = 0.0f;
		}
		if ( !vr.mode_applied )
			vr.cam_driving = false;
		vr.cam_valid = false;
		return;
	}

	VR_GetHeadAngles( head );

	if ( vr.cam_valid )
	{
		d = VR_NormalizeAngle( head[YAW] - vr.cam_prev_yaw );
		if ( fabs( d ) < VR_YAW_JUMP_DEG )
			cl.viewangles[YAW] += d;
	}
	vr.cam_prev_yaw = head[YAW];
	vr.cam_valid = true;

	pitch = head[PITCH];
	if ( vr.aim_mode == VR_AIM_GYRO_STICK )
		pitch = VR_AimPitch( pitch );

	cl.viewangles[PITCH] = pitch - VR_ServerDelta( PITCH );
	cl.viewangles[ROLL] = head[ROLL] - VR_ServerDelta( ROLL );
	vr.cam_driving = true;
}

void VR_DrawDebug( void )
{
	float a[3], ap = 0.0f, ar = 0.0f;
	float scale;
	int y, step;
	char aim[32];

	if ( !vr.inited || vr_debug->value == 0.0f )
		return;

	VR_CameraAngles( a );

	/* Масштаб меню: в сплит-скрине 2D жмётся вдвое по горизонтали, и
	   обычный однопиксельный шрифт в шлеме нечитаем в принципе. */
	scale = SCR_GetMenuScale();
	step = (int)( 8 * scale );
	y = step;

	DrawStringScaled( step, y, va( "VR rot=%d  %s  mode %d%s", vr.rotation,
			VR_Active() ? "STREAMING" : "no data", (int)vr.mode_applied,
			vr.cam_driving ? " cam" : "" ), scale );
	y += step;
	/* va() у движка — один статический буфер, вложенный вызов затёр бы
	   внешний: смещение форматируем отдельно. */
	aim[0] = '\0';
	if ( vr.aim_mode == VR_AIM_GYRO_STICK )
		Com_sprintf( aim, sizeof( aim ), "  aim %+.1f", vr.aim_pitch );

	DrawStringScaled( step, y, va( "pitch %+6.1f yaw %+6.1f roll %+6.1f%s",
			a[0], VR_NormalizeAngle( a[1] - vr.yaw_offset ), a[2], aim ), scale );
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
bool VR_ModeEnabled( void ) { return false; }
void VR_GetHeadAngles( float angles[3] ) { angles[0] = angles[1] = angles[2] = 0.0f; }
void VR_ApplyHeadToView( void ) {}
bool VR_AimPitchTakesStick( void ) { return false; }
void VR_AimPitchAdd( float delta ) { (void)delta; }
int VR_LockTransform( int transform ) { return transform; }
void VR_DrawDebug( void ) {}

#endif /* AURORA_VR */
