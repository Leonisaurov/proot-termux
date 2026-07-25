/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * Supervise mode for PRoot
 *
 * Implements --supervise and --exec for managing multiple tracees
 * within the same proot context. Uses abstract Unix sockets for
 * communication between proot instances.
 *
 * --supervise: persists the event loop after root tracee exits,
 *              listens on an abstract socket for --exec requests.
 * --exec: connects to a running supervisor and spawns a new
 *         tracee within its context (same rootfs, bindings, proxy).
 *
 * Copyright (C) 2025 Licensed under GPL v2 or later.
 */

#ifndef SUPERVISE_H
#define SUPERVISE_H

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include "tracee/tracee.h"

/* ===========================================================================
 * Paths & Limits
 * =========================================================================== */

#define SUPERVISE_SOCKET_PREFIX	 "proot-exec-"
#define SUPERVISE_TMP_DIR	 "/data/data/com.termux/files/usr/tmp"
#define SUPERVISE_LOG_PREFIX	 "proot-exit-"
#define SUPERVISE_MAX_CLIENTS	 16
#define SUPERVISE_MAX_COMMAND	 4096
#define SUPERVISE_MAX_PATH	 4096

/* ===========================================================================
 * Protocol: exec request / response (client ↔ supervisor via socket)
 * =========================================================================== */

typedef struct {
	int    argc;
	char   argv[SUPERVISE_MAX_COMMAND];  /* All args, null-separated */
	char   cwd[SUPERVISE_MAX_PATH];
} ExecRequest;

typedef struct {
	int    exit_status;  /* WEXITSTATUS or 128+signal */
	bool   signaled;     /* true if killed by signal */
	int    termsig;      /* signal number if signaled */
} ExecResponse;

/* ===========================================================================
 * SCM_RIGHTS: file descriptor passing for stdin/stdout/stderr
 * =========================================================================== */

/* Maximum number of fds we can send via SCM_RIGHTS */
#define EXEC_FD_MAX 3

/* ===========================================================================
 * Supervisor API (called by proot event loop when --supervise is active)
 * =========================================================================== */

/**
 * Initialize supervisor mode.
 * Creates the abstract listen socket, signalfd for SIGCHLD.
 * Returns 0 on success, -1 on error.
 * On success, *ctl_fd and *sig_fd are populated.
 */
extern int supervise_init(int *ctl_fd, int *sig_fd, int verbose_level);

/**
 * Shutdown supervisor mode.
 * Closes the listen socket, removes the log file.
 */
extern void supervise_fini(void);

/**
 * Accept a new exec client connection and spawn its tracee.
 * Called when ctl_fd has POLLIN.
 */
extern void supervise_accept_client(int ctl_fd, Tracee *root_tracee);

/**
 * Handle completed tracee in supervise mode.
 * Checks if the tracee was spawned by an exec client.
 * If so, sends the exit status back to the client.
 * Returns the number of clients still pending, or 0 if none.
 */
extern int supervise_tracee_exited(Tracee *root_tracee, pid_t pid, int status);

/**
 * Log the exit reason of the root tracee.
 */
extern void supervise_log_exit(const char *who, int status);

/**
 * Get the number of pending exec clients.
 */
extern int supervise_pending_clients(void);

/* ===========================================================================
 * Exec API (called by proot --exec mode)
 * =========================================================================== */

/**
 * Connect to a running supervisor and execute a command.
 * target_pid: PID of the proot --supervise instance.
 * argc/argv: command to execute inside the supervisor context.
 *
 * Returns the exit code (0-255) on success, or -1 on error.
 * On error, errno is set appropriately.
 */
extern int exec_connect(pid_t target_pid, int argc, char *const argv[]);

#endif /* SUPERVISE_H */
