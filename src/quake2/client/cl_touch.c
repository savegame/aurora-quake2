/*
 * Touch overlay (ОС Аврора). См. cl_touch.h.
 *
 * Меню (по требованию пользователя):
 * - справа экрана: кнопки «вверх» и «вниз» (вертикальная пара);
 * - чуть левее них: «OK»;
 * - слева экрана: «влево» и «вправо» (горизонтальная пара);
 * - верхний левый угол: «назад» (с отступом от края — у края системные жесты).
 * Меню-кнопки шлют Key_Event (K_UPARROW/.../K_ENTER/K_ESCAPE) — меню их
 * получает через штатный роутинг Key_Event → M_Keydown.
 *
 * Игра (key_game, не cinematic), docs/touch_ui.md:
 * - левая половина — плавающий виртуальный стик (база появляется там, где
 *   палец коснулся экрана), вектор уходит в движение через IN_SetTouchStick;
 * - правая половина — тачпад осмотра, дельты уходят в IN_AddTouchLook
 *   (тот же путь, что и мышь);
 * - кнопки действий привязаны к РЕАЛЬНЫМ action'ам движка (консольные
 *   команды +attack/+moveup/+movedown, inv- и weap-команды, "cmd help"
 *   через Cbuf), а не к эмуляции клавиш — не ломаются при перебиндинге;
 * - кнопки «залипают» за fingerId: палец можно увести с области кнопки,
 *   отпускание — только по FINGERUP этого пальца; FIRE/JUMP (флаг
 *   TBF_LOOK) одновременно захватывают тачпад осмотра — зажатый выстрел/
 *   прыжок не мешает целиться;
 * - у всех кнопок предусмотрены иконки: из отдельной текстуры (icon,
 *   Draw_StretchPic) или из атласа (icon + iconS*, Draw_SubPic);
 * - отрисовка оверлея — через imgui (C-шим aurora_imgui), легаси-фолбэк —
 *   Draw_FillAlpha, если imgui недоступен;
 * - раскладку можно переопределить конфигом: cvar touch_uiscale и команда
 *   «touchbtn <label> <x> <y> <w> <h>» (exec touchui.cfg в Touch_Init).
 *
 * Видеозаставки (cinematic): только кнопка SKIP — движок пропускает ролик
 * по action-кнопке (cl_input.c: cmd->buttons, >1с ролика, не attractloop),
 * поэтому SKIP шлёт +attack/-attack через Cbuf.
 *
 * Мультитач: каждый fingerId захватывается не более чем одним элементом
 * (кнопка / стик / тачпад) и отслеживается до FINGERUP.
 */

#include "client.h"
#include "cl_touch.h"

#if defined(AURORA_OS)

#include "backends/input.h"
#include "aurora_imgui.h"

#if defined(AURORA_FBO)
#include "refresh/r_fbo.h"
#endif

#include <SDL2/SDL.h>
#include <math.h>

/* Где видна/активна кнопка. */
#define TBF_MENU      1 /* в меню */
#define TBF_GAME      2 /* в игре (не cinematic) */
#define TBF_CINEMATIC 4 /* в игре на видеозаставке */
#define TBF_LOOK      8 /* кнопка дополнительно захватывает тачпад осмотра
                           (тот же палец крутит обзор, пока кнопка зажата) */
#define TBF_ATTRACT  16 /* attract-заставка (демо-цикл: лого id и т.п.) —
                           там action-кнопки ролик не пропускают, нужен ESC */

typedef struct
{
	int key;              /* >0: Key_Event(key); -1: командная кнопка */
	const char *downCmd;  /* командная: команда на нажатие (Cbuf) */
	const char *upCmd;    /* командная: команда на отпускание (может NULL) */
	const char *label;
	const char *icon;     /* имя pic'а (как для Draw_Pic); NULL — подпись */
	int iconSx, iconSy;   /* подрегион атласа в пикселях текстуры... */
	int iconSw, iconSh;   /* ...iconSw <= 0 — вся текстура */
	int flags;            /* TBF_* */
	int x, y, w, h;
	bool pressed;
	SDL_FingerID fingerId;
} touchButton_t;

#define TB_KEYCMD (-1)

