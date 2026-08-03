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
#include <sys/socket.h> /* AF_*, SOCK_*, sa_family_t */
#include <sys/un.h>     /* struct sockaddr_un, */
#include <netinet/in.h> /* struct sockaddr_in, htons(3), ntohs(3),
                         * IN6_IS_ADDR_LOOPBACK, IN6_IS_ADDR_UNSPECIFIED */
#include <stdio.h>      /* snprintf(3), */
#include <sys/stat.h>   /* mkdir(2) */
#include <signal.h>     /* kill(2) */
#include <sys/wait.h>   /* waitpid(2) */
#include <sys/file.h>   /* flock(2) */
#include <time.h>       /* clock_gettime(3) */
#include <stddef.h>     /* offsetof(3) */

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
	{ PR_accept,      0 },
	{ PR_accept4,     0 },
	{ PR_connect,     0 },
	{ PR_close,       0 },
	{ PR_getsockname, FILTER_SYSEXIT },
	{ PR_getpeername, FILTER_SYSEXIT },
	{ PR_setsockopt,  0 },
	FILTERED_SYSNUM_END,
};

/* ===========================================================================
 * Helper process management
 * =========================================================================== */

static int vnp_start_helper(VnpConfig *config, const char *proxy_name)
{
	int pipe_main2helper[2] = { -1, -1 };
	int pipe_helper2main[2] = { -1, -1 };
	pid_t pid;

	if (pipe(pipe_main2helper) < 0 || pipe(pipe_helper2main) < 0) {
		close(pipe_main2helper[0]);
		close(pipe_main2helper[1]);
		close(pipe_helper2main[0]);
		close(pipe_helper2main[1]);
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		close(pipe_main2helper[0]);
		close(pipe_main2helper[1]);
		close(pipe_helper2main[0]);
		close(pipe_helper2main[1]);
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

		/* Close inherited fds; helper doesn't need parent's tracee/ptrace fds */
		{
			int max_fd = (int)sysconf(_SC_OPEN_MAX);
			if (max_fd < 0) max_fd = 1024;
			for (int i = 3; i < max_fd; i++)
				close(i);
		}

		char tokbuf[16];
		snprintf(namebuf, sizeof(namebuf), "%s", proxy_name);
		snprintf(tokbuf, sizeof(tokbuf), "%u", config->instance_token);
		execl("/proc/self/exe", "proot", "--vnp-helper", namebuf, tokbuf, (char *)NULL);
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
	do {
		n = write(config->helper_pipe_in, req, sizeof(*req));
	} while (n < 0 && errno == EINTR);
	if (n != sizeof(*req))
		return -1;
	do {
		n = read(config->helper_pipe_out, resp, sizeof(*resp));
	} while (n < 0 && errno == EINTR);
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

/**
 * Check if an IPv6 address is loopback (::1) or unspecified (::).
 */
static int is_ipv6_loopback_or_unspecified(const struct in6_addr *addr)
{
	return IN6_IS_ADDR_LOOPBACK(addr) || IN6_IS_ADDR_UNSPECIFIED(addr);
}

/* ===========================================================================
 * Token generation & registry management
 * =========================================================================== */

/**
 * Generate a unique instance token.
 * Used to create unique abstract socket names per proot instance.
 */
static uint32_t vnp_generate_token(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	/* Mix time with address of stack variable for entropy */
	return (uint32_t)(ts.tv_sec ^ ts.tv_nsec) ^ (uint32_t)(uintptr_t)&ts;
}

/**
 * Ensure the base tmp dir and per-proxy subdirectory exist.
 *
 * The result is cached in a static flag: after the first successful run for
 * a given proxy, subsequent calls (every bind/connect registry open) return
 * immediately without issuing the 2x mkdir() syscalls. The proxy name is
 * cached alongside so that a second --proxy (last-wins) still creates its own
 * subdirectory.
 */
static bool vnp_dirs_ready = false;
static char vnp_dirs_proxy[VNP_MAX_NAME] = "";

static int vnp_ensure_directories(const char *proxy_name)
{
	/* Fast path: directories already created for this proxy */
	if (vnp_dirs_ready && strcmp(vnp_dirs_proxy, proxy_name) == 0)
		return 0;

	/* Create base directory */
	if (mkdir(VNP_TMP_DIR, 0755) < 0 && errno != EEXIST)
		return -1;

	/* Create per-proxy subdirectory */
	char proxy_dir[VNP_SOCKBUF_LEN];
	vnp_net_path(proxy_name, proxy_dir, sizeof(proxy_dir));
	if (mkdir(proxy_dir, 0755) < 0 && errno != EEXIST)
		return -1;

	/* Cache the success so subsequent registry opens skip the mkdir() calls */
	vnp_dirs_ready = true;
	strncpy(vnp_dirs_proxy, proxy_name, sizeof(vnp_dirs_proxy) - 1);
	vnp_dirs_proxy[sizeof(vnp_dirs_proxy) - 1] = '\0';

	return 0;
}

/* ===========================================================================
 * Registry lookup cache (per-instance)
 *
 * vnp_lookup_virtual_port() used to read the full ~59 KB registry on every
 * connect to a non-locally-resolved port. The header carries a `generation`
 * counter that every writer bumps (see vnp_registry_add), so a lookup can
 * first read just the 12-byte header (magic/count/generation) and, if it
 * matches the cache, search the in-memory entries instead. Reads happen while
 * holding LOCK_SH, so no other instance can write concurrently — a matching
 * generation guarantees the cached entries are byte-identical to the file.
 * =========================================================================== */

/* Sentinel: forces a full read on the very first lookup */
#define VNP_CACHE_GENERATION_INVALID UINT32_MAX

static uint32_t vnp_cache_generation = VNP_CACHE_GENERATION_INVALID;
static uint32_t vnp_cache_count = 0;
static struct VnpRegistryEntry vnp_cache_entries[VNP_REG_MAX];

/**
 * Refresh the per-instance cache from a fully-read registry header.
 */
static void vnp_cache_update(const struct VnpRegistryHeader *hdr)
{
	vnp_cache_generation = hdr->generation;
	vnp_cache_count = hdr->count;
	memcpy(vnp_cache_entries, hdr->entries,
	       hdr->count * sizeof(struct VnpRegistryEntry));
}

/**
 * Open registry file with the given lock type.
 * Returns fd, or -1 on error.
 */
static int vnp_registry_open(const char *proxy_name, int lock_type)
{
	char path[VNP_SOCKBUF_LEN];
	char dir[VNP_SOCKBUF_LEN];
	int fd;

	snprintf(path, sizeof(path), "%s/%s/%s",
		 VNP_TMP_DIR, proxy_name, VNP_REG_LOCK);

	/* Ensure all parent directories exist */
	if (vnp_ensure_directories(proxy_name) < 0) {
		note(NULL, WARNING, INTERNAL,
			"virtual_net: failed to create registry dirs");
		return -1;
	}

	fd = open(path, O_CREAT | O_RDWR, 0644);
	if (fd < 0) {
		note(NULL, WARNING, INTERNAL,
			"virtual_net: failed to open registry %s", path);
		/* If the tmp dirs were deleted at runtime (tmp cleanup), the
		 * cached vnp_dirs_ready is stale: the fast path would return 0
		 * without re-creating them and bind/connect would silently stop
		 * being virtual (the mkdirs in vnp_ensure_directories used to
		 * self-heal).  Reset the cache on ENOENT so the next operation
		 * re-runs the mkdir()s.  */
		if (errno == ENOENT) {
			vnp_dirs_ready = false;
			vnp_dirs_proxy[0] = '\0';
		}
		return -1;
	}
	if (flock(fd, lock_type) < 0) {
		note(NULL, WARNING, INTERNAL,
			"virtual_net: failed to lock registry %s", path);
		close(fd);
		return -1;
	}
	return fd;
}

static void vnp_registry_close(int fd)
{
	if (fd >= 0) {
		flock(fd, LOCK_UN);
		close(fd);
	}
}

static int vnp_registry_read(int fd, struct VnpRegistryHeader *hdr)
{
	lseek(fd, 0, SEEK_SET);
	ssize_t n = read(fd, hdr, sizeof(*hdr));
	if (n != sizeof(*hdr) || hdr->magic != VNP_REG_MAGIC) {
		memset(hdr, 0, sizeof(*hdr));
		hdr->magic = VNP_REG_MAGIC;
		hdr->count = 0;
		hdr->generation = 0;
		return 0;
	}
	/* Defensive clamp: a corrupt/oversized registry header could carry
	 * count > VNP_REG_MAX, which would make vnp_cache_update() memcpy()
	 * past vnp_cache_entries (OOB-write) and
	 * vnp_registry_find_in_entries() walk past the entries array
	 * (OOB-read).  Bound count to what the header actually holds.  */
	if (hdr->count > VNP_REG_MAX)
		hdr->count = VNP_REG_MAX;
	return 0;
}

static int vnp_registry_write(int fd, const struct VnpRegistryHeader *hdr)
{
	ssize_t n;

	if (lseek(fd, 0, SEEK_SET) < 0)
		return -1;

	n = write(fd, hdr, sizeof(*hdr));
	if (n != (ssize_t)sizeof(*hdr))
		return -1;

	if (ftruncate(fd, n) < 0)
		return -1;

	return 0;
}

/**
 * Read only the header fields (magic/count/generation, ~12 bytes) of the
 * registry, skipping the large entries array. Used by the lookup cache to
 * detect whether the on-disk registry changed without reading all ~59 KB.
 */
static void vnp_registry_read_meta(int fd, struct VnpRegistryHeader *hdr)
{
	const size_t meta_size = offsetof(struct VnpRegistryHeader, entries);

	if (lseek(fd, 0, SEEK_SET) < 0
	    || read(fd, hdr, meta_size) != (ssize_t)meta_size
	    || hdr->magic != VNP_REG_MAGIC) {
		memset(hdr, 0, sizeof(*hdr));
		hdr->magic = VNP_REG_MAGIC;
		hdr->count = 0;
		hdr->generation = 0;
		return;
	}
	/* Defensive clamp: same reasoning as vnp_registry_read() — the
	 * fast path compares count against the cache and then searches
	 * vnp_cache_entries, so a bogus count must never exceed the
	 * arrays.  */
	if (hdr->count > VNP_REG_MAX)
		hdr->count = VNP_REG_MAX;
}

static struct VnpRegistryEntry *vnp_registry_find_in_entries(
	const struct VnpRegistryEntry *entries, uint32_t count, uint16_t virtual_port)
{
	uint32_t i;
	for (i = 0; i < count; i++) {
		if (entries[i].virtual_port == virtual_port)
			return (struct VnpRegistryEntry *)&entries[i];
	}
	return NULL;
}

static struct VnpRegistryEntry *vnp_registry_find(
	const struct VnpRegistryHeader *hdr, uint16_t virtual_port)
{
	return vnp_registry_find_in_entries(hdr->entries, hdr->count, virtual_port);
}

static struct VnpRegistryEntry *vnp_registry_add(
	struct VnpRegistryHeader *hdr, uint16_t virtual_port,
	uint32_t token, const char *abstract_name)
{
	struct VnpRegistryEntry *entry = vnp_registry_find(hdr, virtual_port);
	if (entry != NULL) {
		entry->instance_token = token;
		memcpy(entry->abstract_name, abstract_name, sizeof(entry->abstract_name));
		/* Bump generation on updates too: the abstract_name (and thus the
		 * cached lookup result) may have changed, so other instances must
		 * invalidate their cache. */
		hdr->generation++;
		return entry;
	}
	if (hdr->count >= VNP_REG_MAX)
		return NULL;
	entry = &hdr->entries[hdr->count++];
	entry->virtual_port = virtual_port;
	entry->instance_token = token;
	memcpy(entry->abstract_name, abstract_name, sizeof(entry->abstract_name));
	hdr->generation++;
	return entry;
}

/* ===========================================================================
 * Syscall handlers
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * Port extraction helper
 * --------------------------------------------------------------------------- */

/**
 * Extract port and address family from a sockaddr_in/sockaddr_in6
 * in tracee memory.  Returns 0 on success, -1 if not AF_INET/AF_INET6
 * or on error.
 *
 * If 'sockaddr_out' is non-NULL, the sockaddr already read from the tracee
 * (sockaddr_in for AF_INET, sockaddr_in6 for AF_INET6) is copied into it.
 * Callers can then inspect the address without issuing a second read_data().
 */
static inline int extract_port_from_tracee(Tracee *tracee, word_t addr_ptr,
	word_t addrlen, sa_family_t *family, uint16_t *port, void *sockaddr_out)
{
	struct sockaddr_in sa_in;

	if (addrlen < sizeof(struct sockaddr_in))
		return -1;

	if (read_data(tracee, &sa_in, addr_ptr, sizeof(sa_in)) < 0)
		return -1;

	if (sa_in.sin_family == AF_INET) {
		*family = sa_in.sin_family;
		*port = ntohs(sa_in.sin_port);
		if (sockaddr_out != NULL)
			memcpy(sockaddr_out, &sa_in, sizeof(sa_in));
		return 0;
	}

	if (sa_in.sin_family == AF_INET6) {
		struct sockaddr_in6 sa_in6;
		if (addrlen < sizeof(struct sockaddr_in6))
			return -1;
		if (read_data(tracee, &sa_in6, addr_ptr, sizeof(sa_in6)) < 0)
			return -1;
		*family = sa_in6.sin6_family;
		*port = ntohs(sa_in6.sin6_port);
		if (sockaddr_out != NULL)
			memcpy(sockaddr_out, &sa_in6, sizeof(sa_in6));
		return 0;
	}

	return -1;
}

/**
 * Write data to tracee memory via direct PTRACE_POKEDATA.
 * Returns 0 on success, -1 on failure (ptrace error).
 */
static int vnp_write_to_tracee(Tracee *tracee, word_t dest, 
                                const void *src, size_t size)
{
	const word_t *words = (const word_t *)src;
	size_t nwords = size / sizeof(word_t);
	size_t trailing = size % sizeof(word_t);
	size_t i;

	for (i = 0; i < nwords; i++) {
		if (ptrace(PTRACE_POKEDATA, tracee->pid,
			   (word_t)(dest + i * sizeof(word_t)),
			   words[i]) < 0)
			return -1;
	}

	if (trailing > 0) {
		word_t last_word;
		word_t addr = dest + nwords * sizeof(word_t);
		errno = 0;
		last_word = ptrace(PTRACE_PEEKDATA, tracee->pid, (word_t)addr, NULL);
		if (last_word == (word_t)-1 && errno != 0)
			return -1;
		memcpy((uint8_t *)&last_word,
		       (const uint8_t *)src + nwords * sizeof(word_t),
		       trailing);
		if (ptrace(PTRACE_POKEDATA, tracee->pid, (word_t)addr, last_word) < 0)
			return -1;
	}
	return 0;
}

/**
 * Handle socket() — change AF_INET to AF_UNIX.
 * The tracee thinks it creates a TCP socket, but gets a Unix socket.
 */
static int vnp_handle_socket(Tracee *tracee, VnpConfig *config)
{
	word_t domain = peek_reg(tracee, CURRENT, SYSARG_1);
	if (domain == AF_INET || domain == AF_INET6) {
		VERBOSE(tracee, 2, "virtual_net: socket(AF_INET%s, ...) -> AF_UNIX",
			domain == AF_INET6 ? "6" : "");
		poke_reg(tracee, SYSARG_1, AF_UNIX);
		poke_reg(tracee, SYSARG_3, 0);
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
	VnpFdEntry *entry;
	struct sockaddr_un sa_unix;
	sa_family_t family;
	uint16_t port;

	if (extract_port_from_tracee(tracee, addr_ptr, addrlen, &family, &port, NULL) < 0)
		return 0;

	/* Port 0 means "assign automatically" — generate a unique virtual port */
	if (port == 0) {
		port = (uint16_t)(((uint32_t)sockfd ^ config->instance_token) & 0xFFFF);
		if (port == 0) port = 1;
	}

	VERBOSE(tracee, 1, "vnet: bind port %u on fd %lu", port, (unsigned long)sockfd);

	/* Track this fd as virtual */
	entry = vnp_find_fd(config, sockfd, tracee->pid);
	if (entry == NULL) {
		entry = vnp_add_fd(config, tracee->pid, sockfd, port, family);
		if (entry == NULL)
			return 0;
	} else {
		entry->virtual_port = port;
		entry->orig_domain = family;
	}

	/* Check if this port is exposed via -p */
	{
		int i;
		for (i = 0; i < config->expose_count; i++) {
			if (config->expose_map[i].virtual_port == port) {
				entry->exposed_port = port;
				break;
			}
		}
	}

	/* Build unique abstract Unix socket address with instance token */
	vnp_fill_abstract_sa(&sa_unix, config->proxy_name, port, config->instance_token);

	/* Register in shared registry for cross-instance discovery */
	{
		int reg_fd = vnp_registry_open(config->proxy_name, LOCK_EX);
		if (reg_fd >= 0) {
			struct VnpRegistryHeader hdr;
			vnp_registry_read(reg_fd, &hdr);
			/* abstract_name in registry includes the leading \0 */
			vnp_registry_add(&hdr, port, config->instance_token, sa_unix.sun_path);
			vnp_registry_write(reg_fd, &hdr);
			/* Refresh the local cache: generation just changed, so the
			 * next connect() in this instance skips the full read. */
			vnp_cache_update(&hdr);
			vnp_registry_close(reg_fd);
		}
	}

	/* Write new sockaddr_un to tracee's stack and update bind() args */
	{
		word_t new_addr = alloc_mem(tracee, sizeof(struct sockaddr_un));
		if (new_addr == 0)
			return 0;

		if (vnp_write_to_tracee(tracee, new_addr, &sa_unix, sizeof(sa_unix)) < 0)
			return 0;

		poke_reg(tracee, SYSARG_1, sockfd);
		poke_reg(tracee, SYSARG_2, new_addr);
		poke_reg(tracee, SYSARG_3, sizeof(struct sockaddr_un));
	}

	return 0;
}

/**
 * Check if a virtual port is known locally or in the shared registry.
 * Used by connect() to determine if a destination port is virtual.
 *
 * Returns true if the port is virtual, false otherwise.
 * When returning true, 'abstract_name' is filled with the abstract Unix
 * socket name to connect to. For local ports (our own bind), the name
 * is reconstructed via vnp_fill_abstract_sa(). For cross-instance ports,
 * the name is read from the shared registry.
 */
static bool vnp_lookup_virtual_port(VnpConfig *config, uint16_t port, char *abstract_name)
{
	int i;

	/* Check if port belongs to a local socket (our own bind) */
	for (i = 0; i < config->fd_count; i++) {
		if (config->fd_map[i].virtual_port == port) {
			struct sockaddr_un sa;
			vnp_fill_abstract_sa(&sa, config->proxy_name, port, config->instance_token);
			memcpy(abstract_name, sa.sun_path, sizeof(sa.sun_path));
			return true;
		}
	}

	/* Check if port is exposed via -p */
	for (i = 0; i < config->expose_count; i++) {
		if (config->expose_map[i].virtual_port == port) {
			struct sockaddr_un sa;
			vnp_fill_abstract_sa(&sa, config->proxy_name, port, config->instance_token);
			memcpy(abstract_name, sa.sun_path, sizeof(sa.sun_path));
			return true;
		}
	}

	/* Check shared registry for cross-instance ports */
	{
		int reg_fd = vnp_registry_open(config->proxy_name, LOCK_SH);
		if (reg_fd >= 0) {
			struct VnpRegistryHeader hdr;
			struct VnpRegistryEntry *reg_entry;

			/* Fast path: read only the 12-byte header. If generation and
			 * count match the cache, search the in-memory entries instead
			 * of reading the full ~59 KB registry. We hold LOCK_SH, so no
			 * other instance can be mid-write — a matching generation
			 * guarantees the cache mirrors the file exactly. */
			vnp_registry_read_meta(reg_fd, &hdr);
			if (hdr.generation == vnp_cache_generation
			    && hdr.count == vnp_cache_count) {
				reg_entry = vnp_registry_find_in_entries(
					vnp_cache_entries, hdr.count, port);
			} else {
				/* Registry changed (this instance or another one):
				 * read it fully and refresh the cache. */
				vnp_registry_read(reg_fd, &hdr);
				vnp_cache_update(&hdr);
				reg_entry = vnp_registry_find(&hdr, port);
			}

			if (reg_entry != NULL) {
				memcpy(abstract_name, reg_entry->abstract_name, sizeof(reg_entry->abstract_name));
				vnp_registry_close(reg_fd);
				return true;
			}
			vnp_registry_close(reg_fd);
		}
	}

	return false;
}

/**
 * Handle connect() — translate to abstract Unix socket for virtual ports.
 */
static int vnp_handle_connect(Tracee *tracee, VnpConfig *config)
{
	word_t sockfd = peek_reg(tracee, CURRENT, SYSARG_1);
	word_t addr_ptr = peek_reg(tracee, CURRENT, SYSARG_2);
	word_t addrlen = peek_reg(tracee, CURRENT, SYSARG_3);
	VnpFdEntry *entry;
	sa_family_t family;
	uint16_t port;
	char abstract_name[108];
	/* Sockaddr already read by extract_port_from_tracee() — avoids a second
	 * read_data() for the loopback check below. */
	union {
		struct sockaddr_in  sa_in;
		struct sockaddr_in6 sa_in6;
	} sa;

	if (extract_port_from_tracee(tracee, addr_ptr, addrlen, &family, &port, &sa) < 0)
		return 0;

	VERBOSE(tracee, 1, "vnet: connect port %u on fd %lu", port, (unsigned long)sockfd);

	if (family == AF_INET) {
		/* Non-loopback goes to kernel in all cases */
		if ((ntohl(sa.sa_in.sin_addr.s_addr) & 0xFF000000) != 0x7F000000)
			return 0;
	}
	else if (family == AF_INET6) {
		if (!is_ipv6_loopback_or_unspecified(&sa.sa_in6.sin6_addr))
			return 0;
	}
	else {
		return 0;
	}

	/* Check if this port is virtual (local bind, expose, or cross-instance) */
	if (!vnp_lookup_virtual_port(config, port, abstract_name))
		return 0;

	/* Track this fd */
	entry = vnp_find_fd(config, sockfd, tracee->pid);
	if (entry == NULL) {
		entry = vnp_add_fd(config, tracee->pid, sockfd, port, family);
		if (entry == NULL)
			return 0;
	}
	else {
		entry->virtual_port = port;
		entry->orig_domain = family;
	}

	/* Overwrite sockaddr args with abstract Unix socket */
	{
		struct sockaddr_un sa_unix;
		memset(&sa_unix, 0, sizeof(sa_unix));
		sa_unix.sun_family = AF_UNIX;
		memcpy(sa_unix.sun_path, abstract_name, sizeof(abstract_name));

		word_t new_addr = alloc_mem(tracee, sizeof(sa_unix));
		if (new_addr != 0) {
			if (vnp_write_to_tracee(tracee, new_addr, &sa_unix, sizeof(sa_unix)) < 0)
				return 0;
			poke_reg(tracee, SYSARG_2, new_addr);
			poke_reg(tracee, SYSARG_3, sizeof(sa_unix));
		}
	}

	return 0;
}

/**
 * Handle close() — cleanup fd tracking.
 */
static int vnp_handle_close(Tracee *tracee, VnpConfig *config)
{
	word_t fd = peek_reg(tracee, CURRENT, SYSARG_1);
	vnp_remove_fd(config, (int)fd, tracee->pid);
	return 0;
}

/* ===========================================================================
 * Expose management
 * =========================================================================== */

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
			"virtual_net: helper failed to expose port %d -> %d",
			host_port, virtual_port);
		return -1;
	}

	VERBOSE(tracee, 2, "virtual_net: exposed %d -> virtual %d",
		host_port, virtual_port);
	return 0;
}

