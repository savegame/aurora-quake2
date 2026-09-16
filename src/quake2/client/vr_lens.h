/*
 * vr_lens.h -- раскладка изображений глаз под линзы картонного шлема.
 *
 * Чистые функции без движковых типов и без GL, по той же причине, что и
 * vr_math.h: математику раскладки можно разбирать отдельно от клиента и
 * рендера.
 *
 * Что здесь решается. Прицел в центре картинки глаза при параллельных
 * камерах (gl_stereo_convergence 0) — это точка на бесконечности. Чтобы
 * она слилась в одну, центр картинки каждого глаза обязан лежать на
 * оптической оси своей линзы. По умолчанию центры стоят в W/4 и 3W/4 —
 * на 6.5" телефоне это ~75 мм между ними, а линзы картона ~60-65 мм:
 * отсюда двоение. gl_stereo_separation (IPD камер в мире) это не лечит —
 * он двигает камеры, а не картинки на стекле.
 *
 * Хранится всё в миллиметрах экрана: линзы меряются в мм, и дефолт
 * «63 мм» осмыслен на любом телефоне. Пиксели или доли ширины экрана
 * так не умеют — у каждого телефона свой дефолт.
 *
 * Координаты — NDC КОНТЕНТА (до матрицы поворота квада в r_fbo.c):
 * x -1..1 по ширине FBO, y -1..1 снизу вверх. Поэтому поворот экрана
 * калибровку не ломает: смещения задаются в осях картинки, а не панели.
 *
 * Вторая половина файла — дисторсия линз и FOV из геометрии очков.
 * Нормировка везде одна: ТАНГЕНС угла от оси линзы, то есть расстояние по
 * экрану в мм, делённое на расстояние экран-линза (distMm). Ровно так
 * считает Google Cardboard (screen_params = размер экрана /
 * screen_to_lens_distance), поэтому k1/k2 из профилей Cardboard
 * подставляются сюда как есть, без пересчёта.
 */

#ifndef vr_lens_h
#define vr_lens_h

#include <math.h>

/* Разумные пределы. Межлинзовое у картонок и пластиковых шлемов —
   55-70 мм, запас в обе стороны на странные держатели. */
#define VRL_SEP_MIN_MM    40.0f
#define VRL_SEP_MAX_MM    90.0f
#define VRL_VOFS_MAX_MM   30.0f
#define VRL_TILT_MAX_MM   15.0f

#define VRL_SEP_DEFAULT_MM 63.0f

/* Если DPI не пришёл или врёт: длинная сторона типичного 6-6.5" телефона.
   С ним дефолт 63 мм даёт центры на ~0.43 ширины — заметно ближе к
   правде, чем 0.5, а дальше пользователь всё равно подгонит в шлеме. */
#define VRL_FALLBACK_WIDTH_MM 145.0f

static inline float VRL_Clamp( float v, float lo, float hi )
{
	return v < lo ? lo : ( v > hi ? hi : v );
}

/*
 * Физический размер контента в мм.
 *
 * ddpi — диагональный DPI дисплея (SDL_GetDisplayDPI). Берём именно его:
 * в Wayland-бэкенде SDL он считается по диагонали из wl_output::geometry
 * и от поворота не зависит, а hdpi/vdpi там посчитаны без учёта transform.
 * contentWpx/contentHpx — размер контента в пикселях бэкбуфера (он же
 * нативный: r_main.c берёт drawable size, а не логический размер окна).
 *
 * Возвращает 1, если размер получен из DPI, 0 — если взят фолбэк.
 * Проверка правдоподобия нужна потому, что композиторы порой отдают
 * физический размер 0 или заглушку вроде 1x1 мм.
 */
