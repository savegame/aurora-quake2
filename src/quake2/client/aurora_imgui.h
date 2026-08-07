/*
 * Тонкий C-шим над Dear ImGui (ОС Аврора).
 *
 * imgui — C++; чтобы C-код движка (cl_touch и др.) не тащил C++,
 * вся работа с imgui собрана здесь. imgui используется ТОЛЬКО для
 * отрисовки UI-оверлея (подложки, кнопки, стик тач-UI; далее — in-game
 * клавиатура и лаунчер): мультитач-ввод imgui не даёт, хит-тест остаётся
 * в cl_touch.c (см. gameport/docs/touch_ui.md).
 *
 * Рендер — imgui_impl_opengl3 в режиме GLES3 (IMGUI_IMPL_OPENGL_ES3):
 * VAO-путь даёт полную изоляцию vertex attrib state от OpenGLWrapper'а
 * (ES2-путь перетирал его низкие attribute locations и ломал рендер
 * игры). Рисуем в игровой FBO в координатах контента (viddef), поэтому
 * UI поворачивается вместе с картинкой штатным блитом FBO.
 *
 * Активно только под AURORA_OS.
 */

#ifndef aurora_imgui_h
#define aurora_imgui_h

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ленивая инициализация (вызывать, когда GL-контекст текущий).
   fontSizePx — размер шрифта UI в пикселях контента (от DPI).
   false — imgui недоступен, рисовать легаси-путём (Draw_FillAlpha и пр.). */
bool AuroraImgui_Init(float fontSizePx);
void AuroraImgui_Shutdown(void);
bool AuroraImgui_IsReady(void);

/* Кадр UI в координатах контента: NewFrame → примитивы → Render. */
void AuroraImgui_NewFrame(int width, int height);
void AuroraImgui_Render(void);

/* Примитивы тач-оверлея. */
void AuroraImgui_Panel(float x, float y, float w, float h);
void AuroraImgui_Button(float x, float y, float w, float h, const char *label, bool pressed);
void AuroraImgui_StickCircle(float cx, float cy, float r, bool knob);

/* Тема по imgui-theme-spec.md (палитра + метрики от размера шрифта).
   Общая для in-game оверлея и лаунчера. Вызывать при текущем контексте
   imgui (после ImGui::CreateContext). */
void AuroraImgui_ApplyTheme(float fontSizePx);

#ifdef __cplusplus
}
#endif

#endif
