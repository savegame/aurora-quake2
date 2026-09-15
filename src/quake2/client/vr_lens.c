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
static cvar_t *vl_gl_stereo;
static cvar_t *vl_vr_mode;

typedef enum
{
	VRL_AXIS_NONE,
	VRL_AXIS_SEP,
	VRL_AXIS_VOFS,
	VRL_AXIS_TILT
} vrl_axis_t;

static struct
{
	bool       inited;

	/* Что уже отдано в RFBO_SetLensLayout: сравнение значений, а не флаги
	   modified — их сбрасывают посторонние (как и в VR_Frame). */
	bool       layout_valid;
	bool       layout_split;
	float      layout[3];

	bool       active;
	bool       pending;      /* команда пришла до применения vr_mode */
	float      saved[3];     /* значения до входа — для отмены */
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

static cvar_t *VR_LensAxisCvar( vrl_axis_t axis, float *lo, float *hi )
{
	switch ( axis )
	{
	case VRL_AXIS_SEP:
		*lo = VRL_SEP_MIN_MM;
		*hi = VRL_SEP_MAX_MM;
		return vr_lens_sep_mm;
	case VRL_AXIS_VOFS:
		*lo = -VRL_VOFS_MAX_MM;
		*hi = VRL_VOFS_MAX_MM;
		return vr_lens_vofs_mm;
	case VRL_AXIS_TILT:
		*lo = -VRL_TILT_MAX_MM;
		*hi = VRL_TILT_MAX_MM;
		return vr_lens_tilt_mm;
	default:
		return NULL;
	}
}

static void VR_LensNudge( vrl_axis_t axis, float delta )
{
	float lo, hi, v;
	cvar_t *cv = VR_LensAxisCvar( axis, &lo, &hi );

	if ( cv == NULL )
		return;

	/* Округление до 0.1 мм: иначе после сотни шагов в user.cfg окажется
	   62.999996, а пользователь видит в подсказке 63.0. */
	v = VRL_Clamp( cv->value + delta, lo, hi );
	v = floorf( v * 10.0f + 0.5f ) / 10.0f;
	Cvar_Set( cv->name, va( "%.1f", v ) );
}

/* Шаг растёт с удержанием: мелкий для точной подгонки (0.2 мм — около
   трёх пикселей на 400 dpi), крупный — чтобы доехать с 75 до 60 мм не за
   минуту. */
static float VR_LensStep( int held_ms )
{
	if ( held_ms < 1500 )
		return 0.2f;
	if ( held_ms < 3000 )
		return 0.5f;
	return 1.0f;
}

static void VR_LensReset( void )
{
	Cvar_Set( "vr_lens_sep_mm", va( "%.1f", VRL_SEP_DEFAULT_MM ) );
	Cvar_Set( "vr_lens_vofs_mm", "0" );
	Cvar_Set( "vr_lens_tilt_mm", "0" );
}

/* --- вход и выход ------------------------------------------------------ */

static void VR_LensEnter( void )
{
	if ( vl.active )
		return;

	vl.pending = false;
	vl.active = true;
	vl.saved[0] = vr_lens_sep_mm->value;
	vl.saved[1] = vr_lens_vofs_mm->value;
	vl.saved[2] = vr_lens_tilt_mm->value;
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

	Com_Printf( "vr_lens: calibration started (sep %.1f mm, vofs %.1f mm, tilt %.1f mm)\n",
			vl.saved[0], vl.saved[1], vl.saved[2] );
}

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
	}

	if ( vl.paused_by_us )
		CL_Pause( false );
	vl.paused_by_us = false;

	if ( save )
	{
		/* Сразу на диск: приложение на телефоне чаще убивают, чем закрывают
		   через quit, а калибровка в шлеме — не то, что хочется повторять. */
		CL_WriteConfiguration();
		Com_Printf( "vr_lens: saved sep %.1f mm, vofs %.1f mm, tilt %.1f mm\n",
				vr_lens_sep_mm->value, vr_lens_vofs_mm->value, vr_lens_tilt_mm->value );
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
			CL_WriteConfiguration();
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

/* --- публичный API ----------------------------------------------------- */

void VR_LensInit( void )
{
	memset( &vl, 0, sizeof( vl ) );

	vr_lens_sep_mm = Cvar_Get( "vr_lens_sep_mm", va( "%.0f", VRL_SEP_DEFAULT_MM ), CVAR_ARCHIVE );
	vr_lens_vofs_mm = Cvar_Get( "vr_lens_vofs_mm", "0", CVAR_ARCHIVE );
	vr_lens_tilt_mm = Cvar_Get( "vr_lens_tilt_mm", "0", CVAR_ARCHIVE );
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
		VR_LensNudge( vl.held_axis, vl.held_sign * VR_LensStep( now - vl.held_since_ms ) );
		vl.next_repeat_ms = now + VRL_REPEAT_MS;
	}

	/* В FBO-модуль — только изменение. Сплит только в VR-режиме: обычный
	   gl_stereo 2 (half-SBS для телевизора) выводится как раньше. */
	split = mode && vl_gl_stereo->value == 2.0f;
	if ( !vl.layout_valid || split != vl.layout_split
	  || vr_lens_sep_mm->value != vl.layout[0]
	  || vr_lens_vofs_mm->value != vl.layout[1]
	  || vr_lens_tilt_mm->value != vl.layout[2] )
	{
		vl.layout_valid = true;
		vl.layout_split = split;
		vl.layout[0] = vr_lens_sep_mm->value;
		vl.layout[1] = vr_lens_vofs_mm->value;
		vl.layout[2] = vr_lens_tilt_mm->value;
		RFBO_SetLensLayout( split, vl.layout[0], vl.layout[1], vl.layout[2] );
	}
}