/* ===========================================================================
 * Extension callback
 * =========================================================================== */

/**
 * Handle getsockname/getpeername — fake AF_INET/AF_INET6 result
 * for a previously translated virtual socket.
 *
 * Uses vnp_write_to_tracee (raw PTRACE_POKEDATA) instead of write_data
 * to avoid the pokedata workaround (which does PTRACE_CONT and causes
 * syscall relaunch). On Android/aarch64 with SELinux, process_vm_writev
 * may be blocked and write_data falls back to a workaround that does
 * PTRACE_CONT, which is unsafe in any context because it lets the tracee
 * run and triggers syscall cancellation/relaunch, causing the real
 * getsockname/getpeername to execute on the Unix socket and return
 * garbage to curl.
 */
static int vnp_handle_get_name(Tracee *tracee, VnpConfig *config)
{
	word_t sockfd = peek_reg(tracee, CURRENT, SYSARG_1);
	word_t addr_ptr = peek_reg(tracee, CURRENT, SYSARG_2);
	word_t addrlen_ptr = peek_reg(tracee, CURRENT, SYSARG_3);
	VnpFdEntry *entry = vnp_find_fd(config, (int)sockfd, tracee->pid);

	if (entry == NULL)
		return 0;

	VERBOSE(tracee, 2, "vnet: %s fd %lu -> 127.0.0.1:%u",
		get_sysnum(tracee, CURRENT) == PR_getsockname ? "getsockname" : "getpeername",
		(unsigned long)sockfd, entry->virtual_port);

	/* Build fake sockaddr matching the original domain */
	struct sockaddr_in  fake_in;
	struct sockaddr_in6 fake_in6;
	const void *fake_addr;
	socklen_t fake_len;

	if (entry->orig_domain == AF_INET6) {
		memset(&fake_in6, 0, sizeof(fake_in6));
		fake_in6.sin6_family = AF_INET6;
		fake_in6.sin6_port = htons(entry->virtual_port);
		fake_in6.sin6_addr = in6addr_loopback;
		fake_addr = &fake_in6;
		fake_len = sizeof(fake_in6);
	} else {
		memset(&fake_in, 0, sizeof(fake_in));
		fake_in.sin_family = AF_INET;
		fake_in.sin_port = htons(entry->virtual_port);
		fake_in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		fake_addr = &fake_in;
		fake_len = sizeof(fake_in);
	}

	/* Read caller's buffer size (addrlen) */
	uint32_t in_len = 0;
	if (addrlen_ptr != 0) {
		in_len = peek_uint32(tracee, addrlen_ptr);
		if (errno != 0)
			in_len = 0;
	}

	/* Limit copy to caller's buffer, but report the real length (kernel semantics) */
	size_t copy = (in_len < (uint32_t)fake_len) ? in_len : (uint32_t)fake_len;
	if (addr_ptr != 0 && copy > 0) {
		if (vnp_write_to_tracee(tracee, addr_ptr, fake_addr, copy) < 0)
			return -EFAULT;
	}
	if (addrlen_ptr != 0)
		poke_uint32(tracee, addrlen_ptr, (uint32_t)fake_len);

	/* Void the syscall: set result to 0 and change sysnum to PR_void.
	 * The void handler in exit.c (lines 78-89) will restore result to 0
	 * from MODIFIED registers when the EXIT ptrace event fires. */
	poke_reg(tracee, SYSARG_RESULT, 0);
	set_sysnum(tracee, PR_void);
	return 0;
}

