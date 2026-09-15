/*
 * vr_filter_test.c -- синтетический тест математики ориентации головы
 * (research/vr_sensors.md §8.5 п.1).
 *
 * Зачем он есть. Проверить фильтр на устройстве можно только надев шлем,
 * а самые дорогие ошибки в такой математике — не «неточно», а «знак не
 * тот» и «оси перепутаны»: в шлеме это выглядит как «смотрю вниз —
 * камера уезжает вбок», и отлаживается мучительно. На синтетике те же
 * ошибки видны за секунду: генерируем идеальную траекторию головы, из
 * неё честно считаем, что показали бы гироскоп и акселерометр, прогоняем
 * фильтр и сравниваем с истиной.
 *
 * Тест НЕ проверяет: хиральность «буфер окна <-> корпус устройства»
 * (§4.5) и масштаб гироскопа (§3.3) — это измеряется только на железе.
 * Он проверяет, что при ПРАВИЛЬНЫХ входных данных математика верна.
 *
 * Сборка: цель vr_filter_test в CMakeLists.txt (только host, AURORA_VR=ON).
 */

#include "vr_math.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

static void check( int ok, const char *what, double got, double limit )
{
	if ( ok )
	{
		printf( "  ok   %-46s  %8.4f (предел %.4f)\n", what, got, limit );
	}
	else
	{
		printf( "  FAIL %-46s  %8.4f (предел %.4f)\n", what, got, limit );
		g_failures++;
	}
}

/* ------------------------------------------------------------------------
 * Опорная реализация AngleVectors из движка (common/shared/shared.c).
 * Копия здесь намеренная: тест обязан сверяться с тем, что делает Quake 2,
 * а не с тем, что делает vr_math.h.
 * ---------------------------------------------------------------------- */
static void q2_angle_vectors( const float angles[3], float fwd[3], float left[3], float up[3] )
{
	float sp, sy, sr, cp, cy, cr;
	float a;

	a = VRM_DEG2RAD( angles[1] ); /* YAW */
	sy = sinf( a );
	cy = cosf( a );
	a = VRM_DEG2RAD( angles[0] ); /* PITCH */
	sp = sinf( a );
	cp = cosf( a );
	a = VRM_DEG2RAD( angles[2] ); /* ROLL */
	sr = sinf( a );
	cr = cosf( a );

	fwd[0] = cp * cy;
	fwd[1] = cp * sy;
	fwd[2] = -sp;

	/* right из shared.c; left = -right (ось +Y в Q2 смотрит влево) */
	left[0] = -( -1 * sr * sp * cy + -1 * cr * -sy );
	left[1] = -( -1 * sr * sp * sy + -1 * cr * cy );
	left[2] = -( -1 * sr * cp );

	up[0] = cr * sp * cy + -sr * -sy;
	up[1] = cr * sp * sy + -sr * cy;
	up[2] = cr * cp;
}

/* Матрица (столбцы forward/left/up) -> кватернион, метод Шеппарда. */
static void mat_to_quat( const float f[3], const float l[3], const float u[3], float q[4] )
{
	float m00 = f[0], m10 = f[1], m20 = f[2];
	float m01 = l[0], m11 = l[1], m21 = l[2];
	float m02 = u[0], m12 = u[1], m22 = u[2];
	float tr = m00 + m11 + m22;
	float s;

	if ( tr > 0.0f )
	{
		s = sqrtf( tr + 1.0f ) * 2.0f;
		q[0] = 0.25f * s;
		q[1] = ( m21 - m12 ) / s;
		q[2] = ( m02 - m20 ) / s;
		q[3] = ( m10 - m01 ) / s;
	}
	else if ( m00 > m11 && m00 > m22 )
	{
		s = sqrtf( 1.0f + m00 - m11 - m22 ) * 2.0f;
		q[0] = ( m21 - m12 ) / s;
		q[1] = 0.25f * s;
		q[2] = ( m01 + m10 ) / s;
		q[3] = ( m02 + m20 ) / s;
	}
	else if ( m11 > m22 )
	{
		s = sqrtf( 1.0f + m11 - m00 - m22 ) * 2.0f;
		q[0] = ( m02 - m20 ) / s;
		q[1] = ( m01 + m10 ) / s;
		q[2] = 0.25f * s;
		q[3] = ( m12 + m21 ) / s;
	}
	else
	{
		s = sqrtf( 1.0f + m22 - m00 - m11 ) * 2.0f;
		q[0] = ( m10 - m01 ) / s;
		q[1] = ( m02 + m20 ) / s;
		q[2] = ( m12 + m21 ) / s;
		q[3] = 0.25f * s;
	}
	VRM_QuatNormalize( q );
}

