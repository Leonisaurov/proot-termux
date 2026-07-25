/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * proc_isolation — isolate /proc/, ptrace, kill, and other capabilities
 *                   from host processes using individual flag-controlled
 *                   syscall filtering.
 *
 * Provides fine-grained isolation flags:
 *   ISOLATE_PROC     — /proc/ only shows proot-owned PIDs
 *   ISOLATE_PTRACE   — ptrace() to host PIDs returns ESRCH
 *   ISOLATE_REBOOT   — reboot() returns 0 (no-op)
 *   ISOLATE_SWAP     — swapon/swapoff returns ENOSYS
 *   ISOLATE_KEXEC    — kexec_load returns 0 (no-op)
 *   ISOLATE_IOPORT   — iopl/ioperm returns 0 (no-op)
 *   ISOLATE_BPF      — bpf() returns ENOSYS
 *   ISOLATE_PERF     — perf_event_open returns ENOENT
 *   ISOLATE_HANDLE   — open_by_handle_at returns EOPNOTSUPP
 *
 * Copyright (C) 2025 Licensed under GPL v2 or later.
 */

#include <sys/types.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <talloc.h>
#include <linux/limits.h>
#include <stdint.h>

#include "extension/proc_isolation/proc_isolation.h"
#include "extension/extension.h"
#include "tracee/tracee.h"
#include "tracee/reg.h"
#include "tracee/mem.h"
#include "syscall/sysnum.h"
#include "syscall/seccomp.h"
#include "syscall/syscall.h"
#include "path/path.h"
#include "cli/note.h"

#define HPC_MAX_BUF 4096

/* ================================================================
 * Filtered sysnums — ensures seccomp intercepts these syscalls.
 * Installed once at init; cannot be changed after seccomp attach.
 * Always includes all syscalls regardless of which flags are set.
 * ================================================================ */

static FilteredSysnum hpc_filtered_sysnums[] = {
    { PR_getdents64,        FILTER_SYSEXIT },
    { PR_getdents,          FILTER_SYSEXIT },
    { PR_ptrace,            0 },
    { PR_kill,              0 },
    { PR_tkill,             0 },
    { PR_tgkill,            0 },
    { PR_reboot,            0 },
    { PR_swapon,            0 },
    { PR_swapoff,           0 },
    { PR_kexec_load,        0 },
    { PR_iopl,              0 },
    { PR_ioperm,            0 },
    { PR_bpf,               0 },
    { PR_perf_event_open,   0 },
    { PR_open_by_handle_at, 0 },
    FILTERED_SYSNUM_END,
};

/* ================================================================
 * Helpers
 * ================================================================ */

static bool hpc_is_numeric(const char *name)
{
    int i;
    if (name[0] == '\0')
        return false;
    for (i = 0; name[i] != '\0'; i++) {
        if (name[i] < '0' || name[i] > '9')
            return false;
    }
    return true;
}

static bool hpc_is_proot_pid(pid_t pid)
{
    Tracees *list = get_tracees_list_head();
    if (list == NULL)
        return false;

    Tracee *t;
    LIST_FOREACH(t, list, link) {
        if (t->pid == pid)
            return true;
    }
    return false;
}

/* ================================================================
 * SYSCALL_ENTER_START — ptrace interception (before built-in handler)
 * ================================================================ */

static int hpc_handle_ptrace_enter(Tracee *tracee)
{
    pid_t target_pid;

    target_pid = (pid_t)peek_reg(tracee, CURRENT, SYSARG_2);

    /* PTRACE_TRACEME (pid=0) always allowed */
    if (target_pid == 0)
        return 0;

    /* Allow if it's a proot process */
    if (hpc_is_proot_pid(target_pid))
        return 0;

    /* Host process: ESRCH */
    set_sysnum(tracee, PR_void);
    poke_reg(tracee, SYSARG_RESULT, -ESRCH);
    VERBOSE(tracee, 2, "proc_isolation: blocked ptrace to host pid %d", target_pid);
    return 1;
}

/* ================================================================
 * SYSCALL_ENTER_END — kill interception + all other void/block handlers
 * ================================================================ */

