/*
 * Touch overlay (ОС Аврора).
 *
 * Экранные кнопки и зоны ввода поверх меню, игры и видеозаставок:
 * - меню: навигационные кнопки (вверх/вниз/OK/влево/вправо/назад) шлют
 *   настоящие Key_Event движка;
 * - игра: плавающий стик движения (левая половина), тачпад осмотра
 *   (правая половина), кнопки действий (огонь/прыжок/присед, предметы,
 *   оружие, HELP) — привязаны к реальным action'ам движка через Cbuf;
 * - видеозаставки: кнопка SKIP.
 * У кнопок предусмотрены иконки (отдельная текстура или атлас).
 * Отрисовывается последним в кадре из SCR_UpdateScreen.
 * Активно только под AURORA_OS.
 */

#ifndef client_touch_h
#define client_touch_h

#include <stdbool.h>

/* Инициализация (расчёт размеров от физического DPI). Вызов из IN_Init. */
void Touch_Init(void);

/* Обработка SDL_FINGERDOWN/SDL_FINGERMOTION/SDL_FINGERUP.
   x, y — координаты в пикселях экрана (контента/FBO). */
void Touch_FingerEvent(int sdlEventType, long long fingerId, float x, float y);

/* Per-frame проверка зависших нажатий (контекст сменился с зажатым
   пальцем). Вызов из IN_Update. */
void Touch_Frame(void);

/* Отрисовка оверлея. Вызов из SCR_UpdateScreen (меню/игра/cinematic). */
void Touch_DrawOverlay(void);

#endif