static void angles_to_quat( const float angles[3], float q[4] )
{
	float f[3], l[3], u[3];

	q2_angle_vectors( angles, f, l, u );
	mat_to_quat( f, l, u, q );
}

static float ang_diff( float a, float b )
{
	float d = a - b;

	while ( d > 180.0f )
		d -= 360.0f;
	while ( d < -180.0f )
		d += 360.0f;
	return fabsf( d );
}

/* ------------------------------------------------------------------------
 * 1. Таблица осей (§4.4): каждая строка обязана быть правой тройкой
 * ---------------------------------------------------------------------- */
static void test_axis_maps( void )
{
	int rot, i, j;
	float worst_orth = 0.0f, worst_det = 0.0f;

	printf( "[1] таблица маппинга осей устройство -> камера\n" );

	for ( rot = 0; rot < 4; rot++ )
	{
		float m[3][3];
		float det, cross[3];

		/* Строки матрицы получаем, прогнав через маппинг базис устройства. */
		for ( j = 0; j < 3; j++ )
		{
			float e[3] = { 0.0f, 0.0f, 0.0f };
			float o[3];

			e[j] = 1.0f;
			VRM_MapAxes( rot, e, o );
			for ( i = 0; i < 3; i++ )
				m[i][j] = o[i];
		}

		/* Ортонормированность строк. */
		for ( i = 0; i < 3; i++ )
		{
			for ( j = 0; j < 3; j++ )
			{
				float d = m[i][0] * m[j][0] + m[i][1] * m[j][1] + m[i][2] * m[j][2];
				float want = ( i == j ) ? 1.0f : 0.0f;

				if ( fabsf( d - want ) > worst_orth )
					worst_orth = fabsf( d - want );
			}
		}

		/* forward x left == up: правая тройка, а не зеркальная (§4.4). */
		cross[0] = m[0][1] * m[1][2] - m[0][2] * m[1][1];
		cross[1] = m[0][2] * m[1][0] - m[0][0] * m[1][2];
		cross[2] = m[0][0] * m[1][1] - m[0][1] * m[1][0];
		det = 0.0f;
		for ( i = 0; i < 3; i++ )
			det += ( cross[i] - m[2][i] ) * ( cross[i] - m[2][i] );
		if ( sqrtf( det ) > worst_det )
			worst_det = sqrtf( det );

		printf( "       rot=%d  fwd=(%+.0f %+.0f %+.0f) left=(%+.0f %+.0f %+.0f) up=(%+.0f %+.0f %+.0f)\n",
				rot, m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2],
				m[2][0], m[2][1], m[2][2] );
	}

	check( worst_orth < 1e-6f, "строки ортонормированы", worst_orth, 1e-6 );
	check( worst_det < 1e-6f, "forward x left == up (правая тройка)", worst_det, 1e-6 );
}

/* ------------------------------------------------------------------------
 * 2. Извлечение углов Q2 из кватерниона — точный обратный ход AngleVectors
 * ---------------------------------------------------------------------- */
static void test_angle_roundtrip( void )
{
	float worst = 0.0f;
	int p, y, r;

	printf( "[2] кватернион -> углы Quake 2 (сверка с AngleVectors)\n" );

	for ( p = -80; p <= 80; p += 20 )
	{
		for ( y = -180; y < 180; y += 45 )
		{
			for ( r = -60; r <= 60; r += 30 )
			{
				float in[3], q[4], out[3];
				float d;

				in[0] = (float)p;
				in[1] = (float)y;
				in[2] = (float)r;

				angles_to_quat( in, q );
				VRM_QuatToQ2Angles( q, out );

				d = ang_diff( out[0], in[0] );
				if ( d > worst )
					worst = d;
				d = ang_diff( out[1], in[1] );
				if ( d > worst )
					worst = d;
				d = ang_diff( out[2], in[2] );
				if ( d > worst )
					worst = d;
			}
		}
	}

	check( worst < 0.05f, "макс. ошибка по всем трём углам, град", worst, 0.05 );
}

