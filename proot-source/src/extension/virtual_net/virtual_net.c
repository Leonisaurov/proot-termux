/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * Virtual Network Extension for PRoot
 *
 * Implements isolated virtual networking using Abstract Unix Domain Sockets.
 * When --proxy NAME is active:
 *   - socket(AF_INET) is silently changed to AF_UNIX
 *   - bind/connect translate to abstract Unix socket names
 *   - getsockname/getpeername fake AF_INET results
 *   - listen/accept work naturally on abstract sockets
 *   - No real TCP ports are used (except via -p expose)
 *
 * Copyright (C) 2025 Licensed under GPL v2 or later.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <string.h>     /* str*(3), */
#include <stdlib.h>     /* atoi(3), */
#include <unistd.h>     /* close(2), read(2), write(2), fork(2), execve(2) */
#include <errno.h>      /* E*, */
#include <sys/socket.h> /* AF_*, SOCK_*, */
#include <sys/un.h>     /* struct sockaddr_un, */
#include <netinet/in.h> /* struct sockaddr_in, htons(3), ntohs(3) */
#include <stdio.h>      /* snprintf(3), */
#include <sys/stat.h>   /* mkdir(2) */
#include <signal.h>     /* kill(2) */
#include <sys/wait.h>  /* waitpid(2) */

#include "extension/extension.h"
#include "extension/virtual_net/virtual_net.h"
#include "extension/virtual_net/virtual_net_internal.h"
#include "tracee/tracee.h"
#include "tracee/mem.h"
#include "tracee/reg.h"
#include "syscall/syscall.h"
#include "cli/note.h"
#include "attribute.h"
#include "syscall/seccomp.h"

#include "arch.h"
#include "path/path.h"

/**
 * Filtered sysnums for this extension.
 */
static FilteredSysnum syss[] = {
	{ PR_socket,      0 },
	{ PR_bind,        0 },
	{ PR_listen,      0 },
	{ PR_accept,      0 },
	{ PR_accept4,     0 },
	{ PR_connect,     0 },
	{ PR_close,       0 },
	{ PR_getsockname, 0 },
	{ PR_getpeername, 0 },
	{ PR_getsockopt,  0 },
	{ PR_setsockopt,  0 },
	FILTERED_SYSNUM_END,
};

/* ================================================================
 * Helper process management
 * ================================================================ */

static int vnp_start_helper(VnpConfig *config, const char *proxy_name)
{
	int pipe_main2helper[2];
	int pipe_helper2main[2];
	pid_t pid;

	if (pipe(pipe_main2helper) < 0 || pipe(pipe_helper2main) < 0)
		return -1;

	pid = fork();
	if (pid < 0) {
		close(pipe_main2helper[0]); close(pipe_main2helper[1]);
		close(pipe_helper2main[0]); close(pipe_helper2main[1]);
		return -1;
	}

	if (pid == 0) {
		/* Child: become the helper */
		char namebuf[VNP_MAX_NAME];
		close(pipe_main2helper[1]);
		close(pipe_helper2main[0]);
		dup2(pipe_main2helper[0], STDIN_FILENO);
		dup2(pipe_helper2main[1], STDOUT_FILENO);
		close(pipe_main2helper[0]);
		close(pipe_helper2main[1]);
		snprintf(namebuf, sizeof(namebuf), "%s", proxy_name);
		execl("/proc/self/exe", "proot", "--vnp-helper", namebuf, (char *)NULL);
		_exit(1);
	}

	/* Parent */
	close(pipe_main2helper[0]);
	close(pipe_helper2main[1]);

	config->helper_pid = pid;
	config->helper_pipe_in = pipe_main2helper[1];
	config->helper_pipe_out = pipe_helper2main[0];

	/* Wait for HELLO response */
	{
		struct VnpResponse resp;
		ssize_t n = read(config->helper_pipe_out, &resp, sizeof(resp));
		if (n != sizeof(resp) || resp.result != 0) {
			close(config->helper_pipe_in);
			close(config->helper_pipe_out);
			config->helper_pipe_in = -1;
			config->helper_pipe_out = -1;
			config->helper_pid = -1;
			return -1;
		}
	}

	return 0;
}

static int vnp_helper_request(VnpConfig *config, struct VnpRequest *req,
                               struct VnpResponse *resp)
{
	ssize_t n;
	if (config->helper_pipe_in < 0 || config->helper_pipe_out < 0)
		return -1;
	n = write(config->helper_pipe_in, req, sizeof(*req));
	if (n != sizeof(*req))
		return -1;
	n = read(config->helper_pipe_out, resp, sizeof(*resp));
	if (n != sizeof(*resp))
		return -1;
	return 0;
}

