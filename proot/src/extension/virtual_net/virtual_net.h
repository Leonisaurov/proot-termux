/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of proot-termux, a fork of PRoot.
 *
 * Copyright (C) 2025-2026 Leonisaurov
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef VIRTUAL_NET_H
#define VIRTUAL_NET_H

#include "extension/extension.h"
#include <sys/socket.h>

typedef enum {
	VNP_NET_CLASS_UNKNOWN = 0,
	VNP_NET_CLASS_VIRTUAL = 1,
	VNP_NET_CLASS_EXTERNAL = 2,
	VNP_NET_CLASS_BRIDGE = 3,
} VnpNetworkClass;

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
extern int vnp_add_expose(Tracee *tracee, uint16_t host_port, uint16_t virtual_port,
			  const struct sockaddr_storage *host_addr);

/**
 * Configure virtual network with the given proxy name.
 * Called from handle_option_proxy().
 * @param tracee     Current tracee
 * @param proxy_name Name for this virtual network (isolation boundary)
 * @return 0 success, -1 error
 */
extern int vnp_configure(Tracee *tracee, const char *proxy_name);

/* Classify an already decoded guest destination.  This is deliberately a
 * read-only query: it consults only the active instance's fd/expose state and
 * the registry cache populated by a real virtual-net operation.  It must not
 * create directories, open the registry, or take a lock. */
extern VnpNetworkClass vnp_classify_destination(Tracee *tracee,
						const struct sockaddr_storage *addr,
						uint16_t port,
						char *proxy, size_t proxy_size);

#endif /* VIRTUAL_NET_H */
