/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * Supervise mode implementation for PRoot
 *
 * --supervise:
 *   Uses signalfd + poll() instead of blocking waitpid() so the event loop
 *   can also accept new tracee requests via an abstract Unix socket.
 *   When the root tracee exits, logs the reason and keeps running until
 *   all exec clients finish, then exits cleanly.
 *
 * --exec:
 *   Connects to a running supervisor's abstract socket, sends the command
 *   to execute, and blocks until the tracee finishes. Returns the exit code.
 *   If the supervisor is not found, reads its exit log from tmpdir.
 *
 * Copyright (C) 2025 Licensed under GPL v2 or later.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <string.h>     /* str*(3), */
#include <stdlib.h>     /* atoi(3), */
#include <stdio.h>      /* snprintf(3), fopen(3), */
#include <unistd.h>     /* close(2), read(2), write(2), fork(2), execvp(3) */
#include <errno.h>      /* E*, */
#include <sys/socket.h> /* AF_UNIX, SOCK_STREAM, socket(2), bind(2), listen(2), accept(2), connect(2) */
#include <sys/un.h>     /* struct sockaddr_un, */
#include <sys/signalfd.h> /* signalfd(2), */
#include <signal.h>     /* sigaddset(3), sigprocmask(3), */
#include <poll.h>       /* poll(2), */
#include <sys/wait.h>   /* waitpid(2), WIFEXITED, WIFSIGNALED, WEXITSTATUS, WTERMSIG */
#include <time.h>       /* time(2), */
#include <stdbool.h>    /* bool, true, false */
#include <sys/stat.h>   /* mkdir(2) */
#include <linux/sched.h> /* CLONE_VM, CLONE_FS */
#include <inttypes.h>    /* PRIu64 */
#include <sys/uio.h>    /* iovec, writev(2) */

#include <assert.h>

#include "supervise/supervise.h"
#include "cli/note.h"
#include "tracee/tracee.h"
#include "tracee/mem.h"
#include "path/binding.h"
#include "extension/extension.h"

/* NULL tracee for VERBOSE calls in this file.
 * We use a typed variable instead of NULL literal because the Android NDK
 * defines NULL as (void*)0, and the VERBOSE macro accesses tracee->verbose
 * which the compiler rejects on void* even with short-circuit evaluation. */
static const Tracee *supervise_tracee;

/* ===========================================================================
 * Internal state
 * =========================================================================== */

/* A pending exec client: we're waiting for its tracee to finish */
typedef struct {
	int   fd;           /* Client socket (for sending response) */
	pid_t tracee_pid;   /* PID of the spawned tracee */
	bool  active;       /* Still waiting for completion */
} ExecClientEntry;

static ExecClientEntry exec_clients[SUPERVISE_MAX_CLIENTS];
static int             num_exec_clients = 0;

static int      ctl_fd_global = -1;  /* Listen socket for incoming --exec */
static pid_t    own_pid = 0;         /* Our PID (for socket name) */
static time_t   start_time = 0;      /* When supervise started */

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */

static void build_sockaddr(struct sockaddr_un *sa, pid_t pid)
{
	memset(sa, 0, sizeof(*sa));
	sa->sun_family = AF_UNIX;
	sa->sun_path[0] = '\0';
	snprintf(&sa->sun_path[1], sizeof(sa->sun_path) - 1,
		 "%s%u", SUPERVISE_SOCKET_PREFIX, pid);
}

static void build_logpath(char *buf, size_t bufsz, pid_t pid)
{
	snprintf(buf, bufsz, "%s/%s%d.log",
		 SUPERVISE_TMP_DIR, SUPERVISE_LOG_PREFIX, pid);
}

static int add_client(int fd, pid_t tracee_pid)
{
	int i;
	for (i = 0; i < SUPERVISE_MAX_CLIENTS; i++) {
		if (!exec_clients[i].active) {
			exec_clients[i].fd          = fd;
			exec_clients[i].tracee_pid  = tracee_pid;
			exec_clients[i].active      = true;
			if (i >= num_exec_clients)
				num_exec_clients = i + 1;
			return 0;
		}
	}
	return -1; /* Too many clients */
}

