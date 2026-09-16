/*
 * vr_math.h -- математика ориентации головы для картонного VR.
 *
 * Здесь нет ни движковых типов, ни cvar'ов, ни сенсоров: только чистые
 * функции над float[3]/float[4]. Причина такая: ошибку в знаках и осях на
 * устройстве видно только надев шлем, поэтому математику держим отдельно
 * от клиента и сенсоров и разбираем её независимо. Держать её в vr_head.c
 * было бы неудобно: тот тянет за собой весь клиент Quake 2.
 *
 * Теория и вывод формул — research/vr_sensors.md, §4.4 (оси) и §5 (фильтр).
 *
 * Соглашение об осях камеры Quake 2 (§4.3): +X вперёд, +Y влево, +Z вверх;
 * положительный PITCH смотрит ВНИЗ, положительный ROLL кладёт голову на
 * правое плечо.
 */

#ifndef vr_math_h
#define vr_math_h

#include <math.h>

#ifndef VRM_PI
#define VRM_PI 3.14159265358979323846f
#endif

#define VRM_RAD2DEG( x ) ( (x) * ( 180.0f / VRM_PI ) )
#define VRM_DEG2RAD( x ) ( (x) * ( VRM_PI / 180.0f ) )

/* ------------------------------------------------------------------------
 * Маппинг осей: корпус устройства -> камера Quake 2
 * ---------------------------------------------------------------------- */

/* Элемент таблицы: из какой оси устройства берём значение и с каким знаком. */
typedef struct
{
	int   axis;
	float sign;
} vrm_axis_map_t;

/*
 * Оси сенсора -> оси камеры Q2, по значению wl_output_transform (§4.4).
 * Индекс — RFBO_GetRotation() & 3. Тип панели в расчёте НЕ участвует: он
 * уже учтён внутри R_AuroraComputeTransform, когда выбирался transform,
 * и именно этим transform повёрнут квад, который увидит глаз. Брать
 * вместо него SDL_GetDisplayOrientation или зашитое «панель портретная»
 * нельзя — на ландшафтном устройстве left и up поменяются местами
 * (§4.6).
 *
 * Строка: [forward, left, up]; ось 0/1/2 = Xd/Yd/Zd устройства.
 * Направление взгляда всегда -Zd: экран всегда перед глазами.
 */
static const vrm_axis_map_t vrm_axis_maps[4][3] =
{
	/* 0   NORMAL */ { { 2, -1.0f }, { 0, -1.0f }, { 1, +1.0f } },
	/* 1   90 CCW */ { { 2, -1.0f }, { 1, -1.0f }, { 0, -1.0f } },
	/* 2   180    */ { { 2, -1.0f }, { 0, +1.0f }, { 1, -1.0f } },
	/* 3   270CCW */ { { 2, -1.0f }, { 1, +1.0f }, { 0, +1.0f } },
};

/* Вектор в осях устройства -> вектор в осях камеры. Годится и для
   гравитации, и для угловой скорости: обе — обычные векторы. */
static inline void VRM_MapAxes( int rotation, const float d[3], float out[3] )
{
	const vrm_axis_map_t *m = vrm_axis_maps[rotation & 3];

	out[0] = m[0].sign * d[m[0].axis]; /* forward */
	out[1] = m[1].sign * d[m[1].axis]; /* left    */
	out[2] = m[2].sign * d[m[2].axis]; /* up      */
}

/* ------------------------------------------------------------------------
 * Кватернионы
 * ---------------------------------------------------------------------- */

/* q = (w, x, y, z), поворот «оси камеры -> мир». */
static inline void VRM_QuatMul( const float a[4], const float b[4], float out[4] )
{
	out[0] = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
	out[1] = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
	out[2] = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
	out[3] = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
}

static inline void VRM_QuatNormalize( float q[4] )
{
	float n = sqrtf( q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] );

	if ( n < 1e-8f )
	{
		q[0] = 1.0f;
		q[1] = q[2] = q[3] = 0.0f;
		return;
	}
	n = 1.0f / n;
	q[0] *= n;
	q[1] *= n;
	q[2] *= n;
	q[3] *= n;
}

/* Столбцы матрицы поворота кватерниона: куда в мире смотрят оси X/Y/Z
   той системы, ориентацию которой q описывает. */
