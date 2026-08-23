#ifndef PROOT_NET_POLICY_H
#define PROOT_NET_POLICY_H

#include <stdint.h>

#include "extension/extension.h"

typedef enum {
	NET_POLICY_OFF = 0,
	NET_POLICY_DENY,
	NET_POLICY_ALLOW,
} NetPolicyMode;

extern int net_policy_callback(Extension *extension, ExtensionEvent event,
				       intptr_t data1, intptr_t data2);
extern int net_policy_configure(Tracee *tracee, const char *mode);
extern int net_policy_add_destination(Tracee *tracee, const char *value,
					      int deny);
extern int net_policy_add_bind(Tracee *tracee, const char *value, int deny);
extern int net_policy_is_active(Tracee *tracee);
extern int net_policy_allow_publication(Tracee *tracee, uint16_t host_port,
					uint16_t guest_port);
extern int net_policy_set_ask_fd(Tracee *tracee, const char *value);

#endif /* PROOT_NET_POLICY_H */
