#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <asm/types.h>
#include <linux/net.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include "extension/extension.h"
#include "tracee/tracee.h"
#include "tracee/mem.h"
#include "cli/note.h"

#define PORT_THRESHOLD  1024
#define PORT_ADDITION   2000
#define MAX_PORT_SEARCH 100
#define SOCKETCALL_ARGS_SIZE (sizeof(long) * 6)

static inline size_t get_sockaddr_len(const struct sockaddr_storage *addr) {
    if (addr->ss_family == AF_INET)
        return sizeof(struct sockaddr_in);
    else if (addr->ss_family == AF_INET6)
        return sizeof(struct sockaddr_in6);
    return sizeof(struct sockaddr_storage);
}

static inline uint16_t extract_port(const struct sockaddr_storage *addr) {
    if (addr->ss_family == AF_INET)
        return ntohs(((const struct sockaddr_in *)addr)->sin_port);
    else if (addr->ss_family == AF_INET6)
        return ntohs(((const struct sockaddr_in6 *)addr)->sin6_port);
    return 0;
}

static bool is_port_available(uint16_t port, int family);
static uint16_t find_available_port(uint16_t start_port, int family);
static int find_mapping(PortSwitchConfig *config, uint16_t container_port);
static void write_back_port(Tracee *tracee, bool is_socketcall, bool is_udp,
                            void *sockaddr, size_t sockaddr_size, long *socketcall_arg2);
static bool is_localhost(struct sockaddr_storage *my_sockaddr);