static touchButton_t touchButtons[] =
{
	/* Меню. */
	{ K_ESCAPE,     NULL,          NULL,           "ESC",  NULL, 0,0,0,0, TBF_MENU | TBF_GAME | TBF_ATTRACT, 0,0,0,0, false, 0 },
	{ K_UPARROW,    NULL,          NULL,           "UP",   NULL, 0,0,0,0, TBF_MENU, 0,0,0,0, false, 0 },
	{ K_DOWNARROW,  NULL,          NULL,           "DN",   NULL, 0,0,0,0, TBF_MENU, 0,0,0,0, false, 0 },
	{ K_ENTER,      NULL,          NULL,           "OK",   NULL, 0,0,0,0, TBF_MENU, 0,0,0,0, false, 0 },
	{ K_LEFTARROW,  NULL,          NULL,           "<",    NULL, 0,0,0,0, TBF_MENU, 0,0,0,0, false, 0 },
	{ K_RIGHTARROW, NULL,          NULL,           ">",    NULL, 0,0,0,0, TBF_MENU, 0,0,0,0, false, 0 },
	/* Видеозаставка: только пропуск (action-кнопка, не клавиша). */
	{ TB_KEYCMD,    "+attack\n",   "-attack\n",    "SKIP", NULL, 0,0,0,0, TBF_CINEMATIC, 0,0,0,0, false, 0 },
	/* Игра. */
	{ TB_KEYCMD,    "cmd help\n",  NULL,           "F1",   NULL, 0,0,0,0, TBF_GAME, 0,0,0,0, false, 0 },
	{ TB_KEYCMD,    "+attack\n",   "-attack\n",    "FIRE", NULL, 0,0,0,0, TBF_GAME | TBF_LOOK, 0,0,0,0, false, 0 },
	{ TB_KEYCMD,    "+moveup\n",   "-moveup\n",    "JUMP", NULL, 0,0,0,0, TBF_GAME | TBF_LOOK, 0,0,0,0, false, 0 },
	{ TB_KEYCMD,    "+movedown\n", "-movedown\n",  "DUCK", NULL, 0,0,0,0, TBF_GAME, 0,0,0,0, false, 0 },
	{ TB_KEYCMD,    "invprev\n",   NULL,           "IT<",  NULL, 0,0,0,0, TBF_GAME, 0,0,0,0, false, 0 },
	{ TB_KEYCMD,    "invuse\n",    NULL,           "USE",  NULL, 0,0,0,0, TBF_GAME, 0,0,0,0, false, 0 },
	{ TB_KEYCMD,    "invnext\n",   NULL,           "IT>",  NULL, 0,0,0,0, TBF_GAME, 0,0,0,0, false, 0 },
	{ TB_KEYCMD,    "weapprev\n",  NULL,           "WP<",  NULL, 0,0,0,0, TBF_GAME, 0,0,0,0, false, 0 },
	{ TB_KEYCMD,    "weapnext\n",  NULL,           "WP>",  NULL, 0,0,0,0, TBF_GAME, 0,0,0,0, false, 0 },
};
#define TOUCH_NUM_BUTTONS (sizeof(touchButtons) / sizeof(touchButtons[0]))

enum
{
	TB_ESC, TB_UP, TB_DOWN, TB_OK, TB_LEFT, TB_RIGHT,
	TB_SKIP,
	TB_HELP, TB_FIRE, TB_JUMP, TB_CROUCH,
	TB_ITEM_PREV, TB_ITEM_USE, TB_ITEM_NEXT,
	TB_WEAP_PREV, TB_WEAP_NEXT
};

static int touchMm;          /* экранных пикселей в миллиметре (от DPI дисплея) */
static int touchLayoutW = 0; /* размеры экрана, под которые посчитана раскладка */
static int touchLayoutH = 0;

static cvar_t *touch_looksens;
static cvar_t *touch_uiscale;
static float touchLayoutScale = 0.0f;    /* touch_uiscale, под который посчитана раскладка */
static float touchLayoutFboScale = 0.0f; /* scale FBO, под который посчитана раскладка */

/* Переопределения раскладки из конфига (команда touchbtn, exec touchui.cfg). */
static bool touchOvr[TOUCH_NUM_BUTTONS];
static int touchOvrX[TOUCH_NUM_BUTTONS], touchOvrY[TOUCH_NUM_BUTTONS];
static int touchOvrW[TOUCH_NUM_BUTTONS], touchOvrH[TOUCH_NUM_BUTTONS];

