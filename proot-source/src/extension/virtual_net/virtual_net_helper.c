/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * Virtual Network Helper Process
 *
 * Runs as a separate process (re-exec'd proot --vnp-helper NAME).
 * Handles TCP→Unix bridging for exposed ports (-p HOST:VIRTUAL).
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
#include <string.h>     /* str*(3), */
#include <stdlib.h>     /* atoi(3), */
#include <unistd.h>     /* close(2), read(2), write(2) */
#include <errno.h>      /* E*, */
#include <sys/socket.h> /* AF_*, SOCK_*, socket(2), bind(2), listen(2), accept(2), connect(2) */
#include <sys/un.h>     /* struct sockaddr_un, */
#include <netinet/in.h> /* struct sockaddr_in, htons(3) */
#include <sys/select.h> /* select(2) */
#include <stdio.h>      /* snprintf(3) */
#include <signal.h>     /* signal(2) */

#include "extension/virtual_net/virtual_net_internal.h"

/* ================================================================
 * Globals
 * ================================================================ */

static char g_proxy_name[VNP_MAX_NAME];

/* ================================================================
 * Helpers
 * ================================================================ */

/**
 * Send a VnpResponse to stdout (back to tracer).
 */
static void helper_send_response(int result, uint16_t host_port)
{
	struct VnpResponse resp;
	memset(&resp, 0, sizeof(resp));
	resp.result = result;
	resp.host_port = host_port;
	write(STDOUT_FILENO, &resp, sizeof(resp));
}

/**
 * Build abstract Unix socket name for a virtual port.
 * Name: "\0proot-vnet-{proxy_name}-{port}"
 */
static void build_abstract_name(char *sun_path, size_t pathlen,
                                 const char *proxy_name, uint16_t port)
{
	/* Abstract socket: first byte is '\0' */
	sun_path[0] = '\0';
	snprintf(&sun_path[1], pathlen - 1, "%s%s-%u",
		VNP_ABSTRACT_PREFIX, proxy_name, port);
}

/* ================================================================
 * Bridge: bidirectional data forwarding between two fds
 * ================================================================ */

/**
 * Bridge data between client_fd (TCP) and unix_fd (abstract Unix socket).
 * Uses select() for bidirectional forwarding.
 * Returns when either side closes or errors.
 */
static void bridge_fds(int client_fd, int unix_fd)
{
	int maxfd = (client_fd > unix_fd ? client_fd : unix_fd) + 1;
	char buf[4096];

	while (1) {
		fd_set rfds;
		struct timeval tv;
		int ret;

		FD_ZERO(&rfds);
		FD_SET(client_fd, &rfds);
		FD_SET(unix_fd, &rfds);
		tv.tv_sec = 30;  /* 30s timeout */
		tv.tv_usec = 0;

		ret = select(maxfd, &rfds, NULL, NULL, &tv);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (ret == 0)
			break;  /* timeout */

		/* Client → Unix socket */
		if (FD_ISSET(client_fd, &rfds)) {
			ssize_t n = read(client_fd, buf, sizeof(buf));
			if (n <= 0)
				break;
			if (write(unix_fd, buf, n) != n)
				break;
		}

		/* Unix socket → Client */
		if (FD_ISSET(unix_fd, &rfds)) {
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
 * EXPOSE handler
 * ================================================================ */

/**
 * Handle EXPOSE command:
 * 1. Create TCP listener on 0.0.0.0:host_port
 * 2. Connect to abstract Unix socket @proxy_name-virtual_port
 * 3. Fork child to bridge data
 */
static void helper_handle_expose(uint16_t host_port, uint16_t virtual_port)
{
	int tcp_fd;
	struct sockaddr_in tcp_addr;
	int optval = 1;
	struct sockaddr_un unix_addr;
	int unix_fd;
	pid_t pid;

	/* Create TCP listener */
	tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (tcp_fd < 0) {
		helper_send_response(-errno, host_port);
		return;
	}

	setsockopt(tcp_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	memset(&tcp_addr, 0, sizeof(tcp_addr));
	tcp_addr.sin_family = AF_INET;
	tcp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	tcp_addr.sin_port = htons(host_port);

	if (bind(tcp_fd, (struct sockaddr *)&tcp_addr, sizeof(tcp_addr)) < 0) {
		close(tcp_fd);
		helper_send_response(-errno, host_port);
		return;
	}

	if (listen(tcp_fd, 16) < 0) {
		close(tcp_fd);
		helper_send_response(-errno, host_port);
		return;
	}

	/* Success — tell tracer */
	helper_send_response(0, host_port);

	/* Accept loop: for each connection, connect to abstract Unix socket and bridge */
	while (1) {
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		int client_fd;

		client_fd = accept(tcp_fd, (struct sockaddr *)&client_addr, &client_len);
		if (client_fd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		/* Connect to the abstract Unix socket */
		unix_fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (unix_fd < 0) {
			close(client_fd);
			continue;
		}

		memset(&unix_addr, 0, sizeof(unix_addr));
		unix_addr.sun_family = AF_UNIX;
		build_abstract_name(unix_addr.sun_path, sizeof(unix_addr.sun_path),
			g_proxy_name, virtual_port);

		if (connect(unix_fd, (struct sockaddr *)&unix_addr,
			    sizeof(unix_addr)) < 0) {
			close(client_fd);
			close(unix_fd);
			continue;
		}

		/* Fork a child to bridge this connection */
		pid = fork();
		if (pid == 0) {
			/* Child: bridge the two fds */
			close(tcp_fd);  /* Don't need the listener */
			bridge_fds(client_fd, unix_fd);
			_exit(0);
		}

		/* Parent: close the connection fds (child owns them) */
		close(client_fd);
		close(unix_fd);
	}

	close(tcp_fd);
}

/* ================================================================
 * Main event loop
 * ================================================================ */

/**
 * Helper main entry point.
 * Called from cli.c when proot is invoked with --vnp-helper NAME.
 */
int vnp_helper_main(int argc, char *argv[])
{
	struct VnpRequest req;
	struct sigaction sa;

	if (argc < 3) {
		_exit(1);
	}

	/* Store proxy name */
	strncpy(g_proxy_name, argv[2], VNP_MAX_NAME - 1);
	g_proxy_name[VNP_MAX_NAME - 1] = '\0';

	/* Ignore SIGCHLD to auto-reap bridged children */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = SA_NOCLDWAIT;
	sigaction(SIGCHLD, &sa, NULL);

	/* Send HELLO response */
	{
		struct VnpResponse resp;
		memset(&resp, 0, sizeof(resp));
		resp.result = 0;
		write(STDOUT_FILENO, &resp, sizeof(resp));
	}

	/* Main loop: read requests from stdin */
	while (read(STDIN_FILENO, &req, sizeof(req)) == sizeof(req)) {
		switch (req.opcode) {
		case VNP_EXPOSE:
			helper_handle_expose(req.host_port, req.virtual_port);
			break;
		case VNP_UNEXPOSE:
			/* TODO: remove exposed port */
			helper_send_response(0, req.host_port);
			break;
		case VNP_BYE:
			helper_send_response(0, 0);
			_exit(0);
			break;
		default:
			helper_send_response(-EINVAL, 0);
			break;
		}
	}

	_exit(0);
	return 0;
}