int port_switch_callback(Extension *extension, ExtensionEvent event, intptr_t data1 UNUSED, intptr_t data2 UNUSED) {
    switch (event) {
    case INITIALIZATION: {
        static FilteredSysnum filtered_sysnums[] = {
            { PR_bind, FILTER_SYSEXIT },
            { PR_connect, FILTER_SYSEXIT },
            { PR_socketcall, FILTER_SYSEXIT },
            { PR_sendto, FILTER_SYSEXIT },
            { PR_recvfrom, FILTER_SYSEXIT },
            FILTERED_SYSNUM_END
        };
        extension->filtered_sysnums = filtered_sysnums;

        PortSwitchConfig *config = talloc_zero(extension, PortSwitchConfig);
        if (config == NULL)
            return -1;
        config->auto_redirect = false;
        config->count = 0;
        extension->config = config;

        return 0;
    }

    case SYSCALL_ENTER_END: {
        Tracee *tracee = TRACEE(extension);

        switch(get_sysnum(tracee, ORIGINAL)) {

            case PR_bind: {
                struct sockaddr_storage my_sockaddr;
                if (read_data(tracee, &my_sockaddr, peek_reg(tracee, ORIGINAL, SYSARG_2), sizeof(struct sockaddr_storage)) < 0)
                    return 0;

                PortSwitchConfig *config = talloc_get_type_abort(extension->config, PortSwitchConfig);
                int family = my_sockaddr.ss_family;
                uint16_t original_port = extract_port(&my_sockaddr);

                if (original_port == 0)
                    return 0;

                int idx = find_mapping(config, original_port);
                if (idx >= 0) {
                    uint16_t target = config->mappings[idx].host_port;
                    target = find_available_port(target, family);
                    if (target == 0)
                        return 0;

                    if (family == AF_INET)
                        ((struct sockaddr_in *)&my_sockaddr)->sin_port = htons(target);
                    else if (family == AF_INET6)
                        ((struct sockaddr_in6 *)&my_sockaddr)->sin6_port = htons(target);

                    VERBOSE(tracee, 1, "port mapping: container:%d -> host:%d", original_port, target);
                    write_back_port(tracee, false, false, &my_sockaddr, get_sockaddr_len(&my_sockaddr), NULL);
                    return 0;
                }

                if (config->auto_redirect && original_port < PORT_THRESHOLD) {
                    uint16_t target = original_port + PORT_ADDITION;
                    target = find_available_port(target, family);
                    if (target == 0)
                        return 0;

                    if (family == AF_INET)
                        ((struct sockaddr_in *)&my_sockaddr)->sin_port = htons(target);
                    else if (family == AF_INET6)
                        ((struct sockaddr_in6 *)&my_sockaddr)->sin6_port = htons(target);

                    VERBOSE(tracee, 1, "privileged port: %d -> %d (auto-redirected)", original_port, target);
                    write_back_port(tracee, false, false, &my_sockaddr, get_sockaddr_len(&my_sockaddr), NULL);
                }

                return 0;
            }

            case PR_connect: {
                struct sockaddr_storage my_sockaddr;
                if (read_data(tracee, &my_sockaddr, peek_reg(tracee, ORIGINAL, SYSARG_2), sizeof(struct sockaddr_storage)) < 0)
                    return 0;

                if(is_localhost(&my_sockaddr)) {
                    PortSwitchConfig *config = talloc_get_type_abort(extension->config, PortSwitchConfig);
                    int family = my_sockaddr.ss_family;
                    uint16_t original_port = extract_port(&my_sockaddr);

                    if (original_port == 0)
                        return 0;

                    int idx = find_mapping(config, original_port);
                    if (idx >= 0) {
                        uint16_t target = config->mappings[idx].host_port;
                        target = find_available_port(target, family);
                        if (target == 0)
                            return 0;

                        if (family == AF_INET)
                            ((struct sockaddr_in *)&my_sockaddr)->sin_port = htons(target);
                        else if (family == AF_INET6)
                            ((struct sockaddr_in6 *)&my_sockaddr)->sin6_port = htons(target);

                        write_back_port(tracee, false, false, &my_sockaddr, get_sockaddr_len(&my_sockaddr), NULL);
                    } else if (config->auto_redirect && original_port < PORT_THRESHOLD) {
                        uint16_t target = original_port + PORT_ADDITION;
                        target = find_available_port(target, family);
                        if (target == 0)
                            return 0;

                        if (family == AF_INET)
                            ((struct sockaddr_in *)&my_sockaddr)->sin_port = htons(target);
                        else if (family == AF_INET6)
                            ((struct sockaddr_in6 *)&my_sockaddr)->sin6_port = htons(target);

                        write_back_port(tracee, false, false, &my_sockaddr, get_sockaddr_len(&my_sockaddr), NULL);
                    }
                }
                return 0;
            }

            case PR_sendto: {
                struct sockaddr_storage my_sockaddr;
                if(peek_reg(tracee, ORIGINAL, SYSARG_5) != 0) {
                    if (read_data(tracee, &my_sockaddr, peek_reg(tracee, ORIGINAL, SYSARG_5), sizeof(struct sockaddr_storage)) < 0)
                        return 0;

                    if(is_localhost(&my_sockaddr)) {
                        PortSwitchConfig *config = talloc_get_type_abort(extension->config, PortSwitchConfig);
                        int family = my_sockaddr.ss_family;
                        uint16_t original_port = extract_port(&my_sockaddr);

                        if (original_port == 0)
                            return 0;

                        int idx = find_mapping(config, original_port);
                        if (idx >= 0) {
                            uint16_t target = config->mappings[idx].host_port;
                            target = find_available_port(target, family);
                            if (target == 0)
                                return 0;

                            if (family == AF_INET)
                                ((struct sockaddr_in *)&my_sockaddr)->sin_port = htons(target);
                            else if (family == AF_INET6)
                                ((struct sockaddr_in6 *)&my_sockaddr)->sin6_port = htons(target);

                            write_back_port(tracee, false, true, &my_sockaddr, get_sockaddr_len(&my_sockaddr), NULL);
                        } else if (config->auto_redirect && original_port < PORT_THRESHOLD) {
                            uint16_t target = original_port + PORT_ADDITION;
                            target = find_available_port(target, family);
                            if (target == 0)
                                return 0;

                            if (family == AF_INET)
                                ((struct sockaddr_in *)&my_sockaddr)->sin_port = htons(target);
                            else if (family == AF_INET6)
                                ((struct sockaddr_in6 *)&my_sockaddr)->sin6_port = htons(target);

                            write_back_port(tracee, false, true, &my_sockaddr, get_sockaddr_len(&my_sockaddr), NULL);
                        }
                    }
                }
                return 0;
            }

            case PR_socketcall: {
                int call;
                long a[6];
                call = peek_reg(tracee, ORIGINAL, SYSARG_1);
                read_data(tracee, a, peek_reg(tracee, ORIGINAL, SYSARG_2), sizeof(a));
                switch(call) {

                    case SYS_BIND: {
                        struct sockaddr_storage my_sockaddr;
                        if (read_data(tracee, &my_sockaddr, a[1], sizeof(struct sockaddr_storage)) < 0)
                            break;

                        PortSwitchConfig *config = talloc_get_type_abort(extension->config, PortSwitchConfig);
                        int family = my_sockaddr.ss_family;
                        uint16_t original_port = extract_port(&my_sockaddr);

                        if (original_port == 0)
                            break;

                        int idx = find_mapping(config, original_port);
                        if (idx >= 0) {
                            uint16_t target = config->mappings[idx].host_port;
                            target = find_available_port(target, family);
                            if (target == 0)
                                break;

                            if (family == AF_INET)
                                ((struct sockaddr_in *)&my_sockaddr)->sin_port = htons(target);
                            else if (family == AF_INET6)
                                ((struct sockaddr_in6 *)&my_sockaddr)->sin6_port = htons(target);

                            VERBOSE(tracee, 1, "port mapping: container:%d -> host:%d", original_port, target);
                            write_back_port(tracee, true, false, &my_sockaddr, get_sockaddr_len(&my_sockaddr), a);
                        } else if (config->auto_redirect && original_port < PORT_THRESHOLD) {
                            uint16_t target = original_port + PORT_ADDITION;
                            target = find_available_port(target, family);
                            if (target == 0)
                                break;

                            if (family == AF_INET)
                                ((struct sockaddr_in *)&my_sockaddr)->sin_port = htons(target);
                            else if (family == AF_INET6)
                                ((struct sockaddr_in6 *)&my_sockaddr)->sin6_port = htons(target);

                            VERBOSE(tracee, 1, "privileged port: %d -> %d (auto-redirected)", original_port, target);
                            write_back_port(tracee, true, false, &my_sockaddr, get_sockaddr_len(&my_sockaddr), a);
                        }

                        break;
                    }

                    case SYS_CONNECT: {
                        struct sockaddr_storage my_sockaddr;
                        if (read_data(tracee, &my_sockaddr, a[1], sizeof(struct sockaddr_storage)) < 0)
                            break;

                        if(is_localhost(&my_sockaddr)) {
                            PortSwitchConfig *config = talloc_get_type_abort(extension->config, PortSwitchConfig);
                            int family = my_sockaddr.ss_family;
                            uint16_t original_port = extract_port(&my_sockaddr);

                            if (original_port == 0)
                                break;

                            int idx = find_mapping(config, original_port);
                            if (idx >= 0) {
                                uint16_t target = config->mappings[idx].host_port;
                                target = find_available_port(target, family);
                                if (target == 0)
                                    break;

                                if (family == AF_INET)
                                    ((struct sockaddr_in *)&my_sockaddr)->sin_port = htons(target);
                                else if (family == AF_INET6)
                                    ((struct sockaddr_in6 *)&my_sockaddr)->sin6_port = htons(target);

                                write_back_port(tracee, true, false, &my_sockaddr, get_sockaddr_len(&my_sockaddr), a);
                            } else if (config->auto_redirect && original_port < PORT_THRESHOLD) {
                                uint16_t target = original_port + PORT_ADDITION;
                                target = find_available_port(target, family);
                                if (target == 0)
                                    break;

                                if (family == AF_INET)
                                    ((struct sockaddr_in *)&my_sockaddr)->sin_port = htons(target);
                                else if (family == AF_INET6)
                                    ((struct sockaddr_in6 *)&my_sockaddr)->sin6_port = htons(target);

                                write_back_port(tracee, true, false, &my_sockaddr, get_sockaddr_len(&my_sockaddr), a);
                            }
                        }

                        break;
                    }

                    case SYS_SENDTO: {
                        struct sockaddr_storage my_sockaddr;
                        if(a[4] != 0) {
                            if (read_data(tracee, &my_sockaddr, a[4], sizeof(struct sockaddr_storage)) < 0)
                                break;

                            if(is_localhost(&my_sockaddr)) {
                                PortSwitchConfig *config = talloc_get_type_abort(extension->config, PortSwitchConfig);
                                int family = my_sockaddr.ss_family;
                                uint16_t original_port = extract_port(&my_sockaddr);

                                if (original_port == 0)
                                    break;

                                int idx = find_mapping(config, original_port);
                                if (idx >= 0) {
                                    uint16_t target = config->mappings[idx].host_port;
                                    target = find_available_port(target, family);
                                    if (target == 0)
                                        break;

                                    if (family == AF_INET)
                                        ((struct sockaddr_in *)&my_sockaddr)->sin_port = htons(target);
                                    else if (family == AF_INET6)
                                        ((struct sockaddr_in6 *)&my_sockaddr)->sin6_port = htons(target);

                                    write_back_port(tracee, true, true, &my_sockaddr, get_sockaddr_len(&my_sockaddr), a);
                                } else if (config->auto_redirect && original_port < PORT_THRESHOLD) {
                                    uint16_t target = original_port + PORT_ADDITION;
                                    target = find_available_port(target, family);
                                    if (target == 0)
                                        break;

                                    if (family == AF_INET)
                                        ((struct sockaddr_in *)&my_sockaddr)->sin_port = htons(target);
                                    else if (family == AF_INET6)
                                        ((struct sockaddr_in6 *)&my_sockaddr)->sin6_port = htons(target);

                                    write_back_port(tracee, true, true, &my_sockaddr, get_sockaddr_len(&my_sockaddr), a);
                                }
                            }
                        }

                        break;
                    }
                    default:
                        break;
                }

                return 0;
            }

            default:
                return 0;
        }
    }
    default:
        return 0;
    }
}