static int hpc_handle_kill_enter(Tracee *tracee, int pid_reg)
{
    pid_t target_pid;

    target_pid = (pid_t)peek_reg(tracee, CURRENT, pid_reg);

    /* kill(0, ...) means self process group — allow */
    if (target_pid <= 0)
        return 0;

    if (hpc_is_proot_pid(target_pid))
        return 0;

    set_sysnum(tracee, PR_void);
    poke_reg(tracee, SYSARG_RESULT, -ESRCH);
    VERBOSE(tracee, 2, "proc_isolation: blocked kill(%d) to host process", target_pid);
    return 0;
}

/* ================================================================
 * SYSCALL_EXIT_END — getdents64/ getdents filtering
 * ================================================================ */

static int hpc_handle_getdents_exit(Tracee *tracee, Sysnum num)
{
    word_t result, fd, buf;
    char proc_path[PATH_MAX];
    int status;

    result = peek_reg(tracee, CURRENT, SYSARG_RESULT);
    if ((int)result <= 0)
        return 0;

    /* Use ORIGINAL for fd: on ARM64 CURRENT SYSARG_1 == return value */
    fd = peek_reg(tracee, ORIGINAL, SYSARG_1);

    /* Check if this fd points to /proc/ root */
    status = readlink_proc_pid_fd(tracee->pid, (int)fd, proc_path);
    if (status < 0)
        return 0;

    /* Only filter /proc/ root itself */
    if (strcmp(proc_path, "/proc") != 0 && strcmp(proc_path, "/proc/") != 0)
        return 0;

    buf = peek_reg(tracee, CURRENT, SYSARG_2);

    if ((int)result > HPC_MAX_BUF)
        return 0;

    char data[HPC_MAX_BUF];
    if (read_data(tracee, data, buf, result) < 0)
        return 0;

    int nleft = 0;
    char *ptr = data;
    int remaining = (int)result;

    while (remaining > 0) {
        unsigned short reclen;
        char *d_name;

        if (num == PR_getdents64) {
            struct linux_dirent64 {
                unsigned long long  d_ino;
                long long           d_off;
                unsigned short      d_reclen;
                unsigned char       d_type;
                char                d_name[];
            } *dirent = (void *)ptr;
            reclen = dirent->d_reclen;
            d_name = dirent->d_name;
        }
        else {
            struct linux_dirent {
                unsigned long   d_ino;
                unsigned long   d_off;
                unsigned short  d_reclen;
                char            d_name[];
            } *dirent = (void *)ptr;
            reclen = dirent->d_reclen;
            d_name = dirent->d_name;
        }

        if (reclen == 0 || reclen > (unsigned short)remaining)
            break;

        bool is_numeric = hpc_is_numeric(d_name);
        pid_t entry_pid = is_numeric ? (pid_t)atoi(d_name) : 0;
        bool keep = !is_numeric || hpc_is_proot_pid(entry_pid);

        if (keep) {
            if (ptr != data + nleft)
                memmove(data + nleft, ptr, reclen);
            nleft += reclen;
        }

        ptr += reclen;
        remaining -= reclen;
    }

    if (nleft < (int)result) {
        if (nleft > 0)
            write_data(tracee, buf, data, nleft);
        poke_reg(tracee, SYSARG_RESULT, (word_t)nleft);
        VERBOSE(tracee, 3, "proc_isolation: filtered getdents %d -> %d bytes",
            (int)result, nleft);
    }

    return 0;
}

/* ================================================================
 * Callback
 * ================================================================ */

