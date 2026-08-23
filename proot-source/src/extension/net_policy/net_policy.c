/* Static network policy.  This extension deliberately runs before the
 * virtual_net and port_switch translations.  It therefore cannot leave
 * virtual registry or helper state behind when a request is denied. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <stddef.h>
#include <netdb.h>

#include "attribute.h"
#include "cli/note.h"
#include "extension/net_policy/net_policy.h"
#include "extension/virtual_net/virtual_net.h"
#include "tracee/mem.h"
#include "tracee/tracee.h"

#define NET_POLICY_MAX_RULES 128
#define NET_POLICY_RULE_LEN 256
#define NET_POLICY_MAX_BINDS 256
#define NET_ASK_VERSION 1U
#define NET_ASK_TIMEOUT_MS 1000
#define NET_DNS_MAX_QUERIES 64
#define NET_DNS_MAX_LEASES 128
#define NET_DNS_NAME_LEN 256

enum {
	NET_ASK_BIND = 1,
	NET_ASK_CONNECT = 2,
	NET_ASK_PUBLICATION = 3,
	NET_ASK_DNS = 4,
	NET_ASK_BRIDGE_CREATE = 5,
	NET_ASK_BRIDGE_CLOSE = 6,
};

enum {
	NET_DECISION_DENY = 0,
	NET_DECISION_ALLOW = 1,
};

typedef struct __attribute__((packed)) {
	uint32_t version;
	uint32_t event_type;
	uint64_t request_id;
	int32_t guest_pid;
	int32_t host_pid;
	uint16_t family;
	uint16_t protocol;
	uint16_t guest_port;
	uint16_t host_port;
	uint8_t address[16];
	uint8_t virtual_class;
	uint8_t real_exposure;
	char proxy[64];
	char domain[128];
} NetAskRequest;

typedef struct __attribute__((packed)) {
	uint32_t version;
	uint64_t request_id;
	uint8_t decision;
	uint8_t reason_code;
	char reason[96];
} NetAskResponse;

typedef struct {
	char *value;
	char source[NET_DNS_NAME_LEN];
	/* Domain rules are configuration records only.  They never participate in
	 * destination_matches(); resolve_pending_domains() appends their fixed
	 * numeric rules to the same list. */
	int is_domain;
	int resolved;
} NetPolicyRule;

typedef struct {
	pid_t pid;
	int fd;
	unsigned int port;
} NetPolicyBind;

typedef struct {
	pid_t pid;
	int fd;
	uint16_t request_id;
	uint16_t qtype;
	char name[NET_DNS_NAME_LEN];
	char cname[NET_DNS_NAME_LEN];
	struct sockaddr_storage server;
	unsigned char response[4096];
	size_t response_len;
	int response_ready;
} NetDnsQuery;

typedef struct {
	pid_t pid;
	uint16_t request_id;
	char name[NET_DNS_NAME_LEN];
	char cname[NET_DNS_NAME_LEN];
	uint8_t address[16];
	uint8_t family;
	uint64_t expires_ms;
	struct sockaddr_storage server;
} NetDnsLease;

typedef struct {
	NetPolicyMode mode;
	NetPolicyRule allow[NET_POLICY_MAX_RULES];
	NetPolicyRule deny[NET_POLICY_MAX_RULES];
	unsigned int allow_count;
	unsigned int deny_count;
	NetPolicyRule allow_bind[NET_POLICY_MAX_RULES];
	NetPolicyRule deny_bind[NET_POLICY_MAX_RULES];
	unsigned int allow_bind_count;
	unsigned int deny_bind_count;
	NetPolicyBind binds[NET_POLICY_MAX_BINDS];
	unsigned int bind_count;
	int ask_fd;
	uint64_t next_request_id;
	char proxy[64];
	NetDnsQuery dns_queries[NET_DNS_MAX_QUERIES];
	unsigned int dns_query_count;
	NetDnsLease dns_leases[NET_DNS_MAX_LEASES];
	unsigned int dns_lease_count;
} NetPolicyConfig;

static FilteredSysnum net_policy_sysnums[] = {
	{ PR_bind, 0 },
	{ PR_listen, 0 },
	{ PR_connect, 0 },
	{ PR_sendto, FILTER_SYSEXIT },
	{ PR_recvfrom, FILTER_SYSEXIT },
	{ PR_recvmsg, FILTER_SYSEXIT },
	{ PR_ppoll, FILTER_SYSEXIT },
	{ PR_read, FILTER_SYSEXIT },
	FILTERED_SYSNUM_END
};

static int read_sockaddr(Tracee *tracee, word_t ptr, struct sockaddr_storage *addr);
static void dns_prepare_response(NetPolicyConfig *config, NetDnsQuery *query,
					 const unsigned char *packet, size_t length,
					 Tracee *tracee);

