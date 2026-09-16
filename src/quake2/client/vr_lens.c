/*
 * vr_lens.c -- положение изображений глаз под линзы шлема и калибровка
 * его прямо в игре. См. vr_head.h (раздел vr_lens) и vr_lens.h.
 *
 * Почему это отдельная калибровка, а не gl_stereo_separation. Прицел
 * рисуется в центре 2D-вьюпорта глаза (SCR_DrawCrosshair по scr_vrect), а
 * камеры параллельны (gl_stereo_convergence 0) — значит центр картинки
 * глаза смотрит на бесконечность. Двоится он потому, что центры картинок на
 * стекле (W/4 и 3W/4) не совпадают с осями линз. Разнос камер в мире на это
 * не влияет вовсе: он меняет глубину ближних предметов, но не положение
 * бесконечности. Поэтому двигаем сами картинки — при выводе FBO на экран.
 *
 * Почему в мм. Линзы меряются в мм, и дефолт 63 мм даёт почти правильную
 * картинку на любом телефоне ещё до калибровки. Перевод в пиксели делает
 * FBO-модуль по DPI дисплея (с фолбэком, если DPI нет).
 *
 * Управление в шлеме — только геймпад: консоль там недоступна. Вход —
 * долгое нажатие Y (коротким Y остаётся vr_recenter, он перед калибровкой
 * только полезен), или команда vr_lens_calibrate (командная строка, cfg,
 * бинд). Строки — латиница: conchars не умеет кириллицу (шапка vr_head.c).
 */

#include "client.h"
#include "vr_head.h"

#if defined(AURORA_VR)

#include "vr_lens.h"
#include "refresh/r_fbo.h"

#include <math.h>

/* Удержание Y для входа. Короче — рецентр случайно превращался бы в
   калибровку; длиннее — в шлеме кажется, что ничего не работает. */
#define VRL_LONGPRESS_MS    1200
#define VRL_REPEAT_DELAY_MS 350
#define VRL_REPEAT_MS       60

static cvar_t *vr_lens_sep_mm;
static cvar_t *vr_lens_vofs_mm;
static cvar_t *vr_lens_tilt_mm;
static cvar_t *vr_lens_k1;
static cvar_t *vr_lens_k2;
static cvar_t *vr_lens_dist_mm;
static cvar_t *vr_lens_distort;
static cvar_t *vl_gl_stereo;
static cvar_t *vl_vr_mode;

typedef enum
{
	VRL_AXIS_NONE,
	VRL_AXIS_SEP,
	VRL_AXIS_VOFS,
	VRL_AXIS_TILT,
	VRL_AXIS_K1,
	VRL_AXIS_K2,
	VRL_AXIS_DIST
} vrl_axis_t;

/* Страницы калибровки: положение картинок и оптика. Переключаются Y —
   внутри экрана он свободен (короткий Y это vr_recenter, но во время
   калибровки все нажатия съедает VR_LensKeyEvent, а длинное удержание
   работает только при !vl.active). */
typedef enum
{
	VRL_PAGE_POSITION,
	VRL_PAGE_OPTICS,
	VRL_PAGE_COUNT
} vrl_page_t;

/*
 * Описание оси: cvar, пределы, шаг по времени удержания и точность записи.
 * Таблицей, а не switch'ом: осей стало шесть, и у них разные масштабы —
 * миллиметры крутятся десятыми, коэффициенты дисторсии тысячными.
 */
typedef struct
{
	cvar_t **cv;
	float lo, hi;
	float step0, step1, step2;
	int   prec;
} vrl_axis_info_t;

static const vrl_axis_info_t l_axes[] =
{
	{ NULL, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1 },
	{ &vr_lens_sep_mm,   VRL_SEP_MIN_MM,    VRL_SEP_MAX_MM,     0.2f,  0.5f,  1.0f, 1 },
	{ &vr_lens_vofs_mm, -VRL_VOFS_MAX_MM,   VRL_VOFS_MAX_MM,    0.2f,  0.5f,  1.0f, 1 },
	{ &vr_lens_tilt_mm, -VRL_TILT_MAX_MM,   VRL_TILT_MAX_MM,    0.2f,  0.5f,  1.0f, 1 },
	{ &vr_lens_k1,       VRL_K_MIN,         VRL_K_MAX,          0.005f, 0.01f, 0.02f, 3 },
	{ &vr_lens_k2,       VRL_K_MIN,         VRL_K_MAX,          0.005f, 0.01f, 0.02f, 3 },
	{ &vr_lens_dist_mm,  VRL_DIST_MIN_MM,   VRL_DIST_MAX_MM,    0.5f,  1.0f,  2.0f, 1 },
};

