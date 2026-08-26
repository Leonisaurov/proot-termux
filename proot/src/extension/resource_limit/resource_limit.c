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

/* Syscalls filtered by this extension.  The array is built dynamically
 * (see rlimit_build_sysnums) so that only the dimensions actually
 * requested add seccomp entries: with cpu_limit == 0 nothing about
 * sched_getaffinity is filtered and with proc_limit == 0 nothing about
 * fork/clone/vfork is, keeping the seccomp filter minimal per
 * dimension (zero overhead for an unused one).
 *
 * FILTER_SYSEXIT (sched_getaffinity): the real syscall must run first
 * so the kernel resolves the pid and returns the byte count; the mask
 * is rewritten at SYSCALL_EXIT_END.  flags == 0 (ENTER-only) for
 * fork/clone/vfork: they are intercepted before they execute, so the
 * process-count check can answer -EAGAIN without the child ever being
 * created.
 *
 * PR_fork/PR_vfork only exist on some ABIs (x86_64, i386, ...); on
 * arm64 they are discarded by detranslate_sysnum() (SYSCALL_AVOIDER)
 * and every thread/process creation goes through PR_clone, which is
 * already traced by proot_sysnums anyway.  PR_clone3 is included for
 * the same reason (clone3(2) uses it on newer libcs).  Registered via
 * extension->filtered_sysnums, i.e. only when the extension is
 * initialized (cpu_limit or proc_limit > 0).
 *
 * @return a talloc array (child of @extension, last entry the
 *         FILTERED_SYSNUM_END terminator), or NULL on allocation
 *         failure.
 */
static FilteredSysnum *rlimit_build_sysnums(Extension *extension,
					    const RlimitConfig *config)
{
	FilteredSysnum *sysnums;
	int count = 0;
	int idx = 0;

	/* Count the entries: one per active dimension.  */
	if (config->cpu_limit > 0)
		count++;
	if (config->proc_limit > 0)
		count += 4;

	/* count + 1 for the terminator.  */
	sysnums = talloc_array(extension, FilteredSysnum, count + 1);
	if (sysnums == NULL)
		return NULL;

	if (config->cpu_limit > 0) {
		sysnums[idx].value = PR_sched_getaffinity;
		sysnums[idx].flags = FILTER_SYSEXIT;
		idx++;
	}

	if (config->proc_limit > 0) {
		sysnums[idx].value = PR_clone;
		sysnums[idx].flags = 0;
		idx++;
		sysnums[idx].value = PR_clone3;
		sysnums[idx].flags = 0;
		idx++;
		sysnums[idx].value = PR_fork;
		sysnums[idx].flags = 0;
		idx++;
		sysnums[idx].value = PR_vfork;
		sysnums[idx].flags = 0;
		idx++;
	}

	/* Terminator (FILTERED_SYSNUM_END).  */
	sysnums[idx].value = PR_void;
	sysnums[idx].flags = 0;

	return sysnums;
}

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
 * Count the live tracees of this proot instance, stopping as soon as
 * @limit live tracees are found (early exit).
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
 * The early-exit (break once count >= limit) turns the walk into
 * O(limit) instead of O(N): the caller only needs to know whether the
 * limit was reached, not the exact total, so a low limit never pays
 * for walking a long tracees list.
 *
 * @param limit the process limit; counting stops as soon as this many
 *        live tracees are found (<= 0 returns 0 immediately).
 * @return the number of live tracees found (possibly capped at
 *         @limit), or 0 if the list is empty.
 */