static bool is_port_available(uint16_t port, int family) {
    int sock = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
        return false;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    bool available = false;

    if (family == AF_INET) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        available = (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    } else if (family == AF_INET6) {
        struct sockaddr_in6 addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(port);
        addr.sin6_addr = in6addr_loopback;
        available = (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    }

    close(sock);
    return available;
}

static uint16_t find_available_port(uint16_t start_port, int family) {
    for (uint16_t i = 0; i < MAX_PORT_SEARCH; i++) {
        uint16_t port = start_port + i;
        if (is_port_available(port, family))
            return port;
    }
    return 0;
}

static int find_mapping(PortSwitchConfig *config, uint16_t container_port) {
    for (int i = 0; i < config->count; i++) {
        if (config->mappings[i].container_port == container_port)
            return i;
    }
    return -1;
}

static void write_back_port(Tracee *tracee, bool is_socketcall, bool is_udp,
                            void *sockaddr, size_t sockaddr_size, long *socketcall_arg2) {
    if (is_socketcall && is_udp) {
        write_data(tracee, socketcall_arg2[4], sockaddr, sockaddr_size);
        write_data(tracee, peek_reg(tracee, CURRENT, SYSARG_2), socketcall_arg2, SOCKETCALL_ARGS_SIZE);
    } else if (!is_socketcall && is_udp) {
        write_data(tracee, peek_reg(tracee, CURRENT, SYSARG_5), sockaddr, sockaddr_size);
    } else if (is_socketcall && !is_udp) {
        write_data(tracee, socketcall_arg2[1], sockaddr, sockaddr_size);
        write_data(tracee, peek_reg(tracee, CURRENT, SYSARG_2), socketcall_arg2, SOCKETCALL_ARGS_SIZE);
    } else if (!is_socketcall && !is_udp) {
        write_data(tracee, peek_reg(tracee, CURRENT, SYSARG_2), sockaddr, sockaddr_size);
    }
}

bool is_localhost(struct sockaddr_storage *my_sockaddr) {
    switch(my_sockaddr->ss_family) {
        case AF_INET: {
            const struct sockaddr_in *in = (const struct sockaddr_in *)my_sockaddr;
            return (in->sin_addr.s_addr == htonl(INADDR_LOOPBACK));
        }
        case AF_INET6: {
            const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)my_sockaddr;
            return IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr);
        }
        default:
            return false;
    }
}