/**
 * Handle accept/accept4 exit — write fake AF_INET/AF_INET6 loopback
 * address into the tracee's buffer so the caller thinks it got a
 * real TCP connection from localhost.
 *
 * Supports both AF_INET and AF_INET6 based on the listening socket's
 * original domain.
 */
static int vnp_handle_accept_exit(Tracee *tracee, VnpConfig *config,
                                   VnpFdEntry *listen_entry, int newfd,
                                   word_t addr_ptr, word_t addrlen_ptr)
{
	/* Track the new fd with same virtual port */
	VnpFdEntry *entry = vnp_find_fd(config, newfd, tracee->pid);
	if (entry == NULL) {
		entry = vnp_add_fd(config, tracee->pid, newfd, listen_entry->virtual_port,
				    listen_entry->orig_domain);
		if (entry != NULL)
			entry->exposed_port = listen_entry->exposed_port;
	}

	/* Fake the client address as AF_INET or AF_INET6 loopback,
	 * matching the original domain of the listening socket.
	 * We use raw PTRACE_POKEDATA instead of write_data or
	 * process_vm_writev because on Android/aarch64:
	 * - process_vm_writev may be blocked by SELinux
	 * - write_data's ptrace workaround does PTRACE_CONT
	 *   which is unsafe in the EXIT handler context */
	if (addr_ptr != 0 && addrlen_ptr != 0) {
		bool write_ok = true;
		if (listen_entry->orig_domain == AF_INET6) {
			struct sockaddr_in6 fake6;
			memset(&fake6, 0, sizeof(fake6));
			fake6.sin6_family = AF_INET6;
			fake6.sin6_port = 0;
			fake6.sin6_addr = in6addr_loopback;
			if (vnp_write_to_tracee(tracee, addr_ptr, &fake6, sizeof(fake6)) < 0)
				write_ok = false;
		} else {
			struct sockaddr_in fake;
			memset(&fake, 0, sizeof(fake));
			fake.sin_family = AF_INET;
			fake.sin_port = 0;
			fake.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			if (vnp_write_to_tracee(tracee, addr_ptr, &fake, sizeof(fake)) < 0)
				write_ok = false;
		}
		if (write_ok) {
			uint32_t addrlen_val = (listen_entry->orig_domain == AF_INET6)
				? (uint32_t)sizeof(struct sockaddr_in6)
				: (uint32_t)sizeof(struct sockaddr_in);
			vnp_write_to_tracee(tracee, addrlen_ptr, &addrlen_val, sizeof(addrlen_val));
		}
	}
	return 0;
}

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

		/* Validate proxy_name: no path separators or dots (prevents path traversal) */
		if (strchr(proxy_name, '/') != NULL ||
		    strchr(proxy_name, '\\') != NULL ||
		    strcmp(proxy_name, ".") == 0 ||
		    strcmp(proxy_name, "..") == 0) {
			note(TRACEE(extension), ERROR, USER, "virtual_net: invalid proxy name '%s'", proxy_name);
			return -1;
		}

		strncpy(config->proxy_name, proxy_name, VNP_MAX_NAME - 1);
		config->proxy_name[VNP_MAX_NAME - 1] = '\0';
		config->instance_token = vnp_generate_token();
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
			return vnp_handle_socket(tracee, config);
		case PR_bind:
			return vnp_handle_bind(tracee, config);
		case PR_connect:
			return vnp_handle_connect(tracee, config);
		case PR_close:
			return vnp_handle_close(tracee, config);
		case PR_setsockopt: {
			word_t level = peek_reg(tracee, CURRENT, SYSARG_2);
			if (level == IPPROTO_TCP || level == IPPROTO_IP) {
				poke_reg(tracee, SYSARG_RESULT, 0);
				set_sysnum(tracee, PR_void);
			}
			return 0;
		}
		case PR_getsockname:
		case PR_getpeername:
			return vnp_handle_get_name(tracee, config);
		default:
			return 0;
		}
	}

	case SYSCALL_EXIT_END: {
		Tracee *tracee = TRACEE(extension);
		VnpConfig *config = talloc_get_type_abort(extension->config, VnpConfig);
		word_t result = peek_reg(tracee, CURRENT, SYSARG_RESULT);

		if ((intptr_t)result < 0)
			return 0;

		switch (get_sysnum(tracee, ORIGINAL)) {
		case PR_connect:
		case PR_bind:
			return 0;
		case PR_socket: {
			int fd = (int)result;
			word_t original_domain = peek_reg(tracee, ORIGINAL, SYSARG_1);
			if (original_domain != AF_INET && original_domain != AF_INET6)
				return 0;
			VnpFdEntry *entry = vnp_find_fd(config, fd, tracee->pid);
			if (entry == NULL) {
				vnp_add_fd(config, tracee->pid, fd, 0,
					   (original_domain == AF_INET6) ? AF_INET6 : AF_INET);
			}
			return 0;
		}
		case PR_accept:
		case PR_accept4: {
			word_t listen_fd = peek_reg(tracee, ORIGINAL, SYSARG_1);
			word_t addr_ptr = peek_reg(tracee, ORIGINAL, SYSARG_2);
			word_t addrlen_ptr = peek_reg(tracee, ORIGINAL, SYSARG_3);
			int newfd = (int)result;

			VnpFdEntry *listen_entry = vnp_find_fd(config, (int)listen_fd, tracee->pid);
			if (listen_entry == NULL || listen_entry->virtual_port == 0)
				return 0;

			return vnp_handle_accept_exit(tracee, config, listen_entry, newfd,
						      addr_ptr, addrlen_ptr);
		}
		default:
			return 0;
		}
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

/* ===========================================================================
 * Public API
 * =========================================================================== */

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

	if (vnp_ensure_directories(proxy_name) < 0) {
		note(tracee, WARNING, SYSTEM, "vnet: can't create %s/%s: %s",
		     VNP_TMP_DIR, proxy_name, strerror(errno));
		return -1;
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