static inline int VRL_ContentSizeMm( float ddpi, int contentWpx, int contentHpx,
		float *widthMm, float *heightMm )
{
	float w, h;

	if ( contentWpx <= 0 || contentHpx <= 0 )
	{
		*widthMm = VRL_FALLBACK_WIDTH_MM;
		*heightMm = VRL_FALLBACK_WIDTH_MM * 9.0f / 16.0f;
		return 0;
	}

	if ( ddpi >= 72.0f && ddpi <= 2000.0f )
	{
		w = contentWpx * 25.4f / ddpi;
		h = contentHpx * 25.4f / ddpi;
		if ( w >= 40.0f && w <= 1000.0f )
		{
			*widthMm = w;
			*heightMm = h;
			return 1;
		}
	}

	*widthMm = VRL_FALLBACK_WIDTH_MM;
	*heightMm = VRL_FALLBACK_WIDTH_MM * (float)contentHpx / (float)contentWpx;
	return 0;
}

/*
 * Центры изображений глаз в NDC контента.
 *
 * sepMm  — расстояние между центрами по горизонтали;
 * vofsMm — общий сдвиг обоих вверх (+) / вниз (-);
 * tiltMm — перекос: правое изображение выше левого на tiltMm (левое
 *          опускается на половину, правое поднимается на половину). Нужен
 *          держателям, где телефон стоит чуть под углом: вертикальное
 *          двоение глаза не сводят вообще, в отличие от горизонтального.
 *
 * c[0],c[1] — левый глаз (x,y), c[2],c[3] — правый. «Левый» = левая
 * половина текстуры FBO и левая половина контента.
 */
static inline void VRL_EyeCenters( float widthMm, float heightMm,
		float sepMm, float vofsMm, float tiltMm, float c[4] )
{
	float sx = VRL_Clamp( sepMm, VRL_SEP_MIN_MM, VRL_SEP_MAX_MM ) / widthMm;   /* NDC/2 */
	float vy = 2.0f * VRL_Clamp( vofsMm, -VRL_VOFS_MAX_MM, VRL_VOFS_MAX_MM ) / heightMm;
	float ty = VRL_Clamp( tiltMm, -VRL_TILT_MAX_MM, VRL_TILT_MAX_MM ) / heightMm; /* половина перекоса в NDC */

	/* Разнос в NDC = 2*sep/width, центры симметрично от середины. */
	c[0] = -sx;
	c[1] = vy - ty;
	c[2] = sx;
	c[3] = vy + ty;
}

/*
 * Два квада (по 6 вершин pos2+uv2, как l_quadVerts в r_fbo.c) — левая и
 * правая половины текстуры, каждая размером в половину экрана и со своим
 * центром.
 *
 * Внутренний край обрезается по середине контента (x = 0): там стоит
 * перегородка шлема, и если центры сближены, картинка одного глаза иначе
 * наползла бы на другой. UV обрезаются вместе с позицией, поэтому
 * изображение не сжимается, а именно подрезается. Внешние края и верх/низ
 * отсекает сам вьюпорт экрана — вершины за пределами ±1 это нормально.
 * Непокрытое остаётся цветом очистки (чёрным).
 *
 * UV здесь — это просто данные вершин, посчитанные один раз: в шейдер они
 * идут как записаны (правило FBO-модуля).
 *
 * Возвращает число вершин (12).
 */
static inline int VRL_BuildEyeQuads( const float c[4], float out[48] )
{
	int e, n = 0;

	for ( e = 0; e < 2; e++ )
	{
		float cx = c[e * 2], cy = c[e * 2 + 1];
		float x0 = cx - 0.5f, x1 = cx + 0.5f;
		float u0 = e ? 0.5f : 0.0f, u1 = e ? 1.0f : 0.5f;
		float y0 = cy - 1.0f, y1 = cy + 1.0f;
		const float quad[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };
		int i;

		if ( e == 0 && x1 > 0.0f )
		{
			u1 = u0 + ( 0.0f - x0 ) * 0.5f; /* 1 NDC ширины квада = 0.5 UV */
			x1 = 0.0f;
		}
		else if ( e == 1 && x0 < 0.0f )
		{
			u0 = u1 - ( x1 - 0.0f ) * 0.5f;
			x0 = 0.0f;
		}

		/* Центры разведены так, что квад целиком ушёл за середину, — такого
		   пределы VRL_SEP_* не допускают, но пустой квад лучше мусора. */
		if ( x1 < x0 )
			x1 = x0;
		if ( e == 0 && u1 < u0 )
			u1 = u0;
		if ( e == 1 && u0 > u1 )
			u0 = u1;

		for ( i = 0; i < 6; i++ )
		{
			out[n++] = quad[i][0] ? x1 : x0;
			out[n++] = quad[i][1] ? y1 : y0;
			out[n++] = quad[i][0] ? u1 : u0;
			out[n++] = quad[i][1] ? 1.0f : 0.0f;
		}
	}

	return n / 4;
}

