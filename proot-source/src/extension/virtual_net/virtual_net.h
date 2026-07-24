#ifndef VIRTUAL_NET_H
#define VIRTUAL_NET_H

#include "extension/extension.h"

/**
 * Virtual network extension callback.
 * Intercepts socket syscalls to implement isolated virtual networks
 * using Abstract Unix Domain Sockets when --proxy NAME is active.
 *
 * Without -p, all bind/connect stay virtual (zero TCP ports used).
 * With -p HOST:VIRTUAL, a TCP→Unix bridge is created for external access.
 */
extern int vnp_callback(Extension *extension, ExtensionEvent event,
                         intptr_t data1, intptr_t data2);

/**
 * Register a port exposed to the host via -p HOST:VIRTUAL.
 * Called from handle_option_port_mapping() when --proxy is active.
 * @param tracee       Current tracee
 * @param host_port    TCP port on host (0.0.0.0:host_port)
 * @param virtual_port Virtual port to bridge to
 * @return 0 success, -1 error
 */
extern int vnp_add_expose(Tracee *tracee, uint16_t host_port, uint16_t virtual_port);

/**
 * Configure virtual network with the given proxy name.
 * Called from handle_option_proxy().
 * @param tracee     Current tracee
 * @param proxy_name Name for this virtual network (isolation boundary)
 * @return 0 success, -1 error
 */
extern int vnp_configure(Tracee *tracee, const char *proxy_name);

#endif /* VIRTUAL_NET_H */