static struct
{
	bool       inited;

	/* Что уже отдано в RFBO_SetLensLayout: сравнение значений, а не флаги
	   modified — их сбрасывают посторонние (как и в VR_Frame). Значения
	   ЭФФЕКТИВНЫЕ: при vr_lens_distort 0 в k1/k2 лежат нули, поэтому
	   выключение коррекции доезжает до FBO само собой. */
	bool       layout_valid;
	bool       layout_split;
	float      layout[6];

	bool       active;
	vrl_page_t page;
	bool       pending;      /* команда пришла до применения vr_mode */
	float      saved[6];     /* значения до входа — для отмены */
	bool       paused_by_us;

	bool       y_held;
	bool       y_used;       /* это удержание уже открыло калибровку */
	int        y_down_ms;

	int        held_key;
	vrl_axis_t held_axis;
	float      held_sign;
	int        held_since_ms;
	int        next_repeat_ms;
} vl;

/* --- значения ---------------------------------------------------------- */

/* Шаг растёт с удержанием: мелкий для точной подгонки (0.2 мм — около
   трёх пикселей на 400 dpi; 0.005 по k1 — предел различимого глазом на
   краю поля), крупный — чтобы доехать с 75 до 60 мм не за минуту. */
static void VR_LensNudge( vrl_axis_t axis, float sign, int held_ms )
{
	const vrl_axis_info_t *a;
	cvar_t *cv;
	float step, q, v;

	if ( axis <= VRL_AXIS_NONE || (size_t)axis >= sizeof( l_axes ) / sizeof( l_axes[0] ) )
		return;
	a = &l_axes[axis];
	cv = *a->cv;
	if ( cv == NULL )
		return;

	step = ( held_ms < 1500 ) ? a->step0 : ( held_ms < 3000 ? a->step1 : a->step2 );

	/* Округление до шага записи: иначе после сотни нажатий в user.cfg
	   окажется 62.999996, а пользователь видит в подсказке 63.0. */
	q = ( a->prec == 3 ) ? 1000.0f : 10.0f;
	v = VRL_Clamp( cv->value + sign * step, a->lo, a->hi );
	v = floorf( v * q + 0.5f ) / q;
	Cvar_Set( cv->name, va( a->prec == 3 ? "%.3f" : "%.1f", v ) );
}

/* Сброс — только текущей страницы: угробить подобранное положение картинок,
   промахнувшись мимо кнопки на странице оптики, было бы обидно. */
static void VR_LensReset( void )
{
	if ( vl.page == VRL_PAGE_OPTICS )
	{
		Cvar_Set( "vr_lens_k1", va( "%.3f", VRL_K1_DEFAULT ) );
		Cvar_Set( "vr_lens_k2", va( "%.3f", VRL_K2_DEFAULT ) );
		Cvar_Set( "vr_lens_dist_mm", va( "%.1f", VRL_DIST_DEFAULT_MM ) );
	}
	else
	{
		Cvar_Set( "vr_lens_sep_mm", va( "%.1f", VRL_SEP_DEFAULT_MM ) );
		Cvar_Set( "vr_lens_vofs_mm", "0" );
		Cvar_Set( "vr_lens_tilt_mm", "0" );
	}
}

/* --- вход и выход ------------------------------------------------------ */

static void VR_LensEnter( void )
{
	if ( vl.active )
		return;

	vl.pending = false;
	vl.active = true;
	vl.page = VRL_PAGE_POSITION;
	vl.saved[0] = vr_lens_sep_mm->value;
	vl.saved[1] = vr_lens_vofs_mm->value;
	vl.saved[2] = vr_lens_tilt_mm->value;
	vl.saved[3] = vr_lens_k1->value;
	vl.saved[4] = vr_lens_k2->value;
	vl.saved[5] = vr_lens_dist_mm->value;
	vl.held_axis = VRL_AXIS_NONE;
	vl.y_used = true;

	/* Остановленная сцена: как пауза при открытом меню. CL_Pause сам
	   откажет в сетевой игре — там мишень рисуется поверх живого мира,
	   но игрок стоит (ввод съеден). */
	vl.paused_by_us = false;
	if ( Cvar_VariableValue( "paused" ) == 0.0f )
	{
		CL_Pause( true );
		vl.paused_by_us = ( Cvar_VariableValue( "paused" ) != 0.0f );
	}

	Com_Printf( "vr_lens: calibration started (sep %.1f mm, vofs %.1f mm, tilt %.1f mm, "
			"k1 %.3f, k2 %.3f, lens dist %.1f mm)\n",
			vl.saved[0], vl.saved[1], vl.saved[2], vl.saved[3], vl.saved[4], vl.saved[5] );
}

