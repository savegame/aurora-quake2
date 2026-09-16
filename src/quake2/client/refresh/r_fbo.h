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

/* Картонный VR (AURORA_VR): раскладка изображений глаз под линзы.
   split = false — вывод обычным полноэкранным квадом, как без VR.
   split = true — левая и правая половины текстуры рисуются сеткой
   дисторсии (по одной на глаз) с центрами на заданном расстоянии
   (мм экрана), см. client/vr_lens.h.
   distMm — расстояние экран-линза, k1/k2 — коэффициенты Brown-Conrady
   в тангенсах угла (профили Cardboard подставляются как есть).
   Вершины пересчитываются здесь и при смене поворота/размера/дисплея —
   не в кадре. Вызывать только при изменении параметров. */
void RFBO_SetLensLayout(bool split, float sepMm, float vofsMm, float tiltMm,
	float distMm, float k1, float k2);

/* Окно переехало на другой дисплей: перечитать DPI и пересчитать раскладку. */
void RFBO_RefreshDisplayMetrics(void);

/* Физический размер контента, по которому посчитана раскладка (мм).
   Возвращает false, если DPI не получен и взят фолбэк. */
bool RFBO_GetLensScreenMm(float *widthMm, float *heightMm);

/* Вертикальный угол обзора (градусы), который глаз реально видит через
   линзу после дисторсии. Посчитан по событию в RFBO_SetLensLayout, кадр
   только читает. false — раскладки глаз нет, FOV считает движок как обычно. */
bool RFBO_GetLensFovY(float *fovYdeg);

/* Во сколько раз дисторсия растягивает центр картинки: столько экранных
   пикселей приходится на тексель кадра в центре линзы при масштабе FBO 1.0.
   Ориентир для r_3d_scale. Возвращает false, если раскладки глаз нет. */
bool RFBO_GetLensCenterMagnification(float *mag);

#endif