static inline void VRM_QuatAxes( const float q[4], float ax[3], float ay[3], float az[3] )
{
	float w = q[0], x = q[1], y = q[2], z = q[3];

	ax[0] = 1.0f - 2.0f * ( y * y + z * z );
	ax[1] = 2.0f * ( x * y + w * z );
	ax[2] = 2.0f * ( x * z - w * y );

	ay[0] = 2.0f * ( x * y - w * z );
	ay[1] = 1.0f - 2.0f * ( x * x + z * z );
	ay[2] = 2.0f * ( y * z + w * x );

	az[0] = 2.0f * ( x * z + w * y );
	az[1] = 2.0f * ( y * z - w * x );
	az[2] = 1.0f - 2.0f * ( x * x + y * y );
}

/*
 * Углы Quake 2 из мировых векторов forward/left/up камеры.
 * Знак asinf(-fwd[2]) соответствует forward[2] = -sin(pitch) в
 * AngleVectors (shared.c).
 *
 * ROLL — через atan2(left[2], up[2]), а не asin(left[2] / cos(pitch)):
 * left[2] = sin(roll)*cos(pitch), up[2] = cos(roll)*cos(pitch), и atan2
 * даёт полный диапазон ±180. asin обрезал бы roll до ±90, и перевёрнутая
 * камера (телефон вверх ногами, в логе с устройства roll «+89») читалась
 * бы неверно. В зените оба аргумента -> 0, roll не определён, но pitch
 * камеры всё равно зажимается 89 градусами уже в движке.
 */
static inline void VRM_AxesToQ2Angles( const float fwd[3], const float left[3],
                                       const float up[3], float angles[3] )
{
	float cp, s;

	s = fwd[2];
	if ( s > 1.0f )
		s = 1.0f;
	if ( s < -1.0f )
		s = -1.0f;

	angles[1] = VRM_RAD2DEG( atan2f( fwd[1], fwd[0] ) ); /* YAW   */
	angles[0] = VRM_RAD2DEG( asinf( -s ) );              /* PITCH */

	cp = sqrtf( fwd[0] * fwd[0] + fwd[1] * fwd[1] );
	if ( cp < 1e-4f )
		angles[2] = 0.0f;
	else
		angles[2] = VRM_RAD2DEG( atan2f( left[2], up[2] ) ); /* ROLL */
}

/* Извлечение углов Quake 2 (§5.3) из кватерниона «оси камеры -> мир»:
   forward/left/up — это столбцы его матрицы поворота. */
static inline void VRM_QuatToQ2Angles( const float q[4], float angles[3] )
{
	float fwd[3], left[3], up[3];

	VRM_QuatAxes( q, fwd, left, up );
	VRM_AxesToQ2Angles( fwd, left, up, angles );
}

/*
 * Углы камеры из ориентации КОРПУСА устройства. q — «оси устройства ->
 * мир» (так его ведёт фильтр), rotation — текущий RFBO_GetRotation().
 *
 * Маппинг устройство -> камера стоит здесь, на выходе, а не на входе
 * фильтра, и это исправление дефекта, пойманного на устройстве: при
 * смене rotation (3 -> 1) кватернион фильтра оставался в старом базисе,
 * смена строки таблицы выглядела для него мгновенным поворотом на 180°
 * вокруг forward, и Махони гасил его секундами (yaw -160, roll +89).
 * Корпус при смене rotation физически не поворачивается — значит и
 * состояние фильтра меняться не должно; меняется только то, какая ось
 * корпуса сейчас считается «влево» и «вверх» для глаза.
 *
 * Строка таблицы m[k] = (axis, sign) говорит: k-я ось камеры равна
 * sign * (ось axis устройства). В мире ось устройства — столбец матрицы
 * q, отсюда forward/left/up камеры без промежуточных кватернионов.
 */
static inline void VRM_DeviceQuatToQ2Angles( const float q[4], int rotation, float angles[3] )
{
	const vrm_axis_map_t *m = vrm_axis_maps[rotation & 3];
	float dev[3][3], cam[3][3];
	int k, i;

	VRM_QuatAxes( q, dev[0], dev[1], dev[2] );

	for ( k = 0; k < 3; k++ )
		for ( i = 0; i < 3; i++ )
			cam[k][i] = m[k].sign * dev[m[k].axis][i];

	VRM_AxesToQ2Angles( cam[0], cam[1], cam[2], angles );
}

/*
 * Наклон по измеренной гравитации, без гироскопа. Используется для
 * кросс-проверки масштаба гироскопа (§3.3, шаг 2) и для отладочного
 * вывода. g — вектор акселерометра уже в осях камеры, ненулевой.
 *
 * Вывод: в покое акселерометр меряет +1 g вдоль мировой вертикали, то
 * есть g/|g| — это мировой «вверх», записанный в осях камеры. Его
 * проекция на forward равна -sin(pitch) (тот же знак, что у
 * forward[2] = -sin(pitch) в AngleVectors), проекция на left —
 * sin(roll)*cos(pitch).
 */