/* Плавающий стик (левая половина). */
static bool stickActive;
static SDL_FingerID stickFinger;
static int stickBaseX, stickBaseY; /* центр (куда коснулись) */
static int stickKnobX, stickKnobY; /* текущая позиция ручки (с клампом) */
static int stickMaxOffset;         /* макс. смещение ручки, пиксели */
static int stickBaseR, stickKnobR; /* радиусы отрисовки */

/* Тачпад осмотра (правая половина). */
static bool lookActive;
static SDL_FingerID lookFinger;
static float lookLastX, lookLastY;

/* Масштаб буфера рендеринга («3D scale» из лаунчера). Сцена и тач-UI
   рисуются в FBO размером экран*scale, а затем FBO растягивается квадом
   на весь экран — пиксель FBO на экране занимает 1/scale экранных пикселей.
   Чтобы физический размер элементов UI (в мм) не зависел от 3D scale, все
   переводы мм->px делаем в пиксели FBO с этим коэффициентом. Тач-ввод уже
   приходит в пикселях FBO (RFBO_TransformTouch), поэтому хит-тест и
   отрисовка остаются в одной системе координат при любом scale. */
static float Touch_FboScale(void)
{
#if defined(AURORA_FBO)
	return RFBO_GetScale();
#else
	return 1.0f;
#endif
}

static int Touch_MmToPx(float mm)
{
	return (int)(mm * touchMm * Touch_FboScale() + 0.5f);
}

static void Touch_Layout(void)
{
	float uscale = touch_uiscale ? touch_uiscale->value : 1.0f;
	if (uscale < 0.5f)
		uscale = 0.5f;
	if (uscale > 3.0f)
		uscale = 3.0f;

	float fboScale = Touch_FboScale();
	if (viddef.width == touchLayoutW && viddef.height == touchLayoutH &&
	    uscale == touchLayoutScale && fboScale == touchLayoutFboScale)
		return;

	touchLayoutW = viddef.width;
	touchLayoutH = viddef.height;
	touchLayoutScale = uscale;
	touchLayoutFboScale = fboScale;

	int size = Touch_MmToPx(13.0f * uscale);
	int sizeBig = Touch_MmToPx(16.0f * uscale);
	int gap = Touch_MmToPx(4.0f * uscale);
	int margin = Touch_MmToPx(5.0f);
	int marginV = Touch_MmToPx(1.0f); /* верх/низ — вплотную к краю экрана */
	int marginC = Touch_MmToPx(1.0f); /* угловые кнопки (ESC/SKIP/F1) — у края */
	int cx = viddef.width / 2;
	int cy = viddef.height / 2;

	stickMaxOffset = Touch_MmToPx(15.0f * uscale);
	stickBaseR = Touch_MmToPx(18.0f * uscale);
	stickKnobR = Touch_MmToPx(9.0f * uscale);

	/* «Назад» — верхний левый угол, вплотную к краю (1 мм). */
	touchButtons[TB_ESC].x = marginC;
	touchButtons[TB_ESC].y = marginC;

	/* HELP (в игре) и SKIP (на заставке) — правее «назад»; одновременно
	   не видны (разные контексты), поэтому слот общий. */
	touchButtons[TB_SKIP].x = marginC + size + gap;
	touchButtons[TB_SKIP].y = marginC;
	touchButtons[TB_HELP].x = touchButtons[TB_SKIP].x;
	touchButtons[TB_HELP].y = touchButtons[TB_SKIP].y;

	/* Меню: справа «вверх» над «вниз». */
	touchButtons[TB_UP].x = viddef.width - margin - size;
	touchButtons[TB_UP].y = cy - size - gap / 2;
	touchButtons[TB_DOWN].x = touchButtons[TB_UP].x;
	touchButtons[TB_DOWN].y = cy + gap / 2;

	/* Меню: «OK» — левее пары вверх/вниз, по их вертикальному центру. */
	touchButtons[TB_OK].x = touchButtons[TB_UP].x - gap - size;
	touchButtons[TB_OK].y = cy - size / 2;

	/* Меню: «влево» и «вправо» горизонтальной парой слева. */
	touchButtons[TB_LEFT].x = margin;
	touchButtons[TB_LEFT].y = cy - size / 2;
	touchButtons[TB_RIGHT].x = margin + size + gap;
	touchButtons[TB_RIGHT].y = touchButtons[TB_LEFT].y;

	/* Игра, правая сторона: FIRE по центру, JUMP выше, DUCK ниже. */
	touchButtons[TB_FIRE].x = viddef.width - margin - sizeBig;
	touchButtons[TB_FIRE].y = cy - sizeBig / 2;
	touchButtons[TB_JUMP].x = viddef.width - margin - size;
	touchButtons[TB_JUMP].y = touchButtons[TB_FIRE].y - gap - size;
	touchButtons[TB_CROUCH].x = touchButtons[TB_JUMP].x;
	touchButtons[TB_CROUCH].y = touchButtons[TB_FIRE].y + sizeBig + gap;

	/* Игра, верхняя середина: prev/use/next item. */
	touchButtons[TB_ITEM_USE].x = cx - size / 2;
	touchButtons[TB_ITEM_USE].y = marginV;
	touchButtons[TB_ITEM_PREV].x = touchButtons[TB_ITEM_USE].x - gap - size;
	touchButtons[TB_ITEM_PREV].y = marginV;
	touchButtons[TB_ITEM_NEXT].x = touchButtons[TB_ITEM_USE].x + size + gap;
	touchButtons[TB_ITEM_NEXT].y = marginV;

	/* Игра, нижняя середина: prev/next weapon; между ними промежуток,
	   чтобы была видна иконка текущего оружия из игрового UI. */
	int weapGap = size * 2;
	touchButtons[TB_WEAP_PREV].x = cx - weapGap / 2 - size;
	touchButtons[TB_WEAP_PREV].y = viddef.height - marginV - size;
	touchButtons[TB_WEAP_NEXT].x = cx + weapGap / 2;
	touchButtons[TB_WEAP_NEXT].y = touchButtons[TB_WEAP_PREV].y;

	for (int i = 0; i < (int)TOUCH_NUM_BUTTONS; i++)
	{
		touchButtons[i].w = size;
		touchButtons[i].h = size;
	}
	touchButtons[TB_FIRE].w = sizeBig;
	touchButtons[TB_FIRE].h = sizeBig;

	/* Переопределения из конфига (touchui.cfg, команда touchbtn). */
	for (int i = 0; i < (int)TOUCH_NUM_BUTTONS; i++)
	{
		if (!touchOvr[i])
			continue;
		touchButtons[i].x = touchOvrX[i];
		touchButtons[i].y = touchOvrY[i];
		touchButtons[i].w = touchOvrW[i];
		touchButtons[i].h = touchOvrH[i];
	}
}

