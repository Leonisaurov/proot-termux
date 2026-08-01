/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * resource_limit — guest-side perception of CPU/process resource limits.
 *
 * CPU side: intercepts sched_getaffinity() so that a tracee only
 * perceives the first N CPUs, where N comes from the Fase 1 flags
 * --cpu-limit N, --single-core and --resource-isolated (stored in the
 * static resource_config in cli/proot.c and exposed through
 * resource_config_cpu_limit()).
 *
 * Process side: intercepts fork(2)/clone(2)/vfork(2) at ENTER so that
 * --proc-limit N caps the number of live tracees of THIS proot
 * instance (guest processes + threads).  When the limit is reached the
 * syscall is voided and answered with -EAGAIN, the natural kernel error
 * for an exceeded RLIMIT_NPROC.  Unlike the old host-side prlimit64
 * RLIMIT_NPROC (removed), this never affects the rest of the Termux
 * UID: only this sandbox's processes are counted.
 *
 * The real sched_getaffinity syscall is left to run ("emulate, never
 * deny"): the kernel resolves pid 0, applies the real (already
 * restricted) affinity and returns the byte count; only the mask
 * content is rewritten at SYSCALL_EXIT_END so the guest sees bits
 * 0..N-1.  When neither cpu_limit nor proc_limit is set the extension
 * is never initialized, so there is zero seccomp overhead (the sysnums
 * are only filtered through extension->filtered_sysnums).
 *
 * Copyright (C) 2026 Licensed under GPL v2 or later.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <string.h>     /* memset(3), strerror(3), */
#include <errno.h>      /* E*, */
#include <sched.h>      /* CPU_SETSIZE, */
#include <talloc.h>     /* talloc_zero(3), talloc_get_type_abort(3), */

#include "extension/resource_limit/resource_limit.h"
#include "extension/resource_limit/resource_limit_internal.h"
#include "extension/extension.h"
#include "tracee/tracee.h"
#include "tracee/reg.h"
#include "tracee/mem.h"
#include "syscall/sysnum.h"
#include "syscall/seccomp.h"
#include "cli/note.h"

#include "attribute.h"

/* cpu_limit value stored by the --cpu-limit/--single-core/
 * --resource-isolated handlers in cli/proot.c.  Declared locally (the
 * same extern pattern as execve/exit.c uses for the --mem-limit getter)
 * to avoid pulling the whole generated CLI table.  */
extern int resource_config_cpu_limit(void);

/* proc_limit value stored by the --proc-limit handler in cli/proot.c.  */
extern int resource_config_proc_limit(void);

/* Syscalls filtered by this extension.  FILTER_SYSEXIT: the real
 * sched_getaffinity() must run first so the kernel resolves the pid
 * and returns the byte count; the mask is rewritten at
 * SYSCALL_EXIT_END.  flags == 0 (ENTER-only): fork/clone/vfork are
 * intercepted before they execute, so the process-count check can
 * answer -EAGAIN without the child ever being created.  Registered
 * via extension->filtered_sysnums, i.e. only when the extension is
 * initialized (cpu_limit or proc_limit > 0).
 *
 * PR_fork/PR_vfork only exist on some ABIs (x86_64, i386, ...); on
 * arm64 they are discarded by detranslate_sysnum() (SYSCALL_AVOIDER)
 * and every thread/process creation goes through PR_clone, which is
 * already traced by proot_sysnums anyway.  PR_clone3 is included for
 * the same reason (clone3(2) uses it on newer libcs).  */
static FilteredSysnum syss[] = {
	{ PR_sched_getaffinity, FILTER_SYSEXIT },
	{ PR_clone, 0 },
	{ PR_clone3, 0 },
	{ PR_fork, 0 },
	{ PR_vfork, 0 },
	FILTERED_SYSNUM_END,
};

/**
 * Rewrite the mask returned by sched_getaffinity() so that only the
 * first @config->cpu_limit CPUs (bits 0..cpu_limit-1) are reported to
 * the guest.  Called at SYSCALL_EXIT_END, after the real syscall ran
 * successfully: the guest-visible return value (number of bytes copied)
 * is left untouched and only the buffer content is limited.
 *
 * Returns 0 (never fails the syscall: on any error here the original
 * result is kept so the guest still sees a successful call).
 */