/* ======================================================================
 * Дисторсия линз и FOV из геометрии очков.
 * ====================================================================== */

#define VRL_PI 3.14159265358979323846f

/* Коэффициенты Brown-Conrady. Дефолт — профиль Google Cardboard v2
   (I/O 2015): k1 0.34, k2 0.55 при расстоянии экран-линза 39 мм. Картонки
   с AliExpress оптически сделаны по тому же чертежу, так что дефолт даёт
   заметно более прямые линии, чем отсутствие коррекции, а дальше
   пользователь подгоняет k1 в шлеме.

   Предел снизу отрицательный: у части профилей k2 слегка отрицателен
   (сложная асферика). Но при отрицательных k радиальная функция может
   потерять монотонность — за этим следит VRL_MonotonicMaxR. */
#define VRL_K1_DEFAULT  0.34f
#define VRL_K2_DEFAULT  0.55f
#define VRL_K_MIN      -0.20f
#define VRL_K_MAX       1.50f

/* Расстояние экран-линза (оно же фокус в первом приближении). Cardboard v2
   — 39 мм, v1 — 42 мм. Задаёт и силу дисторсии (нормировка тангенсов), и
   FOV: чем ближе экран, тем шире поле. */
#define VRL_DIST_DEFAULT_MM 39.0f
#define VRL_DIST_MIN_MM     25.0f
#define VRL_DIST_MAX_MM     80.0f

/* Потолок половинного угла поля зрения. У Cardboard это device fov из
   профиля (60° на сторону): дальше свет всё равно срезает оправа линзы,
   и рендерить туда — впустую жечь GPU. */
#define VRL_FOV_HALF_MAX_DEG 60.0f

/* Дальше этого радиуса (в тангенсах) сетку не строим ни при каких k:
   аргумент-страховка от деления на ноль и бесконечных циклов. */
#define VRL_R_LIMIT 8.0f

/* Прямая дисторсия: тангенс на ЭКРАНЕ -> тангенс, который видит ГЛАЗ.
   Это модель самой линзы (она увеличивает тем сильнее, чем дальше от оси —
   отсюда подушка). Обратное преобразование нужно, чтобы понять, куда на
   экране класть точку кадра. */
static inline float VRL_DistortScale( float r2, float k1, float k2 )
{
	return 1.0f + r2 * ( k1 + k2 * r2 );
}

static inline float VRL_Distort( float r, float k1, float k2 )
{
	return r * VRL_DistortScale( r * r, k1, k2 );
}

/*
 * Радиус, до которого r'(r) строго растёт. За ним сетка «сворачивается»:
 * соседние точки экрана начали бы брать кадр в обратном порядке, и
 * картинка складывается сама в себя.
 *
 * Производная 1 + 3*k1*u + 5*k2*u², где u = r². Ищем наименьший
 * положительный корень; если его нет — ограничения нет (возвращаем
 * VRL_R_LIMIT). При k1 >= 0 и k2 >= 0 корней нет никогда.
 *
 * Определена до VRL_DistortInverse: та ограничивает им свой интервал поиска.
 */