/* ------------------------------------------------------------------------
 * 3. Наклон по акселерометру
 * ---------------------------------------------------------------------- */
static void test_accel_angles( void )
{
	float worst = 0.0f;
	int p, r;

	printf( "[3] pitch/roll из вектора гравитации\n" );

	for ( p = -60; p <= 60; p += 15 )
	{
		for ( r = -60; r <= 60; r += 15 )
		{
			float in[3], f[3], l[3], u[3], g[3];
			float gp, gr, d;

			in[0] = (float)p;
			in[1] = 37.0f; /* yaw не должен влиять вообще */
			in[2] = (float)r;

			q2_angle_vectors( in, f, l, u );
			/* акселерометр в покое = мировой «вверх» в осях камеры */
			g[0] = f[2] * 1000.0f;
			g[1] = l[2] * 1000.0f;
			g[2] = u[2] * 1000.0f;

			VRM_AccelToPitchRoll( g, &gp, &gr );
			d = ang_diff( gp, in[0] );
			if ( d > worst )
				worst = d;
			d = ang_diff( gr, in[2] );
			if ( d > worst )
				worst = d;
		}
	}

	check( worst < 0.05f, "макс. ошибка pitch/roll, град", worst, 0.05 );
}

/* ------------------------------------------------------------------------
 * Генератор траектории: «идеальная» голова + её показания сенсоров
 * ---------------------------------------------------------------------- */
typedef struct
{
	float yaw_amp, yaw_hz;
	float pitch_amp, pitch_hz;
	float roll_amp, roll_hz;
} traj_t;

static void traj_angles( const traj_t *t, float time, float angles[3] )
{
	angles[0] = t->pitch_amp * sinf( 2.0f * VRM_PI * t->pitch_hz * time );
	angles[1] = t->yaw_amp * sinf( 2.0f * VRM_PI * t->yaw_hz * time );
	angles[2] = t->roll_amp * sinf( 2.0f * VRM_PI * t->roll_hz * time );
}

/* Угловая скорость в осях камеры из двух соседних кватернионов:
   q(t+dt) = q(t) * dq, dq ~ (1, w*dt/2) — ровно та же связь, которую
   интегрирует VRM_Step, так что тест проверяет её, а не дублирует. */
static void traj_omega( const float q_prev[4], const float q_cur[4], float dt, float w[3] )
{
	float inv[4], dq[4];

	inv[0] = q_prev[0];
	inv[1] = -q_prev[1];
	inv[2] = -q_prev[2];
	inv[3] = -q_prev[3];

	VRM_QuatMul( inv, q_cur, dq );
	if ( dq[0] < 0.0f ) /* та же дуга, короткий путь */
	{
		dq[0] = -dq[0];
		dq[1] = -dq[1];
		dq[2] = -dq[2];
		dq[3] = -dq[3];
	}

	w[0] = 2.0f * dq[1] / dt;
	w[1] = 2.0f * dq[2] / dt;
	w[2] = 2.0f * dq[3] / dt;
}

/* Детерминированный «шум»: воспроизводимость важнее статистической
   чистоты — падающий тест должен падать одинаково. */
static unsigned g_rng = 12345u;

static float noise( float amp )
{
	g_rng = g_rng * 1103515245u + 12345u;
	return amp * ( ( (float)( ( g_rng >> 16 ) & 0x7fff ) / 16383.5f ) - 1.0f );
}

/* ------------------------------------------------------------------------
 * 4. Сходимость фильтра при заданном bias гироскопа
 * ---------------------------------------------------------------------- */