static void VR_LensSaveShared( void );

static void VR_LensLeave( bool save )
{
	if ( !vl.active )
		return;

	vl.active = false;
	vl.held_axis = VRL_AXIS_NONE;
	/* Y, зажатый к моменту выхода, не должен тут же открыть калибровку снова. */
	vl.y_used = true;

	if ( !save )
	{
		Cvar_Set( "vr_lens_sep_mm", va( "%.1f", vl.saved[0] ) );
		Cvar_Set( "vr_lens_vofs_mm", va( "%.1f", vl.saved[1] ) );
		Cvar_Set( "vr_lens_tilt_mm", va( "%.1f", vl.saved[2] ) );
		Cvar_Set( "vr_lens_k1", va( "%.3f", vl.saved[3] ) );
		Cvar_Set( "vr_lens_k2", va( "%.3f", vl.saved[4] ) );
		Cvar_Set( "vr_lens_dist_mm", va( "%.1f", vl.saved[5] ) );
	}

	if ( vl.paused_by_us )
		CL_Pause( false );
	vl.paused_by_us = false;

	if ( save )
	{
		/* Сразу на диск: приложение на телефоне чаще убивают, чем закрывают
		   через quit, а калибровка в шлеме — не то, что хочется повторять. */
		CL_WriteConfiguration();
		VR_LensSaveShared();
		Com_Printf( "vr_lens: saved sep %.1f mm, vofs %.1f mm, tilt %.1f mm, "
				"k1 %.3f, k2 %.3f, lens dist %.1f mm\n",
				vr_lens_sep_mm->value, vr_lens_vofs_mm->value, vr_lens_tilt_mm->value,
				vr_lens_k1->value, vr_lens_k2->value, vr_lens_dist_mm->value );
	}
	else
	{
		Com_Printf( "vr_lens: calibration cancelled\n" );
	}
}

static void VR_LensCalibrate_f( void )
{
	char *arg = ( Cmd_Argc() > 1 ) ? Cmd_Argv( 1 ) : "";

	if ( !Q_stricmp( arg, "save" ) )
	{
		if ( vl.active )
			VR_LensLeave( true );
		else
		{
			CL_WriteConfiguration();
			VR_LensSaveShared();
		}
	}
	else if ( !Q_stricmp( arg, "cancel" ) )
	{
		vl.pending = false;
		VR_LensLeave( false );
	}
	else if ( !Q_stricmp( arg, "reset" ) )
	{
		VR_LensReset();
	}
	else if ( arg[0] != '\0' )
	{
		Com_Printf( "usage: vr_lens_calibrate [save|cancel|reset]\n" );
		Com_Printf( "  without arguments: start interactive calibration (needs vr_mode 1)\n" );
	}
	else if ( VR_ModeEnabled() )
	{
		VR_LensEnter();
	}
	else if ( vl_vr_mode->value != 0.0f )
	{
		/* +vr_lens_calibrate из командной строки выполняется раньше первого
		   VR_Frame, который и применяет vr_mode, — дождёмся его. */
		vl.pending = true;
	}
	else
	{
		Com_Printf( "vr_lens_calibrate: needs vr_mode 1\n" );
	}
}

/* --- общее хранилище калибровки ---------------------------------------- */

/*
 * Калибровка — свойство очков и телефона, а не мода. Архивные cvar'ы же
 * движок пишет в user.cfg каталога ТЕКУЩЕГО мода (CL_WriteConfiguration ->
 * FS_WritableGamedir), поэтому откалиброванное в baseq2 не видно в
 * xatrix/rogue/ctf, и наоборот. Источник истины — launcher.conf
 * (~/.config/<org>/<app>, разрешён песочницей): лаунчер хранит там свои
 * настройки и не трогает чужие ключи, движок подмешивает значения после
 * exec user.cfg и пишет их обратно при сохранении калибровки из любого мода.
 * Работает и без лаунчера (AURORA_LAUNCHER_SKIP): файл читается напрямую.
 */
