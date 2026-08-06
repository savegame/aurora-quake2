/*
 * Touch overlay для меню (ОС Аврора). См. cl_touch.h.
 *
 * Раскладка (по требованию пользователя):
 * - справа экрана: кнопки «вверх» и «вниз» (вертикальная пара);
 * - чуть левее них: «OK»;
 * - слева экрана: «влево» и «вправо» (горизонтальная пара);
 * - верхний левый угол: «назад» (с отступом от края — у края системные жесты).
 *
 * Нажатия отслеживаются по fingerId (мультитач), кнопки шлют Key_Event
 * с кодами K_UPARROW/K_DOWNARROW/K_LEFTARROW/K_RIGHTARROW/K_ENTER/K_ESCAPE —
 * меню их получает через штатный роутинг Key_Event → M_Keydown.
 */

#include "client.h"
#include "cl_touch.h"

#if defined(AURORA_OS)

#include <SDL2/SDL.h>

typedef struct
{
	int key;
	const char *label;
	int x, y, w, h;
	bool pressed;
	SDL_FingerID fingerId;
} touchButton_t;

static touchButton_t touchButtons[] =
{
	{ K_ESCAPE,     "ESC", 0, 0, 0, 0, false, 0 },
	{ K_UPARROW,    "UP",  0, 0, 0, 0, false, 0 },
	{ K_DOWNARROW,  "DN",  0, 0, 0, 0, false, 0 },
	{ K_ENTER,      "OK",  0, 0, 0, 0, false, 0 },
	{ K_LEFTARROW,  "<",   0, 0, 0, 0, false, 0 },
	{ K_RIGHTARROW, ">",   0, 0, 0, 0, false, 0 },
};
#define TOUCH_NUM_BUTTONS (sizeof(touchButtons) / sizeof(touchButtons[0]))

enum { TB_ESC, TB_UP, TB_DOWN, TB_OK, TB_LEFT, TB_RIGHT };

static int touchMm;          /* пикселей в миллиметре */
static int touchLayoutW = 0; /* размеры экрана, под которые посчитана раскладка */
static int touchLayoutH = 0;

static int Touch_MmToPx(float mm)
{
	return (int)(mm * touchMm + 0.5f);
}

static void Touch_Layout(void)
{
	if (viddef.width == touchLayoutW && viddef.height == touchLayoutH)
		return;

	touchLayoutW = viddef.width;
	touchLayoutH = viddef.height;

	int size = Touch_MmToPx(13.0f);
	int gap = Touch_MmToPx(4.0f);
	int margin = Touch_MmToPx(5.0f);
	int cy = viddef.height / 2;

	/* «Назад» — верхний левый угол, с отступом от краёв. */
	touchButtons[TB_ESC].x = margin;
	touchButtons[TB_ESC].y = margin;

	/* Справа: «вверх» над «вниз». */
	touchButtons[TB_UP].x = viddef.width - margin - size;
	touchButtons[TB_UP].y = cy - size - gap / 2;
	touchButtons[TB_DOWN].x = touchButtons[TB_UP].x;
	touchButtons[TB_DOWN].y = cy + gap / 2;

	/* «OK» — левее пары вверх/вниз, по их вертикальному центру. */
	touchButtons[TB_OK].x = touchButtons[TB_UP].x - gap - size;
	touchButtons[TB_OK].y = cy - size / 2;

	/* Слева: «влево» и «вправо» горизонтальной парой. */
	touchButtons[TB_LEFT].x = margin;
	touchButtons[TB_LEFT].y = cy - size / 2;
	touchButtons[TB_RIGHT].x = margin + size + gap;
	touchButtons[TB_RIGHT].y = touchButtons[TB_LEFT].y;

	for (int i = 0; i < (int)TOUCH_NUM_BUTTONS; i++)
	{
		touchButtons[i].w = size;
		touchButtons[i].h = size;
	}
}

void Touch_Init(void)
{
	float ddpi = 0.0f;
	if (SDL_GetDisplayDPI(0, &ddpi, NULL, NULL) != 0 || ddpi <= 0.0f)
		ddpi = 320.0f;
	touchMm = (int)(ddpi / 25.4f + 0.5f);
	if (touchMm < 1)
		touchMm = 1;

	touchLayoutW = touchLayoutH = 0;
	Touch_Layout();
}

static touchButton_t *Touch_HitTest(int x, int y)
{
	for (int i = 0; i < (int)TOUCH_NUM_BUTTONS; i++)
	{
		touchButton_t *b = &touchButtons[i];
		if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h)
			return b;
	}
	return NULL;
}

static void Touch_Press(touchButton_t *b, SDL_FingerID fingerId)
{
	b->pressed = true;
	b->fingerId = fingerId;
	Key_Event(b->key, true);
}

static void Touch_Release(touchButton_t *b)
{
	if (!b->pressed)
		return;
	b->pressed = false;
	Key_Event(b->key, false);
}

void Touch_FingerEvent(int sdlEventType, long long fingerId, float x, float y)
{
	Touch_Layout();

	if (sdlEventType == SDL_FINGERDOWN)
	{
		/* В игре доступна только кнопка «назад» (ESC — открывает меню),
		   полный набор кнопок — только в меню. Игровой тач-UI — отдельный этап. */
		if (cls.key_dest != key_menu && cls.key_dest != key_game)
			return;

		touchButton_t *b = Touch_HitTest((int)x, (int)y);
		if (b != NULL && !b->pressed)
		{
			if (cls.key_dest == key_game && b->key != K_ESCAPE)
				return;
			Touch_Press(b, fingerId);
		}
	}
	else if (sdlEventType == SDL_FINGERUP)
	{
		for (int i = 0; i < (int)TOUCH_NUM_BUTTONS; i++)
		{
			if (touchButtons[i].pressed && touchButtons[i].fingerId == fingerId)
				Touch_Release(&touchButtons[i]);
		}
	}
	/* FINGERMOTION намеренно игнорируется: кнопка остаётся зажатой,
	   пока палец не поднят — так меньше ложных отпусканий от дрожания. */
}

void Touch_Frame(void)
{
	/* Если меню закрыли с зажатой кнопкой — отпустить все. */
	if (cls.key_dest == key_menu || cls.key_dest == key_game)
		return;

	for (int i = 0; i < (int)TOUCH_NUM_BUTTONS; i++)
		Touch_Release(&touchButtons[i]);
}

void Touch_DrawOverlay(void)
{
	if (cls.key_dest != key_menu && cls.key_dest != key_game)
		return;

	Touch_Layout();

	for (int i = 0; i < (int)TOUCH_NUM_BUTTONS; i++)
	{
		touchButton_t *b = &touchButtons[i];

		/* В игре рисуем только «назад» (вход в меню). */
		if (cls.key_dest == key_game && b->key != K_ESCAPE)
			continue;

		if (b->pressed)
			Draw_FillAlpha(b->x, b->y, b->w, b->h, 0.9f, 0.9f, 0.9f, 0.6f);
		else
			Draw_FillAlpha(b->x, b->y, b->w, b->h, 0.1f, 0.1f, 0.1f, 0.45f);

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