static void vnp_stop_helper(VnpConfig *config)
{
	if (config->helper_pid > 0) {
		struct VnpRequest req;
		struct VnpResponse resp;
		memset(&req, 0, sizeof(req));
		req.opcode = VNP_BYE;
		vnp_helper_request(config, &req, &resp);
		kill(config->helper_pid, SIGTERM);
		waitpid(config->helper_pid, NULL, 0);
		config->helper_pid = -1;
	}
	if (config->helper_pipe_in >= 0) {
		close(config->helper_pipe_in);
		config->helper_pipe_in = -1;
	}
	if (config->helper_pipe_out >= 0) {
		close(config->helper_pipe_out);
		config->helper_pipe_out = -1;
	}
}

/* ================================================================
 * Syscall handlers
 * ================================================================ */

/**
 * Handle socket() — change AF_INET to AF_UNIX.
 * The tracee thinks it creates a TCP socket, but gets a Unix socket.
 */
static int vnp_handle_socket(Tracee *tracee)
{
	word_t domain = peek_reg(tracee, CURRENT, SYSARG_1);
	if (domain == AF_INET || domain == AF_INET6) {
		poke_reg(tracee, SYSARG_1, AF_UNIX);
	}
	return 0;
}

/**
 * Handle bind() — translate to abstract Unix socket.
 * For virtual ports: bind to @proot-vnet-{name}-{port}
 */
static int vnp_handle_bind(Tracee *tracee, VnpConfig *config)
{
	word_t sockfd = peek_reg(tracee, CURRENT, SYSARG_1);
	word_t addr_ptr = peek_reg(tracee, CURRENT, SYSARG_2);
	word_t addrlen = peek_reg(tracee, CURRENT, SYSARG_3);
	struct sockaddr_in sa_in;
	VnpFdEntry *entry;
	struct sockaddr_un sa_unix;

	if (addrlen < sizeof(struct sockaddr_in))
		return 0;

	if (read_data(tracee, &sa_in, addr_ptr, sizeof(sa_in)) < 0)
		return 0;

	if (sa_in.sin_family != AF_INET)
		return 0;

	uint16_t port = ntohs(sa_in.sin_port);

	/* Check if this port is exposed via -p */
	int is_exposed = 0;
	int i;
	for (i = 0; i < config->expose_count; i++) {
		if (config->expose_map[i].virtual_port == port) {
			is_exposed = 1;
			break;
		}
	}

	/* Track this fd as virtual */
	entry = vnp_find_fd(config, sockfd);
	if (entry == NULL) {
		entry = vnp_add_fd(config, sockfd, port, AF_INET);
		if (entry == NULL)
			return 0;
	} else {
		entry->virtual_port = port;
		entry->orig_domain = AF_INET;
	}
	if (is_exposed)
		entry->exposed_port = port;

	/* Build abstract Unix socket address */
	vnp_fill_abstract_sa(&sa_unix, config->proxy_name, port);

	/* Write new sockaddr_un to tracee's stack and update bind() args */
	word_t new_addr = alloc_mem(tracee, sizeof(struct sockaddr_un));
	if (new_addr == 0)
		return 0;

	if (write_data(tracee, new_addr, &sa_unix, sizeof(sa_unix)) < 0)
		return 0;

	poke_reg(tracee, SYSARG_1, sockfd);
	poke_reg(tracee, SYSARG_2, new_addr);
	poke_reg(tracee, SYSARG_3, sizeof(struct sockaddr_un));

	return 0;
}

/**
 * Handle connect() — translate to abstract Unix socket for virtual ports.
 */
static int vnp_handle_connect(Tracee *tracee, VnpConfig *config)
{
	word_t sockfd = peek_reg(tracee, CURRENT, SYSARG_1);
	word_t addr_ptr = peek_reg(tracee, CURRENT, SYSARG_2);
	word_t addrlen = peek_reg(tracee, CURRENT, SYSARG_3);
	struct sockaddr_in sa_in;
	struct sockaddr_un sa_unix;
	VnpFdEntry *entry;

	if (addrlen < sizeof(struct sockaddr_in))
		return 0;

	if (read_data(tracee, &sa_in, addr_ptr, sizeof(sa_in)) < 0)
		return 0;

	if (sa_in.sin_family != AF_INET)
		return 0;

	/* Only intercept loopback connections */
	if ((ntohl(sa_in.sin_addr.s_addr) & 0xFF000000) != 0x7F000000)
		return 0;

	uint16_t port = ntohs(sa_in.sin_port);

	/* Check if this port is virtual */
	int is_virtual = 0;
	int i;
	for (i = 0; i < config->fd_count; i++) {
		if (config->fd_map[i].virtual_port == port) {
			is_virtual = 1;
			break;
		}
	}
	if (!is_virtual) {
		for (i = 0; i < config->expose_count; i++) {
			if (config->expose_map[i].virtual_port == port) {
				is_virtual = 1;
				break;
			}
		}
	}

	if (!is_virtual)
		return 0;

	/* Track this fd */
	entry = vnp_find_fd(config, sockfd);
	if (entry == NULL) {
		entry = vnp_add_fd(config, sockfd, port, AF_INET);
		if (entry == NULL)
			return 0;
	}

	/* Build abstract Unix socket address */
	vnp_fill_abstract_sa(&sa_unix, config->proxy_name, port);

	word_t new_addr = alloc_mem(tracee, sizeof(struct sockaddr_un));
	if (new_addr == 0)
		return 0;

	if (write_data(tracee, new_addr, &sa_unix, sizeof(sa_unix)) < 0)
		return 0;

	poke_reg(tracee, SYSARG_2, new_addr);
	poke_reg(tracee, SYSARG_3, sizeof(struct sockaddr_un));

	return 0;
}