static void dns_canonical(const char *input, char *output, size_t size)
{
	size_t n = strlen(input);
	if (n != 0 && input[n - 1] == '.') n--;
	if (n >= size) n = size - 1;
	for (size_t i = 0; i < n; i++) {
		char c = input[i];
		output[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
	}
	output[n] = '\0';
}

static int dns_names_equal(const char *a, const char *b)
{
	char ca[NET_DNS_NAME_LEN], cb[NET_DNS_NAME_LEN];
	dns_canonical(a, ca, sizeof(ca));
	dns_canonical(b, cb, sizeof(cb));
	return strcmp(ca, cb) == 0;
}

static uint64_t dns_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static int dns_skip_name(const unsigned char *packet, size_t length, size_t offset,
			 size_t *next)
{
	size_t cursor = offset;
	unsigned int jumps = 0;
	while (cursor < length) {
		unsigned char label = packet[cursor++];
		if (label == 0) {
			*next = cursor;
			return 0;
		}
		if ((label & 0xc0) == 0xc0) {
			if (cursor >= length || ++jumps > 16) return -1;
			*next = cursor + 1;
			return 0;
		}
		if ((label & 0xc0) != 0 || label > 63 || cursor + label > length)
			return -1;
		cursor += label;
	}
	return -1;
}

static int dns_read_name(const unsigned char *packet, size_t length, size_t offset,
			 char *name, size_t name_size, size_t *next)
{
	size_t cursor = offset, resume = 0;
	unsigned int jumps = 0;
	size_t used = 0;
	int jumped = 0;
	int overflow = 0;
	if (name_size == 0) return -1;
	name[0] = '\0';
	while (cursor < length) {
		unsigned char label = packet[cursor++];
		if (label == 0) {
			if (!jumped) resume = cursor;
			if (overflow)
				return -1;
			if (used == 0) name[0] = '.';
			name[used < name_size ? used : name_size - 1] = '\0';
			*next = resume;
			return 0;
		}
		if ((label & 0xc0) == 0xc0) {
			unsigned int target;
			if (cursor >= length || ++jumps > 16) return -1;
			target = ((unsigned int)(label & 0x3f) << 8) | packet[cursor++];
			if (target >= length) return -1;
			if (!jumped) resume = cursor;
			cursor = target;
			jumped = 1;
			continue;
		}
		if ((label & 0xc0) != 0 || label > 63 || cursor + label > length)
			return -1;
		if (used != 0) {
			if (used + 1 < name_size)
				name[used++] = '.';
			else
				overflow = 1;
		}
		while (label-- != 0) {
			unsigned char c = packet[cursor++];
			if (used + 1 < name_size)
				name[used++] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
			else
				overflow = 1;
		}
	}
	return -1;
}

static void dns_remember_query(NetPolicyConfig *config, pid_t pid, int fd,
				       const unsigned char *packet, size_t length,
				       const struct sockaddr_storage *server, Tracee *tracee)
{
	NetDnsQuery *query;
	char name[NET_DNS_NAME_LEN];
	size_t next;
	if (length < 12 || (packet[2] & 0x80) ||
	    (((unsigned int)packet[4] << 8) | packet[5]) == 0 ||
	    dns_read_name(packet, length, 12, name, sizeof(name), &next) < 0)
		return;
	if (config->dns_query_count == NET_DNS_MAX_QUERIES)
		config->dns_query_count = NET_DNS_MAX_QUERIES - 1;
	query = &config->dns_queries[config->dns_query_count++];
	memset(query, 0, sizeof(*query));
	query->pid = pid;
	query->fd = fd;
	query->request_id = ((uint16_t)packet[0] << 8) | packet[1];
	if (next + 4 > length)
		return;
	query->qtype = ((uint16_t)packet[next] << 8) | packet[next + 1];
	memcpy(query->name, name, sizeof(query->name));
	if (server != NULL) query->server = *server;
	dns_prepare_response(config, query, packet, length, tracee);
	VERBOSE(tracee, 3, "net_policy: DNS query id=%u name=%s", query->request_id, query->name);
}

static void dns_remember_response(NetPolicyConfig *config, pid_t pid, int fd,
					  const unsigned char *packet, size_t length,
					  const struct sockaddr_storage *server,
					  Tracee *tracee)
{
	unsigned int i, qd, an;
	NetDnsQuery *query = NULL;
	size_t offset = 12, next;
	uint64_t now = dns_now_ms();
	if (length < 12 || !(packet[2] & 0x80)) return;
	for (i = 0; i < config->dns_query_count; i++)
		if (config->dns_queries[i].pid == pid && config->dns_queries[i].fd == fd &&
		    config->dns_queries[i].request_id == (((uint16_t)packet[0] << 8) | packet[1])) {
			query = &config->dns_queries[i];
			break;
		}
	if (query == NULL) return;
	qd = ((unsigned int)packet[4] << 8) | packet[5];
	an = ((unsigned int)packet[6] << 8) | packet[7];
	if (packet[3] & 0x0f) {
		VERBOSE(tracee, 3, "net_policy: DNS response id=%u error=%u",
			query->request_id, packet[3] & 0x0f);
		return;
	}
	for (i = 0; i < qd; i++) {
		if (dns_skip_name(packet, length, offset, &next) < 0 || next + 4 > length)
			return;
		offset = next + 4;
	}
	for (i = 0; i < an && offset < length; i++) {
		uint16_t type, rdlength;
		uint32_t ttl;
		char owner[NET_DNS_NAME_LEN], cname[NET_DNS_NAME_LEN];
		if (dns_read_name(packet, length, offset, owner, sizeof(owner), &next) < 0 ||
		    next + 10 > length) return;
		offset = next;
		type = ((uint16_t)packet[offset] << 8) | packet[offset + 1];
		ttl = ((uint32_t)packet[offset + 4] << 24) |
		      ((uint32_t)packet[offset + 5] << 16) |
		      ((uint32_t)packet[offset + 6] << 8) | packet[offset + 7];
		rdlength = ((uint16_t)packet[offset + 8] << 8) | packet[offset + 9];
		offset += 10;
		if (offset + rdlength > length) return;
		if (type == 1 || type == 28) {
			NetDnsLease *lease;
			if ((type == 1 && rdlength != 4) || (type == 28 && rdlength != 16)) {
				offset += rdlength;
				continue;
			}
			if (config->dns_lease_count == NET_DNS_MAX_LEASES)
				config->dns_lease_count = NET_DNS_MAX_LEASES - 1;
			lease = &config->dns_leases[config->dns_lease_count++];
			memset(lease, 0, sizeof(*lease));
			lease->pid = pid;
			lease->request_id = query->request_id;
			lease->family = type == 1 ? AF_INET : AF_INET6;
			memcpy(lease->address, packet + offset, rdlength);
			memcpy(lease->name, query->name, sizeof(lease->name));
			memcpy(lease->cname, query->cname, sizeof(lease->cname));
			lease->expires_ms = now + (uint64_t)ttl * 1000U;
			lease->server = server == NULL ? query->server : *server;
			VERBOSE(tracee, 3, "net_policy: DNS association %s ttl=%u",
				lease->name, ttl);
		} else if (type == 5 && dns_read_name(packet, length, offset, cname,
								 sizeof(cname), &next) == 0) {
			memcpy(query->cname, cname, sizeof(query->cname));
			VERBOSE(tracee, 3, "net_policy: DNS CNAME %s -> %s", owner, cname);
		}
		offset += rdlength;
	}
}

static void dns_observe_send(NetPolicyConfig *config, Tracee *tracee)
{
	struct sockaddr_storage server;
	unsigned char packet[4096];
	word_t addr_ptr = peek_reg(tracee, ORIGINAL, SYSARG_5);
	word_t packet_ptr = peek_reg(tracee, ORIGINAL, SYSARG_2);
	word_t packet_len = peek_reg(tracee, ORIGINAL, SYSARG_3);
	if (addr_ptr == 0 || packet_ptr == 0 || packet_len == 0 || packet_len > sizeof(packet) ||
	    read_sockaddr(tracee, addr_ptr, &server) < 0 ||
	    (server.ss_family != AF_INET && server.ss_family != AF_INET6) ||
	    (server.ss_family == AF_INET ? ntohs(((struct sockaddr_in *)&server)->sin_port) :
	     ntohs(((struct sockaddr_in6 *)&server)->sin6_port)) != 53 ||
	    read_data(tracee, packet, packet_ptr, packet_len) < 0)
		return;
	dns_remember_query(config, tracee->pid, (int)peek_reg(tracee, ORIGINAL, SYSARG_1),
				   packet, packet_len, &server, tracee);
}

static void dns_observe_receive(NetPolicyConfig *config, Tracee *tracee)
{
	struct sockaddr_storage server;
	unsigned char packet[4096];
	word_t packet_len = peek_reg(tracee, CURRENT, SYSARG_RESULT);
	word_t packet_ptr = peek_reg(tracee, ORIGINAL, SYSARG_1);
	word_t addr_ptr = peek_reg(tracee, ORIGINAL, SYSARG_5);
	if ((intptr_t)packet_len <= 0 || packet_len > sizeof(packet) || addr_ptr == 0 ||
	    packet_ptr == 0 || read_sockaddr(tracee, addr_ptr, &server) < 0 ||
	    (server.ss_family != AF_INET && server.ss_family != AF_INET6) ||
	    (server.ss_family == AF_INET ? ntohs(((struct sockaddr_in *)&server)->sin_port) :
	     ntohs(((struct sockaddr_in6 *)&server)->sin6_port)) != 53 ||
	    read_data(tracee, packet, packet_ptr, packet_len) < 0)
		return;
	dns_remember_response(config, tracee->pid,
				      (int)peek_reg(tracee, ORIGINAL, SYSARG_1), packet,
				      packet_len, &server, tracee);
}

static NetDnsQuery *dns_pending_query(NetPolicyConfig *config, pid_t pid, int fd)
{
	for (int i = (int)config->dns_query_count - 1; i >= 0; i--)
		if (config->dns_queries[i].pid == pid && config->dns_queries[i].fd == fd &&
		    config->dns_queries[i].response_ready)
			return &config->dns_queries[i];
	return NULL;
}

static int dns_write_response(Tracee *tracee, word_t buffer, size_t capacity,
				      const NetDnsQuery *query, size_t *copied)
{
	size_t write_len;

	*copied = capacity < query->response_len ? capacity : query->response_len;
	write_len = *copied;
	if (buffer == 0 && write_len != 0)
		return -EFAULT;
	/* Keep the common full-buffer path word aligned for Android's ptrace
	 * fallback.  Never extend a write beyond the guest-provided capacity. */
	if (write_len != 0 && write_len % sizeof(word_t) != 0 &&
	    capacity >= write_len + sizeof(word_t) - write_len % sizeof(word_t))
		write_len += sizeof(word_t) - write_len % sizeof(word_t);
	if (write_len != 0 && write_data(tracee, buffer, query->response, write_len) < 0)
		return -EFAULT;
	return 0;
}

static int dns_server_port(const struct sockaddr_storage *addr)
{
	if (addr->ss_family == AF_INET)
		return ntohs(((const struct sockaddr_in *)addr)->sin_port);
	if (addr->ss_family == AF_INET6)
		return ntohs(((const struct sockaddr_in6 *)addr)->sin6_port);
	return -1;
}

static int dns_emulate_send(NetPolicyConfig *config, Tracee *tracee)
{
	struct sockaddr_storage server;
	unsigned char packet[4096];
	word_t addr_ptr = peek_reg(tracee, CURRENT, SYSARG_5);
	word_t packet_ptr = peek_reg(tracee, CURRENT, SYSARG_2);
	word_t packet_len = peek_reg(tracee, CURRENT, SYSARG_3);
	unsigned int old_count = config->dns_query_count;
	if (addr_ptr == 0 || packet_ptr == 0 || packet_len == 0 || packet_len > sizeof(packet) ||
	    read_sockaddr(tracee, addr_ptr, &server) < 0 || dns_server_port(&server) != 53 ||
	    read_data(tracee, packet, packet_ptr, packet_len) < 0)
		return 0;
	dns_remember_query(config, tracee->pid,
				   (int)peek_reg(tracee, CURRENT, SYSARG_1), packet,
				   packet_len, &server, tracee);
	if (config->dns_query_count == old_count && old_count < NET_DNS_MAX_QUERIES)
		return 0;
	NetDnsQuery *query = &config->dns_queries[config->dns_query_count - 1];
	if (query->pid != tracee->pid || query->fd != (int)peek_reg(tracee, CURRENT, SYSARG_1) ||
	    !query->response_ready)
		return 0;
	set_sysnum(tracee, PR_void);
	poke_reg(tracee, SYSARG_RESULT, packet_len);
	return 1;
}

static int dns_emulate_receive(NetPolicyConfig *config, Tracee *tracee)
{
	int fd = (int)peek_reg(tracee, CURRENT, SYSARG_1);
	NetDnsQuery *query = dns_pending_query(config, tracee->pid, fd);
	word_t buf, len, addr_ptr, size_ptr;
	size_t copied;
	if (query == NULL)
		return 0;
	buf = peek_reg(tracee, CURRENT, SYSARG_2);
	len = peek_reg(tracee, CURRENT, SYSARG_3);
	addr_ptr = peek_reg(tracee, CURRENT, SYSARG_5);
	size_ptr = peek_reg(tracee, CURRENT, SYSARG_6);
	if (dns_write_response(tracee, buf, len, query, &copied) < 0)
		return 0;
	if (addr_ptr != 0 && size_ptr != 0) {
		socklen_t size;
		if (read_data(tracee, &size, size_ptr, sizeof(size)) < 0)
			return 0;
		if (size > sizeof(query->server))
			size = sizeof(query->server);
		if (write_data(tracee, addr_ptr, &query->server, size) < 0 ||
		    write_data(tracee, size_ptr, &size, sizeof(size)) < 0)
			return 0;
	}
	query->response_ready = 0;
	set_sysnum(tracee, PR_void);
	poke_reg(tracee, SYSARG_RESULT, copied);
	return 1;
}

static int dns_emulate_recvmsg(NetPolicyConfig *config, Tracee *tracee)
{
	int fd = (int)peek_reg(tracee, CURRENT, SYSARG_1);
	NetDnsQuery *query = dns_pending_query(config, tracee->pid, fd);
	struct msghdr message;
	struct iovec vector;
	word_t message_ptr = peek_reg(tracee, CURRENT, SYSARG_2);
	size_t copied;
	if (query == NULL || message_ptr == 0 ||
	    read_data(tracee, &message, message_ptr, sizeof(message)) < 0 ||
	    message.msg_iov == NULL || message.msg_iovlen == 0 ||
	    read_data(tracee, &vector, (word_t)message.msg_iov, sizeof(vector)) < 0)
		return 0;
	if (dns_write_response(tracee, (word_t)vector.iov_base, vector.iov_len,
				       query, &copied) < 0)
		return 0;
	if (message.msg_name != NULL && message.msg_namelen != 0) {
		socklen_t size = message.msg_namelen < sizeof(query->server) ?
			message.msg_namelen : sizeof(query->server);
		if (write_data(tracee, (word_t)message.msg_name, &query->server, size) < 0 ||
		    write_data(tracee, message_ptr + offsetof(struct msghdr, msg_namelen),
			       &size, sizeof(size)) < 0)
			return 0;
	}
	query->response_ready = 0;
	set_sysnum(tracee, PR_void);
	poke_reg(tracee, SYSARG_RESULT, copied);
	return 1;
}

static int dns_emulate_ppoll(NetPolicyConfig *config, Tracee *tracee)
{
	word_t fds_ptr = peek_reg(tracee, CURRENT, SYSARG_1);
	word_t nfds = peek_reg(tracee, CURRENT, SYSARG_2);
	struct pollfd fds[64];
	unsigned int ready = 0;
	if (fds_ptr == 0 || nfds == 0 || nfds > 64 ||
	    read_data(tracee, fds, fds_ptr, nfds * sizeof(fds[0])) < 0)
		return 0;
	for (unsigned int i = 0; i < nfds; i++) {
		fds[i].revents = 0;
		if ((fds[i].events & POLLIN) != 0 &&
		    dns_pending_query(config, tracee->pid, fds[i].fd) != NULL) {
			fds[i].revents = POLLIN;
			ready++;
		}
	}
	if (ready == 0)
		return 0;
	if (write_data(tracee, fds_ptr, fds, nfds * sizeof(fds[0])) < 0)
		return 0;
	set_sysnum(tracee, PR_void);
	poke_reg(tracee, SYSARG_RESULT, ready);
	return 1;
}

static int dns_emulate_read(NetPolicyConfig *config, Tracee *tracee)
{
	int fd = (int)peek_reg(tracee, CURRENT, SYSARG_1);
	NetDnsQuery *query = dns_pending_query(config, tracee->pid, fd);
	word_t buffer = peek_reg(tracee, CURRENT, SYSARG_2);
	word_t length = peek_reg(tracee, CURRENT, SYSARG_3);
	size_t copied;
	if (query == NULL || buffer == 0)
		return 0;
	if (dns_write_response(tracee, buffer, length, query, &copied) < 0)
		return 0;
	query->response_ready = 0;
	set_sysnum(tracee, PR_void);
	poke_reg(tracee, SYSARG_RESULT, copied);
	return 1;
}

static NetPolicyConfig *policy_config(Tracee *tracee)
{
	Extension *extension = get_extension(tracee, net_policy_callback);
	return extension == NULL ? NULL : talloc_get_type_abort(extension->config,
									 NetPolicyConfig);
}

static int ensure_extension(Tracee *tracee)
{
	if (get_extension(tracee, net_policy_callback) != NULL)
		return 0;
	return initialize_extension(tracee, net_policy_callback, NULL);
}

static int add_rule(Tracee *tracee, NetPolicyRule *rules, unsigned int *count,
			    const char *value)
{
	const char *scheme;
	if (value != NULL && (scheme = strstr(value, "://")) != NULL) {
		size_t length = (size_t)(scheme - value);
		if (!((length == 3 && strncmp(value, "tcp", 3) == 0) ||
		      (length == 3 && strncmp(value, "udp", 3) == 0) ||
		      (length == 7 && strncmp(value, "virtual", 7) == 0) ||
		      (length == 8 && strncmp(value, "external", 8) == 0) ||
		      (length == 6 && strncmp(value, "bridge", 6) == 0)))
			return -EINVAL;
	}
	if (value == NULL || value[0] == '\0' || strlen(value) >= NET_POLICY_RULE_LEN)
		return -EINVAL;
	if (*count >= NET_POLICY_MAX_RULES)
		return -E2BIG;
	rules[*count].value = talloc_strdup(tracee, value);
	if (rules[*count].value == NULL)
		return -ENOMEM;
	rules[*count].is_domain = 0;
	rules[*count].resolved = 0;
	rules[*count].source[0] = '\0';
	(*count)++;
	return 0;
}

/* Return the host portion and its offsets in a destination rule.  The
 * offsets let the resolver retain protocol and port qualifiers when it
 * replaces a hostname with an address. */
static int destination_host(const char *rule, const char **host, size_t *host_len,
				    size_t *prefix_len, size_t *suffix_offset)
{
	const char *scheme = strstr(rule, "://");
	const char *start = scheme == NULL ? rule : scheme + 3;
	const char *end = NULL;
	const char *colon;

	*prefix_len = (size_t)(start - rule);
	if (*start == '[') {
		end = strchr(start + 1, ']');
		if (end == NULL || (end[1] != '\0' && end[1] != ':'))
			return -EINVAL;
		*host = start + 1;
		*host_len = (size_t)(end - start - 1);
		*suffix_offset = (size_t)(end - rule + 1);
		return 0;
	}
	colon = strrchr(start, ':');
	if (colon != NULL && strchr(start, ':') == colon) {
		end = colon;
		*suffix_offset = (size_t)(colon - rule);
	} else {
		end = rule + strlen(rule);
		*suffix_offset = (size_t)(end - rule);
	}
	*host = start;
	*host_len = (size_t)(end - start);
	return *host_len == 0 ? -EINVAL : 0;
}

static int valid_domain(const char *host, size_t length)
{
	size_t i, label_start = 0, label_length;
	if (length == 0 || length > 253)
		return 0;
	/* A final dot is the canonical absolute-DNS spelling. */
	if (host[length - 1] == '.')
		length--;
	if (length == 0)
		return 0;
	for (i = 0; i <= length; i++) {
		if (i != length && host[i] != '.') {
			if (!(host[i] == '-' || host[i] == '_' ||
			      (host[i] >= 'a' && host[i] <= 'z') ||
			      (host[i] >= 'A' && host[i] <= 'Z') ||
			      (host[i] >= '0' && host[i] <= '9')))
				return 0;
			continue;
		}
		label_length = i - label_start;
		if (label_length == 0 || label_length > 63 ||
		    host[label_start] == '-' || host[i - 1] == '-')
			return 0;
		label_start = i + 1;
	}
	return 1;
}

static int host_is_numeric(const char *host, size_t host_len)
{
	char copy[NET_POLICY_RULE_LEN];
	char *slash;
	struct in_addr v4;
	struct in6_addr v6;
	if (host_len == 0 || host_len >= sizeof(copy))
		return 0;
	memcpy(copy, host, host_len);
	copy[host_len] = '\0';
	slash = strchr(copy, '/');
	if (slash != NULL)
		*slash = '\0';
	return inet_pton(AF_INET, copy, &v4) == 1 ||
	       inet_pton(AF_INET6, copy, &v6) == 1;
}

static int add_fixed_rule(Tracee *tracee, NetPolicyRule *rules, unsigned int *count,
				  const char *original, const char *ip)
{
	const char *host;
	size_t host_len, prefix_len, suffix_offset;
	char fixed[NET_POLICY_RULE_LEN];
	int n;
	if (destination_host(original, &host, &host_len, &prefix_len, &suffix_offset) < 0)
		return -EINVAL;
	(void)host;
	(void)host_len;
	if (original[0] == '[' || strchr(ip, ':') != NULL)
		n = snprintf(fixed, sizeof(fixed), "%.*s[%s]%s", (int)prefix_len,
			     original, ip, original + suffix_offset);
	else
		n = snprintf(fixed, sizeof(fixed), "%.*s%s%s", (int)prefix_len,
			     original, ip, original + suffix_offset);
	if (n < 0 || (size_t)n >= sizeof(fixed) || *count >= NET_POLICY_MAX_RULES)
		return -E2BIG;
	if (add_rule(tracee, rules, count, fixed) < 0)
		return -ENOMEM;
	{
		const char *source_host;
		size_t source_len, source_prefix, source_suffix;
		if (destination_host(original, &source_host, &source_len,
					     &source_prefix, &source_suffix) == 0 &&
		    source_len < NET_DNS_NAME_LEN) {
			char source[NET_DNS_NAME_LEN];
			memcpy(source, source_host, source_len);
			source[source_len] = '\0';
			dns_canonical(source, rules[*count - 1].source,
				      sizeof(rules[*count - 1].source));
		}
	}
	return 0;
}

static int resolve_rule(Tracee *tracee, NetPolicyRule *rules, unsigned int *count,
				NetPolicyRule *domain_rule)
{
	const char *host;
	size_t host_len, prefix_len, suffix_offset;
	char hostname[NET_POLICY_RULE_LEN];
	struct addrinfo hints, *answers, *it;
	char ip[INET6_ADDRSTRLEN];
	unsigned int answers_found = 0, added = 0;
	int gai_error;

	if (!domain_rule->is_domain || domain_rule->resolved)
		return 0;
	if (destination_host(domain_rule->value, &host, &host_len, &prefix_len,
				     &suffix_offset) < 0 || host_len >= sizeof(hostname))
		return -EINVAL;
	(void)prefix_len;
	(void)suffix_offset;
	memcpy(hostname, host, host_len);
	if (host_len != 0 && hostname[host_len - 1] == '.')
		host_len--;
	if (host_len == 0) {
		note(tracee, ERROR, USER, "invalid network domain '%s'", domain_rule->value);
		return -1;
	}
	hostname[host_len] = '\0';
	if (!valid_domain(hostname, host_len))
		return -EINVAL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	/* Do not use AI_ADDRCONFIG: the snapshot must include every A and AAAA
	 * answer, even when the tracer's current interfaces lack one family. */
	hints.ai_flags = 0;
	gai_error = getaddrinfo(hostname, NULL, &hints, &answers);
	if (gai_error != 0) {
		note(tracee, ERROR, USER, "cannot resolve network domain '%s': %s",
		     hostname, gai_strerror(gai_error));
		return -1;
	}
	for (it = answers; it != NULL; it = it->ai_next) {
		const void *address;
		if (it->ai_family == AF_INET)
			address = &((const struct sockaddr_in *)it->ai_addr)->sin_addr;
		else if (it->ai_family == AF_INET6)
			address = &((const struct sockaddr_in6 *)it->ai_addr)->sin6_addr;
		else
			continue;
		if (inet_ntop(it->ai_family, address, ip, sizeof(ip)) == NULL)
			continue;
		answers_found++;
		/* getaddrinfo may return the same address through several aliases. */
		{
			char candidate[NET_POLICY_RULE_LEN];
			unsigned int j;
			const char *candidate_host;
			size_t candidate_len, candidate_prefix, candidate_suffix;
			if (destination_host(domain_rule->value, &candidate_host, &candidate_len,
					    &candidate_prefix, &candidate_suffix) < 0)
				continue;
			(void)candidate_host;
			(void)candidate_len;
			if (domain_rule->value[0] == '[' || strchr(ip, ':') != NULL)
				snprintf(candidate, sizeof(candidate), "%.*s[%s]%s", (int)candidate_prefix,
					 domain_rule->value, ip, domain_rule->value + candidate_suffix);
			else
				snprintf(candidate, sizeof(candidate), "%.*s%s%s", (int)candidate_prefix,
					 domain_rule->value, ip, domain_rule->value + candidate_suffix);
			for (j = 0; j < *count; j++)
				if (!rules[j].is_domain && strcmp(rules[j].value, candidate) == 0)
					break;
			if (j != *count)
				continue;
			if (add_fixed_rule(tracee, rules, count, domain_rule->value, ip) < 0) {
				freeaddrinfo(answers);
				return -1;
			}
			added++;
			VERBOSE(tracee, 1, "net_policy: fixed domain %s -> %s",
				hostname, ip);
		}
	}
	freeaddrinfo(answers);
	if (answers_found == 0) {
		note(tracee, ERROR, USER, "network domain '%s' has no A or AAAA answers", hostname);
		return -1;
	}
	domain_rule->resolved = 1;
	VERBOSE(tracee, 1, "net_policy: fixed domain %s (%u A/AAAA answers, %u new rules)",
			hostname, answers_found, added);
	return 0;
}

static int resolve_pending_domains(Tracee *tracee, NetPolicyRule *rules,
					 unsigned int *count)
{
	unsigned int i, original_count = *count;
	for (i = 0; i < original_count; i++)
		if (resolve_rule(tracee, rules, count, &rules[i]) < 0)
			return -1;
	return 0;
}

static void dns_prepare_response(NetPolicyConfig *config, NetDnsQuery *query,
					 const unsigned char *packet, size_t length,
					 Tracee *tracee)
{
	char wanted[NET_DNS_NAME_LEN];
	size_t question_end;
	unsigned int answers = 0;
	unsigned char *out = query->response;

	if (length < 12 || ((unsigned int)packet[4] << 8 | packet[5]) != 1 ||
	    dns_skip_name(packet, length, 12, &question_end) < 0 ||
	    question_end + 4 > length ||
	    (query->qtype != 1 && query->qtype != 28))
		return;
	question_end += 4;
	if (question_end > sizeof(query->response))
		return;
	dns_canonical(query->name, wanted, sizeof(wanted));
	memcpy(out, packet, question_end);
	out[2] = 0x81;
	out[3] = 0x80;
	out[4] = 0;
	out[5] = 1;
	out[6] = out[7] = 0;
	query->response_len = question_end;

	/* Only the allow snapshot is a source of synthetic answers.  A deny rule
	 * must never re-authorize an address merely because it was resolved on
	 * the host; deny remains higher priority at the IP decision point. */
	for (unsigned int i = 0; i < config->allow_count; i++) {
		const char *host;
		size_t host_len, prefix, suffix;
		unsigned char address[16];
		int family;
		if (config->allow[i].is_domain || config->allow[i].source[0] == '\0' ||
		    !dns_names_equal(config->allow[i].source, wanted) ||
		    destination_host(config->allow[i].value, &host, &host_len,
				     &prefix, &suffix) < 0 ||
		    !host_is_numeric(host, host_len) || host_len >= NET_POLICY_RULE_LEN)
			continue;
		(void)prefix;
		(void)suffix;
		char ip[NET_POLICY_RULE_LEN];
		memcpy(ip, host, host_len);
		ip[host_len] = '\0';
		char *slash = strchr(ip, '/');
		if (slash != NULL)
			*slash = '\0';
		family = strchr(ip, ':') != NULL ? AF_INET6 : AF_INET;
		if ((query->qtype == 1 && family != AF_INET) ||
		    (query->qtype == 28 && family != AF_INET6) ||
		    inet_pton(family, ip, address) != 1)
			continue;
		int duplicate = 0;
		/* The fixed-rule list is already de-duplicated by resolve_rule;
		 * duplicate allow entries are harmless but avoid emitting them. */
		for (unsigned int j = 0; j < answers; j++) {
			size_t rr = question_end + j * (12 + (family == AF_INET ? 4 : 16));
			if (rr + 12 + (family == AF_INET ? 4 : 16) <= query->response_len &&
			    out[rr + 0] == 0xc0 && out[rr + 1] == 0x0c &&
			    out[rr + 2] == 0 && out[rr + 3] == query->qtype &&
			    memcmp(out + rr + 12, address, family == AF_INET ? 4 : 16) == 0)
				duplicate = 1;
		}
		if (duplicate)
			continue;
		if (query->response_len + 12 + (family == AF_INET ? 4 : 16) >
		    sizeof(query->response) || answers == 65535)
			continue;
		unsigned char *rr = out + query->response_len;
		rr[0] = 0xc0;
		rr[1] = 0x0c;
		rr[2] = 0;
		rr[3] = query->qtype;
		rr[4] = 0;
		rr[5] = 1;
		rr[6] = 0;
		rr[7] = 0;
		rr[8] = 0;
		rr[9] = 60;
		uint16_t rdlen = family == AF_INET ? 4 : 16;
		rr[10] = 0;
		rr[11] = (unsigned char)rdlen;
		memcpy(rr + 12, address, rdlen);
		query->response_len += 12 + rdlen;
		answers++;
	}
	if (answers == 0) {
		out[3] = 0x83; /* NXDOMAIN: no guest DNS data was authorized. */
	}
	out[6] = (unsigned char)(answers >> 8);
	out[7] = (unsigned char)answers;
	query->response_ready = 1;
	VERBOSE(tracee, 3, "net_policy: synthetic DNS response name=%s answers=%u",
		query->name, answers);
}

static int parse_mode(const char *mode, NetPolicyMode *result)
{
	if (strcmp(mode, "off") == 0)
		*result = NET_POLICY_OFF;
	else if (strcmp(mode, "deny") == 0)
		*result = NET_POLICY_DENY;
	else if (strcmp(mode, "allow") == 0)
		*result = NET_POLICY_ALLOW;
	else
		return -EINVAL;
	return 0;
}

static int parse_port(const char *value, unsigned int *port)
{
	char *end;
	unsigned long parsed;
	if (strcmp(value, "*") == 0) {
		*port = 0;
		return 0;
	}
	parsed = strtoul(value, &end, 10);
	if (*value == '\0' || *end != '\0' || parsed > 65535)
		return -EINVAL;
	*port = (unsigned int)parsed;
	return 0;
}

static int rule_port(const char *rule, const char **host, unsigned int *port,
			     int *has_port)
{
	const char *colon;
	char portbuf[16];
	*host = rule;
	*has_port = 0;
	*port = 0;
	if (rule[0] == '[') {
		const char *close = strchr(rule, ']');
		if (close == NULL || close[1] != ':')
			return 0;
		colon = close + 1;
		*host = rule + 1;
		if ((size_t)(close - rule - 1) >= NET_POLICY_RULE_LEN)
			return -EINVAL;
		/* The caller only needs the host text for numeric matching; bracketed
		 * rules are copied into a local buffer by destination_matches(). */
	} else {
		colon = strrchr(rule, ':');
		if (colon == NULL || strchr(rule, ':') != colon)
			return 0;
	}
	if (colon[1] == '\0' || strlen(colon + 1) >= sizeof(portbuf))
		return -EINVAL;
	strcpy(portbuf, colon + 1);
	if (parse_port(portbuf, port) < 0)
		return -EINVAL;
	*has_port = 1;
	return 0;
}

static int ip_matches(const char *rule_host, const struct sockaddr_storage *addr)
{
	char buf[INET6_ADDRSTRLEN];
	char *slash;
	unsigned char rule_addr[16], actual[16];
	int family = addr->ss_family;
	int bits, bytes, rem;

	if (family == AF_INET)
		inet_ntop(AF_INET, &((const struct sockaddr_in *)addr)->sin_addr,
			  buf, sizeof(buf));
	else if (family == AF_INET6)
		inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)addr)->sin6_addr,
			  buf, sizeof(buf));
	else
		return 0;

	char rule_copy[NET_POLICY_RULE_LEN];
	strncpy(rule_copy, rule_host, sizeof(rule_copy) - 1);
	rule_copy[sizeof(rule_copy) - 1] = '\0';
	slash = strchr(rule_copy, '/');
	bits = family == AF_INET ? 32 : 128;
	if (slash != NULL) {
		char *end;
		*slash++ = '\0';
		bits = (int)strtol(slash, &end, 10);
		if (*slash == '\0' || *end != '\0' || bits < 0 || bits > (family == AF_INET ? 32 : 128))
			return 0;
	}
	if (inet_pton(family, rule_copy, rule_addr) != 1 ||
	    inet_pton(family, buf, actual) != 1)
		return 0;
	bytes = bits / 8;
	rem = bits % 8;
	if (bytes != 0 && memcmp(rule_addr, actual, (size_t)bytes) != 0)
		return 0;
	return rem == 0 || (rule_addr[bytes] & (unsigned char)(0xff << (8 - rem))) ==
		(actual[bytes] & (unsigned char)(0xff << (8 - rem)));
}