static void test_convergence( void )
{
	traj_t t = { 20.0f, 0.10f, 15.0f, 0.07f, 8.0f, 0.05f };
	vrm_filter_t f;
	vrm_params_t p;
	/* 1.5, -0.8, 1.0 °/с — типичный MEMS-bias из §5.1 */
	float bias[3] = { VRM_DEG2RAD( 1.5f ), VRM_DEG2RAD( -0.8f ), VRM_DEG2RAD( 1.0f ) };
	float dt = 0.005f; /* 200 Гц — то, что просим у sensorfwd (§3.4) */
	float q_prev[4], q_cur[4];
	float worst_pitch = 0.0f, worst_roll = 0.0f, worst_bias = 0.0f;
	float angles[3];
	/* 100 с прогона, замер по последним 40: И-звено Махони при Ki 0.02
	   учит bias десятками секунд — мерить надо установившийся режим, а
	   не переходный процесс. */
	int step, steps = (int)( 100.0f / dt );
	int i;

	printf( "[4] фильтр Махони: сходимость pitch/roll при bias гироскопа\n" );

	VRM_Reset( &f );
	VRM_ParamsDefault( &p );
	p.zupt = 0; /* ZUPT проверяется отдельно, тут мешал бы */

	traj_angles( &t, 0.0f, angles );
	angles_to_quat( angles, q_prev );
	/* фильтр стартует из единичного кватерниона — пусть сам доедет */

	for ( step = 1; step < steps; step++ )
	{
		float time = step * dt;
		float w_true[3], w_meas[3], g_cam[3];
		float fwd[3], left[3], up[3], est[3];

		traj_angles( &t, time, angles );
		angles_to_quat( angles, q_cur );
		traj_omega( q_prev, q_cur, dt, w_true );
		memcpy( q_prev, q_cur, sizeof( q_prev ) );

		/* «Показания» гироскопа: истина + bias + шум */
		for ( i = 0; i < 3; i++ )
			w_meas[i] = w_true[i] + bias[i] + noise( VRM_DEG2RAD( 0.3f ) );

		/* «Показания» акселерометра: мировой «вверх» в осях камеры,
		   в сырых единицах ~1000 (milli-g) плюс шум */
		q2_angle_vectors( angles, fwd, left, up );
		g_cam[0] = fwd[2] * 1000.0f + noise( 8.0f );
		g_cam[1] = left[2] * 1000.0f + noise( 8.0f );
		g_cam[2] = up[2] * 1000.0f + noise( 8.0f );

		VRM_FeedAccel( &f, g_cam );
		VRM_Step( &f, &p, w_meas, dt );

		if ( time < 60.0f ) /* прогрев: фильтр ищет вертикаль и bias */
			continue;

		VRM_QuatToQ2Angles( f.q, est );
		if ( ang_diff( est[0], angles[0] ) > worst_pitch )
			worst_pitch = ang_diff( est[0], angles[0] );
		if ( ang_diff( est[2], angles[2] ) > worst_roll )
			worst_roll = ang_diff( est[2], angles[2] );
	}

	/* И-звено обязано было выучить bias (по двум осям, где есть
	   гравитационная опора; yaw без ZUPT не корректируется — §5.5). */
	for ( i = 0; i < 3; i++ )
	{
		float err = fabsf( f.integral[i] + bias[i] );

		if ( i != 2 && err > worst_bias ) /* i=2 — ось «вверх», то есть yaw */
			worst_bias = err;
	}

	check( worst_pitch < 2.0f, "макс. ошибка PITCH в установившемся режиме", worst_pitch, 2.0 );
	check( worst_roll < 2.0f, "макс. ошибка ROLL в установившемся режиме", worst_roll, 2.0 );
	check( worst_bias < VRM_DEG2RAD( 0.5f ), "остаточная ошибка оценки bias, рад/с",
			worst_bias, VRM_DEG2RAD( 0.5f ) );
	check( f.gravity_ref > 900.0f && f.gravity_ref < 1100.0f,
			"автокалибровка gravity_ref, сырые ед.", f.gravity_ref, 1000.0 );
}

/* ------------------------------------------------------------------------
 * 5. Гейт доверия акселерометру (§5.4): фильтр не должен «клевать»
 *    на линейное ускорение
 * ---------------------------------------------------------------------- */
