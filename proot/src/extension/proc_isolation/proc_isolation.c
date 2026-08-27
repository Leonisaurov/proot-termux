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
#include <limits.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <limits.h>

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
#include "path/temp.h"
#include "cli/note.h"

/* ================================================================
 * Flag-to-sysnum mapping — filtered_sysnums is built dynamically
 * from this table based on which isolation flags are active.
 * ================================================================ */

typedef struct {
    unsigned int flag;       /* ISOLATE_* flag bit */
    int sysnums[32];         /* Syscall numbers, -1 terminated */
} FlagSysnumMap;

static const FlagSysnumMap flag_sysnum_map[] = {
    { ISOLATE_PROC,   { PR_getdents64, PR_getdents, PR_kill, PR_tkill, PR_tgkill,
                        PR_open, PR_openat, PR_openat2, PR_read,
                        PR_pread64, PR_close, PR_dup, PR_dup2, PR_dup3, PR_fcntl,
                        PR_readv, PR_preadv, PR_preadv2,
                        PR_stat, PR_lstat, PR_stat64, PR_lstat64,
                        PR_fstatat64, PR_newfstatat, PR_statx,
                        PR_readlink, PR_readlinkat,
                        PR_pidfd_open, -1 } },  /* E5: moved from ISOLATE_PTRACE */
    { ISOLATE_PTRACE, { PR_ptrace, PR_process_vm_readv, PR_process_vm_writev,
                         -1 } },
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

/* Normalize only the lexical spelling of a guest path.  This deliberately
 * does not consult the host filesystem: proc paths must be classified before
 * proot translates them, and a host lookup here would reintroduce the leak we
 * are trying to prevent. */
static bool hpc_normalize_path(const char *path, char *out, size_t size)
{
	char work[PATH_MAX];
	const char *p, *start;
	char *q;
	char *end = out + size;
	bool needs_normalization = false;

	if (path == NULL || out == NULL || size < 2 || path[0] != '/')
		return false;
	if (strnlen(path, sizeof(work)) >= sizeof(work))
		return false;
	/* Most proc paths are already canonical.  Keep this common case to a
	 * single bounded copy and avoid the component bookkeeping below. */
	for (p = path; *p != '\0'; p++) {
		if (*p == '.')
			needs_normalization = true;
		if (*p == '/' && p[1] == '/')
			needs_normalization = true;
	}
	if (!needs_normalization) {
		if (strlen(path) >= size)
			return false;
		memcpy(out, path, (size_t)(p - path) + 1);
		return true;
	}
	strcpy(work, path);
	p = work;
	q = out;
	if (q + 1 >= end)
		return false;
	*q++ = '/';
	while (*p != '\0') {
		size_t len;
		while (*p == '/') p++;
		if (*p == '\0') break;
		start = p;
		while (*p != '\0' && *p != '/') p++;
		len = (size_t)(p - start);
		if (len == 1 && start[0] == '.')
			continue;
		if (len == 2 && start[0] == '.' && start[1] == '.') {
			if (q > out + 1) {
				q--;
				while (q > out + 1 && q[-1] != '/')
					q--;
			}
			continue;
		}
		if (q + len + 1 >= end)
			return false;
		if (q > out + 1 && q[-1] != '/')
			*q++ = '/';
		memcpy(q, start, len);
		q += len;
	}
	*q = '\0';
	return true;
}

static bool hpc_proc_dirfd_path(Tracee *tracee, int dirfd, char *out,
				 size_t size)
{
	char link[PATH_MAX];
	int n;
	if (dirfd == AT_FDCWD)
		return false;
	n = readlink_proc_pid_fd(tracee->pid, dirfd, link);
	if (n < 0 || (size_t)n >= size)
		return false;
	link[n] = '\0';
	return hpc_normalize_path(link, out, size);
}

/* Resolve a syscall path without consulting the host filesystem.  Absolute
 * paths are normalized lexically; relative paths are resolved only when the
 * supplied dirfd is a proc directory. */
static bool hpc_resolve_syscall_path(Tracee *tracee, RegVersion version,
					Reg path_reg, int dirfd, char *out, size_t size)
{
	char raw[PATH_MAX];
	char base[PATH_MAX];
	char joined[PATH_MAX];
	word_t addr;

	addr = peek_reg(tracee, version, path_reg);
	if (addr == 0 || read_string(tracee, raw, addr, sizeof(raw) - 1) < 0)
		return false;
	raw[sizeof(raw) - 1] = '\0';
	if (raw[0] == '/')
		return hpc_normalize_path(raw, out, size);
	if (!hpc_proc_dirfd_path(tracee, dirfd, base, sizeof(base)))
		return false;
	if (strcmp(base, "/proc") != 0 && strncmp(base, "/proc/", 6) != 0)
		return false;
	if (snprintf(joined, sizeof(joined), "%s/%s", base, raw) >= (int)sizeof(joined))
		return false;
	return hpc_normalize_path(joined, out, size);
}

/* Return a canonical absolute guest path for open/openat/openat2. */
static bool hpc_open_proc_path(Tracee *tracee, Sysnum num, char *out,
				       size_t size)
{
	Reg path_reg = (num == PR_open) ? SYSARG_1 : SYSARG_2;
	int dirfd = (num == PR_open) ? AT_FDCWD :
		(int)peek_reg(tracee, CURRENT, SYSARG_1);
	return hpc_resolve_syscall_path(tracee, CURRENT, path_reg, dirfd,
					out, size);
}

static bool hpc_is_proot_pid(pid_t pid)
{
	return tracee_is_tracked(pid);
}

/* ================================================================
 * /proc/ direct-path helpers — close the information leak that the
 * getdents listing filter cannot: opening "/proc/123/status" or
 * "/proc/net/tcp" reads host data unconditionally, without ever
 * listing the directory.
 *
 * Paths are normalized lexically before these checks, including repeated
 * separators and dot components.  Relative paths are resolved against a
 * tracked /proc dirfd when the syscall supplies one; unresolved relative
 * paths are left to the normal kernel/PRoot path handling and are never
 * classified as safe proc paths here.
 * ================================================================ */

/* Extract the numeric pid from a "/proc/<pid>[/...]" path.  Returns
 * false when @path is not a direct pid path (e.g. "self",
 * "thread-self", "net", a plain file such as "cpuinfo", or a path
 * outside /proc).  When true, *pid_out holds the pid. */
static bool hpc_proc_path_pid(const char *path, pid_t *pid_out)
{
    const char *p = path + 6;   /* after "/proc/" */
    char *end;
    long pid;

    if (strncmp(path, "/proc/", 6) != 0)
        return false;

    errno = 0;
    pid = strtol(p, &end, 10);
    if (end == p || (*end != '\0' && *end != '/'))
        return false;   /* non-numeric component */

    /* An out-of-range pid is either rejected by strtol (ERANGE) or
     * does not exist, in which case the kernel returns ENOENT on its
     * own.  pid > INT32_MAX is only a cast safeguard for pid_t. */
    if (errno == ERANGE || pid <= 0 || pid > INT32_MAX)
        return false;

    *pid_out = (pid_t)pid;
    return true;
}

/* True when opening @path would leak a host resource that the
 * getdents filter cannot hide: a file under /proc/net/ (host TCP/UDP
 * connections, routes, arp cache, ...), a /proc/<pid>/... path of a
 * process that is not a tracee of this proot instance, or the magic
 * "net" directory of any process.  The kernel resolves the "net"
 * component in the HOST's network namespace, so even
 * /proc/self/net/ and /proc/<tracee-pid>/net/ leak host data.
 * Returns false (allow) for /proc/self, /proc/thread-self and every
 * other non-numeric entry, which resolve to the tracee's own files
 * (their "net" children are caught above). */
static bool hpc_is_host_proc_path(const char *path)
{
	char normalized[PATH_MAX];
	pid_t pid;
	if (path == NULL || !hpc_normalize_path(path, normalized, sizeof(normalized)))
		return false;
	path = normalized;

    /* /proc/net/ exposes the host's network stack.  Children only:
     * /proc/net itself is left alone. */
    if (strncmp(path, "/proc/net/", 9) == 0)
        return true;

    /* The "net" magic symlink of any process resolves to the host's
     * network namespace, and /proc/sys/net/ holds the host's network
     * sysctls.  Block their children unconditionally. */
    if (strncmp(path, "/proc/self/net/", 15) == 0
        || strncmp(path, "/proc/thread-self/net/", 22) == 0
        || strncmp(path, "/proc/sys/net/", 14) == 0)
        return true;

    if (!hpc_proc_path_pid(path, &pid))
        return false;

    /* /proc/<pid>/net/ leaks the host netns for tracee pids too, so
     * it is always blocked regardless of sandbox membership. */
    {
        const char *next = strchr(path + 6, '/');
        if (next != NULL && next[1] == 'n' && next[2] == 'e'
            && next[3] == 't' && (next[4] == '/' || next[4] == '\0'))
            return true;
    }

    /* Tracee pids belong to the sandbox; every other numeric pid is a
     * host process that must not be reachable by path. */
    return !hpc_is_proot_pid(pid);
}

static bool hpc_is_allowed_proc_root_name(const char *name)
{
	/* Keep only entries for which this extension supplies a guest-safe view.
	 * In particular, do not preserve arbitrary non-numeric Android procfs
	 * directories merely because they are not PIDs. */
	static const char *const allowed[] = {
		".", "..", "self", "thread-self", "cpuinfo", "meminfo",
		"stat", "uptime", "loadavg", "mounts", "mountinfo", NULL
	};
	for (int i = 0; allowed[i] != NULL; i++)
		if (strcmp(name, allowed[i]) == 0)
			return true;
	return false;
}

static bool hpc_sensitive_android_prefix(const char *path)
{
	static const char *const prefixes[] = {
		"/system/", "/vendor/", "/apex/", "/dev/block/", NULL
	};
	for (int i = 0; prefixes[i] != NULL; i++)
		if (strncmp(path, prefixes[i], strlen(prefixes[i])) == 0)
			return true;
	return false;
}

#define PROC_SYNTH_NONE 0
static int hpc_proc_synth_kind(const char *path);

static bool hpc_is_task_maps_suffix(const char *path)
{
	const char *slash;
	char tid[24];
	size_t len;

	if (path == NULL || strncmp(path, "task/", 5) != 0)
		return false;
	path += 5;
	slash = strchr(path, '/');
	if (slash == NULL || strcmp(slash, "/maps") != 0)
		return false;
	len = (size_t)(slash - path);
	if (len == 0 || len >= sizeof(tid))
		return false;
	memcpy(tid, path, len);
	tid[len] = '\0';
	return hpc_is_numeric(tid);
}

static bool hpc_is_self_task_maps_path(const char *path)
{
	return path != NULL &&
	       ((strncmp(path, "/proc/self/", 11) == 0 &&
	         hpc_is_task_maps_suffix(path + 11)) ||
	        (strncmp(path, "/proc/thread-self/", 18) == 0 &&
	         hpc_is_task_maps_suffix(path + 18)));
}

static bool hpc_is_guest_proc_path(const char *path)
{
	const char *p;
	const char *task;
	if (strcmp(path, "/proc") == 0 || strcmp(path, "/proc/") == 0)
		return true;
	if (strncmp(path, "/proc/", 6) != 0)
		return false;
	p = path + 6;
	if (strcmp(p, "self") == 0 || strcmp(p, "thread-self") == 0)
		return true;
	if (hpc_is_allowed_proc_root_name(p))
		return true;
	/* Thread runtimes commonly inspect /proc/self/task/<tid>/maps rather
	 * than /proc/self/maps.  The tid is still inside the current guest
	 * process, so allow this maps view while keeping all other task files
	 * subject to the normal synthetic/blocking rules. */
	if (strncmp(p, "self/task/", 10) == 0 ||
	    strncmp(p, "thread-self/task/", 17) == 0) {
		task = p + (p[0] == 's' ? 5 : 12);
		return hpc_is_task_maps_suffix(task);
	}
	{
		const char *slash = strchr(p, '/');
		char pidbuf[24];
		if (slash != NULL && (size_t)(slash - p) < sizeof(pidbuf)) {
			memcpy(pidbuf, p, (size_t)(slash - p));
			pidbuf[slash - p] = '\0';
			if (hpc_is_numeric(pidbuf) &&
			    hpc_is_proot_pid((pid_t)strtol(pidbuf, NULL, 10))) {
				const char *file = slash + 1;
				return hpc_proc_synth_kind(path) != PROC_SYNTH_NONE
					|| strcmp(file, "maps") == 0
					|| hpc_is_task_maps_suffix(file)
					|| strcmp(file, "fd") == 0
					|| strcmp(file, "task") == 0;
			}
		}
	}
	if (strncmp(p, "self/", 5) == 0 || strncmp(p, "thread-self/", 12) == 0) {
		const char *file = strchr(p, '/') + 1;
		return hpc_proc_synth_kind(path) != PROC_SYNTH_NONE
			|| strcmp(file, "maps") == 0
			|| strcmp(file, "exe") == 0
			|| strcmp(file, "fd") == 0 || strcmp(file, "task") == 0;
	}
	if (hpc_is_numeric(p))
		return true;
	return false;
}

enum {
	PROC_SYNTH_CPUINFO,
	PROC_SYNTH_MEMINFO,
	PROC_SYNTH_STAT,
	PROC_SYNTH_UPTIME,
	PROC_SYNTH_LOADAVG,
	PROC_SYNTH_STATUS,
	PROC_SYNTH_STAT_ONE,
	PROC_SYNTH_STATM,
	PROC_SYNTH_CMDLINE,
	PROC_SYNTH_COMM,
	PROC_SYNTH_LIMITS,
	PROC_SYNTH_IO,
	PROC_SYNTH_SCHED,
	PROC_SYNTH_OOM_SCORE,
	PROC_SYNTH_WCHAN,
	PROC_SYNTH_SYSCALL,
	PROC_SYNTH_ATTR,
	PROC_SYNTH_MOUNTINFO,
};

static int hpc_proc_synth_kind(const char *path)
{
	char normalized[PATH_MAX];
	const char *p;
	bool per_process = false;
	if (path == NULL || !hpc_normalize_path(path, normalized, sizeof(normalized)))
		return PROC_SYNTH_NONE;
	p = normalized;
	if (strncmp(p, "/proc/", 6) != 0)
		return PROC_SYNTH_NONE;
	p += 6;
	if (strncmp(p, "self/", 5) == 0 || strncmp(p, "thread-self/", 12) == 0) {
		per_process = true;
		p = strchr(p, '/') + 1;
	}
	else if (hpc_is_numeric(p)) {
		per_process = true;
		p = strchr(p, '/');
		if (p == NULL)
			return PROC_SYNTH_NONE;
		p++;
	}
	else {
		const char *slash = strchr(p, '/');
		if (slash != NULL) {
			char pidbuf[24];
			size_t len = (size_t)(slash - p);
			if (len < sizeof(pidbuf)) {
				memcpy(pidbuf, p, len);
				pidbuf[len] = '\0';
				if (hpc_is_numeric(pidbuf)) {
					per_process = true;
					p = slash + 1;
				}
			}
		}
	}
	/* If this is not a per-process path, p already names a global file. */
	if (strcmp(p, "cpuinfo") == 0) return PROC_SYNTH_CPUINFO;
	if (strcmp(p, "meminfo") == 0) return PROC_SYNTH_MEMINFO;
	if (strcmp(p, "stat") == 0) return per_process ? PROC_SYNTH_STAT_ONE : PROC_SYNTH_STAT;
	if (strcmp(p, "uptime") == 0) return PROC_SYNTH_UPTIME;
	if (strcmp(p, "loadavg") == 0) return PROC_SYNTH_LOADAVG;
	if (strcmp(p, "status") == 0) return PROC_SYNTH_STATUS;
	if (strcmp(p, "statm") == 0) return PROC_SYNTH_STATM;
	if (strcmp(p, "cmdline") == 0) return PROC_SYNTH_CMDLINE;
	if (strcmp(p, "comm") == 0) return PROC_SYNTH_COMM;
	if (strcmp(p, "limits") == 0) return PROC_SYNTH_LIMITS;
	if (strcmp(p, "io") == 0) return PROC_SYNTH_IO;
	if (strcmp(p, "sched") == 0) return PROC_SYNTH_SCHED;
	if (strcmp(p, "oom_score") == 0) return PROC_SYNTH_OOM_SCORE;
	if (strcmp(p, "wchan") == 0) return PROC_SYNTH_WCHAN;
	if (strcmp(p, "syscall") == 0) return PROC_SYNTH_SYSCALL;
	if (strcmp(p, "attr/current") == 0) return PROC_SYNTH_ATTR;
	if (strcmp(p, "mounts") == 0) return PROC_SYNTH_MOUNTINFO;
	if (strcmp(p, "mountinfo") == 0) return PROC_SYNTH_MOUNTINFO;
	return PROC_SYNTH_NONE;
}

static int hpc_proc_synth_slot(Tracee *tracee, int fd)
{
	int i;
	for (i = 0; i < tracee->proc_synth_count; i++)
		if (tracee->proc_synth_fds[i] == fd)
			return i;
	return -1;
}

static void hpc_remove_synth_fd(Tracee *tracee, int fd)
{
	int slot = hpc_proc_synth_slot(tracee, fd);
	if (slot < 0) return;
	tracee->proc_synth_count--;
	if (slot != tracee->proc_synth_count) {
		tracee->proc_synth_fds[slot] = tracee->proc_synth_fds[tracee->proc_synth_count];
		tracee->proc_synth_kinds[slot] = tracee->proc_synth_kinds[tracee->proc_synth_count];
		tracee->proc_synth_offsets[slot] = tracee->proc_synth_offsets[tracee->proc_synth_count];
	}
}

static void hpc_copy_synth_fd(Tracee *tracee, int oldfd, int newfd)
{
	unsigned char kind;
	size_t offset;
	if (oldfd == newfd) return;
	int oldslot = hpc_proc_synth_slot(tracee, oldfd);
	int newslot = hpc_proc_synth_slot(tracee, newfd);
	if (oldslot < 0 || tracee->proc_synth_count >= MAX_PROC_SYNTH_FDS)
		return;
	kind = tracee->proc_synth_kinds[oldslot];
	offset = tracee->proc_synth_offsets[oldslot];
	if (newslot >= 0) {
		/* Save the source metadata before removal: compacting the array can
		 * move the source slot when newfd precedes oldfd. */
		hpc_remove_synth_fd(tracee, newfd);
	}
	if (tracee->proc_synth_count >= MAX_PROC_SYNTH_FDS)
		return;
	newslot = tracee->proc_synth_count++;
	tracee->proc_synth_fds[newslot] = newfd;
	tracee->proc_synth_kinds[newslot] = kind;
	tracee->proc_synth_offsets[newslot] = offset;
}

static void hpc_track_synth_open(Tracee *tracee)
{
	char path[PATH_MAX];
	Reg path_reg = (get_sysnum(tracee, ORIGINAL) == PR_open) ? SYSARG_1 : SYSARG_2;
	int fd = (int)peek_reg(tracee, CURRENT, SYSARG_RESULT);
	int kind = tracee->proc_synth_pending_kind;
	tracee->proc_synth_pending_kind = PROC_SYNTH_NONE;
	if (fd < 0 || read_string(tracee, path, peek_reg(tracee, ORIGINAL, path_reg),
	                          sizeof(path) - 1) < 0)
		return;
	path[sizeof(path) - 1] = '\0';
	if (kind == PROC_SYNTH_NONE)
		kind = hpc_proc_synth_kind(path);
	if (kind == PROC_SYNTH_NONE || hpc_proc_synth_slot(tracee, fd) >= 0)
		return;
	if (tracee->proc_synth_count < MAX_PROC_SYNTH_FDS) {
		int n = tracee->proc_synth_count++;
		tracee->proc_synth_fds[n] = fd;
		tracee->proc_synth_kinds[n] = (unsigned char)kind;
		tracee->proc_synth_offsets[n] = 0;
	}
}

static size_t hpc_proc_synth_text(Tracee *tracee, int kind, char *out, size_t size)
{
	Tracees *list = get_tracees_list_head();
	Tracee *t;
	unsigned int nproc = 0;
	if (list != NULL)
		LIST_FOREACH(t, list, link)
			if (!t->terminated) nproc++;
	switch (kind) {
	case PROC_SYNTH_CPUINFO:
		return (size_t)snprintf(out, size,
			"processor\t: 0\nBogoMIPS\t: 0.00\nFeatures\t: \nCPU implementer\t: 0x00\n\n");
	case PROC_SYNTH_MEMINFO:
		return (size_t)snprintf(out, size,
			"MemTotal:           0 kB\nMemFree:            0 kB\nMemAvailable:       0 kB\nBuffers:            0 kB\nCached:             0 kB\nSwapCached:         0 kB\nSwapTotal:          0 kB\nSwapFree:           0 kB\n");
	case PROC_SYNTH_STAT:
		return (size_t)snprintf(out, size,
			"cpu  0 0 0 0 0 0 0 0 0 0\nprocs_running %u\nprocs_blocked 0\n", nproc ? 1 : 0);
	case PROC_SYNTH_UPTIME:
		return (size_t)snprintf(out, size, "0.00 0.00\n");
	case PROC_SYNTH_LOADAVG:
		return (size_t)snprintf(out, size, "0.00 0.00 0.00 %u/%u 1\n", nproc, nproc);
	case PROC_SYNTH_STATUS:
		return (size_t)snprintf(out, size,
			"Name:\tproot-guest\nUmask:\t0000\nState:\tR (running)\n"
			"Pid:\t%d\nPPid:\t%d\nTracerPid:\t0\nUid:\t0\t0\t0\t0\n"
			"Gid:\t0\t0\t0\t0\nNSpid:\t%d\nThreads:\t1\n",
			(int)tracee->pid, tracee->parent ? (int)tracee->parent->pid : 0,
			(int)tracee->pid);
	case PROC_SYNTH_STAT_ONE:
		/* bionic pthread_getattr_np() reads field 28 (startstack) and
		 * locates that address in /proc/self/maps.  The current tracee SP
		 * is inside its real main-thread stack and is safe in its own
		 * synthetic stat record. */
		return (size_t)snprintf(out, size,
			"%d (proot-guest) R %d "
			"0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 "
			"%" PRIuPTR "\n",
			(int)tracee->pid, tracee->parent ? (int)tracee->parent->pid : 0,
			(uintptr_t)peek_reg(tracee, CURRENT, STACK_POINTER));
	case PROC_SYNTH_STATM:
		return (size_t)snprintf(out, size, "0 0 0 0 0 0 0\n");
	case PROC_SYNTH_CMDLINE: {
		static const char cmdline[] = "proot-guest";
		if (size < sizeof(cmdline)) return 0;
		memcpy(out, cmdline, sizeof(cmdline));
		return sizeof(cmdline);
	}
	case PROC_SYNTH_COMM:
		return (size_t)snprintf(out, size, "proot-guest\n");
	case PROC_SYNTH_LIMITS:
		return (size_t)snprintf(out, size,
			"Limit                     Soft Limit           Hard Limit           Units\n"
			"Max cpu time              unlimited            unlimited            seconds\n"
			"Max open files            1024                 1024                 files\n"
			"Max processes             4096                 4096                 processes\n"
			"Max address space         unlimited            unlimited            bytes\n");
	case PROC_SYNTH_IO:
		return (size_t)snprintf(out, size, "rchar: 0\nwchar: 0\nsyscr: 0\nsyscw: 0\nread_bytes: 0\nwrite_bytes: 0\ncancelled_write_bytes: 0\n");
	case PROC_SYNTH_SCHED:
		return (size_t)snprintf(out, size, "proot-guest (%d, #threads: 1)\nse.exec_start: 0.000000\nruntime: 0\n",
			(int)tracee->pid);
	case PROC_SYNTH_OOM_SCORE:
		return (size_t)snprintf(out, size, "0\n");
	case PROC_SYNTH_WCHAN:
		return (size_t)snprintf(out, size, "0\n");
	case PROC_SYNTH_SYSCALL:
		return (size_t)snprintf(out, size, "-1 0x0 0x0 0x0 0x0 0x0 0x0 0x0 0x0 0x0\n");
	case PROC_SYNTH_ATTR:
		return (size_t)snprintf(out, size, "u:r:proot:s0\n");
	case PROC_SYNTH_MOUNTINFO:
		return (size_t)snprintf(out, size,
			"100 99 0:1 / / rw,relatime - proot proot rw\n"
			"101 100 0:2 / /proc rw,nosuid,nodev,noexec - proc proc rw\n"
			"102 100 0:3 / /sys ro,nosuid,nodev,noexec - sysfs sysfs ro\n"
			"103 100 0:4 / /dev rw,nosuid - devtmpfs devtmpfs rw\n");
	default:
		return 0;
	}
}

static int hpc_handle_synth_read_exit(Tracee *tracee)
{
	int fd = (int)peek_reg(tracee, ORIGINAL, SYSARG_1);
	int slot = hpc_proc_synth_slot(tracee, fd);
	char fd_path[PATH_MAX];
	word_t buf = peek_reg(tracee, ORIGINAL, SYSARG_2);
	word_t count = peek_reg(tracee, ORIGINAL, SYSARG_3);
	char data[2048];
	size_t len, available, take;
	if (slot < 0 || buf == 0 || count == 0)
		return 0;
	if (readlink_proc_pid_fd(tracee->pid, fd, fd_path) < 0 ||
		strcmp(fd_path, "/dev/null") != 0) {
		hpc_remove_synth_fd(tracee, fd);
		return 0;
	}
	len = hpc_proc_synth_text(tracee, tracee->proc_synth_kinds[slot], data, sizeof(data));
	if (tracee->proc_synth_offsets[slot] >= len) {
		poke_reg(tracee, SYSARG_RESULT, 0);
		return 0;
	}
	available = len - tracee->proc_synth_offsets[slot];
	take = available < (size_t)count ? available : (size_t)count;
	if (write_data(tracee, buf, data + tracee->proc_synth_offsets[slot], take) < 0)
		return 0;
	tracee->proc_synth_offsets[slot] += take;
	poke_reg(tracee, SYSARG_RESULT, take);
	return 0;
}

static void hpc_track_fd_lifecycle_exit(Tracee *tracee, Sysnum num)
{
	int oldfd, newfd, cmd;
	if (peek_reg(tracee, CURRENT, SYSARG_RESULT) < 0)
		return;
	switch (num) {
	case PR_close:
		hpc_remove_synth_fd(tracee, (int)peek_reg(tracee, ORIGINAL, SYSARG_1));
		break;
	case PR_dup:
		oldfd = (int)peek_reg(tracee, ORIGINAL, SYSARG_1);
		hpc_copy_synth_fd(tracee, oldfd, (int)peek_reg(tracee, CURRENT, SYSARG_RESULT));
		break;
	case PR_dup2:
	case PR_dup3:
		oldfd = (int)peek_reg(tracee, ORIGINAL, SYSARG_1);
		newfd = (int)peek_reg(tracee, ORIGINAL, SYSARG_2);
		hpc_copy_synth_fd(tracee, oldfd, newfd);
		break;
	case PR_fcntl:
		cmd = (int)peek_reg(tracee, ORIGINAL, SYSARG_2);
		if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC)
			hpc_copy_synth_fd(tracee, (int)peek_reg(tracee, ORIGINAL, SYSARG_1),
					 (int)peek_reg(tracee, CURRENT, SYSARG_RESULT));
		break;
	default:
		break;
	}
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
 * SYSCALL_ENTER_START — stat/readlink family: close the pid-existence
 * oracle and host-path leak via stat/fstatat/statx/readlink on
 * /proc/<host-pid>/... (FIXES.md A2).  The path is read from the raw
 * argument, before PRoot translates it, exactly like the open family
 * above.  Host pids answer ENOENT (emulate-never-deny); tracee pids
 * and /proc/self pass through untouched.
 * ================================================================ */

/* True when @num's path argument resolves to a /proc path of a host
 * process (the stat/readlink A2 oracle).  @version selects the register
 * set: CURRENT in SYSCALL_ENTER_START (raw guest path), ORIGINAL in
 * SYSCALL_EXIT_END.  When @out_path is non-NULL the path string is
 * copied there (up to @out_size bytes) for logging. */
static bool hpc_statlike_is_host_proc_path(Tracee *tracee, Sysnum num,
					   RegVersion version,
					   char *out_path, size_t out_size)
{
    char path[PATH_MAX];
    Reg path_reg;
    int dirfd = AT_FDCWD;

    switch (num) {
    case PR_stat:
    case PR_stat64:
    case PR_lstat:
    case PR_lstat64:
    case PR_readlink:
        path_reg = SYSARG_1;
        break;
    case PR_newfstatat:
    case PR_fstatat64:
    case PR_statx:
    case PR_readlinkat:
        path_reg = SYSARG_2;
	dirfd = (int)peek_reg(tracee, version, SYSARG_1);
        break;
    default:
        return false;
    }

	if (!hpc_resolve_syscall_path(tracee, version, path_reg, dirfd,
					path, sizeof(path)))
        return false;

    if (!hpc_is_host_proc_path(path))
        return false;

    if (out_path != NULL && out_size > 0) {
        strncpy(out_path, path, out_size - 1);
        out_path[out_size - 1] = '\0';
    }
    return true;
}

static int hpc_handle_statlike_enter(Tracee *tracee, Sysnum num)
{
    char path[PATH_MAX];

    if (!hpc_statlike_is_host_proc_path(tracee, num, CURRENT,
					path, sizeof(path)))
        return 0;

    VERBOSE(tracee, 2, "proc_isolation: blocked stat/readlink %s (ENOENT)", path);
    set_sysnum(tracee, PR_void);
    poke_reg(tracee, SYSARG_RESULT, -ENOENT);
    return 1;
}

static int hpc_handle_proc_readlink_enter(Tracee *tracee, Sysnum num)
{
	char path[PATH_MAX], target[PATH_MAX];
	word_t buf_addr, bufsz;
	Reg path_reg = (num == PR_readlink) ? SYSARG_1 : SYSARG_2;
	Reg buf_reg = (num == PR_readlink) ? SYSARG_2 : SYSARG_3;
	Reg size_reg = (num == PR_readlink) ? SYSARG_3 : SYSARG_4;
	int dirfd = (num == PR_readlink) ? AT_FDCWD :
		(int)peek_reg(tracee, CURRENT, SYSARG_1);
	const char *suffix;
	Tracee *target_tracee = tracee;
	pid_t pid;
	size_t len, take;

	if (!hpc_resolve_syscall_path(tracee, CURRENT, path_reg, dirfd,
					path, sizeof(path)))
		return 0;
	if (strncmp(path, "/proc/", 6) != 0 && strcmp(path, "/proc") != 0)
		return 0;
	if (strncmp(path, "/proc/self/", 11) == 0) {
		char resolved[PATH_MAX];
		snprintf(resolved, sizeof(resolved), "/proc/%d%s", (int)tracee->pid,
			 path + 10);
		strcpy(path, resolved);
	}
	else if (strncmp(path, "/proc/thread-self/", 18) == 0) {
		char resolved[PATH_MAX];
		snprintf(resolved, sizeof(resolved), "/proc/%d%s", (int)tracee->pid,
			 path + 17);
		strcpy(path, resolved);
	}

	if (strcmp(path, "/proc/self") == 0 || strcmp(path, "/proc/thread-self") == 0) {
		snprintf(target, sizeof(target), "%d", (int)tracee->pid);
	}
	else if (hpc_proc_path_pid(path, &pid)) {
		/* The path detranslation below may use the target tracee's
		 * temporary context, so retain get_tracee()'s context lifecycle
		 * here. */
		target_tracee = get_tracee(tracee, pid, false);
		if (target_tracee == NULL) {
			set_sysnum(tracee, PR_void);
			poke_reg(tracee, SYSARG_RESULT, -ENOENT);
			return 1;
		}
		suffix = strchr(path + 6, '/');
		if (suffix == NULL) {
			snprintf(target, sizeof(target), "%d", (int)pid);
		}
		else if (strcmp(suffix, "/exe") == 0) {
			char guest_path[PATH_MAX];
			strncpy(guest_path, target_tracee->exe ?: "/bin/sh", sizeof(guest_path) - 1);
			guest_path[sizeof(guest_path) - 1] = '\0';
			if (detranslate_path(target_tracee, guest_path, NULL) > 0 ||
			    belongs_to_guestfs(target_tracee, guest_path))
				strncpy(target, guest_path, sizeof(target) - 1);
			else
				strcpy(target, "/bin/sh");
			target[sizeof(target) - 1] = '\0';
		}
		else if (strcmp(suffix, "/cwd") == 0) {
			char guest_path[PATH_MAX];
			strncpy(guest_path, target_tracee->fs->cwd ?: "/", sizeof(guest_path) - 1);
			guest_path[sizeof(guest_path) - 1] = '\0';
			if (detranslate_path(target_tracee, guest_path, NULL) > 0 ||
			    belongs_to_guestfs(target_tracee, guest_path))
				strncpy(target, guest_path, sizeof(target) - 1);
			else
				strcpy(target, "/");
			target[sizeof(target) - 1] = '\0';
		}
		else if (strncmp(suffix, "/fd/", 4) == 0) {
			char *end;
			long fd;
			errno = 0;
			fd = strtol(suffix + 4, &end, 10);
			if (errno != 0 || end == suffix + 4 || *end != '\0' ||
			    fd < 0 || fd > INT_MAX ||
			    readlink_proc_pid_fd(pid, (int)fd, target) < 0) {
				set_sysnum(tracee, PR_void);
				poke_reg(tracee, SYSARG_RESULT, -ENOENT);
				return 1;
			}
			/* Preserve the guest meaning of filesystem descriptors;
			 * anonymous descriptors retain only their type and do not
			 * expose a host inode number. */
			if (target[0] == '/') {
				(void)detranslate_path(target_tracee, target, NULL);
			} else if (strncmp(target, "pipe:[", 6) == 0) {
				strcpy(target, "pipe:[0]");
			} else if (strncmp(target, "socket:[", 8) == 0) {
				strcpy(target, "socket:[0]");
			}
		}
		else {
			set_sysnum(tracee, PR_void);
			poke_reg(tracee, SYSARG_RESULT, -ENOENT);
			return 1;
		}
	}
	else {
		set_sysnum(tracee, PR_void);
		poke_reg(tracee, SYSARG_RESULT, -ENOENT);
		return 1;
	}

	buf_addr = peek_reg(tracee, CURRENT, buf_reg);
	bufsz = peek_reg(tracee, CURRENT, size_reg);
	len = strlen(target);
	take = len < (size_t)bufsz ? len : (size_t)bufsz;
	if (buf_addr == 0 || write_data(tracee, buf_addr, target, take) < 0)
		return 0;
	set_sysnum(tracee, PR_void);
	poke_reg(tracee, SYSARG_RESULT, take);
	return 1;
}

/* Public accessor for tracee/seccomp.c: on kernels where seccomp is
 * evaluated BEFORE the ptrace sysenter stop (legacy 3.4.x backports,
 * seccomp_after_ptrace_enter == false), a blocked statx is answered
 * entirely in the SIGSYS path and the extension's SYSCALL_ENTER_START /
 * SYSCALL_EXIT_END handlers never run for it.  The core then asks the
 * extension whether the statx path targets a host /proc path, so it can
 * answer ENOENT (the same emulation as the enter side) instead of
 * running a real host stat() that would leak host data (FIXES.md A2).
 * Uses CURRENT: at SIGSYS time save_current_regs() fills the
 * ORIGINAL_SECCOMP_REWRITE slot, not plain ORIGINAL, so ORIGINAL may
 * hold stale registers from an earlier syscall.  Returns false when the
 * proc_isolation extension is absent or ISOLATE_PROC is inactive, so
 * mode A (no isolation) is never affected. */
bool proc_isolation_statx_is_host_proc_path(Tracee *tracee,
					    char *out_path, size_t out_size)
{
    Extension *extension = get_extension(tracee, hpc_callback);
    HpcConfig *config;

    if (extension == NULL)
        return false;

    config = (HpcConfig *)extension->config;
    if (config == NULL || !(config->flags & ISOLATE_PROC))
        return false;

    return hpc_statlike_is_host_proc_path(tracee, PR_statx, CURRENT,
					  out_path, out_size);
}

bool proc_isolation_is_active(Tracee *tracee)
{
	Extension *extension = get_extension(tracee, hpc_callback);
	HpcConfig *config;
	if (extension == NULL)
		return false;
	config = (HpcConfig *)extension->config;
	return config != NULL && (config->flags & ISOLATE_PROC) != 0;
}

/* ================================================================
 * SYSCALL_ENTER_END — kill interception + all other void/block handlers
 * ================================================================ */

/* Deliver @sig to every live tracee of this instance matching a process
 * group: @group_pid == -1 selects every live tracee (broadcast);
 * otherwise only tracees whose real host pgid equals @group_pid.  When
 * @include_self is false the sender (tracee->pid) is never targeted.
 * Returns the number of tracees that matched.
 *
 * sig == 0 is the kill(2) existence oracle: kill(pid, 0) sends nothing,
 * so the syscall doubles as a live-ness probe (which also closes the
 * race of an exit between the list walk and the check).  sig != 0
 * delivers the real signal.  The pgids come from getpgid(2) — a
 * read-only query, valid even on ptrace-stopped processes — and PRoot
 * does not trap setpgid/setsid, so a guest that changed its own group
 * is reflected here exactly as the kernel would see it. */
static int hpc_kill_tracees_matching(Tracee *tracee, int sig,
				     pid_t group_pid, bool include_self)
{
    Tracees *list = get_tracees_list_head();
    Tracee *t;
    int n = 0;

    if (list == NULL)
        return 0;

    LIST_FOREACH(t, list, link) {
        if (t->terminated)
            continue;
        if (!include_self && t->pid == tracee->pid)
            continue;
        if (group_pid != -1 && getpgid(t->pid) != group_pid)
            continue;
        if (sig == 0) {
            if (kill(t->pid, 0) == 0)
                n++;
        }
        else {
            kill(t->pid, sig);
            n++;
        }
    }
    return n;
}

/* POSIX: kill(2) requires @sig to be 0 or a valid signal number;
 * anything else answers -EINVAL (emulated, syscall voided).  Shared by
 * every PR_kill target this extension emulates (kill(-1), kill(0),
 * kill(-pgid)); positive pids pass through and the kernel answers
 * EINVAL by itself.  @sig is already the int the kernel would see in
 * the 32-bit signal argument on ARM64.  Returns true when it voided
 * the syscall with -EINVAL. */
static bool hpc_kill_invalid_signal(Tracee *tracee, pid_t target_pid, int sig)
{
    if (sig != 0 && (sig < 1 || sig >= NSIG)) {
        set_sysnum(tracee, PR_void);
        poke_reg(tracee, SYSARG_RESULT, -EINVAL);
        VERBOSE(tracee, 2, "proc_isolation: kill(%d,%d) -> EINVAL (invalid signal)",
                target_pid, sig);
        return true;
    }
    return false;
}

static int hpc_handle_kill_enter(Tracee *tracee, Sysnum num, int pid_reg)
{
    pid_t target_pid;
    int sig;

    target_pid = (pid_t)peek_reg(tracee, CURRENT, pid_reg);

    /* kill(-1, sig): broadcast to every process the caller may signal.
     * On the host that is every same-uid process, so it must never
     * reach the kernel (FIXES.md C1).  sig == 0 is an existence oracle:
     * answer ESRCH ("no such process") so the sandbox looks empty; real
     * signals are emulated as a broadcast confined to the sandbox --
     * the signal is delivered to every live tracee of this proot
     * instance except the sender, which is exactly what a guest
     * init/shutdown (openrc/busybox "kill -TERM -1") expects of its own
     * children (emulate-never-deny: natural success, real effect inside
     * the sandbox, host untouched).  Only kill(2) can express this;
     * tkill/tgkill pids are always positive. */
    if (target_pid == -1 && num == PR_kill) {
        sig = (int)peek_reg(tracee, CURRENT, SYSARG_2);
        if (hpc_kill_invalid_signal(tracee, target_pid, sig))
            return 0;
        if (sig == 0) {
            set_sysnum(tracee, PR_void);
            poke_reg(tracee, SYSARG_RESULT, -ESRCH);
            VERBOSE(tracee, 2, "proc_isolation: kill(-1,0) -> ESRCH (broadcast oracle voided)");
        }
        else {
            /* Deliver @sig to every live tracee of this instance except
             * the sender (host-side kill() over guest PIDs -- the
             * kernel resolves them as sandbox processes, never host
             * ones).  An empty list delivers nothing and still returns
             * 0, the natural result of a broadcast over zero processes. */
            int delivered = hpc_kill_tracees_matching(tracee, sig, -1, false);

            set_sysnum(tracee, PR_void);
            poke_reg(tracee, SYSARG_RESULT, 0);
            VERBOSE(tracee, 2, "proc_isolation: kill(-1,%d) -> 0 emulated (signal delivered to %d tracee(s))",
                    sig, delivered);
        }
        return 0;
    }

    /* kill(0, ...) targets the caller's own process group and
     * kill(-pgid, ...) a group of the sandbox.  On the host that group
     * is PRoot's own (every tracee inherits proot's pgid), so passing
     * them through would let a guest signal the HOST process group —
     * proot itself and the user's shell — an automatic sandbox self-DoS
     * (pentest proc.k-sig-kill0-0: "grupo de procesos alcanzable").
     * Emulate them confined to the guest (emulate-never-deny): the
     * signal is delivered only to tracees of this instance in the
     * requested group, the kernel is never invoked.  Only kill(2) can
     * express these targets; tkill/tgkill take positive pids only and
     * the kernel answers EINVAL for anything else (left untouched). */
    if (target_pid <= 0) {
        pid_t group_pid;

        if (num != PR_kill)
            return 0;

        if (target_pid == 0) {
            /* kill(0): the caller's own group.  getpgid on our own live
             * tracee cannot fail; the fallback keeps an error value
             * (-1) from meaning "broadcast" in the helper. */
            group_pid = getpgid(tracee->pid);
            if (group_pid < 0)
                group_pid = tracee->pid;
        }
        else
            /* kill(-pgid): group id is the magnitude.  Computed in a
             * wider type so INT32_MIN (kill(-2147483648)) stays
             * well-defined; no tracee can match it, so the emulation
             * answers ESRCH/0 exactly like a nonexistent group. */
            group_pid = (pid_t)(-(int64_t)target_pid);

        sig = (int)peek_reg(tracee, CURRENT, SYSARG_2);

        if (hpc_kill_invalid_signal(tracee, target_pid, sig))
            return 0;

        if (sig == 0) {
            /* Group existence oracle with natural semantics: kill(0,0)
             * always answers 0 (the caller belongs to its own group, so
             * the guest sees its own processes); kill(-pgid,0) answers
             * ESRCH when no tracee is in that group. */
            int found = hpc_kill_tracees_matching(tracee, 0, group_pid, true);

            set_sysnum(tracee, PR_void);
            poke_reg(tracee, SYSARG_RESULT, (found > 0) ? 0 : -ESRCH);
            VERBOSE(tracee, 2, "proc_isolation: kill(%d,0) -> %s emulated (group %d, %d tracee(s))",
                    target_pid, (found > 0) ? "0" : "ESRCH",
                    (int)group_pid, found);
        }
        else {
            /* Group membership including the sender (same semantics as
             * the sig==0 oracle).  For a SPECIFIC pgid (target_pid < -1)
             * Linux answers ESRCH when the group does not exist at all,
             * so an empty requested group must not get a fake success —
             * consistent with our own oracle, which already answers
             * kill(-pgid, 0) with ESRCH for it.  kill(-1) and kill(0)
             * always succeed: the sender is itself signalable / belongs
             * to its own group, guaranteeing at least one member. */
            int members = hpc_kill_tracees_matching(tracee, 0,
                                                    group_pid, true);

            if (target_pid < -1 && members == 0) {
                set_sysnum(tracee, PR_void);
                poke_reg(tracee, SYSARG_RESULT, -ESRCH);
                VERBOSE(tracee, 2, "proc_isolation: kill(%d,%d) -> ESRCH (no tracee in group %d)",
                        target_pid, sig, (int)group_pid);
                return 0;
            }

            /* Deliver @sig to every live tracee in the requested group.
             * The sender is excluded: a deliberate deviation from POSIX
             * (real kill(0) includes the caller) that protects the
             * tracee which is itself stopped in ptrace at this very
             * syscall — signaling it would inject a signal mid-emulation
             * and could kill the guest shell before it finishes its
             * script.  It also makes a guest "kill -KILL 0" unable to
             * destroy its own shell (kill(-1) uses the same rule).  An
             * empty group delivers nothing and returns 0, the natural
             * result of a broadcast over zero processes. */
            int delivered = hpc_kill_tracees_matching(tracee, sig,
                                                      group_pid, false);

            set_sysnum(tracee, PR_void);
            poke_reg(tracee, SYSARG_RESULT, 0);
            VERBOSE(tracee, 2, "proc_isolation: kill(%d,%d) -> 0 emulated (group %d, %d tracee(s))",
                    target_pid, sig, (int)group_pid, delivered);
        }
        return 0;
    }

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

	if (result > SIZE_MAX)
		return 0;

	char *data = talloc_size(tracee->ctx, (size_t)result);
	if (data == NULL || read_data(tracee, data, buf, result) < 0) {
		talloc_free(data);
		return 0;
	}

    size_t nleft = 0;
    char *ptr = data;
    size_t remaining = (size_t)result;

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

        if (reclen == 0 || (size_t)reclen > remaining)
            break;

		char *name_end = memchr(d_name, '\0', reclen);
		bool is_numeric;
		pid_t entry_pid;
		bool keep;
		if (name_end == NULL)
			break;
		is_numeric = hpc_is_numeric(d_name);
		entry_pid = is_numeric ? (pid_t)strtol(d_name, NULL, 10) : 0;
		keep = is_numeric ? hpc_is_proot_pid(entry_pid)
				 : hpc_is_allowed_proc_root_name(d_name);

        if (keep) {
            if (ptr != data + nleft)
                memmove(data + nleft, ptr, reclen);
            nleft += reclen;
        }

        ptr += reclen;
        remaining -= reclen;
    }

    if (nleft < (size_t)result) {
        if (nleft > 0)
            write_data(tracee, buf, data, nleft);
        poke_reg(tracee, SYSARG_RESULT, (word_t)nleft);
        VERBOSE(tracee, 3, "proc_isolation: filtered getdents %zu -> %zu bytes",
            (size_t)result, nleft);
    }

	talloc_free(data);
    return 0;
}