static int rlimit_handle_sched_getaffinity(Tracee *tracee, const RlimitConfig *config)
{
	word_t result;
	word_t cpusetsize;
	word_t mask_addr;
	word_t n;
	uint8_t buf[CPU_SETSIZE / 8];
	int i;
	int status;

	/* Only rewrite successful calls: on error the tracee's buffer
	 * was not written by the kernel and must stay untouched.  */
	result = peek_reg(tracee, CURRENT, SYSARG_RESULT);
	if ((intptr_t)result < 0)
		return 0;

	/* Config guard (defense in depth: the extension is only
	 * initialized when cpu_limit > 0, but keep the check).  */
	if (config == NULL || config->cpu_limit <= 0)
		return 0;

	/* int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask);
	 *
	 * @result holds how many bytes the kernel copied, which is at
	 * most cpusetsize and never more than sizeof(cpu_set_t).  Never
	 * write more bytes than the tracee's buffer can hold, and cap at
	 * the local buffer size (CPU_SETSIZE / 8).  */
	cpusetsize = peek_reg(tracee, ORIGINAL, SYSARG_2);
	mask_addr   = peek_reg(tracee, ORIGINAL, SYSARG_3);

	n = result;
	if (n > cpusetsize)
		n = cpusetsize;
	if (n > sizeof(buf))
		n = sizeof(buf);
	if (n == 0)
		return 0;

	/* Build the limited mask: only the first cpu_limit bits are set
	 * (bits 0..cpu_limit-1).  Bits beyond the copied bytes are not
	 * reported by the kernel anyway, so they are simply left clear.  */
	memset(buf, 0, sizeof(buf));
	for (i = 0; i < config->cpu_limit; i++) {
		if (i / 8 < (int)n)
			buf[i / 8] |= (uint8_t)(1U << (i % 8));
	}

	status = write_data(tracee, mask_addr, buf, n);
	if (status < 0) {
		/* Rewriting the mask failed (shouldn't happen after a
		 * successful syscall): keep the original result, the
		 * guest still sees success.  */
		VERBOSE(tracee, 1, "resource_limit: write_data(mask) failed: %s",
			strerror(-status));
		return 0;
	}

	VERBOSE(tracee, 4, "resource_limit: sched_getaffinity reports %d CPU(s)",
		config->cpu_limit);
	return 0;
}

/**
 * Count the live tracees of this proot instance.
 *
 * Proot tracks every guest process AND thread as a Tracee (it sets
 * PTRACE_O_TRACEFORK|TRACEVFORK|TRACECLONE), so counting the entries
 * of the global tracees list that are not terminated yet gives the
 * exact number of processes+threads currently running inside the
 * guest.  This is consistent with the kernel's RLIMIT_NPROC, which
 * also counts threads.
 *
 * The caller tracee is part of the list (it is alive), so the count
 * includes the process that is about to fork/clone — matching the
 * natural RLIMIT_NPROC semantics where the current task already
 * counts towards the limit.
 *
 * @return the number of live tracees, or 0 if the list is empty.
 */
static int rlimit_count_live_tracees(void)
{
	Tracees *list = get_tracees_list_head();
	Tracee *tracee;
	int count = 0;

	if (list == NULL)
		return 0;

	LIST_FOREACH(tracee, list, link) {
		if (!tracee->terminated)
			count++;
	}

	return count;
}

/**
 * Enforce the --proc-limit N guest-side process cap at fork/clone/
 * vfork enter time.
 *
 * When @config->proc_limit > 0 and the number of live tracees already
 * reaches the limit, the syscall is voided and answered with -EAGAIN,
 * the exact error the kernel returns when RLIMIT_NPROC is exceeded.
 * The child is never created, so it can't spawn a runaway process
 * tree inside the guest.
 *
 * The check happens at SYSCALL_ENTER_START (before the syscall runs):
 * the new child does not exist yet, so the count reflects the current
 * number of processes.  If count >= proc_limit the fork is refused.
 *
 * @return 1 when the syscall was voided (handled), 0 to pass through.
 */