static void test_accel_gate( void )
{
	vrm_filter_t f;
	vrm_params_t p;
	float dt = 0.005f;
	float w[3] = { 0.0f, 0.0f, 0.0f };
	float g_ok[3] = { 0.0f, 0.0f, 1000.0f };
	float g_bad[3] = { 700.0f, 0.0f, 1000.0f }; /* +0.7 g вперёд: тангенциальное */
	float est[3];
	float tilt_gated, tilt_ungated;
	int step;

	printf( "[5] гейт доверия акселерометру\n" );

	/* (а) гейт работает: |a| = 1.22 g, выходит за ±6 % — коррекции нет */
	VRM_Reset( &f );
	VRM_ParamsDefault( &p );
	for ( step = 0; step < 200; step++ ) /* набрать gravity_ref в покое */
	{
		VRM_FeedAccel( &f, g_ok );
		VRM_Step( &f, &p, w, dt );
	}
	for ( step = 0; step < 400; step++ ) /* 2 с «рывка» */
	{
		VRM_FeedAccel( &f, g_bad );
		VRM_Step( &f, &p, w, dt );
	}
	VRM_QuatToQ2Angles( f.q, est );
	tilt_gated = fabsf( est[0] );

	/* (б) тот же ввод с распахнутым гейтом — для сравнения: так выглядит
	      завал горизонта, ради предотвращения которого гейт и нужен */
	VRM_Reset( &f );
	VRM_ParamsDefault( &p );
	p.accel_gate = 10.0f;
	p.gate_dps = 100000.0f;
	for ( step = 0; step < 200; step++ )
	{
		VRM_FeedAccel( &f, g_ok );
		VRM_Step( &f, &p, w, dt );
	}
	for ( step = 0; step < 400; step++ )
	{
		VRM_FeedAccel( &f, g_bad );
		VRM_Step( &f, &p, w, dt );
	}
	VRM_QuatToQ2Angles( f.q, est );
	tilt_ungated = fabsf( est[0] );

	printf( "       без гейта горизонт ушёл бы на %.1f град\n", tilt_ungated );
	check( tilt_gated < 0.1f, "завал горизонта с гейтом, град", tilt_gated, 0.1 );
	check( tilt_ungated > 5.0f, "без гейта завал заметен (контроль теста)", tilt_ungated, 5.0 );
}

/* ------------------------------------------------------------------------
 * 6. ZUPT: гашение дрейфа yaw в покое (§5.5 в)
 * ---------------------------------------------------------------------- */
static float yaw_drift_per_min( int zupt )
{
	vrm_filter_t f;
	vrm_params_t p;
	float dt = 0.005f;
	float bias[3] = { 0.0f, 0.0f, VRM_DEG2RAD( 1.0f ) }; /* 1 °/с по оси «вверх» */
	float g_ok[3] = { 0.0f, 0.0f, 1000.0f };
	float a1[3], a2[3];
	int step;
	int steps = (int)( 60.0f / dt );

	VRM_Reset( &f );
	VRM_ParamsDefault( &p );
	p.zupt = zupt;

	for ( step = 0; step < steps; step++ )
	{
		VRM_FeedAccel( &f, g_ok );
		VRM_Step( &f, &p, bias, dt );

		if ( step == (int)( 30.0f / dt ) )
			VRM_QuatToQ2Angles( f.q, a1 );
	}
	VRM_QuatToQ2Angles( f.q, a2 );

	/* дрейф за последние 30 с, приведённый к минуте */
	return ang_diff( a2[1], a1[1] ) * 2.0f;
}

static void test_zupt( void )
{
	float with_zupt, without;

	printf( "[6] ZUPT: дрейф yaw в покое при bias 1 град/с\n" );

	without = yaw_drift_per_min( 0 );
	with_zupt = yaw_drift_per_min( 1 );

	printf( "       без ZUPT %.1f град/мин, с ZUPT %.2f град/мин\n", without, with_zupt );
	check( without > 30.0f, "без ZUPT дрейф очевиден (контроль теста)", without, 30.0 );
	check( with_zupt < 1.0f, "с ZUPT дрейф yaw, град/мин", with_zupt, 1.0 );
}