static int destination_matches(const char *rule, const struct sockaddr_storage *addr,
				       unsigned int port, VnpNetworkClass net_class)
{
	char host[NET_POLICY_RULE_LEN];
	const char *rule_host = rule;
	unsigned int rule_port_value;
	int has_port;
	const char *scheme = strstr(rule, "://");
	if (net_class == VNP_NET_CLASS_UNKNOWN)
		return 0;
	if (scheme != NULL) {
		if (strncmp(rule, "virtual://", 10) == 0) {
			if (net_class != VNP_NET_CLASS_VIRTUAL)
				return 0;
		} else if (strncmp(rule, "bridge://", 9) == 0) {
			if (net_class != VNP_NET_CLASS_BRIDGE)
				return 0;
		} else if (strncmp(rule, "external://", 11) == 0) {
			if (net_class != VNP_NET_CLASS_EXTERNAL)
				return 0;
		} else if (strncmp(rule, "tcp://", 6) != 0 &&
			   strncmp(rule, "udp://", 6) != 0)
			return 0;
		rule = scheme + 3;
	}
	if (strcmp(rule, "*") == 0)
		return 1;
	if (rule_port(rule, &rule_host, &rule_port_value, &has_port) < 0)
		return 0;
	if (rule[0] == '[') {
		const char *close = strchr(rule, ']');
		if (close == NULL)
			return 0;
		size_t len = (size_t)(close - rule - 1);
		if (len >= sizeof(host))
			return 0;
		memcpy(host, rule + 1, len);
		host[len] = '\0';
		rule_host = host;
	} else if (has_port) {
		const char *colon = strrchr(rule, ':');
		size_t len = (size_t)(colon - rule);
		if (len >= sizeof(host))
			return 0;
		memcpy(host, rule, len);
		host[len] = '\0';
		rule_host = host;
	}
	if (has_port && rule_port_value != port)
		return 0;
	if (strcmp(rule_host, "*") == 0)
		return 1;
	return ip_matches(rule_host, addr);
}