static bool hpc_maps_host_path(Tracee *tracee, const char *path, size_t len)
{
	(void)len;
	/* Kernel pseudo-mappings ([stack], [heap], [vdso], [vvar], ...)
	 * describe the current process and are not host filesystem paths.  Keep
	 * their labels: runtimes such as Go/cgo use [stack] to locate the thread
	 * stack in /proc/self/maps. */
	if (path != NULL && path[0] == '[')
		return false;
	return hpc_sensitive_android_prefix(path) ||
		!belongs_to_guestfs(tracee, path);
}

/* extract_loader() uses a private temporary file named
 * <PROOT_TMP_DIR>/prooted-<pid>-XXXXXX.  Its pathname is not the installed
 * loader path, so the generic loader marker below cannot identify it.  Do
 * not expose that implementation detail (and especially not the proot PID)
 * through the guest's maps view. */
static bool hpc_is_prooted_loader_path(const char *path)
{
	const char *tmpdir = get_temp_directory();
	size_t tmp_len;
	const char *base;

	if (tmpdir == NULL || path == NULL)
		return false;
	tmp_len = strlen(tmpdir);
	if (tmp_len == 0 || strncmp(path, tmpdir, tmp_len) != 0 ||
		path[tmp_len] != '/')
		return false;

	base = path + tmp_len + 1;
	return strncmp(base, "prooted-", strlen("prooted-")) == 0 &&
		strchr(base, '/') == NULL;
}