/* Команда «touchbtn <label> <x> <y> <w> <h>» — переопределение геометрии
   кнопки (пиксели контента). Живёт в конфиге touchui.cfg (exec в Touch_Init). */
static void Touch_BtnCmd(void)
{
	if (Cmd_Argc() != 6)
	{
		Com_Printf("touchbtn <label> <x> <y> <w> <h>\n");
		return;
	}
	const char *label = Cmd_Argv(1);
	for (int i = 0; i < (int)TOUCH_NUM_BUTTONS; i++)
	{
		if (Q_strcasecmp(touchButtons[i].label, label) != 0)
			continue;
		touchOvr[i] = true;
		touchOvrX[i] = atoi(Cmd_Argv(2));
		touchOvrY[i] = atoi(Cmd_Argv(3));
		touchOvrW[i] = atoi(Cmd_Argv(4));
		touchOvrH[i] = atoi(Cmd_Argv(5));
		touchButtons[i].x = touchOvrX[i];
		touchButtons[i].y = touchOvrY[i];
		touchButtons[i].w = touchOvrW[i];
		touchButtons[i].h = touchOvrH[i];
		Com_Printf("touchbtn: %s -> %d %d %d %d\n", touchButtons[i].label,
			touchOvrX[i], touchOvrY[i], touchOvrW[i], touchOvrH[i]);
		return;
	}
	Com_Printf("touchbtn: кнопка '%s' не найдена\n", label);
}