/* ------------------------------------------------------------------------
 * 7. Смена rotation на лету (дефект с устройства: rot 3 -> 1 давал
 *    yaw -160, roll +89 и секунды переходного процесса)
 *
 *    Фильтр ведёт оси КОРПУСА, маппинг в камеру — только на выходе.
 *    Корпус при смене rotation не двигается, поэтому углы на выходе
 *    обязаны сразу, со следующего семпла, отразить новый поворот
 *    контента: 1 и 3 отличаются поворотом на 180° вокруг forward, то есть
 *    pitch и yaw те же, roll + 180. Контроль — старая схема (маппинг до
 *    фильтра) на тех же данных: у неё переходный процесс обязан быть.
 * ---------------------------------------------------------------------- */

/* Ориентация корпуса по ориентации камеры при данном rotation. Столбец j
   матрицы корпуса = ось j устройства в мире = sum_k M[k][j] * ось k
   камеры, где M — таблица VRM_MapAxes (камера = M * устройство). */
static void cam_to_device_quat( const float angles[3], int rot, float q_dev[4] )
{
	float cam[3][3], dev[3][3];
	int j, k, i;

	q2_angle_vectors( angles, cam[0], cam[1], cam[2] );

	for ( j = 0; j < 3; j++ )
	{
		float e[3] = { 0.0f, 0.0f, 0.0f };
		float m[3];

		e[j] = 1.0f;
		VRM_MapAxes( rot, e, m ); /* m[k] = M[k][j] */
		for ( i = 0; i < 3; i++ )
		{
			dev[j][i] = 0.0f;
			for ( k = 0; k < 3; k++ )
				dev[j][i] += m[k] * cam[k][i];
		}
	}
	mat_to_quat( dev[0], dev[1], dev[2], q_dev );
}

static float max_angle_err( const float a[3], const float b[3] )
{
	float d = 0.0f;
	int i;

	for ( i = 0; i < 3; i++ )
		if ( ang_diff( a[i], b[i] ) > d )
			d = ang_diff( a[i], b[i] );
	return d;
}

