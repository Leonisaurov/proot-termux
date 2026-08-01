#ifndef RESOURCE_LIMIT_H
#define RESOURCE_LIMIT_H

#include "extension/extension.h"

/**
 * Resource limit extension callback (guest-side perception).
 *
 * CPU side: intercepts sched_getaffinity() so that a tracee perceives
 * only the first N CPUs, where N comes from --cpu-limit N /
 * --single-core / --resource-isolated (stored in resource_config by
 * the Fase 1 CLI handlers in cli/proot.c).  With cpu_limit == 0 the
 * syscall is passed through untouched (zero overhead).
 *
 * Process side: intercepts fork(2)/clone(2)/vfork(2) so that
 * --proc-limit N caps the number of live tracees of this proot
 * instance.  With proc_limit == 0 these syscalls are passed through
 * untouched.  The limit only counts this sandbox's processes, so the
 * rest of the Termux UID is never affected.
 *
 * Per the "emulate, never deny" philosophy the real sched_getaffinity
 * is left to run: the kernel is the one that resolves pid 0 and
 * returns the byte count, and only the mask content is rewritten
 * (SYSCALL_EXIT_END).  For the process limit, an exceeded fork/clone/
 * vfork is answered with the natural -EAGAIN the kernel would return
 * for an exceeded RLIMIT_NPROC.
 */
extern int rlimit_callback(Extension *extension, ExtensionEvent event,
                           intptr_t data1, intptr_t data2);

/**
 * Enable the guest-side resource limit perception for @tracee when a
 * CPU and/or process limit was requested on the command line.  Reads
 * resource_config_cpu_limit() and resource_config_proc_limit()
 * (cli/proot.c) and, if any is > 0, initializes the extension (or
 * refreshes the already-running config, so that "the last option
 * wins").
 *
 * Called from the Fase 1 handlers --cpu-limit, --single-core,
 * --proc-limit and --resource-isolated right after they stored the
 * value.
 *
 * @return 0 on success, -errno/-1 on failure.
 */
extern int rlimit_configure(Tracee *tracee);

#endif /* RESOURCE_LIMIT_H */