/**
 * Handle close() — cleanup fd tracking.
 */
static int vnp_handle_close(Tracee *tracee, VnpConfig *config)
{
	word_t fd = peek_reg(tracee, CURRENT, SYSARG_1);
	vnp_remove_fd(config, (int)fd);
	return 0;
}

/**
 * Handle getsockname() — fake AF_INET result for virtual sockets.
 */
static int vnp_handle_getsockname(Tracee *tracee, VnpConfig *config)
{
	word_t sockfd = peek_reg(tracee, CURRENT, SYSARG_1);
	word_t addr_ptr = peek_reg(tracee, CURRENT, SYSARG_2);
	word_t addrlen_ptr = peek_reg(tracee, CURRENT, SYSARG_3);
	VnpFdEntry *entry;

	entry = vnp_find_fd(config, sockfd);
	if (entry == NULL)
		return 0;

	struct sockaddr_in fake;
	memset(&fake, 0, sizeof(fake));
	fake.sin_family = entry->orig_domain;
	fake.sin_port = htons(entry->virtual_port);
	fake.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (write_data(tracee, addr_ptr, &fake, sizeof(fake)) < 0)
		return 0;

	if (addrlen_ptr != 0) {
		word_t cur_len = peek_word(tracee, addrlen_ptr);
		if (cur_len >= sizeof(struct sockaddr_in))
			poke_word(tracee, addrlen_ptr, sizeof(struct sockaddr_in));
	}

	poke_reg(tracee, SYSARG_RESULT, 0);
	set_sysnum(tracee, PR_void);

	return 0;
}

/**
 * Handle getpeername() — fake AF_INET result for virtual sockets.
 */
static int vnp_handle_getpeername(Tracee *tracee, VnpConfig *config)
{
	word_t sockfd = peek_reg(tracee, CURRENT, SYSARG_1);
	word_t addr_ptr = peek_reg(tracee, CURRENT, SYSARG_2);
	word_t addrlen_ptr = peek_reg(tracee, CURRENT, SYSARG_3);
	VnpFdEntry *entry;

	entry = vnp_find_fd(config, sockfd);
	if (entry == NULL)
		return 0;

	struct sockaddr_in fake;
	memset(&fake, 0, sizeof(fake));
	fake.sin_family = entry->orig_domain;
	fake.sin_port = htons(entry->virtual_port);
	fake.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (write_data(tracee, addr_ptr, &fake, sizeof(fake)) < 0)
		return 0;

	if (addrlen_ptr != 0) {
		word_t cur_len = peek_word(tracee, addrlen_ptr);
		if (cur_len >= sizeof(struct sockaddr_in))
			poke_word(tracee, addrlen_ptr, sizeof(struct sockaddr_in));
	}

	poke_reg(tracee, SYSARG_RESULT, 0);
	set_sysnum(tracee, PR_void);

	return 0;
}

/* ================================================================
 * Expose management
 * ================================================================ */

static int vnp_send_expose(Tracee *tracee, VnpConfig *config,
                            uint16_t host_port, uint16_t virtual_port)
{
	struct VnpRequest req;
	struct VnpResponse resp;

	if (config->helper_pid <= 0) {
		if (vnp_start_helper(config, config->proxy_name) < 0) {
			note(tracee, ERROR, INTERNAL,
				"virtual_net: failed to start helper for %s",
				config->proxy_name);
			return -1;
		}
	}

	memset(&req, 0, sizeof(req));
	req.opcode = VNP_EXPOSE;
	req.virtual_port = virtual_port;
	req.host_port = host_port;

	if (vnp_helper_request(config, &req, &resp) < 0 || resp.result != 0) {
		note(tracee, WARNING, INTERNAL,
			"virtual_net: helper failed to expose port %d→%d",
			host_port, virtual_port);
		return -1;
	}

	VERBOSE(tracee, 2, "virtual_net: exposed %d → virtual %d",
		host_port, virtual_port);
	return 0;
}