static void test_rotation_switch( void )
{
	/* Спокойная голова в шлеме: вдали от зенита, где yaw/roll вырождены. */
	traj_t t = { 25.0f, 0.11f, 12.0f, 0.07f, 6.0f, 0.05f };
	vrm_filter_t f, f_old;
	vrm_params_t p;
	float dt = 0.005f;
	float q_prev[4], q_cur[4];
	float angles[3], ideal[3], est[3], est_old[3];
	const int rot_before = 3, rot_after = 1;
	const float t_switch = 30.0f, t_end = 36.0f;
	float err_before = 0.0f, err_first = 0.0f, err_after = 0.0f, err_old = 0.0f;
	int step, steps = (int)( t_end / dt );
	int i;

	printf( "[7] смена rotation %d -> %d на лету: фильтр в осях корпуса\n",
			rot_before, rot_after );

	VRM_Reset( &f );
	VRM_Reset( &f_old );
	VRM_ParamsDefault( &p );

	/* Корпус задан траекторией камеры при rot_before и дальше живёт сам:
	   смена rotation его не поворачивает. */
	traj_angles( &t, 0.0f, angles );
	cam_to_device_quat( angles, rot_before, q_prev );
	/* старт из истины: мерим не прогрев (он в пункте 4), а переключение */
	memcpy( f.q, q_prev, sizeof( f.q ) );
	/* старой схеме — та же фора: её кватернион живёт в осях камеры */
	angles_to_quat( angles, f_old.q );

	for ( step = 1; step < steps; step++ )
	{
		float time = step * dt;
		float dx[3], dy[3], dz[3];
		float w_dev[3], g_dev[3], w_cam[3], g_cam[3];
		int rot = ( time < t_switch ) ? rot_before : rot_after;

		traj_angles( &t, time, angles );
		cam_to_device_quat( angles, rot_before, q_cur );
		traj_omega( q_prev, q_cur, dt, w_dev );
		memcpy( q_prev, q_cur, sizeof( q_prev ) );

		/* мировой «вверх» в осях корпуса — третья строка его матрицы */
		VRM_QuatAxes( q_cur, dx, dy, dz );
		g_dev[0] = dx[2] * 1000.0f;
		g_dev[1] = dy[2] * 1000.0f;
		g_dev[2] = dz[2] * 1000.0f;

		for ( i = 0; i < 3; i++ )
			w_dev[i] += noise( VRM_DEG2RAD( 0.2f ) );

		/* новая схема: сырые оси корпуса */
		VRM_FeedAccel( &f, g_dev );
		VRM_Step( &f, &p, w_dev, dt );
		VRM_DeviceQuatToQ2Angles( f.q, rot, est );

		/* старая схема: маппинг на входе, по текущему rotation */
		VRM_MapAxes( rot, g_dev, g_cam );
		VRM_MapAxes( rot, w_dev, w_cam );
		VRM_FeedAccel( &f_old, g_cam );
		VRM_Step( &f_old, &p, w_cam, dt );
		VRM_QuatToQ2Angles( f_old.q, est_old );

		/* Истина независимо от vr_math: до переключения — траектория,
		   после — та же камера, повёрнутая на 180° вокруг forward. */
		ideal[0] = angles[0];
		ideal[1] = angles[1];
		ideal[2] = ( rot == rot_before ) ? angles[2] : angles[2] + 180.0f;

		if ( time < t_switch )
		{
			if ( time > t_switch - 5.0f && max_angle_err( est, ideal ) > err_before )
				err_before = max_angle_err( est, ideal );
		}
		else
		{
			if ( step == (int)( t_switch / dt ) + 1 )
				err_first = max_angle_err( est, ideal );
			if ( max_angle_err( est, ideal ) > err_after )
				err_after = max_angle_err( est, ideal );
			if ( time > t_switch + 1.0f && max_angle_err( est_old, ideal ) > err_old )
				err_old = max_angle_err( est_old, ideal );
		}
	}

	printf( "       старая схема через 1-6 с после переключения: ошибка до %.1f град\n", err_old );
	check( err_before < 2.0f, "до переключения, макс. ошибка углов, град", err_before, 2.0 );
	check( err_first < 2.0f, "первый семпл после переключения, град", err_first, 2.0 );
	check( err_after < 2.0f, "6 с после переключения, макс. ошибка, град", err_after, 2.0 );
	check( err_old > 20.0f, "старая схема: переходный процесс есть (контроль)", err_old, 20.0 );
}

/* ------------------------------------------------------------------------
 * 8. Затравка по акселерометру: горизонт сразу, без секунд прогрева
 * ---------------------------------------------------------------------- */
static void test_seed( void )
{
	float worst = 0.0f;
	int p, r;

	printf( "[8] затравка кватерниона по гравитации\n" );

	for ( p = -75; p <= 75; p += 15 )
	{
		for ( r = -150; r <= 180; r += 30 )
		{
			float in[3] = { (float)p, 0.0f, (float)r };
			float fwd[3], left[3], up[3], g[3], est[3];
			vrm_filter_t f;
			float d;

			q2_angle_vectors( in, fwd, left, up );
			g[0] = fwd[2] * 1000.0f;
			g[1] = left[2] * 1000.0f;
			g[2] = up[2] * 1000.0f;

			VRM_Reset( &f );
			VRM_SeedFromAccel( &f, g );
			VRM_QuatToQ2Angles( f.q, est );

			/* yaw затравкой не определяется — сравниваем pitch и roll */
			d = ang_diff( est[0], in[0] );
			if ( d > worst )
				worst = d;
			d = ang_diff( est[2], in[2] );
			if ( d > worst )
				worst = d;
		}
	}

	check( worst < 0.05f, "pitch/roll сразу после затравки, град", worst, 0.05 );
}

int main( void )
{
	printf( "=== синтетический тест фильтра ориентации VR ===\n" );

	test_axis_maps();
	test_angle_roundtrip();
	test_accel_angles();
	test_convergence();
	test_accel_gate();
	test_zupt();
	test_rotation_switch();
	test_seed();

	if ( g_failures == 0 )
	{
		printf( "=== всё сошлось ===\n" );
		return 0;
	}
	printf( "=== провалов: %d ===\n", g_failures );
	return 1;
}