int hpc_callback(Extension *extension, ExtensionEvent event,
          intptr_t data1, intptr_t data2 UNUSED)
{
    switch (event) {
    case INITIALIZATION: {
        unsigned int init_flags = (unsigned int)(uintptr_t)data1;
        HpcConfig *config = talloc_zero(extension, HpcConfig);
        if (config == NULL)
            return -ENOMEM;
        config->flags = init_flags;
        config->proc_fd_count = 0;
        extension->config = config;
        extension->filtered_sysnums = hpc_filtered_sysnums;
        return 0;
    }

    case SYSCALL_ENTER_START: {
        Tracee *tracee = TRACEE(extension);
        HpcConfig *config = (HpcConfig *)extension->config;

        if (config && (config->flags & ISOLATE_PTRACE)
            && get_sysnum(tracee, CURRENT) == PR_ptrace)
            return hpc_handle_ptrace_enter(tracee);
        return 0;
    }

    case SYSCALL_ENTER_END: {
        Tracee *tracee = TRACEE(extension);
        HpcConfig *config = (HpcConfig *)extension->config;
        Sysnum num;

        if (config == NULL)
            return 0;

        num = get_sysnum(tracee, CURRENT);
        switch (num) {
        case PR_kill:
            if (config->flags & ISOLATE_PROC)
                return hpc_handle_kill_enter(tracee, SYSARG_1);
            return 0;
        case PR_tkill:
            if (config->flags & ISOLATE_PROC)
                return hpc_handle_kill_enter(tracee, SYSARG_1);
            return 0;
        case PR_tgkill:
            if (config->flags & ISOLATE_PROC)
                return hpc_handle_kill_enter(tracee, SYSARG_2);
            return 0;
        case PR_reboot:
            if (config->flags & ISOLATE_REBOOT) {
                Tracees *list = get_tracees_list_head();
                if (list != NULL) {
                    Tracee *t, *tmp;
                    int count = 0;
                    t = LIST_FIRST(list);
                    while (t != NULL) {
                        tmp = LIST_NEXT(t, link);
                        if (t->pid != tracee->pid) {
                            kill(t->pid, SIGKILL);
                            count++;
                        }
                        t = tmp;
                    }
                    VERBOSE(tracee, 1, "proc_isolation: reboot killed %d processes", count);
                }
                set_sysnum(tracee, PR_void);
                poke_reg(tracee, SYSARG_RESULT, 0);
                kill(tracee->pid, SIGKILL);
            }
            return 0;
        case PR_swapon:
        case PR_swapoff:
            if (config->flags & ISOLATE_SWAP) {
                set_sysnum(tracee, PR_void);
                poke_reg(tracee, SYSARG_RESULT, -ENOSYS);
                VERBOSE(tracee, 1, "proc_isolation: swap blocked (ENOSYS)");
            }
            return 0;
        case PR_kexec_load:
            if (config->flags & ISOLATE_KEXEC) {
                set_sysnum(tracee, PR_void);
                poke_reg(tracee, SYSARG_RESULT, 0);
                VERBOSE(tracee, 1, "proc_isolation: kexec_load voided");
            }
            return 0;
        case PR_iopl:
        case PR_ioperm:
            if (config->flags & ISOLATE_IOPORT) {
                set_sysnum(tracee, PR_void);
                poke_reg(tracee, SYSARG_RESULT, 0);
                VERBOSE(tracee, 1, "proc_isolation: ioport voided");
            }
            return 0;
        case PR_bpf:
            if (config->flags & ISOLATE_BPF) {
                set_sysnum(tracee, PR_void);
                poke_reg(tracee, SYSARG_RESULT, -ENOSYS);
                VERBOSE(tracee, 1, "proc_isolation: bpf blocked (ENOSYS)");
            }
            return 0;
        case PR_perf_event_open:
            if (config->flags & ISOLATE_PERF) {
                set_sysnum(tracee, PR_void);
                poke_reg(tracee, SYSARG_RESULT, -ENOENT);
                VERBOSE(tracee, 1, "proc_isolation: perf_event_open blocked (ENOENT)");
            }
            return 0;
        case PR_open_by_handle_at:
            if (config->flags & ISOLATE_HANDLE) {
                set_sysnum(tracee, PR_void);
                poke_reg(tracee, SYSARG_RESULT, -EOPNOTSUPP);
                VERBOSE(tracee, 1, "proc_isolation: open_by_handle_at blocked (EOPNOTSUPP)");
            }
            return 0;
        default:
            return 0;
        }
    }

    case SYSCALL_EXIT_END: {
        Tracee *tracee = TRACEE(extension);
        HpcConfig *config = (HpcConfig *)extension->config;

        if (config == NULL || !(config->flags & ISOLATE_PROC))
            return 0;

        switch (get_sysnum(tracee, ORIGINAL)) {
        case PR_getdents64:
            return hpc_handle_getdents_exit(tracee, PR_getdents64);
        case PR_getdents:
            return hpc_handle_getdents_exit(tracee, PR_getdents);
        default:
            return 0;
        }
    }

    case REMOVED:
        return 0;

    default:
        return 0;
    }
}
