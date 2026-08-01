#ifndef RESOURCE_LIMIT_H
#define RESOURCE_LIMIT_H

#include "extension/extension.h"

/**
 * Resource limit extension callback (guest-side perception).
 *
 * Intercepts sched_getaffinity() so that a tracee perceives only the
 * first N CPUs, where N comes from --cpu-limit N / --single-core /
 * --resource-isolated (stored in resource_config by the Fase 1 CLI
 * handlers in cli/proot.c).  With cpu_limit == 0 the syscall is passed
 * through untouched (zero overhead).
 *
 * Per the "emulate, never deny" philosophy the real syscall is left to
 * run: the kernel is the one that resolves pid 0 and returns the byte
 * count, and only the mask content is rewritten (SYSCALL_EXIT_END).
 */
extern int rlimit_callback(Extension *extension, ExtensionEvent event,
                           intptr_t data1, intptr_t data2);

/**
 * Enable the guest-side CPU perception for @tracee when a CPU limit was
 * requested on the command line.  Reads resource_config_cpu_limit()
 * (cli/proot.c) and, if > 0, initializes the extension (or refreshes
 * the already-running config, so that "the last option wins").
 *
 * Called from the Fase 1 handlers --cpu-limit, --single-core and
 * --resource-isolated right after they stored the value.
 *
 * @return 0 on success, -errno/-1 on failure.
 */
extern int rlimit_configure(Tracee *tracee);

#endif /* RESOURCE_LIMIT_H */