static const char *vrl_shared_keys[] =
{
	"vr_lens_sep_mm",
	"vr_lens_vofs_mm",
	"vr_lens_tilt_mm",
	"vr_lens_k1",
	"vr_lens_k2",
	"vr_lens_dist_mm",
	"vr_lens_distort",
	NULL
};

static bool VR_LensSharedPath( char *out, size_t size )
{
	const char *home = getenv( "HOME" );
	int n;

	if ( home == NULL || home[0] == '\0' )
		return false;
	n = snprintf( out, size, "%s/.config/" AURORA_ORG "/" AURORA_APP "/launcher.conf", home );
	return n > 0 && (size_t)n < size;
}

/* Индекс ключа калибровки в строке "key=value" или -1. */
static int VR_LensSharedKey( const char *line )
{
	const char *eq;
	size_t len;
	int i;

	while ( *line == ' ' || *line == '\t' )
		line++;
	eq = strchr( line, '=' );
	if ( eq == NULL )
		return -1;
	len = (size_t)( eq - line );
	while ( len > 0 && ( line[len - 1] == ' ' || line[len - 1] == '\t' ) )
		len--;
	for ( i = 0; vrl_shared_keys[i] != NULL; i++ )
	{
		if ( strlen( vrl_shared_keys[i] ) == len && !strncmp( line, vrl_shared_keys[i], len ) )
			return i;
	}
	return -1;
}

void VR_LensLoadShared( void )
{
	char path[MAX_OSPATH];
	char line[4096];
	FILE *f;
	int applied = 0;

	if ( !VR_LensSharedPath( path, sizeof( path ) ) )
		return;
	f = fopen( path, "rb" );
	if ( f == NULL )
		return;

	while ( fgets( line, sizeof( line ), f ) )
	{
		int k = VR_LensSharedKey( line );
		char *eq, *end;
		double v;

		if ( k < 0 )
			continue;
		eq = strchr( line, '=' );
		v = strtod( eq + 1, &end );
		while ( *end == ' ' || *end == '\t' || *end == '\r' || *end == '\n' )
			end++;
		if ( end == eq + 1 || *end != '\0' || !isfinite( v ) )
			continue;
		/* В командный буфер идёт только число, перепечатанное здесь, а не
		   строка из файла как есть. */
		Cbuf_AddText( va( "set %s %g\n", vrl_shared_keys[k], v ) );
		applied++;
	}
	fclose( f );

	if ( applied > 0 )
		Com_Printf( "vr_lens: %d calibration value(s) from %s\n", applied, path );
}

static void VR_LensSaveShared( void )
{
	char path[MAX_OSPATH];
	char tmp[MAX_OSPATH];
	char line[4096];
	FILE *in, *out;
	bool ok;
	int i;

	if ( !VR_LensSharedPath( path, sizeof( path ) ) )
		return;
	i = snprintf( tmp, sizeof( tmp ), "%s.tmp", path );
	if ( i <= 0 || (size_t)i >= sizeof( tmp ) )
		return;

	FS_CreatePath( path );
	out = fopen( tmp, "wb" );
	if ( out == NULL )
	{
		Com_Printf( "vr_lens: couldn't write %s\n", tmp );
		return;
	}

	/* Остальные ключи лаунчера переносятся как есть, прежние значения
	   калибровки выбрасываются и дописываются заново в конце. */
	in = fopen( path, "rb" );
	if ( in != NULL )
	{
		while ( fgets( line, sizeof( line ), in ) )
		{
			size_t len;

			if ( VR_LensSharedKey( line ) >= 0 )
				continue;
			len = strlen( line );
			fputs( line, out );
			if ( len > 0 && line[len - 1] != '\n' )
				fputc( '\n', out );
		}
		fclose( in );
	}

	for ( i = 0; vrl_shared_keys[i] != NULL; i++ )
		fprintf( out, "%s=%s\n", vrl_shared_keys[i], Cvar_VariableString( vrl_shared_keys[i] ) );

	ok = !ferror( out );
	if ( fclose( out ) != 0 )
		ok = false;

	/* Через временный файл и rename: приложение на телефоне могут убить в
	   любой момент, а обрезанный launcher.conf потерял бы и настройки
	   лаунчера. */
	if ( !ok || rename( tmp, path ) != 0 )
	{
		remove( tmp );
		Com_Printf( "vr_lens: couldn't update %s\n", path );
		return;
	}
	Com_Printf( "vr_lens: calibration shared via %s\n", path );
}

/* --- публичный API ----------------------------------------------------- */