static int any_rule_matches(NetPolicyRule *rules, unsigned int count,
				     const struct sockaddr_storage *addr, unsigned int port,
				     VnpNetworkClass net_class)
{
	unsigned int i;
	for (i = 0; i < count; i++)
		if (!rules[i].is_domain && destination_matches(rules[i].value, addr, port, net_class))
			return 1;
	return 0;
}

static int bind_matches(NetPolicyRule *rules, unsigned int count, unsigned int port)
{
	unsigned int i, wanted;
	for (i = 0; i < count; i++) {
		if (strcmp(rules[i].value, "*") == 0)
			return 1;
		if (parse_port(rules[i].value, &wanted) == 0 && wanted == port)
			return 1;
	}
	return 0;
}

static void remember_bind(NetPolicyConfig *config, pid_t pid, int fd,
			  unsigned int port)
{
	unsigned int i;
	for (i = 0; i < config->bind_count; i++) {
		if (config->binds[i].pid == pid && config->binds[i].fd == fd) {
			config->binds[i].port = port;
			return;
		}
	}
	if (config->bind_count < NET_POLICY_MAX_BINDS) {
		config->binds[config->bind_count].pid = pid;
		config->binds[config->bind_count].fd = fd;
		config->binds[config->bind_count].port = port;
		config->bind_count++;
	}
}

