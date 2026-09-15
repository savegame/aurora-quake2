/*
 * vr_lens_test.c -- тест раскладки изображений глаз под линзы (vr_lens.h).
 *
 * Зачем. Ошибка здесь в шлеме выглядит как «прицел всё равно двоится» или
 * «картинка съехала», и по ней не понять, перепутан ли знак, масштаб мм
 * или UV обрезаны не с той стороны. На синтетике всё это проверяется
 * напрямую: переводим полученные вершины обратно в мм и сравниваем с
 * заданными.
 *
 * Сборка: цель vr_lens_test в CMakeLists.txt (только host, AURORA_VR=ON).
 */

#include "vr_lens.h"

#include <math.h>
#include <stdio.h>

static int g_failures;

static void check( int ok, const char *what, double got, double want )
{
	printf( "  %s %-48s  %9.4f (ожидание %.4f)\n", ok ? "ok  " : "FAIL", what, got, want );
	if ( !ok )
		g_failures++;
}

static void check_near( const char *what, double got, double want, double eps )
{
	check( fabs( got - want ) <= eps, what, got, want );
}

/* Разбор квада глаза из массива вершин: границы по x/y и по u. */
typedef struct
{
	float x0, x1, y0, y1, u0, u1;
} quad_t;

static void parse_quad( const float *v, quad_t *q )
{
	int i;

	q->x0 = q->y0 = q->u0 = 1e9f;
	q->x1 = q->y1 = q->u1 = -1e9f;
	for ( i = 0; i < 6; i++ )
	{
		const float *p = v + i * 4;
		if ( p[0] < q->x0 ) q->x0 = p[0];
		if ( p[0] > q->x1 ) q->x1 = p[0];
		if ( p[1] < q->y0 ) q->y0 = p[1];
		if ( p[1] > q->y1 ) q->y1 = p[1];
		if ( p[2] < q->u0 ) q->u0 = p[2];
		if ( p[2] > q->u1 ) q->u1 = p[2];
	}
}

/* Где на экране (NDC контента) окажется центр половины текстуры глаза
   (u = 0.25 или 0.75) — по линейной интерполяции вершин квада. Именно там
   стоит прицел этого глаза. Заодно проверяет, что UV не растянуты: на
   единицу NDC ширины обязано приходиться ровно 0.5 UV. */
static float eye_center_x( const quad_t *q, float u_center, float *texels_per_ndc )
{
	*texels_per_ndc = ( q->u1 - q->u0 ) / ( q->x1 - q->x0 );
	return q->x0 + ( u_center - q->u0 ) / *texels_per_ndc;
}

static void run_layout( const char *name, float wmm, float hmm, float sep, float vofs, float tilt )
{
	float c[4], v[48], k;
	quad_t L, R;
	int n;

	printf( "-- %s: экран %.0fx%.0f мм, sep %.1f, vofs %.1f, tilt %.1f\n", name, wmm, hmm, sep, vofs, tilt );

	VRL_EyeCenters( wmm, hmm, sep, vofs, tilt, c );
	n = VRL_BuildEyeQuads( c, v );
	check( n == 12, "число вершин", n, 12 );

	parse_quad( v, &L );
	parse_quad( v + 24, &R );

	float lx = eye_center_x( &L, 0.25f, &k );
	check_near( "левый: 0.5 UV на 1 NDC (без растяжения)", k, 0.5, 1e-4 );
	float rx = eye_center_x( &R, 0.75f, &k );
	check_near( "правый: 0.5 UV на 1 NDC (без растяжения)", k, 0.5, 1e-4 );

	float sep_c = VRL_Clamp( sep, VRL_SEP_MIN_MM, VRL_SEP_MAX_MM );
	check_near( "расстояние между прицелами, мм", ( rx - lx ) * 0.5f * wmm, sep_c, 1e-3 );
	check_near( "симметрия относительно середины", lx + rx, 0.0, 1e-5 );

	float ly = ( L.y0 + L.y1 ) * 0.5f, ry = ( R.y0 + R.y1 ) * 0.5f;
	float vofs_c = VRL_Clamp( vofs, -VRL_VOFS_MAX_MM, VRL_VOFS_MAX_MM );
	float tilt_c = VRL_Clamp( tilt, -VRL_TILT_MAX_MM, VRL_TILT_MAX_MM );
	check_near( "средний сдвиг вверх, мм", ( ly + ry ) * 0.25f * hmm, vofs_c, 1e-3 );
	check_near( "правый выше левого, мм", ( ry - ly ) * 0.5f * hmm, tilt_c, 1e-3 );
	check_near( "высота квада = весь экран", L.y1 - L.y0, 2.0, 1e-5 );

	/* Внутренние края не заходят за середину, UV внешних краёв целые. */
	check( L.x1 <= 1e-6f, "левый не заходит правее середины", L.x1, 0.0 );
	check( R.x0 >= -1e-6f, "правый не заходит левее середины", R.x0, 0.0 );
	check_near( "левый: внешний край u = 0", L.u0, 0.0, 1e-6 );
	check_near( "правый: внешний край u = 1", R.u1, 1.0, 1e-6 );
	check( L.u1 <= 0.5f + 1e-6f && R.u0 >= 0.5f - 1e-6f, "глаза не берут текстуру друг друга",
			L.u1, 0.5 );
}

