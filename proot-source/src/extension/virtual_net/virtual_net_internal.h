#ifndef VIRTUAL_NET_INTERNAL_H
#define VIRTUAL_NET_INTERNAL_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/un.h>
#include <string.h>  /* memset(3), memcpy(3) */
#include <stdio.h>   /* snprintf(3) */

/* ================================================================
 * Paths & Limits
 * ================================================================ */

#define VNP_TMP_DIR      "/data/data/com.termux/files/usr/tmp/proot-net"
#define VNP_MAX_NAME     64
#define VNP_MAX_PORTS    4096
#define VNP_MAX_FDS      256
#define VNP_EXPOSE_MAX   64
#define VNP_SOCKBUF_LEN  128

/* Abstract Unix socket name prefix.
 * Full name: @proot-vnet-{proxy_name}-{port}\0
 * Max sun_path = 108 bytes, '@' + prefix + name + '-' + port digits < 108 */
#define VNP_ABSTRACT_PREFIX "proot-vnet-"

/* ================================================================
 * Opcodes: communication tracer ↔ helper (via pipe)
 * ================================================================ */

enum VnpOpcode {
	VNP_HELLO    = 0x01,
	VNP_BYE      = 0xFF,
	VNP_EXPOSE   = 0x50,
	VNP_UNEXPOSE = 0x51,
};

/* ================================================================
 * Requests / Responses (tracer ↔ helper via pipe)
 * ================================================================ */

struct VnpRequest {
	uint32_t opcode;
	uint16_t virtual_port;
	uint16_t host_port;
};

struct VnpResponse {
	int32_t  result;     /* 0 = success, -errno = error */
	uint16_t host_port;
};

/* ================================================================
 * Virtual socket fd tracking (in tracer memory)
 * ================================================================ */

typedef struct {
	int      fd;            /* Tracee's file descriptor */
	uint16_t virtual_port;  /* Virtual port this fd is bound/connect to */
	uint16_t exposed_port;  /* If > 0, this port is exposed via -p */
	int      orig_domain;   /* Original AF_INET before we changed to AF_UNIX */
} VnpFdEntry;

/* ================================================================
 * Exposed port entry (in config)
 * ================================================================ */

typedef struct {
	uint16_t host_port;     /* TCP port on host (0.0.0.0:host_port) */
	uint16_t virtual_port;  /* Virtual port (abstract Unix socket) */
} VnpExposeEntry;

/* ================================================================
 * Extension configuration (talloc'd per tracee)
 * ================================================================ */

typedef struct {
	char          proxy_name[VNP_MAX_NAME];
	VnpFdEntry    fd_map[VNP_MAX_FDS];
	int           fd_count;
	VnpExposeEntry expose_map[VNP_EXPOSE_MAX];
	int           expose_count;

	/* Helper process management */
	int           helper_pid;
	int           helper_pipe_in;   /* tracer → helper (write end) */
	int           helper_pipe_out;  /* tracer ← helper (read end) */
} VnpConfig;

/* ================================================================
 * Inline helpers
 * ================================================================ */

/**
 * Build abstract Unix socket name for a virtual port.
 * Result: "@proot-vnet-{proxy_name}-{port}"
 * buf must be at least VNP_SOCKBUF_LEN bytes.
 * Returns length including the leading '@'.
 */
static inline int vnp_abstract_name(const char *proxy_name, uint16_t port,
                                     char *buf, size_t bufsz)
{
	/* Abstract sockets start with '\0' (sun_path[0] = 0).
	 * For readability we use '@' prefix in our internal name,
	 * but the actual sun_path starts with '\0'. */
	int len = snprintf(buf, bufsz, "%s%s-%u", VNP_ABSTRACT_PREFIX, proxy_name, port);
	if (len < 0 || (size_t)len >= bufsz)
		return -1;
	return len;
}

/**
 * Fill a struct sockaddr_un for an abstract Unix socket.
 * Abstract sockets: sun_path[0] = '\0', followed by the name.
 */
static inline void vnp_fill_abstract_sa(struct sockaddr_un *sa, const char *proxy_name,
                                         uint16_t port)
{
	char namebuf[VNP_SOCKBUF_LEN];
	int namelen;

	memset(sa, 0, sizeof(*sa));
	sa->sun_family = AF_UNIX;

	/* Abstract socket: first byte is '\0' */
	sa->sun_path[0] = '\0';
	namelen = vnp_abstract_name(proxy_name, port, namebuf, sizeof(namebuf));
	if (namelen > 0) {
		memcpy(&sa->sun_path[1], namebuf, namelen);
	}
}

/**
 * Find an fd entry in the config's fd_map.
 * Returns pointer to entry, or NULL if not found.
 */
static inline VnpFdEntry *vnp_find_fd(VnpConfig *config, int fd)
{
	int i;
	for (i = 0; i < config->fd_count; i++) {
		if (config->fd_map[i].fd == fd)
			return &config->fd_map[i];
	}
	return NULL;
}

/**
 * Add an fd entry to the config's fd_map.
 * Returns pointer to new entry, or NULL if full.
 */
static inline VnpFdEntry *vnp_add_fd(VnpConfig *config, int fd, uint16_t virtual_port,
                                      int orig_domain)
{
	VnpFdEntry *entry;
	if (config->fd_count >= VNP_MAX_FDS)
		return NULL;
	entry = &config->fd_map[config->fd_count];
	entry->fd = fd;
	entry->virtual_port = virtual_port;
	entry->exposed_port = 0;
	entry->orig_domain = orig_domain;
	config->fd_count++;
	return entry;
}

/**
 * Remove an fd entry from the config's fd_map.
 */
static inline void vnp_remove_fd(VnpConfig *config, int fd)
{
	int i;
	for (i = 0; i < config->fd_count; i++) {
		if (config->fd_map[i].fd == fd) {
			/* Swap with last entry */
			config->fd_map[i] = config->fd_map[config->fd_count - 1];
			config->fd_count--;
			return;
		}
	}
}

/**
 * Build the tmp directory path for this proxy.
 */
static inline void vnp_net_path(const char *proxy_name, char *buf, size_t bufsz)
{
	snprintf(buf, bufsz, "%s/%s", VNP_TMP_DIR, proxy_name);
}

#endif /* VIRTUAL_NET_INTERNAL_H */
