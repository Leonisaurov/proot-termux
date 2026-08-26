#ifndef PROOT_NET_POLICY_H
#define PROOT_NET_POLICY_H

#include <stdint.h>

#include "extension/extension.h"

typedef enum {
	NET_POLICY_OFF = 0,
	NET_POLICY_DENY,
	NET_POLICY_ALLOW,
} NetPolicyMode;

typedef enum {
	NET_CONTROL_PATH_READ = 1,
	NET_CONTROL_PATH_WRITE,
	NET_CONTROL_PATH_CREATE,
	NET_CONTROL_PATH_DELETE,
	NET_CONTROL_PATH_RENAME,
	NET_CONTROL_PATH_METADATA,
} NetControlPathOperation;

typedef enum {
	NET_CONTROL_REASON_STATIC_RO = 1,
	NET_CONTROL_REASON_MASKED_SHADOW,
	NET_CONTROL_REASON_HIDDEN_SHADOW,
	NET_CONTROL_REASON_STATIC_POLICY,
} NetControlPathReason;

extern int net_policy_callback(Extension *extension, ExtensionEvent event,
				       intptr_t data1, intptr_t data2);
extern int net_policy_configure(Tracee *tracee, const char *mode);
extern int net_policy_add_destination(Tracee *tracee, const char *value,
					      int deny);
extern int net_policy_add_bind(Tracee *tracee, const char *value, int deny);
extern int net_policy_is_active(Tracee *tracee);
extern int net_policy_allow_publication(Tracee *tracee, uint16_t host_port,
					uint16_t guest_port);
extern int net_policy_set_control_fd(Tracee *tracee, const char *value);
extern int net_policy_prepare_control_fd(Tracee *tracee);
extern int net_policy_path_access(Tracee *tracee, const char *path,
					 const char *other_path,
					 NetControlPathOperation operation,
					 NetControlPathReason reason);
/* Returns non-zero when this guest path is outside the fixed infrastructure
 * exemptions and therefore needs a PRCT decision for this operation. */
extern int net_policy_path_requires_control(Tracee *tracee, const char *path,
						 NetControlPathOperation operation);
/* Returns 1/-EACCES for a matching proactive rule, 0 when no rule matches. */
extern int net_policy_path_rule_precheck(Tracee *tracee, const char *path,
						 const char *other_path,
						 NetControlPathOperation operation);
extern int net_policy_set_path_peer(Tracee *tracee, const char *path);
extern void net_policy_clear_path_peer(Tracee *tracee);
extern int net_policy_set_path_operation(Tracee *tracee,
						 NetControlPathOperation operation);
extern NetControlPathOperation net_policy_path_operation_override(Tracee *tracee);
/* 0: ordinary path, -ENOENT: hidden, 1: temporarily revealed/allowed. */
extern int net_policy_shadow_access(Tracee *tracee, const char *path,
					   NetControlPathOperation operation);
extern int net_policy_add_shadow(Tracee *tracee, const char *path);

#endif /* PROOT_NET_POLICY_H */