static void test_default_matches_old_quad( void )
{
	float c[4], v[48];
	quad_t L, R;

	/* sep = половина ширины — ровно прежний вывод: [-1,0] и [0,1]. */
	printf( "-- совпадение с прежним полноэкранным квадом\n" );
	VRL_EyeCenters( 150.0f, 70.0f, 75.0f, 0.0f, 0.0f, c );
	VRL_BuildEyeQuads( c, v );
	parse_quad( v, &L );
	parse_quad( v + 24, &R );
	check_near( "левый x0", L.x0, -1.0, 1e-5 );
	check_near( "левый x1", L.x1, 0.0, 1e-5 );
	check_near( "левый u1", L.u1, 0.5, 1e-5 );
	check_near( "правый x0", R.x0, 0.0, 1e-5 );
	check_near( "правый x1", R.x1, 1.0, 1e-5 );
	check_near( "правый u0", R.u0, 0.5, 1e-5 );
}

static void test_screen_mm( void )
{
	float w, h;
	int ok;

	printf( "-- мм экрана из DPI\n" );

	/* 6.5" 2400x1080 ~ 405 dpi. Контент ландшафтный при любом повороте:
	   функции отдают уже ширину контента, от transform она не зависит. */
	ok = VRL_ContentSizeMm( 405.0f, 2400, 1080, &w, &h );
	check( ok == 1, "DPI принят", ok, 1 );
	check_near( "ширина 2400 px @405", w, 2400 * 25.4 / 405.0, 1e-3 );
	check_near( "высота 1080 px @405", h, 1080 * 25.4 / 405.0, 1e-3 );

	ok = VRL_ContentSizeMm( 0.0f, 2400, 1080, &w, &h );
	check( ok == 0, "нет DPI -> фолбэк", ok, 0 );
	check_near( "фолбэк: ширина", w, VRL_FALLBACK_WIDTH_MM, 1e-4 );
	check_near( "фолбэк: пропорции сохранены", h, VRL_FALLBACK_WIDTH_MM * 1080.0 / 2400.0, 1e-3 );

	/* Композитор отдал физический размер-заглушку: DPI астрономический. */
	ok = VRL_ContentSizeMm( 60000.0f, 2400, 1080, &w, &h );
	check( ok == 0, "DPI 60000 -> фолбэк", ok, 0 );

	/* Правдоподобный DPI, но экран вышел 20 мм — тоже враньё. */
	ok = VRL_ContentSizeMm( 1500.0f, 1000, 500, &w, &h );
	check( ok == 0, "экран 17 мм -> фолбэк", ok, 0 );

	/* Дефолт 63 мм на типичном телефоне заметно ближе к центру, чем W/4. */
	{
		float c[4];
		VRL_ContentSizeMm( 405.0f, 2400, 1080, &w, &h );
		VRL_EyeCenters( w, h, VRL_SEP_DEFAULT_MM, 0.0f, 0.0f, c );
		check( c[2] < 0.45f && c[2] > 0.35f, "дефолт: центр правого глаза, NDC", c[2], 0.4 );
	}
}

int main( void )
{
	printf( "=== тест раскладки глаз под линзы ===\n" );

	test_default_matches_old_quad();
	run_layout( "линзы ближе половины экрана", 152.0f, 68.0f, 63.0f, 0.0f, 0.0f );
	run_layout( "сдвиг вверх и перекос", 152.0f, 68.0f, 60.0f, 3.5f, -1.2f );
	run_layout( "линзы шире половины (планшет)", 110.0f, 70.0f, 70.0f, -2.0f, 0.5f );
	run_layout( "за пределами: зажим", 152.0f, 68.0f, 200.0f, 99.0f, -99.0f );
	test_screen_mm();

	if ( g_failures == 0 )
	{
		printf( "=== всё сошлось ===\n" );
		return 0;
	}
	printf( "=== провалов: %d ===\n", g_failures );
	return 1;
}