/* ================================================================
 * Extension callback
 * ================================================================ */

int vnp_callback(Extension *extension, ExtensionEvent event,
                  intptr_t data1, intptr_t data2)
{
	switch (event) {
	case INITIALIZATION: {
		const char *proxy_name = (const char *)data1;
		VnpConfig *config;

		/* Allocate config as child of extension (like port_switch) */
		config = talloc_zero(extension, VnpConfig);
		if (config == NULL)
			return -1;

		strncpy(config->proxy_name, proxy_name, VNP_MAX_NAME - 1);
		config->proxy_name[VNP_MAX_NAME - 1] = '\0';
		config->helper_pid = -1;
		config->helper_pipe_in = -1;
		config->helper_pipe_out = -1;

		extension->config = config;
		extension->filtered_sysnums = syss;

		return 0;
	}

	case SYSCALL_ENTER_END: {
		Tracee *tracee = TRACEE(extension);
		VnpConfig *config = talloc_get_type_abort(extension->config, VnpConfig);

		switch (get_sysnum(tracee, CURRENT)) {
		case PR_socket:
			return vnp_handle_socket(tracee);
		case PR_bind:
			return vnp_handle_bind(tracee, config);
		case PR_connect:
			return vnp_handle_connect(tracee, config);
		case PR_close:
			return vnp_handle_close(tracee, config);
		case PR_getsockname:
			return vnp_handle_getsockname(tracee, config);
		case PR_getpeername:
			return vnp_handle_getpeername(tracee, config);
		default:
			return 0;
		}
	}

	case SYSCALL_EXIT_END: {
		Tracee *tracee = TRACEE(extension);
		VnpConfig *config = talloc_get_type_abort(extension->config, VnpConfig);

		if (get_sysnum(tracee, ORIGINAL) == PR_socket) {
			word_t result = peek_reg(tracee, CURRENT, SYSARG_RESULT);
			if ((intptr_t)result >= 0) {
				int fd = (int)result;
				VnpFdEntry *entry = vnp_find_fd(config, fd);
				if (entry == NULL) {
					vnp_add_fd(config, fd, 0, AF_INET);
				}
			}
		}
		return 0;
	}

	case REMOVED: {
		VnpConfig *config = talloc_get_type_abort(extension->config, VnpConfig);
		vnp_stop_helper(config);
		return 0;
	}

	default:
		return 0;
	}
}

/* ================================================================
 * Public API
 * ================================================================ */

int vnp_configure(Tracee *tracee, const char *proxy_name)
{
	int status;

	{
		void *ext = get_extension(tracee, vnp_callback);
		if (ext != NULL) {
			note(tracee, WARNING, USER,
				"--proxy was already specified, only the last one is used");
			TALLOC_FREE(ext);
		}
	}

	{
		char dirpath[VNP_SOCKBUF_LEN];
		vnp_net_path(proxy_name, dirpath, sizeof(dirpath));
		mkdir(VNP_TMP_DIR, 0755);
		mkdir(dirpath, 0755);
	}

	status = initialize_extension(tracee, vnp_callback, proxy_name);
	if (status < 0) {
		note(tracee, ERROR, INTERNAL,
			"virtual_net: failed to initialize for proxy '%s'",
			proxy_name);
		return -1;
	}

	VERBOSE(tracee, 2, "virtual_net: proxy '%s' activated", proxy_name);
	return 0;
}

int vnp_add_expose(Tracee *tracee, uint16_t host_port, uint16_t virtual_port)
{
	VnpConfig *config;
	Extension *ext;

	ext = get_extension(tracee, vnp_callback);
	if (ext == NULL)
		return -1;

	config = talloc_get_type_abort(((Extension *)ext)->config, VnpConfig);

	{
		int i;
		for (i = 0; i < config->expose_count; i++) {
			if (config->expose_map[i].host_port == host_port) {
				note(tracee, WARNING, USER,
					"virtual_net: host port %d already exposed", host_port);
				return -1;
			}
		}
	}

	if (config->expose_count >= VNP_EXPOSE_MAX) {
		note(tracee, ERROR, USER,
			"virtual_net: too many exposed ports (max %d)", VNP_EXPOSE_MAX);
		return -1;
	}

	config->expose_map[config->expose_count].host_port = host_port;
	config->expose_map[config->expose_count].virtual_port = virtual_port;
	config->expose_count++;

	return vnp_send_expose(tracee, config, host_port, virtual_port);
}
