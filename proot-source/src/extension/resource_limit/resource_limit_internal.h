#ifndef RESOURCE_LIMIT_INTERNAL_H
#define RESOURCE_LIMIT_INTERNAL_H

#include <stdint.h>  /* int*_t, */
#include <stdbool.h> /* bool, */

/* CPU limit value meaning "not set" (== the default of
 * resource_config.cpu_limit in cli/proot.c).  The extension is only
 * initialized when this is > 0.  */
#define RLIMIT_DEFAULT_CPU_LIMIT 0

/* Process limit value meaning "not set" (== the default of
 * resource_config.proc_limit in cli/proot.c).  The extension is only
 * initialized when cpu_limit or proc_limit is > 0.  */
#define RLIMIT_DEFAULT_PROC_LIMIT 0

/* ========================================================================= */
/*  Extension configuration (talloc'd per tracee)                            */
/* ========================================================================= */

typedef struct {
	/* Number of CPUs the guest is allowed to perceive
	 * (--cpu-limit N / --single-core / --resource-isolated).
	 * 0 = not set: sched_getaffinity is passed through untouched.  */
	int cpu_limit;

	/* Maximum number of live tracees (guest processes + threads)
	 * of this proot instance (--proc-limit N).  fork(2)/clone(2)/
	 * vfork(2) calls that would push the count over this limit are
	 * answered with -EAGAIN, the same natural error the kernel
	 * returns when RLIMIT_NPROC is exceeded.  0 = not set: fork/
	 * clone/vfork are passed through untouched.  This is a
	 * guest-side limit: it only counts this proot's own tracees,
	 * so the rest of the Termux UID is never affected.  */
	int proc_limit;
} RlimitConfig;

#endif /* RESOURCE_LIMIT_INTERNAL_H */
