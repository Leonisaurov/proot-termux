/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * Virtual Network Helper Process
 *
 * Runs as a separate process (re-exec'd proot --vnp-helper NAME [TOKEN]).
 * Bridges TCP connections to abstract Unix sockets for -p HOST:VIRTUAL.
 *
 * Uses centralized poll() loop to multiplex stdin commands + TCP listeners.
 * This avoids deadlock with multiple -p (each expose adds a listener).
 *
 * Protocol (pipe stdin/stdout):
 *   HELLO   (0x01) - handshake on startup
 *   EXPOSE  (0x50) - create TCP listener + bridge to abstract Unix socket
 *   UNEXPOSE(0x51) - remove exposed port
 *   BYE     (0xFF) - shutdown
 *
 * Copyright (C) 2025 Licensed under GPL v2 or later.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <signal.h>
#include <sys/wait.h>

#include "extension/virtual_net/virtual_net_internal.h"

#define BRIDGE_BUF_SIZE 4096

/* ================================================================
 * Globals
 * ================================================================ */

static char g_proxy_name[VNP_MAX_NAME];
static uint32_t g_instance_token;

/* Active listeners: exposed TCP ports we're accepting on */
static int      g_listener_fds[VNP_EXPOSE_MAX];
static uint16_t g_listener_vports[VNP_EXPOSE_MAX]; /* virtual_port for each listener */
static uint16_t g_listener_hports[VNP_EXPOSE_MAX]; /* host_port for each listener */
static int      g_num_listeners = 0;



/* ================================================================
 * IPC with tracer
 * ================================================================ */

static void helper_send_response(int result, uint16_t host_port)
{
	struct VnpResponse resp;
	memset(&resp, 0, sizeof(resp));
	resp.result = result;
	resp.host_port = host_port;
	if (write(STDOUT_FILENO, &resp, sizeof(resp)) != sizeof(resp))
		_exit(1);
}

/* ================================================================
 * Abstract socket name
 * ================================================================ */

static void build_abstract_name(char *sun_path, size_t pathlen,
                                 const char *proxy_name, uint16_t port,
                                 uint32_t token)
{
	int len;
	sun_path[0] = '\0';
	len = snprintf(&sun_path[1], pathlen - 1, "%s%s-%u-%u",
		VNP_ABSTRACT_PREFIX, proxy_name, port, token);
	if ((size_t)len >= pathlen - 1)
		sun_path[pathlen - 1] = '\0';
}

/* ================================================================
 * Bridge: bidirectional data forwarding between two fds
 * ================================================================ */

static void bridge_fds(int client_fd, int unix_fd)
{
	struct pollfd fds[2];
	char buf[BRIDGE_BUF_SIZE];

	fds[0].fd = client_fd;
	fds[0].events = POLLIN;
	fds[1].fd = unix_fd;
	fds[1].events = POLLIN;

	while (1) {
		int ret = poll(fds, 2, -1);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (ret == 0)
			continue; /* should not happen with -1 timeout */

		/* Client (TCP) ? Unix socket */
		if (fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLRDHUP)) {
			if (fds[0].revents & (POLLHUP | POLLERR | POLLRDHUP))
				break;
			ssize_t n = read(client_fd, buf, sizeof(buf));
			if (n <= 0)
				break;
			if (write(unix_fd, buf, n) != n)
				break;
		}

		/* Unix socket ? Client (TCP) */
		if (fds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLRDHUP)) {
			if (fds[1].revents & (POLLHUP | POLLERR | POLLRDHUP))
				break;
			ssize_t n = read(unix_fd, buf, sizeof(buf));
			if (n <= 0)
				break;
			if (write(client_fd, buf, n) != n)
				break;
		}
	}

	close(client_fd);
	close(unix_fd);
}

/* ================================================================
 * Create TCP listener
 * ================================================================ */

static int create_tcp_listener(uint16_t host_port)
{
	int tcp_fd;
	struct sockaddr_in tcp_addr;
	int optval = 1;

	tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (tcp_fd < 0)
		return -errno;

	if (setsockopt(tcp_fd, SOL_SOCKET, SO_REUSEADDR,
		       &optval, sizeof(optval)) < 0) {
		int saved = errno;
		close(tcp_fd);
		return -saved;
	}

	memset(&tcp_addr, 0, sizeof(tcp_addr));
	tcp_addr.sin_family = AF_INET;
	tcp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	tcp_addr.sin_port = htons(host_port);

	if (bind(tcp_fd, (struct sockaddr *)&tcp_addr, sizeof(tcp_addr)) < 0) {
		int saved = errno;
		close(tcp_fd);
		return -saved;
	}

	if (listen(tcp_fd, 16) < 0) {
		int saved = errno;
		close(tcp_fd);
		return -saved;
	}

	return tcp_fd;
}

/* ================================================================
 * Accept a single connection on a listener and fork a bridge child
 * ================================================================ */

