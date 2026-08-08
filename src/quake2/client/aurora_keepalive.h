/*
 * Запрет гашения экрана на ОС Аврора (MCE blanking pause).
 * См. aurora_keepalive.c.
 *
 * Реализация активна только при AURORA_MCE_KEEPALIVE (CMake определяет,
 * когда сборка идёт под устройство и найден dbus-1); иначе — пустые
 * заглушки, чтобы host-сборка не зависела от D-Bus/MCE.
 */

#ifndef aurora_keepalive_h
#define aurora_keepalive_h

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Запретить гашение экрана. Вызывается один раз при старте игры. */
void Aurora_KeepaliveInit(void);

/* Периодический keepalive из главного цикла: пауза гашения MCE
   действует ограниченное время, поэтому запрос повторяется. */
void Aurora_KeepaliveFrame(void);

/* Приостановка/возобновление keepalive: false — игра свёрнута (сразу
   шлёт cancel, экран может гаснуть), true — развёрнута (сразу шлёт pause). */
void Aurora_KeepaliveSetActive(bool active);

/* Снять запрет гашения (вызывается при выходе). */
void Aurora_KeepaliveShutdown(void);

#ifdef __cplusplus
}
#endif

#endif