static void remove_client(int index)
{
	if (index >= 0 && index < num_exec_clients) {
		exec_clients[index].active = false;
		/* Shrink num_exec_clients if last slot */
		while (num_exec_clients > 0 && !exec_clients[num_exec_clients - 1].active)
			num_exec_clients--;
	}
}

/* ===========================================================================
 * supervise_init: create signalfd + abstract listen socket
 * =========================================================================== */

int supervise_init(int *ctl_fd, int *sig_fd, int verbose_level)
{
	struct sockaddr_un sa;
	int fd, ret;
	sigset_t mask;

	if (ctl_fd == NULL || sig_fd == NULL)
		return -1;

	own_pid = getpid();
	start_time = time(NULL);

	/* --- Create signalfd for SIGCHLD --- */
	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &mask, NULL);

	fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (fd < 0) {
		note(NULL, WARNING, INTERNAL,
		     "supervise: signalfd: %s", strerror(errno));
		return -1;
	}
	*sig_fd = fd;

	/* --- Create abstract listen socket --- */
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		note(NULL, WARNING, INTERNAL,
		     "supervise: socket: %s", strerror(errno));
		close(*sig_fd);
		*sig_fd = -1;
		return -1;
	}

	build_sockaddr(&sa, own_pid);

	/* Remove stale socket (shouldn't exist, be safe) */
	unlink(sa.sun_path);

	ret = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
	if (ret < 0) {
		note(NULL, WARNING, INTERNAL,
		     "supervise: bind: %s", strerror(errno));
		close(fd);
		close(*sig_fd);
		*sig_fd = -1;
		return -1;
	}

	ret = listen(fd, 5);
	if (ret < 0) {
		note(NULL, WARNING, INTERNAL,
		     "supervise: listen: %s", strerror(errno));
		close(fd);
		close(*sig_fd);
		*sig_fd = -1;
		return -1;
	}

	ctl_fd_global = fd;
	*ctl_fd = fd;

	if (verbose_level >= 1)
		note(NULL, INFO, INTERNAL,
		     "supervise: listening on @proot-exec-%u", own_pid);

	return 0;
}

/* ===========================================================================
 * supervise_fini: cleanup
 * =========================================================================== */

void supervise_fini(void)
{
	/* Free any pending exec child tracees to avoid talloc leaks */
	{
		int i;
		for (i = 0; i < num_exec_clients; i++) {
			if (exec_clients[i].active) {
				kill(exec_clients[i].tracee_pid, SIGKILL);
				waitpid(exec_clients[i].tracee_pid, NULL, WNOHANG);
				close(exec_clients[i].fd);
				exec_clients[i].active = false;
			}
		}
		num_exec_clients = 0;
	}

	int i;
	/* Close all pending client sockets */
	for (i = 0; i < num_exec_clients; i++) {
		if (exec_clients[i].active) {
			close(exec_clients[i].fd);
			exec_clients[i].active = false;
		}
	}
	num_exec_clients = 0;

	if (ctl_fd_global >= 0) {
		close(ctl_fd_global);
		ctl_fd_global = -1;
	}
}

/* ===========================================================================
 * supervise_accept_client: accept a new --exec connection
 * =========================================================================== */