static int rlimit_handle_fork_clone_enter(Tracee *tracee, const RlimitConfig *config)
{
	int count;

	if (config == NULL || config->proc_limit <= 0)
		return 0;

	count = rlimit_count_live_tracees();
	if (count < config->proc_limit)
		return 0;

	/* Limit reached: emulate "Resource temporarily unavailable",
	 * the natural kernel error when RLIMIT_NPROC is exceeded.  */
	set_sysnum(tracee, PR_void);
	poke_reg(tracee, SYSARG_RESULT, -EAGAIN);
	VERBOSE(tracee, 1,
		"resource_limit: fork/clone refused (EAGAIN): %d live tracees, "
		"proc-limit %d", count, config->proc_limit);
	return 1;
}

int rlimit_callback(Extension *extension, ExtensionEvent event,
		    intptr_t data1 UNUSED, intptr_t data2 UNUSED)
{
	switch (event) {
	case INITIALIZATION: {
		RlimitConfig *config;

		/* Allocate config as child of extension (like virtual_net).  */
		config = talloc_zero(extension, RlimitConfig);
		if (config == NULL)
			return -ENOMEM;

		/* The CLI handler stored cpu_limit/proc_limit in
		 * resource_config *before* calling rlimit_configure(), so
		 * the getters already reflect the requested values.  */
		config->cpu_limit = resource_config_cpu_limit();
		config->proc_limit = resource_config_proc_limit();

		extension->config = config;
		extension->filtered_sysnums = syss;

		VERBOSE(TRACEE(extension), 1,
			"resource_limit: guest perceives %d CPU(s), %d process(es)",
			config->cpu_limit, config->proc_limit);
		return 0;
	}

	case SYSCALL_ENTER_START: {
		Tracee *tracee = TRACEE(extension);
		RlimitConfig *config = talloc_get_type_abort(extension->config, RlimitConfig);
		Sysnum num = get_sysnum(tracee, CURRENT);

		switch (num) {
		case PR_clone:
		case PR_clone3:
		case PR_fork:
		case PR_vfork:
			return rlimit_handle_fork_clone_enter(tracee, config);
		default:
			return 0;
		}
	}

	case SYSCALL_EXIT_END: {
		Tracee *tracee = TRACEE(extension);
		RlimitConfig *config = talloc_get_type_abort(extension->config, RlimitConfig);

		switch (get_sysnum(tracee, ORIGINAL)) {
		case PR_sched_getaffinity:
			return rlimit_handle_sched_getaffinity(tracee, config);
		default:
			return 0;
		}
	}

	case INHERIT_PARENT:
		/* Inheritable with a shared configuration: every tracee of
		 * this proot instance (fork/exec children) perceives the
		 * same limited mask.  */
		return 0;

	case REMOVED:
	default:
		return 0;
	}
}

int rlimit_configure(Tracee *tracee)
{
	RlimitConfig *config;
	Extension *extension;

	/* Nothing to do when no CPU and no process limit was requested:
	 * the guest keeps seeing the real machine, with zero overhead (no
	 * extension, no seccomp entry for sched_getaffinity/fork/clone).  */
	if (resource_config_cpu_limit() <= 0
	    && resource_config_proc_limit() <= 0)
		return 0;

	/* Double-init guard + "last option wins": if the extension is
	 * already running (e.g. --cpu-limit 4 followed by
	 * --proc-limit 8), refresh its config instead of creating a
	 * second extension (same pattern as proc_isolation's
	 * handle_proc_isolation_flag).  */
	extension = get_extension(tracee, rlimit_callback);
	if (extension != NULL) {
		config = talloc_get_type_abort(extension->config, RlimitConfig);
		config->cpu_limit = resource_config_cpu_limit();
		config->proc_limit = resource_config_proc_limit();
		VERBOSE(tracee, 2,
			"resource_limit: updated guest perception to %d CPU(s), "
			"%d process(es)", config->cpu_limit, config->proc_limit);
		return 0;
	}

	return initialize_extension(tracee, rlimit_callback, NULL);
}
