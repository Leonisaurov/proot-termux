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

#ifndef PROC_ISOLATION_H
#define PROC_ISOLATION_H

#include "extension/extension.h"

/* Individual isolation flags — shared with proot.c CLI handlers */
#define ISOLATE_PROC      (1 << 0)
#define ISOLATE_PTRACE    (1 << 1)
#define ISOLATE_REBOOT    (1 << 2)
#define ISOLATE_SWAP      (1 << 3)
#define ISOLATE_KEXEC     (1 << 4)
#define ISOLATE_IOPORT    (1 << 5)
#define ISOLATE_BPF       (1 << 6)
#define ISOLATE_PERF      (1 << 7)
#define ISOLATE_HANDLE    (1 << 8)

typedef struct {
    unsigned int flags;
    int          proc_fd_count;
    char       **reboot_argv;
    int          reboot_argc;
} HpcConfig;

extern int hpc_callback(Extension *extension, ExtensionEvent event,
                         intptr_t data1, intptr_t data2);

/* Public accessor used by tracee/seccomp.c (SIGSYS path for statx on
 * legacy kernels where seccomp runs before the ptrace sysenter stop).
 * True when the statx path argument (CURRENT regs) resolves to a host
 * /proc path AND proc_isolation is active with ISOLATE_PROC.  Copies
 * the path into @out_path (up to @out_size bytes) when non-NULL. */
extern bool proc_isolation_statx_is_host_proc_path(Tracee *tracee,
                                                    char *out_path,
                                                    size_t out_size);

extern bool proc_isolation_is_active(Tracee *tracee);

#endif /* PROC_ISOLATION_H */
