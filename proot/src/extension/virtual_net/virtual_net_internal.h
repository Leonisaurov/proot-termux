#ifndef VIRTUAL_NET_INTERNAL_H
#define VIRTUAL_NET_INTERNAL_H

#include <stdint.h>   /* uint*_t, */
#include <stdbool.h>  /* bool, */
#include <sys/types.h>
#include <sys/un.h>
#include <string.h>   /* memset(3), memcpy(3) */
#include <stdio.h>    /* snprintf(3) */
#include <signal.h>   /* kill(2) — B3: pid liveness check in vnp_add_fd */
#include <errno.h>    /* ESRCH — B3: pid liveness check in vnp_add_fd */

/* ========================================================================= */
/*  Paths & Limits                                                           */
/* ========================================================================= */

#define VNP_MAX_NAME     64
#define VNP_MAX_FDS      256
#define VNP_EXPOSE_MAX   64
#define VNP_SOCKBUF_LEN  128

/* Abstract Unix socket name prefix.
 * Full name: @proot-vnet-{proxy_name}-{port}\0
 * Max sun_path = 108 bytes, '@' + prefix + name + '-' + port digits < 108 */
#define VNP_ABSTRACT_PREFIX "proot-vnet-"

/* The runtime directory is selected by the caller's environment. */
const char *vnp_runtime_dir(void);

/* ========================================================================= */
/*  Opcodes: communication tracer ↔ helper (via pipe)                        */
/* ========================================================================= */

enum VnpOpcode {
	VNP_HELLO    = 0x01,
	VNP_BYE      = 0xFF,
	VNP_EXPOSE   = 0x50,
	VNP_UNEXPOSE = 0x51,
};

/* ========================================================================= */
/*  Requests / Responses (tracer ↔ helper via pipe)                          */
/* ========================================================================= */

struct VnpRequest {
	uint32_t opcode;
	uint16_t virtual_port;
	uint16_t host_port;
	uint16_t host_family;
	uint8_t  host_address[16];
} __attribute__((packed));

struct VnpResponse {
	int32_t  result;     /* 0 = success, -errno = error */
	uint16_t host_port;
} __attribute__((packed));

/* ========================================================================= */
/*  Virtual socket fd tracking (in tracer memory)                            */
/* ========================================================================= */

typedef struct {
	int      fd;            /* Tracee's file descriptor */
	pid_t    pid;           /* Tracee's pid (to prevent cross-process removal) */
	uint16_t virtual_port;  /* Virtual port this fd is bound/connect to */
	int      orig_domain;   /* Original AF_INET before we changed to AF_UNIX */
} VnpFdEntry;

/* ========================================================================= */
/*  Exposed port entry (in config)                                           */
/* ========================================================================= */

typedef struct {
	uint16_t host_port;     /* TCP port on host (0.0.0.0:host_port) */
	uint16_t virtual_port;  /* Virtual port (abstract Unix socket) */
	uint16_t host_family;
	uint8_t  host_address[16];
} VnpExposeEntry;

/* ========================================================================= */
/*  Extension configuration (talloc'd per tracee)                            */
/* ========================================================================= */

typedef struct {
	char          proxy_name[VNP_MAX_NAME];    /* Proxy namespace identifier    */
	uint32_t      instance_token;              /* Unique per-instance token    */
	VnpFdEntry    fd_map[VNP_MAX_FDS];         /* Virtual fd tracking table    */
	int           fd_count;                    /* Number of active fds         */
	VnpExposeEntry expose_map[VNP_EXPOSE_MAX]; /* Exposed port mappings        */
	int           expose_count;                /* Number of exposed ports      */

	/* Helper process management */
	int           helper_pid;                  /* PID of helper process        */
	int           helper_pipe_in;              /* tracer → helper (write end)  */
	int           helper_pipe_out;             /* tracer ← helper (read end)   */


} VnpConfig;

/* ========================================================================= */
/*  Cross-instance Registry (shared file with flock)                          */
/*                                                                           */
/*  Each bind creates a unique abstract socket name.                         */
/*  Registry maps virtual_port → unique_name for cross-instance connect.     */
/* ========================================================================= */

#define VNP_REG_MAGIC   0x50524F4E /* "PRON" */
#define VNP_REG_MAX     512
#define VNP_REG_LOCK    "registry.lock"

struct VnpRegistryEntry {
	uint16_t virtual_port;
	uint32_t instance_token;
	char     abstract_name[108];
};