static inline float VRL_MonotonicMaxR( float k1, float k2 )
{
	float a = 5.0f * k2, b = 3.0f * k1, u = -1.0f;

	if ( a == 0.0f )
	{
		if ( b >= 0.0f )
			return VRL_R_LIMIT;
		u = -1.0f / b;
	}
	else
	{
		float disc = b * b - 4.0f * a;
		float sq, u1, u2;

		if ( disc < 0.0f )
			return VRL_R_LIMIT; /* знак не меняется, а при u=0 производная = 1 */
		sq = sqrtf( disc );
		u1 = ( -b - sq ) / ( 2.0f * a );
		u2 = ( -b + sq ) / ( 2.0f * a );
		if ( u1 > 0.0f )
			u = u1;
		if ( u2 > 0.0f && ( u < 0.0f || u2 < u ) )
			u = u2;
	}

	if ( u <= 0.0f )
		return VRL_R_LIMIT;
	{
		float r = sqrtf( u );
		return r < VRL_R_LIMIT ? r : VRL_R_LIMIT;
	}
}

/*
 * Обратная функция: по тангенсу, который видит глаз, — радиус на экране.
 * Вызывается ТОЛЬКО при пересчёте геометрии (смена cvar'ов, поворота,
 * размера) — в кадре её нет, поэтому за скорость тут бороться не нужно.
 *
 * Делением пополам, а не Ньютоном. Ньютон от r = t выглядит очевидным
 * выбором, но врёт в двух местах сразу: на больших радиусах старт уходит
 * далеко вверх (t растёт как r^5) и десятка итераций не хватает, а при
 * отрицательном k2 старт вообще попадает ЗА точку перегиба, где функция
 * уже падает, и итерации уезжают на чужую ветвь. На [0, rMax] функция
 * строго растёт, так что деление пополам сходится всегда.
 *
 * За пределом монотонности обратной точки нет — возвращаем сам предел.
 */
static inline float VRL_DistortInverse( float t, float k1, float k2 )
{
	float lo = 0.0f, hi = VRL_MonotonicMaxR( k1, k2 );
	int i;

	if ( t <= 0.0f )
		return 0.0f;
	if ( t >= VRL_Distort( hi, k1, k2 ) )
		return hi;

	for ( i = 0; i < 50; i++ )
	{
		float mid = 0.5f * ( lo + hi );

		if ( VRL_Distort( mid, k1, k2 ) < t )
			lo = mid;
		else
			hi = mid;
	}

	return 0.5f * ( lo + hi );
}

/* Параметры очков и экрана — всё, из чего считается раскладка. */
typedef struct
{
	float widthMm, heightMm; /* физический размер КОНТЕНТА (VRL_ContentSizeMm) */
	float sepMm, vofsMm, tiltMm;
	float distMm;            /* экран-линза */
	float k1, k2;
} vrl_params_t;

/* Производные величины. Считаются по событию, кадр берёт готовое. */
typedef struct
{
	float c[4];       /* центры глаз в NDC контента (VRL_EyeCenters) */
	float tanX, tanY; /* половины поля рендера в тангенсах */
	float fovXdeg, fovYdeg;
	float centerMag;  /* во сколько раз дисторсия растягивает центр */
	float rMax;       /* предел монотонности в тангенсах экрана */
} vrl_geom_t;

/*
 * FOV из геометрии, а не из вкуса. Глаз в центре линзы видит верх экрана
 * под углом atan(distort(h/2 / d)) — рендер обязан покрыть ровно это поле,
 * иначе мир растянут и поворот головы не совпадает с движением картинки.
 *
 * Считаем от ВЕРТИКАЛИ: она не зависит от сплита и от разноса линз.
 * Горизонталь получается из пропорций половины кадра — так же, как её
 * получит проекция движка (R_View_setupProjection берёт fov_y и aspect).
 *
 * Смещения vofs/tilt в поле зрения НЕ учитываются намеренно: иначе при
 * калибровке высоты менялся бы FOV, а с ним и масштаб мира. Съезжает при
 * этом лишь граница чёрного по краю — она и так за пределом линзы.
 *
 * Асимметричный FOV (к носу меньше, к виску больше) лёг бы сюда: вместо
 * одной пары tanX/tanY — четыре границы на глаз, и R_View_setupProjection
 * пришлось бы кормить ими напрямую, минуя пару (fov_y, aspect).
 */
