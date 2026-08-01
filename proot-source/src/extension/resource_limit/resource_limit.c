/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * resource_limit — guest-side perception of CPU resource limits.
 *
 * Intercepts sched_getaffinity() so that a tracee only perceives the
 * first N CPUs, where N comes from the Fase 1 flags --cpu-limit N,
 * --single-core and --resource-isolated (stored in the static
 * resource_config in cli/proot.c and exposed through
 * resource_config_cpu_limit()).
 *
 * The real syscall is left to run ("emulate, never deny"): the kernel
 * resolves pid 0, applies the real (already restricted) affinity and
 * returns the byte count; only the mask content is rewritten at
 * SYSCALL_EXIT_END so the guest sees bits 0..N-1.  When cpu_limit is
 * not set the extension is never initialized, so there is zero seccomp
 * overhead (PR_sched_getaffinity is only filtered through
 * extension->filtered_sysnums).
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

/* Syscalls filtered by this extension.  FILTER_SYSEXIT: the real
 * sched_getaffinity() must run first so the kernel resolves the pid
 * and returns the byte count; the mask is rewritten at
 * SYSCALL_EXIT_END.  Registered via extension->filtered_sysnums, i.e.
 * only when the extension is initialized (cpu_limit > 0).  */
static FilteredSysnum syss[] = {
	{ PR_sched_getaffinity, FILTER_SYSEXIT },
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

		/* The CLI handler stored cpu_limit in resource_config
		 * *before* calling rlimit_configure(), so the getter
		 * already reflects the requested value.  */
		config->cpu_limit = resource_config_cpu_limit();

		extension->config = config;
		extension->filtered_sysnums = syss;

		VERBOSE(TRACEE(extension), 1,
			"resource_limit: guest perceives %d CPU(s)", config->cpu_limit);
		return 0;
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

	/* Nothing to do when no CPU limit was requested: the guest keeps
	 * seeing the real machine, with zero overhead (no extension, no
	 * seccomp entry for sched_getaffinity).  */
	if (resource_config_cpu_limit() <= 0)
		return 0;

	/* Double-init guard + "last option wins": if the extension is
	 * already running (e.g. --resource-isolated followed by
	 * --cpu-limit 4), refresh its config instead of creating a
	 * second extension (same pattern as proc_isolation's
	 * handle_proc_isolation_flag).  */
	extension = get_extension(tracee, rlimit_callback);
	if (extension != NULL) {
		config = talloc_get_type_abort(extension->config, RlimitConfig);
		config->cpu_limit = resource_config_cpu_limit();
		VERBOSE(tracee, 2, "resource_limit: updated guest CPU perception to %d",
			config->cpu_limit);
		return 0;
	}

	return initialize_extension(tracee, rlimit_callback, NULL);
}