static void accept_and_fork(int listener_idx)
{
	int tcp_fd = g_listener_fds[listener_idx];
	uint16_t virtual_port = g_listener_vports[listener_idx];
	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);
	int client_fd;
	struct sockaddr_un unix_addr;
	int unix_fd;
	pid_t pid;

	client_fd = accept(tcp_fd, (struct sockaddr *)&client_addr, &client_len);
	if (client_fd < 0)
		return;

	/* Connect to the abstract Unix socket */
	unix_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (unix_fd < 0) {
		close(client_fd);
		return;
	}

	memset(&unix_addr, 0, sizeof(unix_addr));
	unix_addr.sun_family = AF_UNIX;
	build_abstract_name(unix_addr.sun_path, sizeof(unix_addr.sun_path),
		g_proxy_name, virtual_port, g_instance_token);

	if (connect(unix_fd, (struct sockaddr *)&unix_addr,
		    sizeof(unix_addr)) < 0) {
		close(client_fd);
		close(unix_fd);
		return;
	}

	pid = fork();
	if (pid < 0) {
		close(client_fd);
		close(unix_fd);
		return;
	}

	if (pid == 0) {
		/* Child: bridge the two fds */
		close(tcp_fd);
		bridge_fds(client_fd, unix_fd);
		_exit(0);
	}

	/* Parent: close connection fds (child owns them) */
	close(client_fd);
	close(unix_fd);
}

/* ================================================================
 * Handle EXPOSE: create TCP listener, add to poll set
 * ================================================================ */

static void helper_handle_expose(uint16_t host_port, uint16_t virtual_port)
{
	int tcp_fd;

	if (g_num_listeners >= VNP_EXPOSE_MAX) {
		helper_send_response(-EMFILE, host_port);
		return;
	}

	tcp_fd = create_tcp_listener(host_port);
	if (tcp_fd < 0) {
		helper_send_response(tcp_fd, host_port);
		return;
	}

	g_listener_fds[g_num_listeners] = tcp_fd;
	g_listener_vports[g_num_listeners] = virtual_port;
	g_listener_hports[g_num_listeners] = host_port;
	g_num_listeners++;

	helper_send_response(0, host_port);
}

/* ================================================================
 * Handle UNEXPOSE: remove TCP listener from poll set
 * ================================================================ */

static void helper_handle_unexpose(uint16_t host_port)
{
	int i;
	for (i = 0; i < g_num_listeners; i++) {
		if (g_listener_hports[i] == host_port) {
			close(g_listener_fds[i]);
			/* swap-with-last removal */
			g_listener_fds[i] = g_listener_fds[g_num_listeners - 1];
			g_listener_vports[i] = g_listener_vports[g_num_listeners - 1];
			g_listener_hports[i] = g_listener_hports[g_num_listeners - 1];
			g_num_listeners--;
			helper_send_response(0, host_port);
			return;
		}
	}
	helper_send_response(-ENOENT, host_port);
}

/* ================================================================
 * Main event loop (poll-based: stdin commands + TCP listeners)
 * ================================================================ */

int vnp_helper_main(int argc, char *argv[])
{
	struct sigaction sa;

	if (argc < 3)
		_exit(1);

	strncpy(g_proxy_name, argv[2], VNP_MAX_NAME - 1);
	g_proxy_name[VNP_MAX_NAME - 1] = '\0';
	if (argc > 3)
		g_instance_token = (uint32_t)strtoul(argv[3], NULL, 10);

	/* Ignore SIGCHLD: auto-reap bridge children */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = SA_NOCLDWAIT;
	if (sigaction(SIGCHLD, &sa, NULL) < 0)
		_exit(1);

	/* Send HELLO response */
	{
		struct VnpResponse resp;
		memset(&resp, 0, sizeof(resp));
		resp.result = 0;
		if (write(STDOUT_FILENO, &resp, sizeof(resp)) != sizeof(resp))
			_exit(1);
	}

	/* Main poll loop: multiplex stdin + all TCP listeners */
	while (1) {
		struct pollfd fds[1 + VNP_EXPOSE_MAX];
		int nfds = 0;
		int ret;
		int i;

		/* stdin (commands from tracer) */
		fds[nfds].fd = STDIN_FILENO;
		fds[nfds].events = POLLIN;
		nfds++;

		/* TCP listeners (one per exposed port) */
		for (i = 0; i < g_num_listeners; i++) {
			fds[nfds].fd = g_listener_fds[i];
			fds[nfds].events = POLLIN;
			nfds++;
		}

		ret = poll(fds, nfds, -1);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		/* Handle command from tracer */
		if (fds[0].revents & (POLLIN | POLLHUP)) {
			if (fds[0].revents & POLLHUP)
				break; /* pipe closed */
			while (1) {
				struct VnpRequest req;
				ssize_t n = read(STDIN_FILENO, &req, sizeof(req));
				if (n == 0)
					goto done;
				if (n < 0) {
					if (errno == EINTR)
						continue;
					goto done;
				}
				if ((size_t)n != sizeof(req))
					goto done;

				switch (req.opcode) {
				case VNP_EXPOSE:
					helper_handle_expose(req.host_port, req.virtual_port);
					break;
				case VNP_UNEXPOSE:
					helper_handle_unexpose(req.host_port);
					break;
				case VNP_BYE:
					helper_send_response(0, 0);
					goto done;
				default:
					helper_send_response(-EINVAL, 0);
					break;
				}
				break; /* one command per poll cycle */
			}
		}

		/* Handle incoming connections on TCP listeners */
		for (i = 0; i < g_num_listeners; i++) {
			int fd_idx = i + 1; /* +1 because stdin is at index 0 */
			if (fd_idx < nfds && (fds[fd_idx].revents & POLLIN))
				accept_and_fork(i);
		}


	}

done:
{
	int i;
	/* Close all listeners on exit */
	for (i = 0; i < g_num_listeners; i++)
		close(g_listener_fds[i]);
}
	_exit(0);
	return 0;
}