static inline void VRL_ComputeGeom( const vrl_params_t *p, vrl_geom_t *g )
{
	float d = VRL_Clamp( p->distMm, VRL_DIST_MIN_MM, VRL_DIST_MAX_MM );
	float k1 = VRL_Clamp( p->k1, VRL_K_MIN, VRL_K_MAX );
	float k2 = VRL_Clamp( p->k2, VRL_K_MIN, VRL_K_MAX );
	float sHalf = ( p->heightMm * 0.5f ) / d; /* тангенс до края экрана по вертикали */
	float tanMax = tanf( VRL_FOV_HALF_MAX_DEG * VRL_PI / 180.0f );
	/* Пропорции половины кадра. Миллиметры получены из пикселей одним и тем
	   же DPI по обеим осям, поэтому mm-отношение равно пиксельному. */
	float aspect = ( p->widthMm * 0.5f ) / p->heightMm;

	VRL_EyeCenters( p->widthMm, p->heightMm, p->sepMm, p->vofsMm, p->tiltMm, g->c );

	g->rMax = VRL_MonotonicMaxR( k1, k2 );
	if ( sHalf > g->rMax )
		sHalf = g->rMax;

	g->tanY = VRL_Distort( sHalf, k1, k2 );
	if ( g->tanY > tanMax )
		g->tanY = tanMax;
	g->tanX = g->tanY * aspect;

	g->fovYdeg = 2.0f * atanf( g->tanY ) * 180.0f / VRL_PI;
	g->fovXdeg = 2.0f * atanf( g->tanX ) * 180.0f / VRL_PI;

	/* Дисторсия сжимает края и тем самым растягивает центр: в центре на
	   один тексель кадра приходится centerMag экранных пикселей (при
	   масштабе FBO 1.0). Отсюда рекомендация по r_3d_scale. */
	g->centerMag = ( sHalf > 0.0f ) ? g->tanY / sHalf : 1.0f;
}

/* ---- сетка дисторсии --------------------------------------------------
 *
 * Вместо квада на глаз — решётка с РОВНЫМИ позициями на экране и заранее
 * посчитанными UV. Направление правильное: для точки экрана считаем, какую
 * точку кадра в неё положить, — то есть прямую VRL_Distort (экран -> глаз).
 * Обратная функция для этого не нужна вовсе: она нужна была бы при сетке,
 * ровной в кадре, а не на экране.
 *
 * Вершина: pos2 (NDC контента) + uv2 (текстура FBO) + bounds2 (границы
 * половины кадра своего глаза по u). Границы — во фрагментный шейдер:
 * всё, что вышло за свою половину, гасится в чёрное, а не залезает к
 * соседу и не размазывается клэмпом. Величина на вершину постоянна внутри
 * глаза, поэтому интерполяция её не портит.
 *
 * Хроматическую аберрацию сюда добавлять так: ещё два набора uv (R и B)
 * со своими k1/k2, VRL_MESH_FLOATS_PER_VERT становится 10, шейдер берёт
 * три сэмпла и собирает каналы. Структура под это уже разложена.
 */
#define VRL_MESH_CELLS 48
#define VRL_MESH_ROW   ( VRL_MESH_CELLS + 1 )
#define VRL_MESH_VERTS ( 2 * VRL_MESH_ROW * VRL_MESH_ROW )
#define VRL_MESH_INDICES ( 2 * VRL_MESH_CELLS * VRL_MESH_CELLS * 6 )
#define VRL_MESH_FLOATS_PER_VERT 6