void supervise_accept_client(int ctl_fd, Tracee *root_tracee)
{
	int client_fd;
	ExecRequest req;
	pid_t pid;
	client_fd = accept(ctl_fd, NULL, NULL);
	if (client_fd < 0)
		return;

	/* Receive stdin/stdout/stderr from client via SCM_RIGHTS */
	int client_fds[EXEC_FD_MAX] = { -1, -1, -1 };
	{
		struct msghdr msg;
		struct iovec iov;
		char cmsgbuf[CMSG_SPACE(EXEC_FD_MAX * sizeof(int))];
		char dummy;
		ssize_t ret;

		memset(&msg, 0, sizeof(msg));
		iov.iov_base = &dummy;
		iov.iov_len  = 1;
		msg.msg_iov        = &iov;
		msg.msg_iovlen     = 1;
		msg.msg_control    = cmsgbuf;
		msg.msg_controllen = sizeof(cmsgbuf);

		ret = recvmsg(client_fd, &msg, 0);
		if (ret >= 0) {
			struct cmsghdr *cmsg;
			for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
			     cmsg = CMSG_NXTHDR(&msg, cmsg)) {
				if (cmsg->cmsg_level == SOL_SOCKET
				    && cmsg->cmsg_type == SCM_RIGHTS) {
					int count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
					if (count > EXEC_FD_MAX) count = EXEC_FD_MAX;
					memcpy(client_fds, CMSG_DATA(cmsg), count * sizeof(int));
					break;
				}
			}
		}
	}

	/* Read the exec request (now proceed normally) */
	ssize_t n = read(client_fd, &req, sizeof(req));
	if (n != sizeof(req) || req.argc < 1) {
		close(client_fd);
		return;
	}

	/* Parse argv from the null-separated buffer */
	char *argv_ptrs[256];
	int argc = 0;
	char *p = req.argv;
	char *end = req.argv + sizeof(req.argv);

	while (argc < req.argc && argc < 255 && p < end) {
		argv_ptrs[argc++] = p;
		p += strlen(p) + 1;
	}
	argv_ptrs[argc] = NULL;

	if (argc < 1) {
		close(client_fd);
		return;
	}

	VERBOSE(root_tracee, 2, "supervise: exec request: %s", argv_ptrs[0]);

	/* Fork a new tracee */
	pid = fork();
	if (pid < 0) {
		close(client_fd);
		return;
	}

	if (pid == 0) {
		/* Child: becomes a new tracee */
		ptrace(PTRACE_TRACEME, 0, NULL, NULL);
		raise(SIGSTOP);  /* Wait for parent to attach */

		/* Forward client's stdin/stdout/stderr to this process */
		if (client_fds[0] >= 0) dup2(client_fds[0], STDIN_FILENO);
		if (client_fds[1] >= 0) dup2(client_fds[1], STDOUT_FILENO);
		if (client_fds[2] >= 0) dup2(client_fds[2], STDERR_FILENO);

		/* Change to requested cwd if provided */
		if (req.cwd[0] != '\0')
			chdir(req.cwd);

		/* Exec the command */
		execvp(argv_ptrs[0], argv_ptrs);
		_exit(127);
	}

	/* Parent: close received fds (only the child needs them) */
	{
		int i;
		for (i = 0; i < EXEC_FD_MAX; i++) {
			if (client_fds[i] >= 0)
				close(client_fds[i]);
		}
	}

	/* Parent: create a proper Tracee struct with inherited context.
	 * This is essential for path translation, bindings, and extensions
	 * (like virtual_net proxy) to work for the --exec command. */
	{
		Tracee *child_tracee = get_tracee(NULL, pid, true);
		if (child_tracee != NULL) {
			/* Sanity check: should be a fresh tracee */
			assert(child_tracee->exe == NULL);
			assert(child_tracee->parent == NULL);

			/* Inherit basic flags */
			child_tracee->verbose  = root_tracee->verbose;
			/* Disable seccomp for exec children: the child process was
		 * forked from the supervisor and did NOT install the seccomp
		 * BPF filter (enable_syscall_filtering() was not called).
		 * If we inherit seccomp=ENABLED, restart_tracee() uses
		 * PTRACE_CONT and expects kernel seccomp events that never
		 * come, so syscalls are not intercepted.
		 * By setting SECCOMP_DISABLED, we force PTRACE_SYSCALL
		 * which intercepts every syscall for path translation. */
		child_tracee->seccomp = DISABLED;
			child_tracee->tool_name = root_tracee->tool_name;

			/* The child just did PTRACE_TRACEME + raise(SIGSTOP).
			 * Set sigstop to PENDING so the event loop knows
			 * to expect and handle this initial stop properly. */
			/* Copy filesystem namespace with bindings.
			 * This gives the child the same rootfs, cwd,
			 * and bindings as the supervisor. */
			TALLOC_FREE(child_tracee->fs);
			child_tracee->fs = talloc_zero(child_tracee, FileSystemNameSpace);
			if (child_tracee->fs != NULL && root_tracee->fs != NULL) {
				child_tracee->fs->cwd = talloc_strdup(child_tracee->fs,
					root_tracee->fs->cwd ?: ".");

				/* Deep-copy bindings (same as new_child
				 * with CLONE_NEWNS stripped) */
				if (root_tracee->fs->bindings.guest != NULL) {
					Binding *iter;

					child_tracee->fs->bindings.guest = talloc_zero(child_tracee->fs, Bindings);
					child_tracee->fs->bindings.host  = talloc_zero(child_tracee->fs, Bindings);
					if (child_tracee->fs->bindings.guest != NULL
					    && child_tracee->fs->bindings.host != NULL) {
						CIRCLEQ_INIT(child_tracee->fs->bindings.guest);
						CIRCLEQ_INIT(child_tracee->fs->bindings.host);

						CIRCLEQ_FOREACH(iter, root_tracee->fs->bindings.guest, link.guest) {
							(void) insort_binding3(child_tracee, child_tracee->fs,
										iter->host.path,
										iter->guest.path);
						}
					}
				}
			}

			/* Copy heap (memory management context) */
			TALLOC_FREE(child_tracee->heap);
			child_tracee->heap = talloc_memdup(child_tracee,
				root_tracee->heap, sizeof(Heap));

			/* Reference shared resources */
			child_tracee->exe   = talloc_reference(child_tracee, root_tracee->exe);
			child_tracee->qemu  = talloc_reference(child_tracee, root_tracee->qemu);
			child_tracee->glue  = talloc_reference(child_tracee, root_tracee->glue);

			/* Set parent relationship */
			child_tracee->parent = root_tracee;

			/* Inherit extensions (virtual_net, fake_id0, etc.) */
			inherit_extensions(child_tracee, root_tracee, CLONE_VM | CLONE_FS);

			VERBOSE(child_tracee, 2, "exec tracee: vpid %" PRIu64 ": pid %d",
				child_tracee->vpid, child_tracee->pid);
		}
	}

	/* Track this client */
	if (add_client(client_fd, pid) < 0) {
		/* Too many clients, kill the tracee and reject */
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		close(client_fd);
		return;
	}

	VERBOSE(root_tracee, 2, "supervise: spawned tracee pid=%d for client fd=%d",
		pid, client_fd);
}