void Touch_Init(void)
{
	float ddpi = 0.0f;
	if (SDL_GetDisplayDPI(0, &ddpi, NULL, NULL) != 0 || ddpi <= 0.0f)
		ddpi = 320.0f;
	touchMm = (int)(ddpi / 25.4f + 0.5f);
	if (touchMm < 1)
		touchMm = 1;

	touch_looksens = Cvar_Get("touch_looksens", "1.0", CVAR_ARCHIVE);
	touch_uiscale = Cvar_Get("touch_uiscale", "1.0", CVAR_ARCHIVE);
	Cmd_AddCommand("touchbtn", Touch_BtnCmd);

	touchLayoutW = touchLayoutH = 0;
	touchLayoutScale = 0.0f;
	touchLayoutFboScale = 0.0f;
	Touch_Layout();

	/* Пользовательские переопределения раскладки (необязательный файл). */
	Cbuf_AddText("exec touchui.cfg\n");
}

/* Текущий контекст экрана. */
static int Touch_Context(void)
{
	if (cls.key_dest == key_menu)
		return TBF_MENU;
	if (cls.key_dest == key_game)
	{
		if (cl.cinematictime > 0)
			return cl.attractloop ? TBF_ATTRACT : TBF_CINEMATIC;
		return TBF_GAME;
	}
	return 0;
}

static bool Touch_ButtonVisible(const touchButton_t *b, int context)
{
	return (b->flags & context) != 0;
}

static touchButton_t *Touch_HitTest(int x, int y, int context)
{
	for (int i = 0; i < (int)TOUCH_NUM_BUTTONS; i++)
	{
		touchButton_t *b = &touchButtons[i];
		if (!Touch_ButtonVisible(b, context))
			continue;
		if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h)
			return b;
	}
	return NULL;
}

static void Touch_Press(touchButton_t *b, SDL_FingerID fingerId)
{
	b->pressed = true;
	b->fingerId = fingerId;
	if (b->key > 0)
		Key_Event(b->key, true);
	else if (b->downCmd != NULL)
		Cbuf_AddText(b->downCmd);
}

static void Touch_Release(touchButton_t *b)
{
	if (!b->pressed)
		return;
	b->pressed = false;
	if (b->key > 0)
		Key_Event(b->key, false);
	else if (b->upCmd != NULL)
		Cbuf_AddText(b->upCmd);
}

static void Touch_StickStop(void)
{
	stickActive = false;
	IN_SetTouchStick(0.0f, 0.0f);
}

static void Touch_StickUpdate(float x, float y)
{
	/* Ручка рисуется прямо под пальцем (без клампа), кламп — только
	   для вектора движения. */
	stickKnobX = (int)x;
	stickKnobY = (int)y;

	float dx = x - stickBaseX;
	float dy = y - stickBaseY;
	float len = sqrtf(dx * dx + dy * dy);
	if (len > stickMaxOffset)
	{
		dx = dx / len * stickMaxOffset;
		dy = dy / len * stickMaxOffset;
		len = stickMaxOffset;
	}

	/* Нормализованный вектор с мёртвой зоной; вверх (dy<0) = вперёд. */
	float nx = dx / stickMaxOffset;
	float ny = -dy / stickMaxOffset;
	const float dead = 0.15f;
	if (len / stickMaxOffset < dead)
	{
		nx = 0.0f;
		ny = 0.0f;
	}
	IN_SetTouchStick(nx, ny);
}