/* Индексы от параметров не зависят — строятся один раз при создании IBO. */
static inline int VRL_BuildMeshIndices( unsigned short *idx )
{
	int e, row, col, n = 0;

	for ( e = 0; e < 2; e++ )
	{
		int base = e * VRL_MESH_ROW * VRL_MESH_ROW;

		for ( row = 0; row < VRL_MESH_CELLS; row++ )
		{
			for ( col = 0; col < VRL_MESH_CELLS; col++ )
			{
				int v0 = base + row * VRL_MESH_ROW + col;
				int v1 = v0 + 1;
				int v2 = v0 + VRL_MESH_ROW;
				int v3 = v2 + 1;

				idx[n++] = (unsigned short)v0;
				idx[n++] = (unsigned short)v1;
				idx[n++] = (unsigned short)v2;
				idx[n++] = (unsigned short)v2;
				idx[n++] = (unsigned short)v1;
				idx[n++] = (unsigned short)v3;
			}
		}
	}

	return n;
}

/*
 * Вершины сетки. Возвращает число вершин (VRL_MESH_VERTS).
 *
 * Позиции ровно покрывают свою половину экрана: левый глаз x -1..0, правый
 * 0..1 — середина контента это перегородка шлема, как и у прежних квадов.
 * UV: переводим позицию в тангенсы от центра линзы, применяем дисторсию,
 * нормируем на поле рендера (tanX/tanY) и кладём в половину текстуры
 * своего глаза.
 *
 * При k1 = k2 = 0 получается ровно прежняя пара квадов: тогда tanY =
 * h/(2d), tanX = w/(4d), и масштаб d сокращается — UV становятся линейными
 * с тем же наклоном 0.5 UV на 1 NDC.
 */
static inline int VRL_BuildEyeMesh( const vrl_params_t *p, const vrl_geom_t *g, float *out )
{
	float k1 = VRL_Clamp( p->k1, VRL_K_MIN, VRL_K_MAX );
	float k2 = VRL_Clamp( p->k2, VRL_K_MIN, VRL_K_MAX );
	float d = VRL_Clamp( p->distMm, VRL_DIST_MIN_MM, VRL_DIST_MAX_MM );
	float halfW = p->widthMm * 0.5f, halfH = p->heightMm * 0.5f;
	int e, row, col, n = 0;

	for ( e = 0; e < 2; e++ )
	{
		float cx = g->c[e * 2], cy = g->c[e * 2 + 1];
		float x0 = e ? 0.0f : -1.0f, x1 = e ? 1.0f : 0.0f;
		float ulo = e ? 0.5f : 0.0f, uhi = e ? 1.0f : 0.5f;

		for ( row = 0; row < VRL_MESH_ROW; row++ )
		{
			float y = -1.0f + 2.0f * (float)row / (float)VRL_MESH_CELLS;
			float sy = ( y - cy ) * halfH / d;

			for ( col = 0; col < VRL_MESH_ROW; col++ )
			{
				float x = x0 + ( x1 - x0 ) * (float)col / (float)VRL_MESH_CELLS;
				float sx = ( x - cx ) * halfW / d;
				float r = sqrtf( sx * sx + sy * sy );
				float u, v;

				if ( r > g->rMax )
				{
					/* За пределом монотонности UV перестают быть осмысленными.
					   Уводим их заведомо за границы — шейдер сделает чёрное. */
					u = ulo - 1.0f;
					v = -1.0f;
				}
				else
				{
					float s = VRL_DistortScale( r * r, k1, k2 );
					float ueye = 0.5f + sx * s / ( 2.0f * g->tanX );

					v = 0.5f + sy * s / ( 2.0f * g->tanY );
					u = ulo + ueye * 0.5f;
				}

				out[n++] = x;
				out[n++] = y;
				out[n++] = u;
				out[n++] = v;
				out[n++] = ulo;
				out[n++] = uhi;
			}
		}
	}

	return n / VRL_MESH_FLOATS_PER_VERT;
}

#endif /* vr_lens_h */
