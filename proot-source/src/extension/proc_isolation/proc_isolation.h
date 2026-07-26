#ifndef PROC_ISOLATION_H
#define PROC_ISOLATION_H

#include "extension/extension.h"

/* Individual isolation flags — shared with proot.c CLI handlers */
#define ISOLATE_PROC      (1 << 0)
#define ISOLATE_PTRACE    (1 << 1)
#define ISOLATE_REBOOT    (1 << 2)
#define ISOLATE_SWAP      (1 << 3)
#define ISOLATE_KEXEC     (1 << 4)
#define ISOLATE_IOPORT    (1 << 5)
#define ISOLATE_BPF       (1 << 6)
#define ISOLATE_PERF      (1 << 7)
#define ISOLATE_HANDLE    (1 << 8)

typedef struct {
    unsigned int flags;
    int          proc_fd_count;
    char       **reboot_argv;
    int          reboot_argc;
} HpcConfig;

extern int hpc_callback(Extension *extension, ExtensionEvent event,
                         intptr_t data1, intptr_t data2);

#endif /* PROC_ISOLATION_H */