void VR_LensInit( void )
{
	memset( &vl, 0, sizeof( vl ) );

	vr_lens_sep_mm = Cvar_Get( "vr_lens_sep_mm", va( "%.0f", VRL_SEP_DEFAULT_MM ), CVAR_ARCHIVE );
	vr_lens_vofs_mm = Cvar_Get( "vr_lens_vofs_mm", "0", CVAR_ARCHIVE );
	vr_lens_tilt_mm = Cvar_Get( "vr_lens_tilt_mm", "0", CVAR_ARCHIVE );

	/* Дисторсия. Дефолт — профиль Google Cardboard v2 (k1 0.34, k2 0.55 при
	   39 мм от экрана до линзы): картонки повторяют этот чертёж, и с ним
	   линии по краям заметно прямее, чем без коррекции. Нормировка — в
	   тангенсах угла, поэтому коэффициенты из чужих профилей Cardboard
	   подставляются сюда как есть. vr_lens_distort 0 выключает коррекцию,
	   не теряя подобранных значений. */
	vr_lens_k1 = Cvar_Get( "vr_lens_k1", va( "%.2f", VRL_K1_DEFAULT ), CVAR_ARCHIVE );
	vr_lens_k2 = Cvar_Get( "vr_lens_k2", va( "%.2f", VRL_K2_DEFAULT ), CVAR_ARCHIVE );
	vr_lens_dist_mm = Cvar_Get( "vr_lens_dist_mm", va( "%.0f", VRL_DIST_DEFAULT_MM ), CVAR_ARCHIVE );
	vr_lens_distort = Cvar_Get( "vr_lens_distort", "1", CVAR_ARCHIVE );
	/* Рендер регистрирует gl_stereo раньше (R_initialize до VR_Init) —
	   Cvar_Get вернёт тот же cvar, флаги и значение не тронет. */
	vl_gl_stereo = Cvar_Get( "gl_stereo", "0", 0 );
	vl_vr_mode = Cvar_Get( "vr_mode", "0", CVAR_ARCHIVE );

	Cmd_AddCommand( "vr_lens_calibrate", VR_LensCalibrate_f );
	vl.inited = true;
}

void VR_LensShutdown( void )
{
	if ( !vl.inited )
		return;
	VR_LensLeave( false );
	Cmd_RemoveCommand( "vr_lens_calibrate" );
	vl.inited = false;
}

void VR_LensFrame( void )
{
	bool mode, split;
	int now;

	if ( !vl.inited )
		return;

	now = Sys_Milliseconds();
	mode = VR_ModeEnabled();

	if ( vl.pending && mode )
		VR_LensEnter();
	if ( vl.active && !mode )
		VR_LensLeave( false ); /* сплита больше нет — калибровать нечего */

	/* Долгое Y — только из игры: в меню и консоли у пользователя явно
	   другие намерения. */
	if ( vl.y_held && !vl.y_used && !vl.active && mode && cls.key_dest == key_game
	  && now - vl.y_down_ms >= VRL_LONGPRESS_MS )
	{
		VR_LensEnter();
	}

	if ( vl.active && vl.held_axis != VRL_AXIS_NONE && now >= vl.next_repeat_ms )
	{
		VR_LensNudge( vl.held_axis, vl.held_sign, now - vl.held_since_ms );
		vl.next_repeat_ms = now + VRL_REPEAT_MS;
	}

	/* В FBO-модуль — только изменение: там на каждый вызов перестраивается
	   и заливается в VBO вся сетка дисторсии. Сплит только в VR-режиме:
	   обычный gl_stereo 2 (half-SBS для телевизора) выводится как раньше. */
	{
		bool on = ( vr_lens_distort->value != 0.0f );
		float k1 = on ? vr_lens_k1->value : 0.0f;
		float k2 = on ? vr_lens_k2->value : 0.0f;

		split = mode && vl_gl_stereo->value == 2.0f;
		if ( !vl.layout_valid || split != vl.layout_split
		  || vr_lens_sep_mm->value != vl.layout[0]
		  || vr_lens_vofs_mm->value != vl.layout[1]
		  || vr_lens_tilt_mm->value != vl.layout[2]
		  || vr_lens_dist_mm->value != vl.layout[3]
		  || k1 != vl.layout[4]
		  || k2 != vl.layout[5] )
		{
			vl.layout_valid = true;
			vl.layout_split = split;
			vl.layout[0] = vr_lens_sep_mm->value;
			vl.layout[1] = vr_lens_vofs_mm->value;
			vl.layout[2] = vr_lens_tilt_mm->value;
			vl.layout[3] = vr_lens_dist_mm->value;
			vl.layout[4] = k1;
			vl.layout[5] = k2;
			RFBO_SetLensLayout( split, vl.layout[0], vl.layout[1], vl.layout[2],
					vl.layout[3], k1, k2 );
		}
	}
}

