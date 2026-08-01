#ifndef RESOURCE_LIMIT_INTERNAL_H
#define RESOURCE_LIMIT_INTERNAL_H

#include <stdint.h>  /* int*_t, */
#include <stdbool.h> /* bool, */

/* CPU limit value meaning "not set" (== the default of
 * resource_config.cpu_limit in cli/proot.c).  The extension is only
 * initialized when this is > 0.  */
#define RLIMIT_DEFAULT_CPU_LIMIT 0

/* ========================================================================= */
/*  Extension configuration (talloc'd per tracee)                            */
/* ========================================================================= */

typedef struct {
	/* Number of CPUs the guest is allowed to perceive
	 * (--cpu-limit N / --single-core / --resource-isolated).
	 * 0 = not set: sched_getaffinity is passed through untouched.  */
	int cpu_limit;
} RlimitConfig;

#endif /* RESOURCE_LIMIT_INTERNAL_H */