static unsigned int remembered_bind(NetPolicyConfig *config, pid_t pid, int fd)
{
	unsigned int i;
	for (i = 0; i < config->bind_count; i++)
		if (config->binds[i].pid == pid && config->binds[i].fd == fd)
			return config->binds[i].port;
	return 0;
}

static int write_full(int fd, const void *data, size_t size)
{
	const unsigned char *ptr = data;
	while (size != 0) {
		ssize_t n = write(fd, ptr, size);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		ptr += n;
		size -= (size_t)n;
	}
	return 0;
}

static int read_full_timeout(int fd, void *data, size_t size)
{
	unsigned char *ptr = data;
	while (size != 0) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int ready;
		do {
			ready = poll(&pfd, 1, NET_ASK_TIMEOUT_MS);
		} while (ready < 0 && errno == EINTR);
		if (ready <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
			return -1;
		ssize_t n = read(fd, ptr, size);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		ptr += n;
		size -= (size_t)n;
	}
	return 0;
}

static int ask_harness(NetPolicyConfig *config, Tracee *tracee,
			       uint32_t event, const struct sockaddr_storage *addr,
			       uint16_t guest_port, uint16_t host_port,
			       uint8_t real_exposure, VnpNetworkClass net_class,
			       const char *proxy)
{
	NetAskRequest request;
	NetAskResponse response;
	if (config->ask_fd < 0)
		return 0;
	memset(&request, 0, sizeof(request));
	request.version = NET_ASK_VERSION;
	request.event_type = event;
	request.request_id = ++config->next_request_id;
	request.guest_pid = (int32_t)tracee->pid;
	request.host_pid = (int32_t)getpid();
	request.guest_port = guest_port;
	request.host_port = host_port;
	request.virtual_class = (uint8_t)net_class;
	request.real_exposure = real_exposure;
	if (addr != NULL) {
		request.family = (uint16_t)addr->ss_family;
		if (addr->ss_family == AF_INET)
			memcpy(request.address, &((const struct sockaddr_in *)addr)->sin_addr, 4);
		else if (addr->ss_family == AF_INET6)
			memcpy(request.address, &((const struct sockaddr_in6 *)addr)->sin6_addr, 16);
	}
	if (proxy == NULL)
		proxy = config->proxy;
	if (proxy[0] != '\0')
		memcpy(request.proxy, proxy, sizeof(request.proxy));
	if (write_full(config->ask_fd, &request, sizeof(request)) < 0 ||
	    read_full_timeout(config->ask_fd, &response, sizeof(response)) < 0 ||
	    response.version != NET_ASK_VERSION ||
	    response.request_id != request.request_id ||
	    (response.decision != NET_DECISION_ALLOW &&
	     response.decision != NET_DECISION_DENY))
		return -EACCES;
	return response.decision == NET_DECISION_ALLOW ? 0 : -EACCES;
}