struct VnpRegistryHeader {
	uint32_t magic;        /* VNP_REG_MAGIC */
	uint32_t count;
	uint32_t generation;
	struct VnpRegistryEntry entries[VNP_REG_MAX];
};

/* ========================================================================= */
/*  Inline helpers                                                           */
/* ========================================================================= */

/**
 * Fill a struct sockaddr_un for a unique abstract Unix socket.
 * Name format: \0proot-vnet-{proxy_name}-{port}-{token}
 * The token ensures uniqueness across proot instances.
 *
 * Note: the sun_path[0] = '\0' prefix makes this an abstract socket per unix(7)
 */
static inline void vnp_fill_abstract_sa(struct sockaddr_un *sa, const char *proxy_name,
                                         uint16_t port, uint32_t token)
{
	memset(sa, 0, sizeof(*sa));
	sa->sun_family = AF_UNIX;
	sa->sun_path[0] = '\0';
	snprintf(&sa->sun_path[1], sizeof(sa->sun_path) - 1,
		 "%s%s-%u-%u", VNP_ABSTRACT_PREFIX, proxy_name, port, token);
}

/**
 * Find an fd entry in the config's fd_map.
 * Returns pointer to entry, or NULL if not found.
 */
static inline VnpFdEntry *vnp_find_fd(VnpConfig *config, int fd, pid_t pid)
{
	int i;
	for (i = 0; i < config->fd_count; i++) {
		if (config->fd_map[i].fd == fd && config->fd_map[i].pid == pid)
			return &config->fd_map[i];
	}
	return NULL;
}

/**
 * Add an fd entry to the config's fd_map.
 * Returns pointer to new entry, or NULL if full.
 *
 * B3 (decision documented): the extension is shared across tracees via
 * talloc_reference and there is NO per-tracee exit event in
 * extension.h, so a dead tracee never purges its fd_map entries on its
 * own (REMOVED fires only when the last talloc reference drops).  A
 * guest that forks short-lived children creating sockets would fill the
 * 256-entry map, making vnp_add_fd() return NULL SILENTLY and breaking
 * fake getsockname/getpeername/accept forever (virtual_net.c:564,734,1026).
 * Instead of adding an exit hook to tracee/event.c (invasive, touches
 * normal mode), we reclaim stale slots right here: when the map is
 * full, sweep for entries whose pid is gone (kill(pid,0) == ESRCH) and
 * compact them swap-with-last (same strategy as vnp_remove_fd), then
 * retry.  Zero overhead in the common path: the sweep runs only when
 * the map is full.  A tracee that is still a zombie (not yet reaped by
 * waitpid) keeps its slot until the next full-map event — bounded and
 * self-healing.
 */
static inline VnpFdEntry *vnp_add_fd(VnpConfig *config, pid_t pid, int fd, uint16_t virtual_port,
                                      int orig_domain)
{
	VnpFdEntry *entry;
	if (config->fd_count >= VNP_MAX_FDS) {
		int i;
		for (i = 0; i < config->fd_count; i++) {
			if (kill(config->fd_map[i].pid, 0) < 0 && errno == ESRCH) {
				/* Swap with last entry */
				config->fd_map[i] = config->fd_map[config->fd_count - 1];
				config->fd_count--;
				i--; /* re-check the entry just swapped in */
			}
		}
	}
	if (config->fd_count >= VNP_MAX_FDS)
		return NULL;
	entry = &config->fd_map[config->fd_count];
	entry->fd = fd;
	entry->pid = pid;
	entry->virtual_port = virtual_port;
	entry->orig_domain = orig_domain;
	config->fd_count++;
	return entry;
}

/**
 * Remove an fd entry from the config's fd_map.
 * Uses swap-with-last strategy (O(1)) to avoid O(n) memmove.
 */
static inline void vnp_remove_fd(VnpConfig *config, int fd, pid_t pid)
{
	int i;
	for (i = 0; i < config->fd_count; i++) {
		if (config->fd_map[i].fd == fd && config->fd_map[i].pid == pid) {
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
	const char *runtime_dir = vnp_runtime_dir();
	if (bufsz == 0)
		return;
	if (runtime_dir == NULL || proxy_name == NULL) {
		buf[0] = '\0';
		return;
	}
	snprintf(buf, bufsz, "%s/proot-net/%s", runtime_dir, proxy_name);
}

#endif /* VIRTUAL_NET_INTERNAL_H */
