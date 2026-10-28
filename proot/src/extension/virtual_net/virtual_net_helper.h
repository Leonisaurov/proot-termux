/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of proot-termux, a fork of PRoot.
 *
 * Copyright (C) 2025-2026 Leonisaurov
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

#ifndef VIRTUAL_NET_HELPER_H
#define VIRTUAL_NET_HELPER_H

/**
 * Helper process entry point.
 * Called from cli.c when proot is invoked with --vnp-helper NAME.
 * Runs the TCP→Unix bridge event loop for exposed ports.
 */
extern int vnp_helper_main(int argc, char *const argv[]);

#endif /* VIRTUAL_NET_HELPER_H */
