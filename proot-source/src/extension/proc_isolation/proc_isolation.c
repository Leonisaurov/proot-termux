/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * proc_isolation — isolate /proc/, ptrace, kill, and other capabilities
 *                   from host processes using individual flag-controlled
 *                   syscall filtering.
 *
 * Provides fine-grained isolation flags:
 *   ISOLATE_PROC     — /proc/ only shows proot-owned PIDs
 *   ISOLATE_PTRACE   — ptrace() to host PIDs returns ESRCH
 *   ISOLATE_REBOOT   — reboot() re-exec's proot (real reboot)
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
#include <fcntl.h>
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
#include "tracee/seccomp.h"
#include "path/path.h"
#include "cli/note.h"

#define HPC_MAX_BUF 4096

/* ================================================================
 * Flag-to-sysnum mapping — filtered_sysnums is built dynamically
 * from this table based on which isolation flags are active.
 * ================================================================ */

typedef struct {
    unsigned int flag;       /* ISOLATE_* flag bit */
    int sysnums[12];         /* Syscall numbers, -1 terminated */
} FlagSysnumMap;

static const FlagSysnumMap flag_sysnum_map[] = {
    { ISOLATE_PROC,   { PR_getdents64, PR_getdents, PR_kill, PR_tkill, PR_tgkill,
                        PR_openat, PR_unshare, PR_mount, -1 } },
    { ISOLATE_PTRACE, { PR_ptrace, PR_process_vm_readv, PR_process_vm_writev, -1 } },
    { ISOLATE_REBOOT, { PR_reboot, -1 } },
    { ISOLATE_SWAP,   { PR_swapon, PR_swapoff, -1 } },
    { ISOLATE_KEXEC,  { PR_kexec_load, -1 } },
    { ISOLATE_IOPORT, { PR_iopl, PR_ioperm, -1 } },
    { ISOLATE_BPF,    { PR_bpf, -1 } },
    { ISOLATE_PERF,   { PR_perf_event_open, -1 } },
    { ISOLATE_HANDLE, { PR_open_by_handle_at, -1 } },
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

        /* Build filtered_sysnums dynamically based on active flags.
         * Only syscalls for active isolation flags are included,
         * reducing seccomp traps when few flags are enabled. */
        int count = 0;
        unsigned int i;
        for (i = 0; i < sizeof(flag_sysnum_map) / sizeof(flag_sysnum_map[0]); i++) {
            if (init_flags & flag_sysnum_map[i].flag) {
                int j;
                for (j = 0; flag_sysnum_map[i].sysnums[j] != -1; j++)
                    count++;
            }
        }

        FilteredSysnum *dynamic_sysnums = talloc_array(extension, FilteredSysnum, count + 1);
        if (dynamic_sysnums == NULL)
            return -ENOMEM;

        int idx = 0;
        for (i = 0; i < sizeof(flag_sysnum_map) / sizeof(flag_sysnum_map[0]); i++) {
            if (init_flags & flag_sysnum_map[i].flag) {
                int j;
                for (j = 0; flag_sysnum_map[i].sysnums[j] != -1; j++) {
                    int sysnum = flag_sysnum_map[i].sysnums[j];
                    word_t flags = 0;
                    /* PR_getdents64 and PR_getdents need FILTER_SYSEXIT
                     * (filter at exit for /proc/ PID filtering) */
                    if (flag_sysnum_map[i].flag == ISOLATE_PROC &&
                        (sysnum == PR_getdents64 || sysnum == PR_getdents))
                        flags = FILTER_SYSEXIT;
                    dynamic_sysnums[idx].value = (Sysnum)sysnum;
                    dynamic_sysnums[idx].flags = flags;
                    idx++;
                }
            }
        }
        /* Terminate the array */
        dynamic_sysnums[idx].value = PR_void;
        dynamic_sysnums[idx].flags = 0;

        extension->filtered_sysnums = dynamic_sysnums;
        return 0;
    }

    case SYSCALL_ENTER_START: {
        Tracee *tracee = TRACEE(extension);
        HpcConfig *config = (HpcConfig *)extension->config;
        Sysnum num;

        if (config == NULL)
            return 0;

        num = get_sysnum(tracee, CURRENT);

        if ((config->flags & ISOLATE_PTRACE) && num == PR_ptrace)
            return hpc_handle_ptrace_enter(tracee);

        if ((config->flags & ISOLATE_PROC) && num == PR_openat) {
            char path[64];
            word_t path_addr = peek_reg(tracee, CURRENT, SYSARG_2);
            if (path_addr != 0 && read_data(tracee, path, path_addr, sizeof(path)) >= 0) {
                path[sizeof(path) - 1] = '\0';
                if (strcmp(path, "/proc/cpuinfo") == 0 ||
                    strcmp(path, "/proc/meminfo") == 0 ||
                    strcmp(path, "/proc/self/mountinfo") == 0 ||
                    strcmp(path, "/proc/self/environ") == 0) {
                    VERBOSE(tracee, 2, "proc_isolation: blocked %s (ENOENT)", path);
                    set_sysnum(tracee, PR_void);
                    poke_reg(tracee, SYSARG_RESULT, -ENOENT);
                    return 1;
                }
            }
        }

        if (config->flags & ISOLATE_PROC) {
            if (num == PR_unshare) {
                set_sysnum(tracee, PR_void);
                poke_reg(tracee, SYSARG_RESULT, -ENOSYS);
                VERBOSE(tracee, 2, "proc_isolation: unshare blocked (ENOSYS)");
                return 1;
            }
            if (num == PR_mount) {
                set_sysnum(tracee, PR_void);
                poke_reg(tracee, SYSARG_RESULT, -ENOSYS);
                VERBOSE(tracee, 2, "proc_isolation: mount blocked (ENOSYS)");
                return 1;
            }
            if (num == PR_socket) {
                word_t domain = peek_reg(tracee, CURRENT, SYSARG_1);
                if (domain == AF_NETLINK) {
                    set_sysnum(tracee, PR_void);
                    poke_reg(tracee, SYSARG_RESULT, -EACCES);
                    VERBOSE(tracee, 2, "proc_isolation: netlink socket blocked");
                    return 1;
                }
            }
        }

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
                /* Kill all other tracees */
                Tracees *list = get_tracees_list_head();
                if (list != NULL) {
                    Tracee *t;
                    LIST_FOREACH(t, list, link) {
                        if (t->pid != tracee->pid)
                            kill(t->pid, SIGKILL);
                    }
                }
                /* Void syscall — caller sees success */
                set_sysnum(tracee, PR_void);
                poke_reg(tracee, SYSARG_RESULT, 0);
                VERBOSE(tracee, 1, "proc_isolation: reboot initiated");

                /* Re-exec proot with same arguments (restart sandbox) */
                {
                    char buf[4096];
                    int fd = open("/proc/self/cmdline", O_RDONLY);
                    if (fd >= 0) {
                        ssize_t n = read(fd, buf, sizeof(buf) - 1);
                        close(fd);
                        if (n > 0) {
                            char *argv[128];
                            int argc = 0;
                            char *p = buf;
                            buf[n] = '\0';
                            while (p < buf + n && argc < 126) {
                                argv[argc++] = p;
                                p += strlen(p) + 1;
                            }
                            argv[argc] = NULL;
                            /* This replaces the proot process */
                            execvp(argv[0], argv);
                        }
                    }
                }
                /* If exec fails, just exit */
                _exit(0);
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

        case PR_process_vm_readv:
        case PR_process_vm_writev:
            if (config->flags & ISOLATE_PTRACE) {
                pid_t target_pid = (pid_t)peek_reg(tracee, CURRENT, SYSARG_1);
                if (target_pid > 0 && !hpc_is_proot_pid(target_pid)) {
                    set_sysnum(tracee, PR_void);
                    poke_reg(tracee, SYSARG_RESULT, -ESRCH);
                    VERBOSE(tracee, 2, "proc_isolation: blocked process_vm to host pid %d", target_pid);
                }
            }
            return 0;

        default:
            return 0;
        }
    }

    case SYSCALL_EXIT_START: {
        Tracee *tracee = TRACEE(extension);
        HpcConfig *config = (HpcConfig *)extension->config;
        Sysnum num;

        if (config == NULL)
            return 0;

        num = get_sysnum(tracee, ORIGINAL);

        /* The built-in exit handler overwrites PR_unshare/PR_mount
         * result to 0 (see exit.c).  Skip it entirely to preserve
         * our -ENOSYS result from SYSCALL_ENTER_START.  */
        if (config->flags & ISOLATE_PROC) {
            if (num == PR_unshare || num == PR_mount) {
                word_t modified_sysnum = peek_reg(tracee, MODIFIED, SYSARG_NUM);
                word_t original_sysnum = peek_reg(tracee, ORIGINAL, SYSARG_NUM);
                if (modified_sysnum == SYSCALL_AVOIDER && modified_sysnum != original_sysnum) {
                    poke_reg(tracee, SYSARG_RESULT, -ENOSYS);
                    VERBOSE(tracee, 3, "proc_isolation: skipping exit handler for %s",
                        num == PR_unshare ? "unshare" : "mount");
                    return 1;
                }
            }
        }

        return 0;
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

    case SIGSYS_OCC: {
        Tracee *tracee = TRACEE(extension);
        HpcConfig *config = (HpcConfig *)extension->config;
        Sysnum num;

        if (config == NULL)
            return 0;

        num = get_sysnum(tracee, CURRENT);

        if ((config->flags & ISOLATE_PROC) && num == PR_mount) {
            set_result_after_seccomp(tracee, -ENOSYS);
            VERBOSE(tracee, 2, "proc_isolation: mount blocked via SIGSYS (ENOSYS)");
            return 2;
        }

        if ((config->flags & ISOLATE_PROC) && num == PR_unshare) {
            set_result_after_seccomp(tracee, -ENOSYS);
            VERBOSE(tracee, 2, "proc_isolation: unshare blocked via SIGSYS (ENOSYS)");
            return 2;
        }

        if ((config->flags & ISOLATE_PROC) && num == PR_socket) {
            word_t domain = peek_reg(tracee, CURRENT, SYSARG_1);
            if (domain == AF_NETLINK) {
                set_result_after_seccomp(tracee, -EACCES);
                VERBOSE(tracee, 2, "proc_isolation: netlink socket blocked via SIGSYS (EACCES)");
                return 2;
            }
        }

        return 0;
    }

    case REMOVED:
        return 0;

    default:
        return 0;
    }
}