static inline void VRM_AccelToPitchRoll( const float g[3], float *pitch_deg, float *roll_deg )
{
	float n = sqrtf( g[0] * g[0] + g[1] * g[1] + g[2] * g[2] );
	float s, cp;

	if ( n < 1e-6f )
	{
		*pitch_deg = 0.0f;
		*roll_deg = 0.0f;
		return;
	}

	s = -g[0] / n;
	if ( s > 1.0f )
		s = 1.0f;
	if ( s < -1.0f )
		s = -1.0f;
	*pitch_deg = VRM_RAD2DEG( asinf( s ) );

	cp = cosf( VRM_DEG2RAD( *pitch_deg ) );
	if ( cp < 1e-4f )
	{
		*roll_deg = 0.0f;
		return;
	}
	s = ( g[1] / n ) / cp;
	if ( s > 1.0f )
		s = 1.0f;
	if ( s < -1.0f )
		s = -1.0f;
	*roll_deg = VRM_RAD2DEG( asinf( s ) );
}

/* ------------------------------------------------------------------------
 * Фильтр Махони (§5.2, §5.3)
 * ---------------------------------------------------------------------- */

/*
 * Фильтр не знает, в каких он осях: он ведёт ориентацию той системы, в
 * которой ему дают гироскоп и акселерометр. vr_head.c кормит его осями
 * КОРПУСА устройства и маппит в оси камеры только на выходе
 * (VRM_DeviceQuatToQ2Angles) — см. там, почему. Для математики самого
 * фильтра выбор осей входных данных безразличен.
 */
typedef struct
{
	float q[4];        /* ориентация: оси входных данных -> мир */
	float integral[3]; /* интегратор PI-звена = оценка -bias, рад/с */

	float accel[3];    /* последний акселерометр, сырые ед. */
	float accel_norm;  /* его длина */
	float gravity_ref; /* длина вектора g в тех же сырых единицах */
	int   have_accel;

	/* Автокалибровка gravity_ref: набираем |a| в первые спокойные семплы,
	   чтобы не зависеть от гипотезы «milli-g» (§3.3). */
	float grav_sum;
	int   grav_count;

	float gyro_speed_dps; /* |w| последнего шага, для гейтов и отладки */
	float still_time;     /* сколько секунд голова считается неподвижной */
} vrm_filter_t;

typedef struct
{
	float kp;         /* П-звено Махони: за сколько подтягивается горизонт */
	float ki;         /* И-звено: скорость обучения bias */
	float accel_gate; /* допустимое отклонение |a| от g, доля (0.06 = ±6 %) */
	float gate_dps;   /* быстрее — акселерометру не верим вообще (§5.4) */
	float zupt_dps;   /* медленнее — считаем голову неподвижной */
	float zupt_time;  /* сколько держать неподвижность до включения ZUPT */
	int   zupt;       /* ZUPT включён */
} vrm_params_t;

/* Сколько спокойных семплов набрать для gravity_ref. При 100-200 Гц это
   доли секунды — ровно «первые ~0.5 с в покое» из §3.3. */
#define VRM_GRAVITY_SAMPLES 50

/* Потолок интегратора. Bias реального MEMS-гироскопа — единицы °/с,
   0.05 рад/с ≈ 2.9 °/с: с запасом, но без windup'а на десятки градусов. */
#define VRM_INTEGRAL_LIMIT 0.05f

static inline void VRM_Reset( vrm_filter_t *f )
{
	int i;

	f->q[0] = 1.0f;
	f->q[1] = f->q[2] = f->q[3] = 0.0f;
	for ( i = 0; i < 3; i++ )
	{
		f->integral[i] = 0.0f;
		f->accel[i] = 0.0f;
	}
	f->accel_norm = 0.0f;
	f->gravity_ref = 0.0f;
	f->have_accel = 0;
	f->grav_sum = 0.0f;
	f->grav_count = 0;
	f->gyro_speed_dps = 0.0f;
	f->still_time = 0.0f;
}

static inline void VRM_ParamsDefault( vrm_params_t *p )
{
	p->kp = 0.5f;
	p->ki = 0.02f;
	p->accel_gate = 0.06f;
	p->gate_dps = 40.0f;
	p->zupt_dps = 2.0f;
	p->zupt_time = 0.5f;
	p->zupt = 1;
}

