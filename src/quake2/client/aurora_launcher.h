/*
 * In-process лаунчер на Dear ImGui (ОС Аврора). См. aurora_launcher.cpp.
 *
 * Лаунчер показывается из Qcommon_Init ДО инициализации подсистем движка:
 * создаёт окно и EGL-контекст штатными средствами движка (sdlwCreateWindow
 * + eglwInitialize) и НЕ освобождает их при выходе — движок продолжает
 * работать с тем же окном/контекстом (на Авроре композитор убивает
 * приложение, если его окно исчезает хотя бы на мгновение).
 *
 * Активно только под AURORA_OS.
 */

#ifndef aurora_launcher_h
#define aurora_launcher_h

#ifdef __cplusplus
extern "C" {
#endif

/* Показать лаунчер (блокирующий цикл до выбора пользователя).
   0 — старт игры (ресурсы/мод/настройки переданы через env:
       AURORA_RESDIR, AURORA_GAME_MOD, AURORA_R_3D_SCALE),
   1 — выход из приложения. */
int Launcher_Run(void);

#ifdef __cplusplus
}
#endif

#endif