/* ===========================================================================
 * supervise_tracee_exited: called when a tracee exits
 * =========================================================================== */

int supervise_tracee_exited(Tracee *root_tracee, pid_t pid, int status)
{
	ExecResponse resp;
	int i;

	/* Find which client owns this tracee */
	for (i = 0; i < num_exec_clients; i++) {
		if (exec_clients[i].active && exec_clients[i].tracee_pid == pid) {
			/* Build response */
			memset(&resp, 0, sizeof(resp));
			if (WIFEXITED(status)) {
				resp.exit_status = WEXITSTATUS(status);
				resp.signaled = false;
				resp.termsig = 0;
			} else if (WIFSIGNALED(status)) {
				resp.exit_status = 128 + WTERMSIG(status);
				resp.signaled = true;
				resp.termsig = WTERMSIG(status);
			}

			/* Send response to client */
			write(exec_clients[i].fd, &resp, sizeof(resp));
			close(exec_clients[i].fd);
			remove_client(i);

			VERBOSE(root_tracee, 2, "supervise: tracee pid=%d exited: status=%d",
				pid, resp.exit_status);

			break;
		}
	}

	return num_exec_clients;
}

/* ===========================================================================
 * supervise_log_exit: write exit log for --exec to read
 * =========================================================================== */

void supervise_log_exit(const char *who, int status)
{
	char path[SUPERVISE_MAX_PATH];
	FILE *f;

	build_logpath(path, sizeof(path), own_pid);
	f = fopen(path, "w");
	if (f == NULL)
		return;

	if (WIFEXITED(status)) {
		fprintf(f, "process '%s' exited with status %d (started %lds)",
			who ? who : "unknown", WEXITSTATUS(status),
			(long)(time(NULL) - start_time));
	} else if (WIFSIGNALED(status)) {
		fprintf(f, "process '%s' killed by signal %d (started %lds)",
			who ? who : "unknown", WTERMSIG(status),
			(long)(time(NULL) - start_time));
	} else {
		fprintf(f, "process '%s' terminated unexpectedly (started %lds)",
			who ? who : "unknown",
			(long)(time(NULL) - start_time));
	}

	fclose(f);
}

