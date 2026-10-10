/*
 * Copyright (c) 2023 Ningyuan Li <https://github.com/mariotaku>.
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once
#include "Limelight.h"

extern AUDIO_RENDERER_CALLBACKS ss4s_aud_callbacks;


#include <stdbool.h>
#include <ss4s.h>
struct session_t;
/** Prepare qualified PCM output before host connection can start a secondary.
 * False leaves normal audio init selected. Never called for a secondary process. */
bool session_audio_prepare_coop_output(struct session_t *context);
/** Safe after normal cleanup and on connection-init failure; joins before player close. */
void session_audio_finish_output(SS4S_Player *player);