/* Remove filesystem metadata from one maps record while preserving the
 * virtual-address range and field shape for procps/runtime consumers.
 *
 * The address range is process-local information and is required by runtimes
 * such as Go/cgo to identify their own stack.  Zeroing it makes a valid maps
 * record look like 0000000000-0000000000; Go then computes an underflowed
 * stack bound and aborts with "bad stack bounds".  Host process maps are
 * blocked before reaching this function, so retaining guest ranges does not
 * expose a host process address space.
 */
static void hpc_sanitize_maps_metadata(char *line, size_t len)
{
	char *p = line;
	char *end = line + len;
	int field = 0;
	while (p < end && field < 5) {
		char *start = p;
		while (p < end && *p != ' ') p++;
		if (field == 2 || field == 3 || field == 4) {
			for (char *q = start; q < p; q++)
				if ((*q >= '0' && *q <= '9') ||
				    (*q >= 'a' && *q <= 'f') ||
				    (*q >= 'A' && *q <= 'F'))
					*q = '0';
		}
		while (p < end && *p == ' ') p++;
		field++;
	}
}

/**
 * Guest-pure /proc/self/maps: remove lines that reference the proot
 * loader.  The tracee sees a maps indistinguishable from a native
 * process of the guest rootfs.
 *
 * E7: lazy maps_fd detection — if maps_fd was never registered by
 * the open handler (non-canonical path, openat with relative path,
 * etc.), detect it here by checking if the fd points to a maps file.
 */
