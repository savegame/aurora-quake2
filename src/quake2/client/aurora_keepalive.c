/*
 * Запрет гашения экрана на ОС Аврора.
 *
 * Гашением экрана управляет MCE (D-Bus сервис com.nokia.mce). Вызов
 * req_display_blanking_pause откладывает гашение на ограниченное время
 * (по умолчанию ~60 с), поэтому запрос повторяется из главного цикла;
 * при выходе пауза снимается req_display_cancel_blanking_pause.
 * Подход повторяет предыдущий порт (sailfish-quake2).
 *
 * Имена сервиса/методов зашиты здесь (константы MCE_* из mce/dbus-names.h
 * пакета mce-devel), чтобы не тянуть mce-devel в сборку.
 *
 * Код собирается только при AURORA_MCE_KEEPALIVE (CMake: устройственная
 * сборка + найден dbus-1); иначе функции — пустые заглушки.
 */

#include "aurora_keepalive.h"

#if defined(AURORA_MCE_KEEPALIVE)

#include <SDL.h>
#include <stdlib.h> /* atexit */
#include <dbus/dbus.h>

/* Константы из mce/dbus-names.h (mce-devel). */
#define MCE_SERVICE                  "com.nokia.mce"
#define MCE_REQUEST_PATH             "/com/nokia/mce/request"
#define MCE_REQUEST_IF               "com.nokia.mce.request"
#define MCE_PREVENT_BLANK_REQ        "req_display_blanking_pause"
#define MCE_CANCEL_PREVENT_BLANK_REQ "req_display_cancel_blanking_pause"

/* Интервал повтора keepalive: пауза гашения по умолчанию ~60 с. */
#define KEEPALIVE_INTERVAL_MS 30000

static DBusConnection *mce_connection = NULL;
static Uint32 mce_last_call = 0;
static bool mce_active = true; /* false — игра свёрнута, keepalive на паузе */

static void MCE_CallMethod(const char *method)
{
	DBusMessage *msg;

	if (mce_connection == NULL)
		return;

	msg = dbus_message_new_method_call(MCE_SERVICE, MCE_REQUEST_PATH,
			MCE_REQUEST_IF, method);
	if (msg != NULL)
	{
		dbus_connection_send(mce_connection, msg, NULL);
		dbus_connection_flush(mce_connection);
		dbus_message_unref(msg);
	}
}

void Aurora_KeepaliveInit(void)
{
	DBusError err;

	dbus_error_init(&err);
	mce_connection = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	if (dbus_error_is_set(&err))
	{
		dbus_error_free(&err);
		mce_connection = NULL;
	}
	if (mce_connection == NULL)
		return;

	/* Общее соединение: не даём libdbus завершить процесс при обрыве шины. */
	dbus_connection_set_exit_on_disconnect(mce_connection, FALSE);

	mce_last_call = SDL_GetTicks();
	MCE_CallMethod(MCE_PREVENT_BLANK_REQ);

	/* Снятие запрета при любом штатном выходе (Com_Quit -> exit). */
	atexit(Aurora_KeepaliveShutdown);
}

void Aurora_KeepaliveFrame(void)
{
	Uint32 now;

	if (mce_connection == NULL || !mce_active)
		return;

	now = SDL_GetTicks();
	if (now - mce_last_call >= KEEPALIVE_INTERVAL_MS)
	{
		mce_last_call = now;
		MCE_CallMethod(MCE_PREVENT_BLANK_REQ);
	}
}

void Aurora_KeepaliveSetActive(bool active)
{
	if (mce_connection == NULL || active == mce_active)
		return;

	mce_active = active;
	if (active)
	{
		/* Возврат из фона: сразу продлеваем паузу гашения. */
		mce_last_call = SDL_GetTicks();
		MCE_CallMethod(MCE_PREVENT_BLANK_REQ);
	}
	else
	{
		/* Уход в фон: свёрнутая игра не должна мешать экрану гаснуть. */
		MCE_CallMethod(MCE_CANCEL_PREVENT_BLANK_REQ);
	}
}

void Aurora_KeepaliveShutdown(void)
{
	if (mce_connection == NULL)
		return;

	MCE_CallMethod(MCE_CANCEL_PREVENT_BLANK_REQ);

	/* Соединение общее (dbus_bus_get) — не закрываем, просто забываем. */
	mce_connection = NULL;
}

#else /* !AURORA_MCE_KEEPALIVE — host-сборка: заглушки */

void Aurora_KeepaliveInit(void) {}
void Aurora_KeepaliveFrame(void) {}
void Aurora_KeepaliveSetActive(bool active) {}
void Aurora_KeepaliveShutdown(void) {}

#endif