static int rlimit_count_live_tracees(int limit)
{
	Tracees *list = get_tracees_list_head();
	Tracee *tracee;
	int count = 0;

	if (list == NULL || limit <= 0)
		return 0;

	LIST_FOREACH(tracee, list, link) {
		if (!tracee->terminated)
			count++;

		/* Early exit: no point walking the rest of the list once
		 * the limit is reached — count is guaranteed >= limit.  */
		if (count >= limit)
			break;
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

	count = rlimit_count_live_tracees(config->proc_limit);
	if (count < config->proc_limit)
		return 0;

	/* Limit reached: emulate "Resource temporarily unavailable",
	 * the natural kernel error when RLIMIT_NPROC is exceeded.  */
	set_sysnum(tracee, PR_void);
	poke_reg(tracee, SYSARG_RESULT, -EAGAIN);
	VERBOSE(tracee, 1,
		"resource_limit: fork/clone refused (EAGAIN): %d live tracees seen, "
		"proc-limit %d", count, config->proc_limit);
	return 1;
}

int rlimit_callback(Extension *extension, ExtensionEvent event,
		    intptr_t data1 UNUSED, intptr_t data2 UNUSED)
{
	switch (event) {
	case INITIALIZATION: {
		RlimitConfig *config;
		FilteredSysnum *sysnums;

		/* Allocate config as child of extension (like virtual_net).  */
		config = talloc_zero(extension, RlimitConfig);
		if (config == NULL)
			return -ENOMEM;

		/* The CLI handler stored cpu_limit/proc_limit in
		 * resource_config *before* calling rlimit_configure(), so
		 * the getters already reflect the requested values.  */
		config->cpu_limit = resource_config_cpu_limit();
		config->proc_limit = resource_config_proc_limit();

		/* Build the filtered syscall set dynamically: only the
		 * syscalls of the active dimensions add seccomp entries
		 * (see rlimit_build_sysnums).  */
		sysnums = rlimit_build_sysnums(extension, config);
		if (sysnums == NULL)
			return -ENOMEM;

		extension->config = config;
		extension->filtered_sysnums = sysnums;

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
		FilteredSysnum *sysnums;

		config = talloc_get_type_abort(extension->config, RlimitConfig);
		config->cpu_limit = resource_config_cpu_limit();
		config->proc_limit = resource_config_proc_limit();

		/* Rebuild the filtered syscall set: "last option wins" may
		 * have just activated a dimension (e.g. --cpu-limit 2
		 * --proc-limit 4), and the set must reflect the current
		 * config before the seccomp filter is installed
		 * (launch_process() runs after parse_config(), so the
		 * refresh always lands before enable_syscall_filtering()).
		 * Without this, a cpu-only first option would leave
		 * PR_clone/PR_clone3/PR_fork/PR_vfork unfiltered and, on
		 * ABIs where fork/vfork are not in proot_sysnums, the
		 * proc-limit would be bypassed.  The old array is a talloc
		 * child of the extension, so freeing it and replacing it
		 * keeps the memory tied to the extension's lifetime.  */
		sysnums = rlimit_build_sysnums(extension, config);
		if (sysnums == NULL) {
			VERBOSE(tracee, 1,
				"resource_limit: failed to rebuild the filtered syscall set");
			return -ENOMEM;
		}
		talloc_free((void *) extension->filtered_sysnums);
		extension->filtered_sysnums = sysnums;

		VERBOSE(tracee, 2,
			"resource_limit: updated guest perception to %d CPU(s), "
			"%d process(es)", config->cpu_limit, config->proc_limit);
		return 0;
	}

	return initialize_extension(tracee, rlimit_callback, NULL);
}

/**
 * Host-side --proc-limit gate: tell a host-side tracee creator (the
 * supervisor's --exec handler) whether a new process would be refused
 * right now.
 *
 * Tracees spawned via --supervise/--exec are forked by the proot
 * process itself and therefore never pass through the guest-side
 * fork/clone/vfork interception (R3 gap in the original --proc-limit
 * design).  This helper lets the supervisor apply the SAME cap at
 * creation time: when a proc-limit is active (> 0) and the number of
 * live tracees of this proot instance already reaches it, the caller
 * must refuse to spawn (EAGAIN) instead of forking.
 *
 * Reads resource_config_proc_limit() directly (not the extension's
 * config): the config in cli/proot.c is the single source of truth and
 * is always populated when --proc-limit was parsed, even if the
 * extension failed to initialize.  When no limit was set it returns
 * false immediately, so the normal unlimited --exec flow is preserved
 * with zero overhead.
 *
 * @return true when a new process would be refused (limit active and
 * reached), false otherwise.
 */
bool rlimit_proc_limit_reached(void)
{
	int limit = resource_config_proc_limit();

	/* No --proc-limit requested: the gate never applies.  */
	if (limit <= 0)
		return false;

	/* The count helper takes the limit for its early exit: it stops
	 * walking the tracees list as soon as @limit live tracees are
	 * found, so this is O(limit), not O(N).  */
	return rlimit_count_live_tracees(limit) >= limit;
}