static int hpc_handle_maps_read_exit(Tracee *tracee)
{
    int fd = (int) peek_reg(tracee, ORIGINAL, SYSARG_1);
    char proc_path[PATH_MAX];
    char self_maps_path[PATH_MAX];
    bool self_maps;
    int status;

    /* E7: lazy detection — if maps_fd not yet registered, check if
     * this fd points to a maps file and register it. */
    status = readlink_proc_pid_fd(tracee->pid, fd, proc_path);
    if (status < 0 || strstr(proc_path, "/maps") == NULL)
        return 0;
    tracee->maps_fd = fd;
    snprintf(self_maps_path, sizeof(self_maps_path), "/proc/%d/maps",
             (int)tracee->pid);
    self_maps = strcmp(proc_path, "/proc/self/maps") == 0 ||
                strcmp(proc_path, self_maps_path) == 0 ||
                (strncmp(proc_path, self_maps_path,
                         strlen(self_maps_path) - strlen("maps")) == 0 &&
                 strstr(proc_path, "/task/") != NULL &&
                 strstr(proc_path, "/maps") != NULL);

    {
        word_t result = peek_reg(tracee, CURRENT, SYSARG_RESULT);
        word_t buf = peek_reg(tracee, CURRENT, SYSARG_2);
        char *data;
        size_t result_len;
        size_t nleft = 0;
        char *p;
        char *end;

        if ((int)result <= 0 || buf == 0)
            return 0;
        result_len = (size_t) result;

        /* Dynamic buffer (talloc): no fixed 64KB cap, so reads larger
         * than that can no longer bypass the loader-line filter. */
        data = talloc_size(tracee->ctx, result_len);
        if (data == NULL)
            return 0;

        if (read_data(tracee, data, buf, result_len) < 0) {
            talloc_free(data);
            return 0;
        }

        const char *marker = "/libexec/proot/loader";
        size_t marker_len = strlen(marker);
        p = data;
        end = data + result_len;

        while (p < end) {
            char *nl = memchr(p, '\n', end - p);
            char *line_end = (nl != NULL) ? nl : end;
            size_t line_len = (size_t)(line_end - p);
            bool keep = true;

            /* The extracted loader is a real executable mapping whose
             * basename embeds the tracer PID.  Remove the whole record,
             * just as we do for the installed proot loader mappings. */
            if (line_len > 0) {
                char *path_start = NULL;
                int fields = 0;
                bool in_spaces = false;
                for (size_t i = 0; i < line_len; i++) {
                    if (p[i] == ' ') {
                        if (!in_spaces) {
                            in_spaces = true;
                            fields++;
                            if (fields == 5) {
                                path_start = p + i + 1;
                                while (path_start < line_end &&
                                       *path_start == ' ')
                                    path_start++;
                                break;
                            }
                        }
                    }
                    else
                        in_spaces = false;
                }
                if (path_start != NULL && path_start < line_end &&
                    hpc_is_prooted_loader_path(path_start))
                    keep = false;
            }

            if (line_len >= marker_len) {
                for (size_t i = 0; i + marker_len <= line_len; i++) {
                    if (memcmp(p + i, marker, marker_len) == 0) {
                        keep = false;
                        break;
                    }
                }
            }

            if (keep) {
			/* Keep the current process's maps byte-compatible with the
			 * kernel.  Android bionic's pthread_getattr_np() parses the
			 * complete record while Go/cgo discovers its stack.  The own
			 * address space is not a host-process leak; host PID maps are
			 * blocked before reaching this path. */
			if (!self_maps)
				hpc_sanitize_maps_metadata(p, line_len);
                size_t copy_len = line_len + (nl != NULL ? 1 : 0);

                /* Android kernels right-align the pathname column: the
                 * inode is followed by ~24 spaces of padding before the
                 * path.  Treat runs of spaces as a single field
                 * separator so path_start lands on the pathname's '/'.
                 * The pathname field (the part after the 5th space) is
                 * then translated from a host path to a guest path so
                 * the tracee never sees the host layout (rootfs prefix,
                 * bindings).  detranslate_path() returns the new length
                 * including the NUL terminator (>0), 0 when the path is
                 * unchanged, or a negative error when the path is outside
                 * the guest fs without a matching binding (keep the host
                 * path then). */
                char *path_start = NULL;
                int fields = 0;
                bool in_spaces = false;
                for (size_t i = 0; i < line_len; i++) {
                    if (p[i] == ' ') {
                        if (!in_spaces) {
                            in_spaces = true;
                            fields++;
                            if (fields == 5) {
                                path_start = p + i + 1;
                                while (path_start < line_end
                                       && *path_start == ' ')
                                    path_start++;
                                break;
                            }
                        }
                    }
                    else
                        in_spaces = false;
                }

                if (path_start != NULL && path_start < line_end
                    && path_start[0] == '/') {
                    size_t path_len = (size_t)(line_end - path_start);
                    if (path_len > 0 && path_len < PATH_MAX) {
                        char pbuf[PATH_MAX];
                        memcpy(pbuf, path_start, path_len);
                        pbuf[path_len] = '\0';
                        int dstatus = detranslate_path(tracee, pbuf, NULL);
                        const char *replacement = NULL;
                        size_t new_len = 0;
                        if (dstatus > 0) {
                            new_len = (size_t)(dstatus - 1);
				if (new_len > path_len) {
					replacement = "[guest]";
					new_len = strlen(replacement);
				}
                        }
                        else if (hpc_maps_host_path(tracee, pbuf, path_len)) {
                            replacement = "[guest]";
                            new_len = strlen(replacement);
                        }
                        if (dstatus > 0 || replacement != NULL) {
                            /* The guest path is usually shorter than the
                             * host one (the rootfs prefix is stripped),
                             * so the line is compacted by shifting it
                             * left.  If it were longer we'd keep the
                             * host path to stay within the allocated
                             * buffer. */
                            if (new_len <= path_len) {
                                /* The pathname is the last field of the
                                 * line, so the only tail content is the
                                 * trailing newline (if any). */
                                size_t tail_len = (nl != NULL ? 1 : 0);
                                memmove(path_start, replacement ?: pbuf, new_len);
                                memmove(path_start + new_len,
                                        path_start + path_len, tail_len);
                                line_end = path_start + new_len + tail_len
                                        - (nl != NULL ? 1 : 0);
                                copy_len = (size_t)(line_end - p)
                                        + (nl != NULL ? 1 : 0);
                            }
                        }
                    }
                }

                if (p != data + nleft)
                    memmove(data + nleft, p, copy_len);
                nleft += copy_len;
            }

            p = (nl != NULL) ? nl + 1 : end;
        }

        if (nleft < result_len) {
            if (nleft > 0)
                write_data(tracee, buf, data, nleft);
            poke_reg(tracee, SYSARG_RESULT, (word_t)nleft);
            VERBOSE(tracee, 3, "proc_isolation: filtered maps %zu -> %zu bytes",
                    result_len, nleft);
        }

        talloc_free(data);
    }
    return 0;
}