void Touch_FingerEvent(int sdlEventType, long long fingerId, float x, float y)
{
	Touch_Layout();
	int context = Touch_Context();

	if (sdlEventType == SDL_FINGERDOWN)
	{
		if (context == 0)
			return;

		/* Сначала кнопки — они перекрывают зоны стика/тачпада. */
		touchButton_t *b = Touch_HitTest((int)x, (int)y, context);
		if (b != NULL && !b->pressed)
		{
			Touch_Press(b, fingerId);
			/* «Залипающие» кнопки (FIRE/JUMP): тот же палец одновременно
			   крутит обзор — иначе кнопка съедает touchId и целиться
			   нельзя. */
			if ((b->flags & TBF_LOOK) && context == TBF_GAME && !lookActive)
			{
				lookActive = true;
				lookFinger = fingerId;
				lookLastX = x;
				lookLastY = y;
			}
			return;
		}

		/* Зоны стика и тачпада — только в геймплее (не меню/заставка). */
		if (context != TBF_GAME)
			return;

		if (x < viddef.width / 2)
		{
			if (!stickActive)
			{
				stickActive = true;
				stickFinger = fingerId;
				stickBaseX = (int)x;
				stickBaseY = (int)y;
				Touch_StickUpdate(x, y);
			}
		}
		else if (!lookActive)
		{
			lookActive = true;
			lookFinger = fingerId;
			lookLastX = x;
			lookLastY = y;
		}
	}
	else if (sdlEventType == SDL_FINGERMOTION)
	{
		/* Для кнопок motion намеренно игнорируется: кнопка остаётся
		   зажатой, пока палец не поднят — так меньше ложных отпусканий,
		   и можно стрелять/прыгать, уведя палец с области кнопки. */
		if (stickActive && fingerId == stickFinger)
			Touch_StickUpdate(x, y);
		else if (lookActive && fingerId == lookFinger)
		{
			/* Чувствительность нормализована к миллиметрам: при высоком DPI
			   за тот же жест (в мм) приходит больше пикселей — компенсируем
			   (32 px/mm — подобрано на устройстве с высоким DPI). Дельты
			   приходят в пикселях FBO, а миллиметр на экране — это
			   touchMm*scale пикселей FBO, поэтому учитываем и 3D scale. */
			float sens = touch_looksens->value * (32.0f / (touchMm * Touch_FboScale()));
			IN_AddTouchLook((x - lookLastX) * sens, (y - lookLastY) * sens);
			lookLastX = x;
			lookLastY = y;
		}
	}
	else if (sdlEventType == SDL_FINGERUP)
	{
		for (int i = 0; i < (int)TOUCH_NUM_BUTTONS; i++)
		{
			if (touchButtons[i].pressed && touchButtons[i].fingerId == fingerId)
				Touch_Release(&touchButtons[i]);
		}
		if (stickActive && fingerId == stickFinger)
			Touch_StickStop();
		if (lookActive && fingerId == lookFinger)
			lookActive = false;
	}
}

void Touch_Frame(void)
{
	int context = Touch_Context();

	/* Меню/игру закрыли с зажатым пальцем — отпустить все кнопки. */
	if (context == 0)
	{
		for (int i = 0; i < (int)TOUCH_NUM_BUTTONS; i++)
			Touch_Release(&touchButtons[i]);
		Touch_StickStop();
		lookActive = false;
		return;
	}

	/* Вышли из геймплея (меню/заставка) — стик и тачпад не действуют. */
	if (context != TBF_GAME)
	{
		Touch_StickStop();
		lookActive = false;
	}

	/* Кнопки, чей контекст кончился с зажатым пальцем (заставка
	   закончилась с зажатым SKIP и т.п.), — отпустить. */
	for (int i = 0; i < (int)TOUCH_NUM_BUTTONS; i++)
	{
		if (touchButtons[i].pressed && !Touch_ButtonVisible(&touchButtons[i], context))
			Touch_Release(&touchButtons[i]);
	}
}

/* Залитый круг горизонтальными полосами (оверлейные Draw_FillAlpha). */
static void Touch_FillCircle(int cx, int cy, int r, float R, float G, float B, float A)
{
	const int steps = 12;
	for (int i = 0; i < steps; i++)
	{
		float y0 = -r + 2.0f * r * i / steps;
		float y1 = -r + 2.0f * r * (i + 1) / steps;
		float ym = (y0 + y1) * 0.5f;
		float hw = sqrtf(r * r - ym * ym);
		Draw_FillAlpha((int)(cx - hw), (int)(cy + y0), (int)(2 * hw), (int)(y1 - y0) + 1, R, G, B, A);
	}
}

/* Отрисовка через imgui (0 — не пробовали, 1 — активен, -1 — недоступен,
   легаси-отрисовка Draw_FillAlpha). */
static int l_imguiState = 0;
static bool l_imgui = false;

static bool Touch_UseImgui(void)
{
	if (l_imguiState == 0)
		/* Высота глифа ~3 мм от DPI (imgui-theme-spec.md, секция 3);
		   Touch_MmToPx даёт пиксели FBO (с 3D scale), так что физический
		   размер шрифта на экране от scale не зависит. Кламп 14..96 px —
		   внутри AuroraImgui_Init. */
		l_imguiState = AuroraImgui_Init((float)Touch_MmToPx(3.0f)) ? 1 : -1;
	return l_imguiState > 0;
}

