/*
 * Copyright (C) 1997-2001 Id Software, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * =======================================================================
 *
 * Header file for the generic input backend.
 *
 * =======================================================================
 */

#ifndef GEN_INPUT_H
#define GEN_INPUT_H

#include "common/shared/shared.h"

/*
 * Initializes the input backend
 */
void IN_Init();

/*
 * Move handling
 */
void IN_Move(usercmd_t *cmd);

/*
 * Shuts the backend down
 */
void IN_Shutdown();

/*
 * Updates the state of the input queue
 */
void IN_Update();

/*
 * Тач-инъекции (ОС Аврора): накопление дельты осмотра (как движение
 * мыши, применяется в IN_Move) и аналоговый вектор движения стика
 * (x — страйф вправо, y — вперёд; оба -1..1, 0 — нет ввода).
 */
void IN_AddTouchLook(float dx, float dy);
void IN_SetTouchStick(float x, float y);

#if defined(AURORA_OS)
/* Подключён ли геймпад (открыт как SDL_GameController, input_sdl.c). */
extern qboolean aurora_gamepad_present;
#endif

#endif