static int hpc_block_maps_vector_enter(Tracee *tracee)
{
	char proc_path[PATH_MAX];
	int fd = (int)peek_reg(tracee, CURRENT, SYSARG_1);
	if (readlink_proc_pid_fd(tracee->pid, fd, proc_path) < 0 ||
	    strstr(proc_path, "/maps") == NULL)
		return 0;
	set_sysnum(tracee, PR_void);
	poke_reg(tracee, SYSARG_RESULT, 0);
	return 1;
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
                    /* PR_getdents64, PR_getdents, PR_read and the open
                     * family need FILTER_SYSEXIT (filter at exit for /proc/
                     * PID and maps filtering) */
					if (flag_sysnum_map[i].flag == ISOLATE_PROC &&
						(sysnum == PR_getdents64 || sysnum == PR_getdents ||
						 sysnum == PR_read || sysnum == PR_pread64 ||
						 sysnum == PR_dup || sysnum == PR_dup2 || sysnum == PR_dup3 ||
						 sysnum == PR_fcntl || sysnum == PR_close ||
                         sysnum == PR_open || sysnum == PR_openat ||
                         sysnum == PR_openat2))
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

		if ((config->flags & ISOLATE_PROC) &&
		    (num == PR_readv || num == PR_preadv || num == PR_preadv2))
			return hpc_block_maps_vector_enter(tracee);

        if ((config->flags & ISOLATE_PTRACE) && num == PR_ptrace)
            return hpc_handle_ptrace_enter(tracee);

        /* E5: pidfd_open now under ISOLATE_PROC (was ISOLATE_PTRACE).
         * Blocks pidfd_open for host PIDs whenever proc isolation is
         * active, preventing pidfd-based /proc bypass. */
        if ((config->flags & ISOLATE_PROC) && num == PR_pidfd_open) {
            pid_t target_pid = (pid_t)peek_reg(tracee, CURRENT, SYSARG_1);
            if (target_pid > 0 && !hpc_is_proot_pid(target_pid)) {
                /* Host process: emulate "no such process" instead of
                 * revealing the host's existence. */
                set_sysnum(tracee, PR_void);
                poke_reg(tracee, SYSARG_RESULT, -ESRCH);
                VERBOSE(tracee, 1, "proc_isolation: pidfd_open(%d) -> ESRCH (host pid)", target_pid);
                return 1;
            }
        }

		if ((config->flags & ISOLATE_PROC)
		    && (num == PR_open || num == PR_openat || num == PR_openat2)) {
		    char path[PATH_MAX];
		    word_t path_addr = peek_reg(tracee, CURRENT,
				    (num == PR_open) ? SYSARG_1 : SYSARG_2);
            /* read_string reads up to the NUL in page-aligned chunks
             * (never a fixed contiguous window), so a string whose
             * NUL is mapped can no longer bypass the checks by having
             * the buffer read cross an unmapped page.  The defensive
             * terminator stays: read_string only NUL-terminates when
             * it finds the NUL within max_size. */
		    if (path_addr != 0 && hpc_open_proc_path(tracee, num,
								 path, sizeof(path))
			    && (strcmp(path, "/proc") == 0
				|| strncmp(path, "/proc/", 6) == 0)) {
			/* PRoot translates /proc/self to the host tracee PID before
			 * the kernel opens the path.  A self/task path can therefore
			 * combine the current tracee PID with a guest-visible TID that
			 * does not exist in the host task directory.  Resolve it to the
			 * current process's maps, preserving Linux self semantics for
			 * Go/cgo and other thread runtimes. */
			if (hpc_is_self_task_maps_path(path)) {
				const char self_maps[] = "/proc/self/maps";
				write_data(tracee, path_addr, self_maps,
					   sizeof(self_maps));
				strcpy(path, self_maps);
			}

                /* Some Android procfs global files are not readable by an
                 * unprivileged tracee even though meminfo is.  Open the
                 * readable file and replace its contents at read exit;
                 * the original kind is retained in the tracee state. */
                {
                    int synth_kind = hpc_proc_synth_kind(path);
                    if (synth_kind != PROC_SYNTH_NONE) {
                        const char fallback[] = "/dev/null";
                        tracee->proc_synth_pending_kind = synth_kind;
                        if (strcmp(path, fallback) != 0)
                            write_data(tracee, path_addr, fallback, sizeof(fallback));
                    }
                }
				if (!hpc_is_guest_proc_path(path)) {
				    VERBOSE(tracee, 2, "proc_isolation: blocked %s (ENOENT)", path);
                    set_sysnum(tracee, PR_void);
                    poke_reg(tracee, SYSARG_RESULT, -ENOENT);
				    return 1;
				}

                /* Files under /proc/net/ (host network state) and
                 * direct /proc/<pid>/... of host processes leak data
                 * that the getdents listing filter cannot hide: the
                 * path is read unconditionally, no directory listing
                 * involved.  Both are blocked with ENOENT — a natural
                 * "does not exist" error, never EPERM, per the
                 * emulate-never-deny philosophy. */
                if (hpc_is_host_proc_path(path)) {
                    VERBOSE(tracee, 2, "proc_isolation: blocked %s (ENOENT)", path);
                    set_sysnum(tracee, PR_void);
                    poke_reg(tracee, SYSARG_RESULT, -ENOENT);
                    return 1;
                }
            }
        }

        /* stat/fstatat/statx/readlink of /proc/<pid>/... must not reach
         * the kernel: they would reveal host process existence (oracle)
         * or leak host paths (FIXES.md A2).  Only enforced when
         * ISOLATE_PROC is active, so mode A keeps its behaviour. */
        if ((config->flags & ISOLATE_PROC)
		    && (num == PR_readlink || num == PR_readlinkat)
		    && hpc_handle_proc_readlink_enter(tracee, num))
		    return 1;

        if ((config->flags & ISOLATE_PROC)
            && (num == PR_stat || num == PR_lstat || num == PR_stat64
                || num == PR_lstat64 || num == PR_fstatat64
                || num == PR_newfstatat || num == PR_statx
                || num == PR_readlink || num == PR_readlinkat))
            return hpc_handle_statlike_enter(tracee, num);

        if (config->flags & ISOLATE_PROC) {
            if (num == PR_socket) {
                int domain = (int) peek_reg(tracee, CURRENT, SYSARG_1);
                int type   = (int) peek_reg(tracee, CURRENT, SYSARG_2);

                if (domain == AF_NETLINK) {
                    /* Emulate AF_NETLINK with an AF_UNIX datagram socket
                     * (kernel-compatible: responses are synthesised by the
                     * fake_netlink machinery in enter.c/exit.c).  The tracee
                     * believes it has a netlink socket; it never touches the
                     * host's netlink. */
                    int protocol = (int) peek_reg(tracee, CURRENT, SYSARG_3);

                    poke_reg(tracee, SYSARG_1, AF_UNIX);
                    poke_reg(tracee, SYSARG_2, SOCK_DGRAM | (type & SOCK_CLOEXEC));
                    poke_reg(tracee, SYSARG_3, 0);

                    tracee->pending_fake_netlink_socket = true;
                    tracee->sysexit_pending = true;
                    tracee->restart_how = PTRACE_SYSCALL;

                    VERBOSE(tracee, 1, "proc_isolation: AF_NETLINK(%d) -> AF_UNIX emulation",
                            protocol);
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
                return hpc_handle_kill_enter(tracee, PR_kill, SYSARG_1);
            return 0;
        case PR_tkill:
            if (config->flags & ISOLATE_PROC)
                return hpc_handle_kill_enter(tracee, PR_tkill, SYSARG_1);
            return 0;
        case PR_tgkill:
            if (config->flags & ISOLATE_PROC)
                return hpc_handle_kill_enter(tracee, PR_tgkill, SYSARG_2);
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

    case SYSCALL_EXIT_START:
        /* No filtering needed on syscall entry side of exit. */
        return 0;

    case SYSCALL_EXIT_END: {
        Tracee *tracee = TRACEE(extension);
        HpcConfig *config = (HpcConfig *)extension->config;

        if (config == NULL || !(config->flags & ISOLATE_PROC))
            return 0;

		switch (get_sysnum(tracee, ORIGINAL)) {
		case PR_close:
		case PR_dup:
		case PR_dup2:
		case PR_dup3:
		case PR_fcntl:
			hpc_track_fd_lifecycle_exit(tracee, get_sysnum(tracee, ORIGINAL));
			return 0;
		case PR_open:
        case PR_openat:
        case PR_openat2:
            hpc_track_synth_open(tracee);
            return 0;
        case PR_getdents64:
            return hpc_handle_getdents_exit(tracee, PR_getdents64);
        case PR_getdents:
            return hpc_handle_getdents_exit(tracee, PR_getdents);
		case PR_read:
		case PR_pread64:
            if (hpc_handle_synth_read_exit(tracee) == 0 &&
                hpc_proc_synth_slot(tracee, (int)peek_reg(tracee, ORIGINAL, SYSARG_1)) >= 0)
                return 0;
            return hpc_handle_maps_read_exit(tracee);
        case PR_statx:
            /* exit.c's handle_statx_syscall() re-runs a host stat() when
             * the kernel result is an error, overwriting the ENOENT we
             * emulated in SYSCALL_ENTER_START with the kernel's EACCES
             * for host /proc paths.  Re-assert the emulated ENOENT for
             * host-proc paths (legitimate statx of tracee pids, /proc/self
             * and non-proc paths are untouched). */
            if (hpc_statlike_is_host_proc_path(tracee, PR_statx, ORIGINAL, NULL, 0))
                poke_reg(tracee, SYSARG_RESULT, -ENOENT);
            return 0;
        default:
            return 0;
        }
    }

    case SIGSYS_OCC:
        /* No signal filtering needed. */
        return 0;

    case REMOVED:
        return 0;

    default:
        return 0;
    }
}