bool VR_LensKeyEvent( int key, bool down )
{
	vrl_axis_t axis = VRL_AXIS_NONE;
	float sign = 0.0f;
	bool optics = ( vl.page == VRL_PAGE_OPTICS );

	if ( !vl.inited )
		return false;

	if ( key == K_GAMEPAD_Y )
	{
		if ( down && !vl.y_held )
		{
			vl.y_held = true;
			vl.y_used = false;
			vl.y_down_ms = Sys_Milliseconds();
		}
		else if ( !down )
		{
			vl.y_held = false;
		}
	}

	if ( !vl.active )
		return false;

	if ( !down )
	{
		if ( key == vl.held_key )
			vl.held_axis = VRL_AXIS_NONE;
		return false;
	}

	/* На Авроре в key_menu A/B/START приходят уже как ENTER/ESCAPE, в игре —
	   START как ESCAPE (IN_AuroraRemapGamepadKey), поэтому ловим обе формы.
	   Клавиатура — для отладки на хосте.

	   Раскладка одна на обе страницы, меняется только смысл осей: D-pad
	   влево-вправо — главная величина страницы (расстояние между картинками
	   либо k1), вверх-вниз — вторая (высота либо k2), бамперы — третья
	   (перекос либо расстояние до линзы). */
	if ( key == K_GAMEPAD_LEFT || key == K_LEFTARROW )
	{
		axis = optics ? VRL_AXIS_K1 : VRL_AXIS_SEP;
		sign = -1.0f;
	}
	else if ( key == K_GAMEPAD_RIGHT || key == K_RIGHTARROW )
	{
		axis = optics ? VRL_AXIS_K1 : VRL_AXIS_SEP;
		sign = 1.0f;
	}
	else if ( key == K_GAMEPAD_UP || key == K_UPARROW )
	{
		axis = optics ? VRL_AXIS_K2 : VRL_AXIS_VOFS;
		sign = 1.0f;
	}
	else if ( key == K_GAMEPAD_DOWN || key == K_DOWNARROW )
	{
		axis = optics ? VRL_AXIS_K2 : VRL_AXIS_VOFS;
		sign = -1.0f;
	}
	else if ( key == K_GAMEPAD_R || key == K_PGUP )
	{
		axis = optics ? VRL_AXIS_DIST : VRL_AXIS_TILT;
		sign = 1.0f;
	}
	else if ( key == K_GAMEPAD_L || key == K_PGDN )
	{
		axis = optics ? VRL_AXIS_DIST : VRL_AXIS_TILT;
		sign = -1.0f;
	}
	else if ( key == K_GAMEPAD_Y || key == K_TAB )
	{
		vl.page = ( vl.page == VRL_PAGE_POSITION ) ? VRL_PAGE_OPTICS : VRL_PAGE_POSITION;
		vl.held_axis = VRL_AXIS_NONE; /* ось под зажатой кнопкой сменила смысл */
	}
	else if ( key == K_GAMEPAD_A || key == K_ENTER )
	{
		VR_LensLeave( true );
	}
	else if ( key == K_GAMEPAD_B || key == K_ESCAPE )
	{
		VR_LensLeave( false );
	}
	else if ( key == K_GAMEPAD_X || key == K_BACKSPACE )
	{
		VR_LensReset();
	}

	if ( axis != VRL_AXIS_NONE )
	{
		int now = Sys_Milliseconds();

		VR_LensNudge( axis, sign, 0 );
		vl.held_key = key;
		vl.held_axis = axis;
		vl.held_sign = sign;
		vl.held_since_ms = now;
		vl.next_repeat_ms = now + VRL_REPEAT_DELAY_MS;
	}

	/* Любое нажатие во время калибровки — её: огонь, прыжок и меню ждут. */
	return true;
}

bool VR_LensCalibrating( void )
{
	return vl.inited && vl.active;
}

/* Рамка квадрата с визуально одинаковой толщиной сторон. kx — во сколько
   раз 2D-координаты по X сжаты во вьюпорте глаза. */
