/*
 * Touch overlay для меню (ОС Аврора).
 *
 * Экранные кнопки навигации по меню: вверх/вниз (справа), OK (левее них),
 * влево/вправо (слева), «назад» (верхний левый угол). Кнопки генерируют
 * настоящие key event'ы движка (Key_Event), отрисовываются последними
 * в кадре из SCR_UpdateScreen. Активно только под AURORA_OS.
 */

#ifndef client_touch_h
#define client_touch_h

#include <stdbool.h>

/* Инициализация (расчёт размеров от физического DPI). Вызов из IN_Init. */
void Touch_Init(void);

/* Обработка SDL_FINGERDOWN/SDL_FINGERMOTION/SDL_FINGERUP.
   x, y — координаты в пикселях экрана. */
void Touch_FingerEvent(int sdlEventType, long long fingerId, float x, float y);

/* Per-frame проверка зависших нажатий (меню закрыли с зажатым пальцем).
   Вызов из IN_Update. */
void Touch_Frame(void);

/* Отрисовка оверлея поверх меню. Вызов из SCR_UpdateScreen после M_Draw. */
void Touch_DrawOverlay(void);

#endif