/*
 * Затравка: выставить q так, чтобы оценка «вверх» совпала с измеренной
 * гравитацией. yaw остаётся произвольным — гравитация его не содержит.
 * Без затравки фильтр стартует из единичного кватерниона и тянет горизонт
 * с постоянной времени ~1/Kp секунд; в камере, где pitch головы
 * абсолютный, это выглядело бы как медленно «приезжающий» горизонт.
 *
 * Нужен поворот R с R^T*(0,0,1) = g/|g|, то есть R переводит g_n в ось Z
 * мира. Кратчайший поворот вектора a в b: q = норм.(1 + a·b, a × b); для
 * b = (0,0,1) это (1 + g_z, g_y, -g_x, 0). При g_n ~ (0,0,-1) (вверх
 * ногами) формула вырождается — тогда 180° вокруг X.
 * Возвращает 0, если вектор нулевой.
 */
static inline int VRM_SeedFromAccel( vrm_filter_t *f, const float a[3] )
{
	float n = sqrtf( a[0] * a[0] + a[1] * a[1] + a[2] * a[2] );
	float gx, gy, gz;

	if ( n < 1e-6f )
		return 0;

	gx = a[0] / n;
	gy = a[1] / n;
	gz = a[2] / n;

	if ( gz < -0.9999f )
	{
		f->q[0] = 0.0f;
		f->q[1] = 1.0f;
		f->q[2] = 0.0f;
		f->q[3] = 0.0f;
	}
	else
	{
		f->q[0] = 1.0f + gz;
		f->q[1] = gy;
		f->q[2] = -gx;
		f->q[3] = 0.0f;
		VRM_QuatNormalize( f->q );
	}
	return 1;
}

/* Семпл акселерометра (в тех же осях, что и гироскоп для VRM_Step). */
static inline void VRM_FeedAccel( vrm_filter_t *f, const float a_cam[3] )
{
	float n = sqrtf( a_cam[0] * a_cam[0] + a_cam[1] * a_cam[1] + a_cam[2] * a_cam[2] );

	f->accel[0] = a_cam[0];
	f->accel[1] = a_cam[1];
	f->accel[2] = a_cam[2];
	f->accel_norm = n;
	f->have_accel = ( n > 1e-6f );

	/* Опорная длина g: абсолютный масштаб акселерометра алгоритму не
	   нужен (вектор используется нормированным), нужна только величина,
	   с которой сравнивать при гейте. Набираем её сами, пока голова
	   спокойна, — так гипотеза о единицах измерения перестаёт быть
	   блокером. */
	if ( f->gravity_ref <= 0.0f && n > 1e-6f && f->gyro_speed_dps < 5.0f )
	{
		f->grav_sum += n;
		if ( ++f->grav_count >= VRM_GRAVITY_SAMPLES )
			f->gravity_ref = f->grav_sum / (float)f->grav_count;
	}
}

/* Можно ли сейчас верить акселерометру как опорной вертикали (§5.4). */
static inline int VRM_AccelTrustworthy( const vrm_filter_t *f, const vrm_params_t *p )
{
	float ratio;

	if ( !f->have_accel || f->gravity_ref <= 0.0f || f->accel_norm <= 0.0f )
		return 0;

	/* 1. Модуль близок к 1 g, иначе к гравитации подмешано линейное или
	      центростремительное ускорение. */
	ratio = f->accel_norm / f->gravity_ref;
	if ( ratio < 1.0f - p->accel_gate || ratio > 1.0f + p->accel_gate )
		return 0;

	/* 2. Голова вращается медленно: на быстром повороте тангенциальное
	      ускорение велико даже при |a| ~ 1 g. */
	if ( f->gyro_speed_dps > p->gate_dps )
		return 0;

	return 1;
}

/*
 * Шаг фильтра на один семпл гироскопа. w_cam — угловая скорость, рад/с,
 * в тех же осях, что и акселерометр (в vr_head.c — оси корпуса), с уже
 * вычтенным аппаратным bias'ом. dt — секунды.
 */
