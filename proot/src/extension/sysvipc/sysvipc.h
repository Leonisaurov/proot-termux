/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of proot-termux, a fork of PRoot.
 *
 * Upstream: termux/proot (GPL v2 or later)
 * Fork modifications: Copyright (C) 2025-2026 Leonisaurov
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef SYSVIPC_H
#define SYSVIPC_H

#include "extension/extension.h"

int sysvipc_callback(Extension *extension, ExtensionEvent event, intptr_t data1, intptr_t data2);
void sysvipc_shm_helper_main() __attribute__((noreturn));

#endif // SYSVIPC_H