bool VR_LensKeyEvent( int key, bool down )
{
	vrl_axis_t axis = VRL_AXIS_NONE;
	float sign = 0.0f;

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
	   Клавиатура — для отладки на хосте. */
	if ( key == K_GAMEPAD_LEFT || key == K_LEFTARROW )
	{
		axis = VRL_AXIS_SEP;
		sign = -1.0f;
	}
	else if ( key == K_GAMEPAD_RIGHT || key == K_RIGHTARROW )
	{
		axis = VRL_AXIS_SEP;
		sign = 1.0f;
	}
	else if ( key == K_GAMEPAD_UP || key == K_UPARROW )
	{
		axis = VRL_AXIS_VOFS;
		sign = 1.0f;
	}
	else if ( key == K_GAMEPAD_DOWN || key == K_DOWNARROW )
	{
		axis = VRL_AXIS_VOFS;
		sign = -1.0f;
	}
	else if ( key == K_GAMEPAD_R || key == K_PGUP )
	{
		axis = VRL_AXIS_TILT;
		sign = 1.0f;
	}
	else if ( key == K_GAMEPAD_L || key == K_PGDN )
	{
		axis = VRL_AXIS_TILT;
		sign = -1.0f;
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

		VR_LensNudge( axis, sign * VR_LensStep( 0 ) );
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

	/* Сетка — видно перекос по вертикали и то, где кончается поле линзы. */
	for ( i = -6; i <= 6; i++ )
	{
		int gx = cx + (int)( i * 10.0f * u * kx );
		int gy = cy + (int)( i * 10.0f * u );

		if ( i == 0 )
			continue;
		if ( gy > 0 && gy < H )
			Draw_FillAlpha( 0, gy - t2 / 2, W, t2, 0.6f, 0.6f, 0.6f, 0.35f );
		if ( gx > 0 && gx < W )
			Draw_FillAlpha( gx - (int)( t2 * kx ) / 2, 0, (int)( t2 * kx ), H, 0.6f, 0.6f, 0.6f, 0.35f );
	}

	/* Кольца-квадраты и крест. Сведённые, они должны совпасть целиком: по
	   кольцам двоение заметнее, чем по одному кресту. */
	VR_LensBox( cx, cy, 8.0f * u, t, kx, 1.0f, 1.0f, 1.0f, 0.9f );
	VR_LensBox( cx, cy, 18.0f * u, t, kx, 1.0f, 0.85f, 0.2f, 0.9f );
	VR_LensBox( cx, cy, 30.0f * u, t, kx, 0.3f, 0.7f, 1.0f, 0.9f );

	Draw_FillAlpha( cx - (int)( 30.0f * u * kx ), cy - (int)( t * 0.5f ),
			(int)( 60.0f * u * kx ), (int)t, 0.2f, 1.0f, 0.2f, 1.0f );
	Draw_FillAlpha( cx - (int)( t * kx * 0.5f ), cy - (int)( 30.0f * u ),
			(int)( t * kx ), (int)( 60.0f * u ), 0.2f, 1.0f, 0.2f, 1.0f );
	Draw_FillAlpha( cx - (int)( 1.5f * u * kx ), cy - (int)( 1.5f * u ),
			(int)( 3.0f * u * kx ), (int)( 3.0f * u ), 1.0f, 0.2f, 0.2f, 1.0f );

	scale = SCR_GetMenuScale();
	lh = (int)( 10 * scale );

	dpi = RFBO_GetLensScreenMm( &wmm, &hmm );

	VR_LensText( cx, cy - (int)( 32.0f * u ) - 2 * lh, "VR LENS CALIBRATION", scale );
	VR_LensText( cx, cy - (int)( 32.0f * u ) - lh, "merge both targets into one sharp cross", scale );

	VR_LensText( cx, cy + (int)( 32.0f * u ), "DPAD L/R distance  U/D height  LB/RB tilt", scale );
	VR_LensText( cx, cy + (int)( 32.0f * u ) + lh, "A save   B cancel   X reset", scale );
	VR_LensText( cx, cy + (int)( 32.0f * u ) + 2 * lh,
			va( "dist %.1fmm  height %+.1f  tilt %+.1f%s",
				vr_lens_sep_mm->value, vr_lens_vofs_mm->value, vr_lens_tilt_mm->value,
				dpi ? "" : "  (no dpi)" ), scale );
}

#else /* !AURORA_VR — сборка без VR: заглушки */

void VR_LensInit( void ) {}
void VR_LensShutdown( void ) {}
void VR_LensFrame( void ) {}
bool VR_LensKeyEvent( int key, bool down ) { (void)key; (void)down; return false; }
bool VR_LensCalibrating( void ) { return false; }
void VR_LensDraw( void ) {}

#endif /* AURORA_VR */