static inline void VRM_Step( vrm_filter_t *f, const vrm_params_t *p,
                             const float w_cam[3], float dt )
{
	float w[3], e[3] = { 0.0f, 0.0f, 0.0f };
	float dq[4], q_new[4];
	int i;

	w[0] = w_cam[0];
	w[1] = w_cam[1];
	w[2] = w_cam[2];

	/* Скорость считаем ДО коррекций: её смотрят гейты, а им нужна
	   измеренная скорость головы, а не подправленная фильтром. */
	f->gyro_speed_dps = VRM_RAD2DEG( sqrtf( w[0] * w[0] + w[1] * w[1] + w[2] * w[2] ) );

	/* --- коррекция по гравитации (только когда ей можно верить) ------- */
	if ( VRM_AccelTrustworthy( f, p ) )
	{
		float g_meas[3], g_est[3];
		float inv = 1.0f / f->accel_norm;

		for ( i = 0; i < 3; i++ )
			g_meas[i] = f->accel[i] * inv;

		/* оценка мирового «вверх» в осях камеры — третий столбец
		   транспонированной матрицы, то есть R^T * (0,0,1) */
		g_est[0] = 2.0f * ( f->q[1] * f->q[3] - f->q[0] * f->q[2] );
		g_est[1] = 2.0f * ( f->q[2] * f->q[3] + f->q[0] * f->q[1] );
		g_est[2] = f->q[0] * f->q[0] - f->q[1] * f->q[1]
		         - f->q[2] * f->q[2] + f->q[3] * f->q[3];

		/*
		 * Ошибка = ось, вокруг которой надо довернуть оценку к измерению.
		 *
		 * Порядок сомножителей здесь ОБРАТНЫЙ к тому, что записано в
		 * research/vr_sensors.md §5.3 (там CrossProduct(g_est, g_meas, e)),
		 * и это исправление, а не вкусовщина. Вывод: кватернион
		 * интегрируется правым умножением q' = q * dq, то есть добавка к
		 * угловой скорости поворачивает оценку в осях камеры, и тогда
		 * g_est' ~ g_est - d x g_est. Подстановка d = k*(g_est x g_meas)
		 * даёт g_est' ~ g_est - k*(g_meas - g_est), то есть уводит оценку
		 * ОТ измерения: знак положительной обратной связи. Нужен обратный
		 * порядок, он же стоит в эталонной реализации Махони.
		 *
		 * Симптом неверного знака — горизонт не подтягивается, а
		 * заваливается тем сильнее, чем больше Kp.
		 */
		e[0] = g_meas[1] * g_est[2] - g_meas[2] * g_est[1];
		e[1] = g_meas[2] * g_est[0] - g_meas[0] * g_est[2];
		e[2] = g_meas[0] * g_est[1] - g_meas[1] * g_est[0];
	}

	/* --- И-звено Махони: учит bias ------------------------------------ */
	for ( i = 0; i < 3; i++ )
	{
		f->integral[i] += e[i] * p->ki * dt;
		if ( f->integral[i] > VRM_INTEGRAL_LIMIT )
			f->integral[i] = VRM_INTEGRAL_LIMIT;
		if ( f->integral[i] < -VRM_INTEGRAL_LIMIT )
			f->integral[i] = -VRM_INTEGRAL_LIMIT;
	}

	/* --- ZUPT: то же самое для yaw, где гравитация не помогает (§5.5) --
	   Векторное произведение e ортогонально g, значит его вертикальная
	   составляющая нулевая и yaw И-звеном не правится принципиально.
	   Зато в покое вся измеренная скорость — это и есть bias, и её можно
	   загнать в тот же интегратор, включая вертикальную компоненту.
	   Порядок важен: сначала И-звено и ZUPT правят integral, и только
	   потом он применяется к w, иначе ZUPT опаздывал бы на шаг. */
	if ( p->zupt && f->gyro_speed_dps < p->zupt_dps )
	{
		f->still_time += dt;
		if ( f->still_time > p->zupt_time )
		{
			for ( i = 0; i < 3; i++ )
				f->integral[i] += ( -w[i] - f->integral[i] ) * 0.5f * dt;
		}
	}
	else
	{
		f->still_time = 0.0f;
	}

	/* --- П-звено + накопленная оценка bias ---------------------------- */
	for ( i = 0; i < 3; i++ )
		w[i] += p->kp * e[i] + f->integral[i];

	/* --- интегрирование кватерниона ----------------------------------- */
	/* dq = (1, w*dt/2) — первый порядок; при dt <= 10 мс и w < 600 °/с
	   ошибка усечения < 0.002° за шаг, нормализация её съедает. */
	dq[0] = 1.0f;
	dq[1] = w[0] * dt * 0.5f;
	dq[2] = w[1] * dt * 0.5f;
	dq[3] = w[2] * dt * 0.5f;

	VRM_QuatMul( f->q, dq, q_new );
	f->q[0] = q_new[0];
	f->q[1] = q_new[1];
	f->q[2] = q_new[2];
	f->q[3] = q_new[3];
	VRM_QuatNormalize( f->q );
}

#endif /* vr_math_h */