static void VR_LensBox( int cx, int cy, float r, float t, float kx,
		float red, float green, float blue, float alpha )
{
	int rx = (int)( r * kx ), ry = (int)r;
	int tx = (int)( t * kx ), ty = (int)t;

	if ( tx < 1 )
		tx = 1;
	if ( ty < 1 )
		ty = 1;
	Draw_FillAlpha( cx - rx, cy - ry, 2 * rx, ty, red, green, blue, alpha );
	Draw_FillAlpha( cx - rx, cy + ry - ty, 2 * rx, ty, red, green, blue, alpha );
	Draw_FillAlpha( cx - rx, cy - ry, tx, 2 * ry, red, green, blue, alpha );
	Draw_FillAlpha( cx + rx - tx, cy - ry, tx, 2 * ry, red, green, blue, alpha );
}

static void VR_LensText( int cx, int y, char *s, float scale )
{
	DrawStringScaled( cx - (int)( strlen( s ) * 8 * scale * 0.5f ), y, s, scale );
}

/* Окружность точками: Draw_* умеет только прямоугольники, а круг на
   странице оптики нужен именно круглым — по нему видно, что коррекция
   радиально-симметрична и центр не уехал. kx сжимает X, как и везде. */
static void VR_LensRing( int cx, int cy, float r, float t, float kx,
		float red, float green, float blue, float alpha )
{
	const int steps = 48;
	int i, sx = (int)( t * kx ), sy = (int)t;

	if ( sx < 1 )
		sx = 1;
	if ( sy < 1 )
		sy = 1;
	for ( i = 0; i < steps; i++ )
	{
		float a = (float)i * ( 2.0f * 3.14159265f / (float)steps );
		int px = cx + (int)( cosf( a ) * r * kx );
		int py = cy + (int)( sinf( a ) * r );

		Draw_FillAlpha( px - sx / 2, py - sy / 2, sx, sy, red, green, blue, alpha );
	}
}