static int read_sockaddr(Tracee *tracee, word_t ptr, struct sockaddr_storage *addr)
{
	unsigned char header[sizeof(word_t)];
	sa_family_t family;
	size_t length;

	/* Android's ptrace fallback cannot reliably read a short, trailing
	 * fragment.  Read only word-aligned portions of the guest sockaddr;
	 * the first word contains the family and port for both IP families. */
	if (ptr == 0 || read_data(tracee, header, ptr, sizeof(header)) < 0)
		return -EFAULT;
	memcpy(&family, header, sizeof(family));
	memset(addr, 0, sizeof(*addr));
	if (family == AF_INET)
		length = sizeof(struct sockaddr_in);
	else if (family == AF_INET6)
		length = offsetof(struct sockaddr_in6, sin6_scope_id);
	else
		return 0;
	if (read_data(tracee, addr, ptr, length) < 0)
		return -EFAULT;
	return 0;
}

static int check_operation(Tracee *tracee, NetPolicyConfig *config, int syscall)
{
	struct sockaddr_storage addr;
	word_t ptr;
	unsigned int port = 0;
	VnpNetworkClass net_class = VNP_NET_CLASS_EXTERNAL;
	char proxy[64];
	int is_bind = syscall == PR_bind || syscall == PR_listen;
	int is_datagram = syscall == PR_sendto || syscall == PR_recvfrom;

	if (config->mode == NET_POLICY_OFF)
		return 0;
	if (syscall == PR_recvmsg || syscall == PR_ppoll || syscall == PR_read)
		return 0;
	if (is_bind) {
		if (syscall == PR_listen) {
			int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
			port = remembered_bind(config, tracee->pid, fd);
			if (bind_matches(config->deny_bind, config->deny_bind_count, port))
				return -EACCES;
			return config->mode == NET_POLICY_DENY &&
				!bind_matches(config->allow_bind, config->allow_bind_count, port)
				? -EACCES : 0;
		}
		ptr = peek_reg(tracee, CURRENT, SYSARG_2);
		if (read_sockaddr(tracee, ptr, &addr) < 0)
			return 0;
		if (addr.ss_family != AF_INET && addr.ss_family != AF_INET6)
			return 0;
		if (addr.ss_family == AF_INET)
			port = ntohs(((struct sockaddr_in *)&addr)->sin_port);
		else if (addr.ss_family == AF_INET6)
			port = ntohs(((struct sockaddr_in6 *)&addr)->sin6_port);
		remember_bind(config, tracee->pid,
			(int) peek_reg(tracee, CURRENT, SYSARG_1), port);
		/* A port-zero bind only asks the kernel for an ephemeral client
		 * port; it does not publish a listener and must not require a
		 * --net-allow-bind rule.  An ensuing listen() is still checked
		 * after the kernel chooses the concrete port. */
		if (port == 0)
			return 0;
		if (bind_matches(config->deny_bind, config->deny_bind_count, port))
			return -EACCES;
		if (config->mode == NET_POLICY_DENY &&
		    !bind_matches(config->allow_bind, config->allow_bind_count, port))
			return -EACCES;
		return 0;
	}

	/* sendto(2)/recvfrom(2) carry the peer address in arg 5; a
	 * NULL address means the connected socket's peer and is left to
	 * connect(2)'s decision. */
	ptr = peek_reg(tracee, CURRENT, is_datagram ? SYSARG_5 : SYSARG_2);
	if (ptr == 0)
		return 0;
	if (read_sockaddr(tracee, ptr, &addr) < 0)
		return 0;
	if (addr.ss_family != AF_INET && addr.ss_family != AF_INET6)
		return 0;
	if (addr.ss_family == AF_INET)
		port = ntohs(((struct sockaddr_in *)&addr)->sin_port);
	else if (addr.ss_family == AF_INET6)
		port = ntohs(((struct sockaddr_in6 *)&addr)->sin6_port);
	/* DNS is a tracer-owned, synthetic service.  The packet itself is
	 * validated and answered below, so this logical connect never reaches a
	 * real DNS server. */
	if (syscall == PR_connect && port == 53)
		return 0;
	net_class = vnp_classify_destination(tracee, &addr, (uint16_t)port,
					     proxy, sizeof(proxy));
	if (any_rule_matches(config->deny, config->deny_count, &addr, port, net_class))
		return -EACCES;
	if (config->mode == NET_POLICY_DENY &&
	    !any_rule_matches(config->allow, config->allow_count, &addr, port, net_class))
		return -EACCES;
	return 0;
}

