/*
 * FBO-модуль порта на ОС Аврора (этап 2 по gameport/AGENTS.MD).
 *
 * Вся сцена и UI рендерятся в текстуру FBO, перед swap содержимое
 * выводится на экран полноэкранным квадом. На этом лягут поворот
 * контента (этап 3) и масштабирование рендеринга (даунскейл/апскейл).
 *
 * Активно только при сборке с -DAURORA_FBO=ON (дефайн AURORA_FBO).
 */

#ifndef r_fbo_h
#define r_fbo_h

#include <stdbool.h>

/* Создание FBO, квада и шейдерной программы блита.
   windowWidth/windowHeight — реальный размер окна (экрана).
   Подменяет viddef.width/height размером FBO (с учётом scale). */
bool RFBO_Init(int windowWidth, int windowHeight);
void RFBO_Shutdown(void);

/* Пересоздание FBO под новый размер окна (ресайз, смена дисплея). */
void RFBO_Resize(int windowWidth, int windowHeight);

/* Бинд FBO как цели рендеринга кадра. Вызов в начале R_Frame_begin. */
void RFBO_BindForFrame(void);

/* Вывод текстуры FBO квадом на экран. Вызов в R_Frame_end перед swap. */
void RFBO_DrawToScreen(void);

/* Размер буфера рендеринга (FBO). */
void RFBO_GetSize(int *width, int *height);

/* Коэффициент масштабирования рендеринга (0.25..2.0, по умолчанию 1.0).
   Применяется при создании/пересоздании FBO. Геттер возвращает 1.0, пока
   FBO не готов (рендер напрямую на экран — масштаб не действует). */
void RFBO_SetScale(float scale);
float RFBO_GetScale(void);

/* Поворот контента — значение enum wl_output_transform (0..3).
   Матрицы поворота готовятся заранее, здесь лишь переключается активная.
   Применяется при выводе квада (этап 3). */
void RFBO_SetRotation(int wlOutputTransform);
int RFBO_GetRotation(void);

/* Тач из координат окна (нормированные 0..1, как в SDL_TouchFingerEvent)
   в координаты контента (пиксели FBO) с учётом поворота квада (этап 4). */
void RFBO_TransformTouch(float fx, float fy, int *x, int *y);

#endif