/* ===========================================================================
 * supervise_pending_clients
 * =========================================================================== */

int supervise_pending_clients(void)
{
	return num_exec_clients;
}

/* ===========================================================================
 * exec_connect: called by proot --exec mode
 * =========================================================================== */

int exec_connect(pid_t target_pid, int argc, char *const argv[])
{
	struct sockaddr_un sa;
	ExecRequest req;
	ExecResponse resp;
	int fd;
	ssize_t n;
	int i;
	char *p;

	/* Build the abstract socket address */
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	sa.sun_path[0] = '\0';
	snprintf(&sa.sun_path[1], sizeof(sa.sun_path) - 1,
		 "%s%u", SUPERVISE_SOCKET_PREFIX, target_pid);

	/* Connect to the supervisor */
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		goto try_log;

	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		fd = -1;
		goto try_log;
	}

	/* --- Send stdin/stdout/stderr via SCM_RIGHTS --- */
	{
		struct msghdr msg;
		struct iovec iov;
		char cmsgbuf[CMSG_SPACE(EXEC_FD_MAX * sizeof(int))];
		struct cmsghdr *cmsg;
		char dummy = 0;
		int fds[EXEC_FD_MAX] = { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };

		memset(&msg, 0, sizeof(msg));

		/* We need at least 1 byte of data to send ancillary data */
		iov.iov_base = &dummy;
		iov.iov_len  = 1;
		msg.msg_iov        = &iov;
		msg.msg_iovlen     = 1;
		msg.msg_control    = cmsgbuf;
		msg.msg_controllen = sizeof(cmsgbuf);

		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type  = SCM_RIGHTS;
		cmsg->cmsg_len   = CMSG_LEN(EXEC_FD_MAX * sizeof(int));
		memcpy(CMSG_DATA(cmsg), fds, EXEC_FD_MAX * sizeof(int));

		if (sendmsg(fd, &msg, 0) < 0) {
			close(fd);
			errno = EIO;
			return -1;
		}
	}

	/* Build the exec request */
	memset(&req, 0, sizeof(req));
	req.argc = argc;

	p = req.argv;
	for (i = 0; i < argc; i++) {
		size_t remaining = sizeof(req.argv) - (p - req.argv);
		size_t len = strlen(argv[i]) + 1;
		if (len > remaining) {
			close(fd);
			errno = E2BIG;
			return -1;
		}
		memcpy(p, argv[i], len);
		p += len;
	}

	/* Send request */
	if (write(fd, &req, sizeof(req)) != sizeof(req)) {
		close(fd);
		errno = EIO;
		return -1;
	}

	/* Wait for response (blocks until the tracee finishes) */
	n = read(fd, &resp, sizeof(resp));
	close(fd);

	if (n != sizeof(resp)) {
		errno = EIO;
		return -1;
	}

	return resp.exit_status;

try_log:
	/* Supervisor not reachable — try reading its exit log */
	{
		char logpath[SUPERVISE_MAX_PATH];
		char buf[256];
		FILE *f;
		int saved_errno = errno;

		build_logpath(logpath, sizeof(logpath), target_pid);
		f = fopen(logpath, "r");
		if (f != NULL) {
			if (fgets(buf, sizeof(buf), f) != NULL) {
				/* Remove trailing newline */
				size_t blen = strlen(buf);
				if (blen > 0 && buf[blen - 1] == '\n')
					buf[blen - 1] = '\0';
				fprintf(stderr, "supervise: target exited: %s\n", buf);
			}
			fclose(f);
			errno = ESRCH;
			return -1;
		}

		/* Neither socket nor log found */
		errno = saved_errno;
		fprintf(stderr, "supervise: no target process %d found\n", target_pid);
		return -1;
	}
}