/* Подложки под группами кнопок УБРАНЫ (указание пользователя 2026-08-06):
   жёстких групп нет, кнопки потом можно будет расставить как угодно. */

static void Touch_DrawButton(const touchButton_t *b)
{
	if (l_imgui)
	{
		/* Рамка/подложка — через imgui; иконка (если есть) — легаси-путём,
		   она ляжет под полупрозрачную рамку при рендере imgui в конце. */
		AuroraImgui_Button((float)b->x, (float)b->y, (float)b->w, (float)b->h,
			b->icon != NULL ? NULL : b->label, b->pressed);
		if (b->icon != NULL)
		{
			int pad = Touch_MmToPx(2.0f);
			if (b->iconSw > 0 && b->iconSh > 0)
				Draw_SubPic(b->x + pad, b->y + pad, b->w - 2 * pad, b->h - 2 * pad,
					(char *)b->icon, b->iconSx, b->iconSy, b->iconSw, b->iconSh);
			else
				Draw_StretchPic(b->x + pad, b->y + pad, b->w - 2 * pad, b->h - 2 * pad,
					(char *)b->icon);
		}
		return;
	}

	if (b->pressed)
		Draw_FillAlpha(b->x, b->y, b->w, b->h, 0.9f, 0.9f, 0.9f, 0.4f);
	else
		Draw_FillAlpha(b->x, b->y, b->w, b->h, 0.1f, 0.1f, 0.1f, 0.15f);

	if (b->icon != NULL)
	{
		/* Иконка: из атласа (подрегион iconS*) или отдельной текстурой. */
		int pad = Touch_MmToPx(2.0f);
		if (b->iconSw > 0 && b->iconSh > 0)
			Draw_SubPic(b->x + pad, b->y + pad, b->w - 2 * pad, b->h - 2 * pad,
				(char *)b->icon, b->iconSx, b->iconSy, b->iconSw, b->iconSh);
		else
			Draw_StretchPic(b->x + pad, b->y + pad, b->w - 2 * pad, b->h - 2 * pad,
				(char *)b->icon);
		return;
	}

	/* Подпись по центру кнопки (символ conchars — 8x8). */
	float scale = b->w / 40.0f;
	if (scale < 1.0f)
		scale = 1.0f;
	int labelW = 0;
	for (const char *s = b->label; *s != '\0'; s++)
		labelW += 8;
	labelW = (int)(labelW * scale);
	Draw_CharBegin();
	for (int j = 0; b->label[j] != '\0'; j++)
		Draw_CharScaled(b->x + (b->w - labelW) / 2 + (int)(j * 8 * scale),
			b->y + (b->h - (int)(8 * scale)) / 2, b->label[j], scale);
	Draw_CharEnd();
}

void Touch_DrawOverlay(void)
{
	int context = Touch_Context();
	if (context == 0)
		return;

	Touch_Layout();

	l_imgui = Touch_UseImgui();
	if (l_imgui)
		AuroraImgui_NewFrame(viddef.width, viddef.height);

	for (int i = 0; i < (int)TOUCH_NUM_BUTTONS; i++)
	{
		touchButton_t *b = &touchButtons[i];
		if (Touch_ButtonVisible(b, context))
			Touch_DrawButton(b);
	}

	/* Плавающий стик: база + ручка. */
	if (stickActive && context == TBF_GAME)
	{
		if (l_imgui)
		{
			AuroraImgui_StickCircle((float)stickBaseX, (float)stickBaseY, (float)stickBaseR, false);
			AuroraImgui_StickCircle((float)stickKnobX, (float)stickKnobY, (float)stickKnobR, true);
		}
		else
		{
			Touch_FillCircle(stickBaseX, stickBaseY, stickBaseR, 0.1f, 0.1f, 0.1f, 0.35f);
			Touch_FillCircle(stickKnobX, stickKnobY, stickKnobR, 0.9f, 0.9f, 0.9f, 0.5f);
		}
	}

	if (l_imgui)
		AuroraImgui_Render();
}

#else /* !AURORA_OS — заглушки, чтобы модуль собирался на всех платформах. */

void Touch_Init(void) {}
void Touch_FingerEvent(int sdlEventType, long long fingerId, float x, float y)
{
	(void)sdlEventType; (void)fingerId; (void)x; (void)y;
}
void Touch_Frame(void) {}
void Touch_DrawOverlay(void) {}

#endif