int net_policy_callback(Extension *extension, ExtensionEvent event,
				intptr_t data1 UNUSED, intptr_t data2 UNUSED)
{
	if (event == INITIALIZATION) {
		NetPolicyConfig *config = talloc_zero(extension, NetPolicyConfig);
		if (config == NULL)
			return -ENOMEM;
		config->mode = NET_POLICY_OFF;
		config->ask_fd = -1;
		config->next_request_id = 0;
		extension->config = config;
		extension->filtered_sysnums = net_policy_sysnums;
		return 0;
	}
	if (event == SYSCALL_ENTER_START) {
		Tracee *tracee = TRACEE(extension);
		NetPolicyConfig *config = talloc_get_type_abort(extension->config, NetPolicyConfig);
		int syscall = get_sysnum(tracee, CURRENT);
		if (config->mode == NET_POLICY_OFF)
			return 0;
		if (syscall == PR_bind || syscall == PR_listen || syscall == PR_connect ||
		    syscall == PR_sendto || syscall == PR_recvfrom || syscall == PR_recvmsg ||
		    syscall == PR_ppoll || syscall == PR_read) {
			if (config->mode != NET_POLICY_OFF && syscall == PR_sendto &&
			    dns_emulate_send(config, tracee))
				return 1;
			if (config->mode != NET_POLICY_OFF && syscall == PR_recvfrom &&
			    dns_emulate_receive(config, tracee))
				return 1;
			if (config->mode != NET_POLICY_OFF && syscall == PR_recvmsg &&
			    dns_emulate_recvmsg(config, tracee))
				return 1;
			if (config->mode != NET_POLICY_OFF && syscall == PR_ppoll &&
			    dns_emulate_ppoll(config, tracee))
				return 1;
			if (config->mode != NET_POLICY_OFF && syscall == PR_read &&
			    dns_emulate_read(config, tracee))
				return 1;
			if (syscall == PR_sendto)
				dns_observe_send(config, tracee);
			int network_syscall = syscall == PR_bind || syscall == PR_listen ||
				syscall == PR_connect || syscall == PR_sendto || syscall == PR_recvfrom;
			int status = network_syscall ? check_operation(tracee, config, syscall) : 0;
			if (status < 0) {
				set_sysnum(tracee, PR_void);
				/* PR_void returns the raw negative errno, just like the
				 * other enter-time emulators (resource_limit, etc.). */
				poke_reg(tracee, SYSARG_RESULT, status);
				VERBOSE(tracee, 1, "net_policy: denied syscall %d", syscall);
				return 1;
			}
			if (network_syscall && syscall != PR_listen && config->ask_fd >= 0) {
				struct sockaddr_storage addr;
				word_t ptr = (syscall == PR_sendto || syscall == PR_recvfrom) ?
					peek_reg(tracee, CURRENT, SYSARG_5) :
					peek_reg(tracee, CURRENT, SYSARG_2);
				if (ptr != 0 && read_sockaddr(tracee, ptr, &addr) == 0) {
					unsigned int port = addr.ss_family == AF_INET ?
						ntohs(((struct sockaddr_in *)&addr)->sin_port) :
						ntohs(((struct sockaddr_in6 *)&addr)->sin6_port);
					status = ask_harness(config, tracee,
						syscall == PR_bind ? NET_ASK_BIND : NET_ASK_CONNECT,
						&addr, (uint16_t)port, (uint16_t)port, 0,
						vnp_classify_destination(tracee, &addr, (uint16_t)port,
									 config->proxy, sizeof(config->proxy)),
						config->proxy);
					if (status < 0) {
						set_sysnum(tracee, PR_void);
						poke_reg(tracee, SYSARG_RESULT, status);
						return 1;
					}
				}
			}
		}
		return 0;
	}
	if (event == SYSCALL_EXIT_START) {
		Tracee *tracee = TRACEE(extension);
		NetPolicyConfig *config = talloc_get_type_abort(extension->config, NetPolicyConfig);
		if (config->mode == NET_POLICY_OFF)
			return 0;
		if (get_sysnum(tracee, ORIGINAL) == PR_recvfrom)
			dns_observe_receive(config, tracee);
		return 0;
	}
	if (event == INHERIT_PARENT)
		return 0;
	return 0;
}