void VR_LensDraw( void )
{
	int W, H, cx, cy, i, lh, t2;
	float u, t, kx, scale, wmm, hmm;
	bool dpi;

	if ( !vl.inited || !vl.active )
		return;

	/* Координаты 2D — весь viddef, но во вьюпорте половины (R_Setup2DViewport):
	   по X всё сжато вдвое. Центр viddef = центр картинки глаза = ось
	   линзы после калибровки. Размеры — от высоты, X домножается на kx,
	   чтобы крест и квадраты в линзе были квадратными. */
	W = viddef.width;
	H = viddef.height;
	cx = W / 2;
	cy = H / 2;
	kx = ( vl_gl_stereo->value == 2.0f ) ? 2.0f : 1.0f;
	u = H / 100.0f;
	t = u * 0.45f;
	if ( t < 2.0f )
		t = 2.0f;
	t2 = (int)( t * 0.5f );
	if ( t2 < 1 )
		t2 = 1;

	Draw_FadeScreen();

	/* Мишень рисуется в КАДР, то есть до дисторсии, и искажается сеткой
	   вместе со сценой. Это и нужно: через линзу её линии обязаны выглядеть
	   прямыми — по ним и подбирается k1. Рисуй мы её после дисторсии,
	   калибровать было бы нечего. */

	/* Сетка — видно перекос по вертикали и то, где кончается поле линзы.
	   На странице оптики она гуще и покрывает всю область глаза: кривизну
	   видно по краям, а там линий и должно быть много. */
	{
		int span = ( vl.page == VRL_PAGE_OPTICS ) ? 14 : 6;
		float alpha = ( vl.page == VRL_PAGE_OPTICS ) ? 0.5f : 0.35f;

		for ( i = -span; i <= span; i++ )
		{
			int gx = cx + (int)( i * 10.0f * u * kx );
			int gy = cy + (int)( i * 10.0f * u );

			if ( i == 0 )
				continue;
			if ( gy > 0 && gy < H )
				Draw_FillAlpha( 0, gy - t2 / 2, W, t2, 0.6f, 0.6f, 0.6f, alpha );
			if ( gx > 0 && gx < W )
				Draw_FillAlpha( gx - (int)( t2 * kx ) / 2, 0, (int)( t2 * kx ), H, 0.6f, 0.6f, 0.6f, alpha );
		}
	}

	if ( vl.page == VRL_PAGE_OPTICS )
	{
		/* Концентрические окружности по всей области глаза: при верном k1
		   они круглые и равномерные, при заниженном — раздуты к краю
		   (остаточная подушка), при завышенном — сжаты (перекоррекция). */
		VR_LensRing( cx, cy, 12.0f * u, t, kx, 1.0f, 1.0f, 1.0f, 0.9f );
		VR_LensRing( cx, cy, 24.0f * u, t, kx, 1.0f, 0.85f, 0.2f, 0.9f );
		VR_LensRing( cx, cy, 36.0f * u, t, kx, 0.3f, 0.7f, 1.0f, 0.9f );
	}
	else
	{
		/* Кольца-квадраты и крест. Сведённые, они должны совпасть целиком: по
		   кольцам двоение заметнее, чем по одному кресту. */
		VR_LensBox( cx, cy, 8.0f * u, t, kx, 1.0f, 1.0f, 1.0f, 0.9f );
		VR_LensBox( cx, cy, 18.0f * u, t, kx, 1.0f, 0.85f, 0.2f, 0.9f );
		VR_LensBox( cx, cy, 30.0f * u, t, kx, 0.3f, 0.7f, 1.0f, 0.9f );
	}

	Draw_FillAlpha( cx - (int)( 30.0f * u * kx ), cy - (int)( t * 0.5f ),
			(int)( 60.0f * u * kx ), (int)t, 0.2f, 1.0f, 0.2f, 1.0f );
	Draw_FillAlpha( cx - (int)( t * kx * 0.5f ), cy - (int)( 30.0f * u ),
			(int)( t * kx ), (int)( 60.0f * u ), 0.2f, 1.0f, 0.2f, 1.0f );
	Draw_FillAlpha( cx - (int)( 1.5f * u * kx ), cy - (int)( 1.5f * u ),
			(int)( 3.0f * u * kx ), (int)( 3.0f * u ), 1.0f, 0.2f, 0.2f, 1.0f );

	scale = SCR_GetMenuScale();
	lh = (int)( 10 * scale );

	dpi = RFBO_GetLensScreenMm( &wmm, &hmm );

	if ( vl.page == VRL_PAGE_OPTICS )
	{
		float fov = 0.0f;
		bool has_fov = RFBO_GetLensFovY( &fov );

		/* 40u, а не 50u: 50u — это ровно половина высоты кадра, там строки
		   упираются в край экрана и обрезаются. Кольца выше кончаются на
		   36u, так что подписи их не перекрывают. */
		VR_LensText( cx, cy - (int)( 40.0f * u ) - 2 * lh, "VR LENS CALIBRATION  2/2 OPTICS", scale );
		VR_LensText( cx, cy - (int)( 40.0f * u ) - lh, "tune k1 until grid lines look straight", scale );

		VR_LensText( cx, cy + (int)( 40.0f * u ), "DPAD L/R k1   U/D k2   LB/RB lens dist", scale );
		VR_LensText( cx, cy + (int)( 40.0f * u ) + lh, "Y position page   A save   B cancel   X reset", scale );
		VR_LensText( cx, cy + (int)( 40.0f * u ) + 2 * lh,
				va( "k1 %.3f  k2 %.3f  lens %.1fmm  fov %.0f%s",
					vr_lens_k1->value, vr_lens_k2->value, vr_lens_dist_mm->value,
					has_fov ? fov : 0.0f,
					( vr_lens_distort->value != 0.0f ) ? "" : "  (distortion off)" ), scale );
	}
	else
	{
		VR_LensText( cx, cy - (int)( 32.0f * u ) - 2 * lh, "VR LENS CALIBRATION  1/2 POSITION", scale );
		VR_LensText( cx, cy - (int)( 32.0f * u ) - lh, "merge both targets into one sharp cross", scale );

		VR_LensText( cx, cy + (int)( 32.0f * u ), "DPAD L/R distance  U/D height  LB/RB tilt", scale );
		VR_LensText( cx, cy + (int)( 32.0f * u ) + lh, "Y optics page   A save   B cancel   X reset", scale );
		VR_LensText( cx, cy + (int)( 32.0f * u ) + 2 * lh,
				va( "dist %.1fmm  height %+.1f  tilt %+.1f%s",
					vr_lens_sep_mm->value, vr_lens_vofs_mm->value, vr_lens_tilt_mm->value,
					dpi ? "" : "  (no dpi)" ), scale );
	}
}

#else /* !AURORA_VR — сборка без VR: заглушки */

void VR_LensInit( void ) {}
void VR_LensShutdown( void ) {}
void VR_LensFrame( void ) {}
bool VR_LensKeyEvent( int key, bool down ) { (void)key; (void)down; return false; }
bool VR_LensCalibrating( void ) { return false; }
void VR_LensDraw( void ) {}

#endif /* AURORA_VR */