int net_policy_configure(Tracee *tracee, const char *mode)
{
	NetPolicyMode parsed;
	NetPolicyConfig *config;
	if (parse_mode(mode, &parsed) < 0) {
		note(tracee, ERROR, USER, "invalid --net-policy '%s' (expected off, deny, or allow)", mode);
		return -1;
	}
	/* Explicit --net-policy off is intentionally a no-op when no rules were
	 * supplied.  This keeps the default and the explicit off spelling on the
	 * same zero-overhead path. */
	if (parsed == NET_POLICY_OFF &&
	    get_extension(tracee, net_policy_callback) == NULL)
		return 0;
	if (ensure_extension(tracee) < 0)
		return -1;
	config = policy_config(tracee);
	config->mode = parsed;
	if (parsed != NET_POLICY_OFF &&
	    (resolve_pending_domains(tracee, config->allow, &config->allow_count) < 0 ||
	     resolve_pending_domains(tracee, config->deny, &config->deny_count) < 0))
		return -1;
	return 0;
}

int net_policy_add_destination(Tracee *tracee, const char *value, int deny)
{
	NetPolicyConfig *config;
	NetPolicyRule *rules;
	unsigned int *count;
	const char *host;
	size_t host_len, prefix_len, suffix_offset;
	if (ensure_extension(tracee) < 0)
		return -1;
	config = policy_config(tracee);
	rules = deny ? config->deny : config->allow;
	count = deny ? &config->deny_count : &config->allow_count;
	if (destination_host(value, &host, &host_len, &prefix_len, &suffix_offset) < 0 ||
	    add_rule(tracee, rules, count, value) < 0) {
		note(tracee, ERROR, USER, "invalid network destination rule '%s'", value);
		return -1;
	}
	(void)prefix_len;
	(void)suffix_offset;
	if (!host_is_numeric(host, host_len) && !(host_len == 1 && host[0] == '*')) {
		size_t domain_len = host_len;
		if (domain_len >= NET_POLICY_RULE_LEN ||
		    (domain_len != 0 && host[domain_len - 1] == '.'))
			domain_len--;
		if (domain_len == 0 || !valid_domain(host, domain_len)) {
			note(tracee, ERROR, USER, "invalid network domain '%.*s'",
			     (int)host_len, host);
			return -1;
		}
		rules[*count - 1].is_domain = 1;
		if (config->mode != NET_POLICY_OFF &&
		    resolve_pending_domains(tracee, rules, count) < 0)
			return -1;
	}
	return 0;
}

int net_policy_add_bind(Tracee *tracee, const char *value, int deny)
{
	NetPolicyConfig *config;
	unsigned int port;
	if (parse_port(value, &port) < 0) {
		note(tracee, ERROR, USER, "invalid network bind port '%s'", value);
		return -1;
	}
	(void)port;
	if (ensure_extension(tracee) < 0)
		return -1;
	config = policy_config(tracee);
	if (add_rule(tracee, deny ? config->deny_bind : config->allow_bind,
			 deny ? &config->deny_bind_count : &config->allow_bind_count, value) < 0)
		return -1;
	return 0;
}

int net_policy_is_active(Tracee *tracee)
{
	NetPolicyConfig *config = policy_config(tracee);
	return config != NULL && config->mode != NET_POLICY_OFF;
}

int net_policy_allow_publication(Tracee *tracee, uint16_t host_port,
				 uint16_t guest_port)
{
	NetPolicyConfig *config = policy_config(tracee);
	(void)host_port;
	if (config == NULL || config->mode == NET_POLICY_OFF)
		return 0;
	if (bind_matches(config->deny_bind, config->deny_bind_count, guest_port))
		return -EACCES;
	if (config->mode == NET_POLICY_DENY &&
	    !bind_matches(config->allow_bind, config->allow_bind_count, guest_port))
		return -EACCES;
	return ask_harness(config, tracee, NET_ASK_PUBLICATION, NULL,
			   guest_port, host_port, 1, VNP_NET_CLASS_BRIDGE,
			   config->proxy);
}

int net_policy_set_ask_fd(Tracee *tracee, const char *value)
{
	NetPolicyConfig *config;
	char *end;
	long fd;
	if (value == NULL || *value == '\0')
		return -EINVAL;
	fd = strtol(value, &end, 10);
	if (*end != '\0' || fd < 0 || fd > INT_MAX)
		return -EINVAL;
	if (ensure_extension(tracee) < 0)
		return -ENOMEM;
	config = policy_config(tracee);
	if (fcntl((int)fd, F_SETFD, FD_CLOEXEC) < 0)
		return -errno;
	/* A closed harness must produce a deny, never terminate the tracer via
	 * SIGPIPE while the fixed-size request is being written. */
	signal(SIGPIPE, SIG_IGN);
	config->ask_fd = (int)fd;
	return 0;
}
