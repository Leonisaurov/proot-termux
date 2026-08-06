/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of PRoot.
 *
 * Copyright (C) 2015 STMicroelectronics
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 */

#include <string.h>    /* str*(3), */
#include <assert.h>    /* assert(3), */
#include <errno.h>     /* errno, */
#include <stdio.h>     /* printf(3), fflush(3), fopen(3), */
#include <stdlib.h>    /* strtoull(3), */
#include <unistd.h>    /* write(2), sysconf(3), */
#include <limits.h>    /* ULLONG_MAX, PATH_MAX, */
#include <sched.h>     /* sched_setaffinity(2), CPU_*, */
#include <sys/resource.h> /* setpriority(2), prlimit64(2), RLIMIT_*, */
#include <sys/types.h> /* pid_t, */

#include "cli/cli.h"
#include "cli/note.h"
#include "extension/extension.h"
#include "extension/sysvipc/sysvipc.h"
#include "extension/virtual_net/virtual_net.h"
#include "extension/virtual_net/virtual_net_internal.h"
#include "extension/proc_isolation/proc_isolation.h"
#include "extension/resource_limit/resource_limit.h"
#include "supervise/supervise.h"
#include "path/binding.h"
#include "attribute.h"

/* These should be included last.  */
#include "build.h"
#include "cli/proot.h"

static int handle_option_r(Tracee *tracee, const Cli *cli UNUSED, const char *value)
{
	Binding *binding;

	/* ``chroot $PATH`` is semantically equivalent to ``mount
	 * --bind $PATH /``.  */
	binding = new_binding(tracee, value, "/", true, BINDING_ACCESS_RW, BINDING_TYPE_REGULAR);
	if (binding == NULL)
		return -1;

	return 0;
}

static int handle_option_b(Tracee *tracee, const Cli *cli UNUSED, const char *value)
{
	char *host;
	char *guest;
	BindingAccess access_mode = BINDING_ACCESS_RW;

	host = talloc_strdup(tracee->ctx, value);
	if (host == NULL) {
		note(tracee, ERROR, INTERNAL, "can't allocate memory");
		return -1;
	}

	guest = strchr(host, ':');
	if (guest != NULL) {
		*guest = '\0';
		guest++;

		char *perm = strchr(guest, ':');
		if (perm != NULL) {
			*perm = '\0';
			perm++;

			if (strcmp(perm, "ro") == 0)
				access_mode = BINDING_ACCESS_RO;
			else if (strcmp(perm, "wo") == 0)
				access_mode = BINDING_ACCESS_WO;
			else if (strcmp(perm, "rw") != 0)
				note(tracee, WARNING, USER,
					"ignoring unknown access mode '%s' for binding", perm);
		}
	}

	new_binding(tracee, host, guest, true, access_mode, BINDING_TYPE_REGULAR);
	return 0;
}

static int handle_option_mbind(Tracee *tracee, const Cli *cli UNUSED, const char *value)
{
	char *host;
	char *guest;

	host = talloc_strdup(tracee->ctx, value);
	if (host == NULL) {
		note(tracee, ERROR, INTERNAL, "can't allocate memory");
		return -1;
	}

	guest = strchr(host, ':');
	if (guest != NULL) {
		*guest = '\0';
		guest++;
	}

	if (guest == NULL || guest[0] == '\0') {
		note(tracee, ERROR, USER,
			"--mbind requires host:guest format, e.g. --mbind /real/run:/run");
		return -1;
	}

	new_binding(tracee, host, guest, true, BINDING_ACCESS_RW, BINDING_TYPE_MBIND);
	return 0;
}

static int handle_option_q(Tracee *tracee, const Cli *cli UNUSED, const char *value)
{
	const char *ptr;
	size_t nb_args;
	bool last;
	size_t i;

	nb_args = 0;
	ptr = value;
	while (1) {
		nb_args++;

		/* Keep consecutive non-space characters.  */
		while (*ptr != ' ' && *ptr != '\0')
			ptr++;

		/* End-of-string ?  */
		if (*ptr == '\0')
			break;

		/* Skip consecutive space separators.  */
		while (*ptr == ' ' && *ptr != '\0')
			ptr++;

		/* End-of-string ?  */
		if (*ptr == '\0')
			break;
	}

	tracee->qemu = talloc_zero_array(tracee, char *, nb_args + 1);
	if (tracee->qemu == NULL)
		return -1;
	talloc_set_name_const(tracee->qemu, "@qemu");

	i = 0;
	ptr = value;
	do {
		const void *start;
		const void *end;
		last = true;

		/* Keep consecutive non-space characters.  */
		start = ptr;
		while (*ptr != ' ' && *ptr != '\0')
			ptr++;
		end = ptr;

		/* End-of-string ?  */
		if (*ptr == '\0')
			goto next;

		/* Remove consecutive space separators.  */
		while (*ptr == ' ' && *ptr != '\0')
			ptr++;

		/* End-of-string ?  */
		if (*ptr == '\0')
			goto next;

		last = false;
	next:
		tracee->qemu[i] = talloc_strndup(tracee->qemu, start, end - start);
		if (tracee->qemu[i] == NULL)
			return -1;
		i++;
	} while (!last);
	assert(i == nb_args);

	new_binding(tracee, "/", HOST_ROOTFS, true, BINDING_ACCESS_RW, BINDING_TYPE_REGULAR);
	new_binding(tracee, "/dev/null", "/etc/ld.so.preload", false, BINDING_ACCESS_RW, BINDING_TYPE_REGULAR);

	return 0;
}

static int handle_option_w(Tracee *tracee, const Cli *cli UNUSED, const char *value)
{
	tracee->fs->cwd = talloc_strdup(tracee->fs, value);
	if (tracee->fs->cwd == NULL)
		return -1;
	talloc_set_name_const(tracee->fs->cwd, "$cwd");
	/* Remember that -w/--cwd/--pwd was explicitly requested so
	 * --exec can propagate the client's cwd to the supervisor's
	 * child tracee (the default "." below never reaches this
	 * handler).  Keep the RAW value too: fs->cwd gets canonicalized
	 * against the client's rootfs later, which is wrong for paths
	 * that only exist in the supervisor's guest rootfs.  "Last
	 * option wins", like handle_option_r's reconfig. */
	TALLOC_FREE(tracee->cwd_raw);
	tracee->cwd_raw = talloc_strdup(tracee, value);
	if (tracee->cwd_raw == NULL)
		return -1;
	talloc_set_name_const(tracee->cwd_raw, "$cwd_raw");
	tracee->cwd_explicit = true;
	return 0;
}

static int handle_option_k(Tracee *tracee, const Cli *cli UNUSED, const char *value)
{
	void *extension;
	int status;

	extension = get_extension(tracee, kompat_callback);
	if (extension != NULL) {
		note(tracee, WARNING, USER, "option -k was already specified");
		note(tracee, INFO, USER, "only the last -k option is enabled");
		TALLOC_FREE(extension);
	}

	status = initialize_extension(tracee, kompat_callback, value);
	if (status < 0)
		note(tracee, WARNING, INTERNAL, "option \"-k %s\" discarded", value);

	return 0;
}

static int handle_option_i(Tracee *tracee, const Cli *cli UNUSED, const char *value)
{
	void *extension;

	extension = get_extension(tracee, fake_id0_callback);
	if (extension != NULL) {
		note(tracee, WARNING, USER, "option -i/-0/-S was already specified");
		note(tracee, INFO, USER, "only the last -i/-0/-S option is enabled");
		TALLOC_FREE(extension);
	}

	(void) initialize_extension(tracee, fake_id0_callback, value);
	return 0;
}

static int handle_option_0(Tracee *tracee, const Cli *cli, const char *value UNUSED)
{
	return handle_option_i(tracee, cli, "0:0");
}

static int handle_option_kill_on_exit(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	tracee->killall_on_exit = true;
	return 0;
}

static int handle_option_v(Tracee *tracee, const Cli *cli UNUSED, const char *value)
{
	int status;

	status = parse_integer_option(tracee, &tracee->verbose, value, "-v");
	if (status < 0)
		return status;

	global_verbose_level = tracee->verbose;
	return 0;
}

extern unsigned char WEAK _binary_licenses_start;
extern unsigned char WEAK _binary_licenses_end;

static int handle_option_V(Tracee *tracee UNUSED, const Cli *cli, const char *value UNUSED)
{
	size_t size;

	print_version(cli);
	printf("\n%s\n", cli->colophon);
	fflush(stdout);

	size = &_binary_licenses_end - &_binary_licenses_start;
	if (size > 0)
		write(1, &_binary_licenses_start, size);

	exit_failure = false;
	return -1;
}

static int handle_option_h(Tracee *tracee, const Cli *cli, const char *value UNUSED)
{
	print_usage(tracee, cli, true);
	exit_failure = false;
	return -1;
}

static void new_bindings(Tracee *tracee, const char *bindings[], const char *value)
{
	int i;

	for (i = 0; bindings[i] != NULL; i++) {
		const char *path;

		path = (strcmp(bindings[i], "*path*") != 0
			? expand_front_variable(tracee->ctx, bindings[i])
			: value);

		new_binding(tracee, path, NULL, false, BINDING_ACCESS_RW, BINDING_TYPE_REGULAR);
	}
}

static int handle_option_R(Tracee *tracee, const Cli *cli, const char *value)
{
	int status;

	status = handle_option_r(tracee, cli, value);
	if (status < 0)
		return status;

	new_bindings(tracee, recommended_bindings, value);

	return 0;
}

static int handle_option_S(Tracee *tracee, const Cli *cli, const char *value)
{
	int status;

	status = handle_option_0(tracee, cli, value);
	if (status < 0)
		return status;

	status = handle_option_r(tracee, cli, value);
	if (status < 0)
		return status;

	new_bindings(tracee, recommended_su_bindings, value);

	return 0;
}

static int handle_option_link2symlink(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	int status;

	/* Initialize the link2symlink extension.  */
	status = initialize_extension(tracee, link2symlink_callback, NULL);
	if (status < 0)
		note(tracee, WARNING, INTERNAL, "link2symlink not initialized");

	return 0;
}

#if defined(__ANDROID__) || defined(__BIONIC__)
static int handle_option_ashmem_memfd(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	int status;

	/* Initialize the ashmem-memfd extension.  */
	status = initialize_extension(tracee, ashmem_memfd_callback, NULL);
	if (status < 0)
		note(tracee, WARNING, INTERNAL, "ashmem-memfd not initialized");

	return 0;
}
#endif /* defined(__ANDROID__) || defined(__BIONIC__) */

static int handle_option_sysvipc(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	int status;

	/* Initialize the sysvipc extension.  */
	status = initialize_extension(tracee, sysvipc_callback, NULL);
	if (status < 0)
		note(tracee, WARNING, INTERNAL, "sysvipc not initialized");

	return 0;
}

static int handle_option_L(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
        (void) initialize_extension(tracee, fix_symlink_size_callback, NULL);
        return 0;
}

static int handle_option_H(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
        (void) initialize_extension(tracee, hidden_files_callback, NULL);
        return 0;
}

static int handle_option_port_mapping(Tracee *tracee, const Cli *cli UNUSED, const char *value)
{
	uint16_t host_port, container_port;

	if (sscanf(value, "%hu:%hu", &host_port, &container_port) != 2) {
		note(tracee, ERROR, USER, "invalid port mapping format: %s (expected host:container)", value);
		return -1;
	}

	/* If --proxy active, delegate to virtual_net */
	Extension *vnp_ext = get_extension(tracee, vnp_callback);
	if (vnp_ext != NULL)
		return vnp_add_expose(tracee, host_port, container_port);

	/* Otherwise use port_switch */
	Extension *ps_ext = get_extension(tracee, port_switch_callback);
	if (ps_ext == NULL) {
		initialize_extension(tracee, port_switch_callback, NULL);
		ps_ext = get_extension(tracee, port_switch_callback);
		if (ps_ext == NULL)
			return -1;
	}

	PortSwitchConfig *config = talloc_get_type_abort(((Extension *)ps_ext)->config, PortSwitchConfig);
	if (config->count >= 64) {
		note(tracee, ERROR, USER, "too many port mappings (max 64)");
		return -1;
	}
	config->mappings[config->count].host_port = host_port;
	config->mappings[config->count].container_port = container_port;
	config->count++;
	return 0;
}

static int handle_option_p(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	Extension *ext = get_extension(tracee, port_switch_callback);
	if (ext == NULL) {
		initialize_extension(tracee, port_switch_callback, NULL);
		ext = get_extension(tracee, port_switch_callback);
		if (ext == NULL)
			return -1;
	}

	PortSwitchConfig *config = talloc_get_type_abort(((Extension *)ext)->config, PortSwitchConfig);
	config->auto_redirect = true;
	return 0;
}

/**
 * Handler for "--proxy name".
 * Activates virtual network isolation for this proot instance.
 */
static int handle_option_proxy(Tracee *tracee, const Cli *cli UNUSED, const char *value)
{
        if (value == NULL || value[0] == '\0') {
                note(tracee, ERROR, USER,
                     "missing proxy name: use --proxy NAME");
                return -1;
        }

        return vnp_configure(tracee, value);
}

/**
 * Handler for "--supervise".
 * Sets the supervise flag on the tracee so the event loop stays alive
 * after the root tracee exits, accepting --exec connections.
 */
static int handle_option_supervise(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	tracee->supervise = true;
	VERBOSE(tracee, 1, "supervise mode enabled");
	return 0;
}

/**
 * Handler for "--exec" (only reached for --help display).
 * The actual dispatch happens in main() before parse_config().
 * If we get here, something went wrong — show usage.
 */
/**
 * Handler for "--exec PID".
 * Stores the target PID in tracee->exec_target.
 * After parse_config, main() will connect to the supervisor
 * instead of launching a local tracee.
 */
static int handle_option_exec(Tracee *tracee, const Cli *cli UNUSED, const char *value)
{
	if (value == NULL || value[0] == '\0') {
		note(tracee, ERROR, USER, "--exec requires a PID");
		return -1;
	}

	char *end = NULL;
	errno = 0;
	pid_t target_pid = (pid_t)strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0') {
		note(tracee, ERROR, USER, "--exec: invalid PID '%s'", value);
		return -1;
	}

	tracee->exec_target = target_pid;
	VERBOSE(tracee, 2, "--exec: will connect to PID %d", target_pid);
	return 0;
}

/**
 * Helper: add an isolation flag to the proc_isolation extension.
 * If the extension already exists, OR the flag into config->flags.
 * Otherwise, create a new extension with just this flag.
 */
static int handle_proc_isolation_flag(Tracee *tracee, unsigned int flag)
{
	int status;

	Extension *ext = get_extension(tracee, hpc_callback);
	if (ext != NULL) {
		HpcConfig *config = talloc_get_type(ext->config, HpcConfig);
		if (config != NULL) {
			config->flags |= flag;
			VERBOSE(tracee, 2, "proc_isolation: added flag 0x%x (now 0x%x)", flag, config->flags);
			return 0;
		}
	}

	status = initialize_extension(tracee, hpc_callback, (void *)(uintptr_t)flag);
	if (status < 0) {
		note(tracee, WARNING, INTERNAL, "proc_isolation not initialized");
		return -1;
	}

	return 0;
}

static int handle_option_proc_isolation(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	int status;

	status = handle_proc_isolation_flag(tracee, ISOLATE_PROC | ISOLATE_PTRACE);
	if (status < 0)
		note(tracee, WARNING, INTERNAL, "proc_isolation not initialized");

	return 0;
}

static int handle_option_proc_isolated(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	return handle_proc_isolation_flag(tracee, ISOLATE_PROC);
}

static int handle_option_ptrace_isolated(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	return handle_proc_isolation_flag(tracee, ISOLATE_PTRACE);
}

static int handle_option_reboot_isolated(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	return handle_proc_isolation_flag(tracee, ISOLATE_REBOOT);
}

static int handle_option_swap_isolated(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	return handle_proc_isolation_flag(tracee, ISOLATE_SWAP);
}

static int handle_option_kexec_isolated(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	return handle_proc_isolation_flag(tracee, ISOLATE_KEXEC);
}

static int handle_option_ioport_isolated(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	return handle_proc_isolation_flag(tracee, ISOLATE_IOPORT);
}

static int handle_option_bpf_isolated(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	return handle_proc_isolation_flag(tracee, ISOLATE_BPF);
}

static int handle_option_perf_isolated(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	return handle_proc_isolation_flag(tracee, ISOLATE_PERF);
}

static int handle_option_handle_isolated(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	return handle_proc_isolation_flag(tracee, ISOLATE_HANDLE);
}

/**
 * Resource limits (Fase 1): host-side + tracee-side --mem-limit.
 *
 * The --*-limit options below only store the parsed values into
 * @resource_config.  The host-side limits (--cpu-limit/--single-core,
 * --nice, --fd-limit, --proc-limit, --resource-isolated) are applied by
 * resource_config_apply(), called from main() after parse_config() and
 * before launch_process(); tracees inherit them through fork/exec.
 *
 * --mem-limit is handled differently and is the ONLY tracee-side limit:
 * RLIMIT_AS must NOT be applied to the host, because proot's virtual
 * address space is huge on bionic (~10 GiB) and an RLIMIT_AS cap would
 * crash proot with SIGSEGV/SIGABRT on the next allocation.  The value is
 * stored here and applied to each tracee after its execve (post-exec, in
 * execve/exit.c), when the guest executable is loaded and the tracee's
 * VSZ is small.  See resource_config_mem_limit_bytes().
 */

/* Minimum accepted --mem-limit: a guest needs a floor of virtual address
 * space to load its executable, libraries and stack.  Values below this
 * are rejected at parse time with a clear error.  */
#define RESOURCE_MEM_LIMIT_MIN (16 * 1024 * 1024)

struct ResourceConfig {
	/* --cpu-limit N / --single-core: restrict to the first N CPUs.  */
	int cpu_limit;		/* 0 = not set, else >= 1.  */

	/* --mem-limit N[KMG]: RLIMIT_AS cap, in bytes.  */
	unsigned long long mem_limit_bytes;	/* 0 = not set.  */

	/* --nice N: setpriority(PRIO_PROCESS, 0, N).  */
	int nice_value;		/* -1 = not set, else [0..19].  */

	/* --fd-limit N: RLIMIT_NOFILE.  */
	unsigned int fd_limit;	/* 0 = not set.  */

	/* --proc-limit N: max live tracees of this proot instance
	 * (guest-side, applied by the resource_limit extension: fork/
	 * clone/vfork answered with -EAGAIN past the limit).  */
	unsigned int proc_limit;	/* 0 = not set.  */
};

static struct ResourceConfig resource_config = {
	.nice_value = -1,
};

/**
 * Initialize the guest-side resource_limit extension (Phase 2: the guest
 * perceives only the limited CPU count via sched_getaffinity and only the
 * limited number of processes via fork/clone/vfork interception).
 *
 * The host-side limits were already applied by resource_config_apply() and
 * tracees inherit them through fork/exec, so a failure here is NOT fatal: it
 * only means the guest keeps seeing all host CPUs and unlimited processes.
 * It must not be silent either, hence the warning.
 */
static void init_guest_resource_limit(Tracee *tracee)
{
	int status;

	status = rlimit_configure(tracee);
	if (status < 0)
		note(tracee, WARNING, SYSTEM,
			"failed to initialize the resource_limit extension "
			"(the guest will see all host CPUs and unlimited processes)");
}

static int handle_option_cpu_limit(Tracee *tracee, const Cli *cli UNUSED, const char *value)
{
	int n;
	long nb_cpus;

	if (parse_integer_option(tracee, &n, value, "--cpu-limit") < 0)
		return -1;

	if (n < 1) {
		note(tracee, ERROR, USER, "--cpu-limit: invalid value %d (expected at least 1 CPU)", n);
		return -1;
	}
	if (n > CPU_SETSIZE) {
		note(tracee, ERROR, USER, "--cpu-limit: %d exceeds the maximum supported CPU count (%d)", n, CPU_SETSIZE);
		return -1;
	}

	/* Reject more CPUs than the kernel actually exposes.  */
	nb_cpus = sysconf(_SC_NPROCESSORS_CONF);
	if (nb_cpus > 0 && n > nb_cpus) {
		note(tracee, ERROR, USER, "--cpu-limit: %d exceeds the number of available CPUs (%ld)", n, nb_cpus);
		return -1;
	}

	resource_config.cpu_limit = n;
	VERBOSE(tracee, 1, "--cpu-limit: will restrict proot to the first %d CPU(s)", n);

	/* Fase 2 (guest-side): make the guest perceive only these N CPUs
	 * (intercepts sched_getaffinity).  No-op when no limit is set.  */
	init_guest_resource_limit(tracee);
	return 0;
}

static int handle_option_single_core(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	resource_config.cpu_limit = 1;
	VERBOSE(tracee, 1, "--single-core: will restrict proot to a single CPU");

	/* Fase 2 (guest-side): the guest perceives a single CPU.  */
	init_guest_resource_limit(tracee);
	return 0;
}

/**
 * Parse a "--mem-limit N[KMG]" value into *@bytes.  The optional suffix
 * K/M/G (case-insensitive) stands for powers of 1024; a plain number is
 * interpreted as bytes.  Returns 0 on success, -1 on parse error.
 */
static int parse_mem_limit_value(const Tracee *tracee, const char *value, unsigned long long *bytes)
{
	char *end = NULL;
	unsigned long long amount;
	unsigned long long multiplier = 1;

	/* Reject negative values: strtoull("-1") returns ULLONG_MAX without
	 * setting errno, so a negative value would otherwise be silently
	 * accepted as RLIMIT_AS = RLIM_INFINITY.  */
	if (value[0] == '-') {
		note(tracee, ERROR, USER, "--mem-limit: invalid value '%s' (negative values are not allowed)", value);
		return -1;
	}

	errno = 0;
	amount = strtoull(value, &end, 10);
	if (errno != 0 || end == value || amount == 0) {
		note(tracee, ERROR, USER, "--mem-limit: invalid value '%s' (expected N[KMG], N > 0)", value);
		return -1;
	}

	if (*end != '\0') {
		if (end[1] != '\0') {
			note(tracee, ERROR, USER, "--mem-limit: invalid suffix in '%s' (expected K, M or G)", value);
			return -1;
		}
		switch (*end) {
		case 'K': case 'k': multiplier = 1024; break;
		case 'M': case 'm': multiplier = 1024 * 1024; break;
		case 'G': case 'g': multiplier = 1024 * 1024 * 1024; break;
		default:
			note(tracee, ERROR, USER, "--mem-limit: invalid suffix '%c' in '%s' (expected K, M or G)", *end, value);
			return -1;
		}
	}

	if (amount > ULLONG_MAX / multiplier) {
		note(tracee, ERROR, USER, "--mem-limit: value '%s' is too large", value);
		return -1;
	}

	*bytes = amount * multiplier;
	return 0;
}

static int handle_option_mem_limit(Tracee *tracee, const Cli *cli UNUSED, const char *value)
{
	unsigned long long bytes;

	if (parse_mem_limit_value(tracee, value, &bytes) < 0)
		return -1;

	if (bytes < RESOURCE_MEM_LIMIT_MIN) {
		note(tracee, ERROR, USER,
			"--mem-limit %s is too low (minimum is %d MiB: a guest needs a floor "
			"of virtual address space to load its executable, libraries and stack)",
			value, RESOURCE_MEM_LIMIT_MIN / (1024 * 1024));
		return -1;
	}

	resource_config.mem_limit_bytes = bytes;
	VERBOSE(tracee, 1, "--mem-limit: will cap each tracee's address space at %llu bytes (applied post-exec, tracee-side)", bytes);
	return 0;
}

static int handle_option_nice(Tracee *tracee, const Cli *cli UNUSED, const char *value)
{
	int n;

	if (parse_integer_option(tracee, &n, value, "--nice") < 0)
		return -1;

	if (n < 0 || n > 19) {
		note(tracee, ERROR, USER, "--nice: invalid value %d (expected 0..19; raising priority needs root)", n);
		return -1;
	}

	resource_config.nice_value = n;
	VERBOSE(tracee, 1, "--nice: will lower proot priority to %d", n);
	return 0;
}

static int handle_option_fd_limit(Tracee *tracee, const Cli *cli UNUSED, const char *value)
{
	int n;

	if (parse_integer_option(tracee, &n, value, "--fd-limit") < 0)
		return -1;

	if (n < 32) {
		note(tracee, ERROR, USER, "--fd-limit: invalid value %d (expected at least 32, proot needs a fair number of file descriptors)", n);
		return -1;
	}

	resource_config.fd_limit = (unsigned int)n;
	VERBOSE(tracee, 1, "--fd-limit: will cap open file descriptors at %d", n);
	return 0;
}

/**
 * Handler for "--proc-limit N".
 *
 * Guest-side process cap: the value is stored in @resource_config and
 * read by the resource_limit extension (resource_config_proc_limit()),
 * which intercepts fork/clone/vfork at ENTER and answers -EAGAIN when
 * the number of live tracees of this proot instance reaches N.  Only
 * the sandbox's own processes+threads are counted, so the rest of the
 * Termux UID is never affected (unlike the old host-side prlimit64
 * RLIMIT_NPROC, which is gone).
 */
static int handle_option_proc_limit(Tracee *tracee, const Cli *cli UNUSED, const char *value)
{
	int n;

	if (parse_integer_option(tracee, &n, value, "--proc-limit") < 0)
		return -1;

	if (n < 1) {
		note(tracee, ERROR, USER, "--proc-limit: invalid value %d (expected at least 1)", n);
		return -1;
	}

	resource_config.proc_limit = (unsigned int)n;
	VERBOSE(tracee, 1, "--proc-limit: will cap the sandbox at %d live process(es) "
		"(guest-side, EAGAIN past the limit; the rest of the UID is not affected)", n);

	/* Fase 2 (guest-side): make the guest pay the price: intercept
	 * fork/clone/vfork and answer EAGAIN past the limit.  No-op when
	 * no limit is set.  Also initializes the extension when only
	 * --proc-limit is used (without --cpu-limit).  */
	init_guest_resource_limit(tracee);
	return 0;
}

static int handle_option_resource_isolated(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
	/* Combo (Fase 1): single core + nice 10, no memory/fd/proc limits.  */
	resource_config.cpu_limit = 1;
	resource_config.nice_value = 10;
	VERBOSE(tracee, 1, "--resource-isolated: single core + nice 10");

	/* Fase 2 (guest-side): the guest perceives a single CPU.  */
	init_guest_resource_limit(tracee);
	return 0;
}

static int apply_fd_limit(const Tracee *tracee)
{
	struct rlimit64 new_limit;

	new_limit.rlim_cur = (rlim_t)resource_config.fd_limit;
	new_limit.rlim_max = (rlim_t)resource_config.fd_limit;
	if (prlimit64(0, RLIMIT_NOFILE, &new_limit, NULL) < 0) {
		note(tracee, ERROR, SYSTEM, "--fd-limit: prlimit64(RLIMIT_NOFILE)");
		return -1;
	}

	VERBOSE(tracee, 1, "applied RLIMIT_NOFILE = %u", resource_config.fd_limit);
	return 0;
}

/**
 * Apply the parsed HOST-side resource limits to the current (host) process.
 * Called from main() after parse_config() and before launch_process().
 * Order: affinity -> nice -> prlimits.  Tracees inherit the resulting
 * limits through fork/exec, so no propagation is needed.
 *
 * --mem-limit is deliberately NOT applied here: RLIMIT_AS on proot would
 * crash it (its VSZ is ~10 GiB on bionic).  It is applied per-tracee
 * after execve by the post-exec hook in execve/exit.c.
 * --proc-limit is also NOT applied here anymore: instead of limiting
 * the host process (RLIMIT_NPROC would affect the whole Termux UID),
 * it is enforced guest-side by the resource_limit extension, which
 * counts this proot's own tracees and answers -EAGAIN to fork/clone
 * past the limit.
 * Returns 0 on success, -1 on fatal error.
 */
int resource_config_apply(const Tracee *tracee)
{
	/* 1. CPU affinity: restrict to the first N CPUs.  */
	if (resource_config.cpu_limit > 0) {
		cpu_set_t set;
		int i;

		CPU_ZERO(&set);
		for (i = 0; i < resource_config.cpu_limit; i++)
			CPU_SET(i, &set);

		if (sched_setaffinity(0, sizeof(set), &set) < 0) {
			note(tracee, ERROR, SYSTEM, "sched_setaffinity(%d CPUs)",
				resource_config.cpu_limit);
			return -1;
		}
		VERBOSE(tracee, 1, "applied CPU affinity: first %d CPU(s)", resource_config.cpu_limit);
	}

	/* 2. Priority (nice >= 0, no root needed).  */
	if (resource_config.nice_value >= 0) {
		if (setpriority(PRIO_PROCESS, 0, resource_config.nice_value) < 0) {
			note(tracee, ERROR, SYSTEM, "setpriority(nice %d)",
				resource_config.nice_value);
			return -1;
		}
		VERBOSE(tracee, 1, "applied nice value %d", resource_config.nice_value);
	}

	/* 3. prlimits: only file descriptors stay host-side.  --mem-limit
	 * and --proc-limit are NOT applied here (see the comments above):
	 * mem is tracee-side (post-exec), proc is guest-side (extension).  */
	if (resource_config.fd_limit > 0
	    && apply_fd_limit(tracee) < 0)
		return -1;

	return 0;
}

/**
 * Return the --mem-limit value in bytes (0 when not set).  Used by the
 * post-exec hook in execve/exit.c to apply RLIMIT_AS to each tracee once
 * its executable has been loaded, so that proot itself is never limited.
 */
unsigned long long resource_config_mem_limit_bytes(void)
{
	return resource_config.mem_limit_bytes;
}

/**
 * Return the --cpu-limit value (0 when not set, else >= 1).  Used by the
 * guest-side resource_limit extension (rlimit_callback) to know how many
 * CPUs sched_getaffinity() must report to the guest.
 */
int resource_config_cpu_limit(void)
{
	return resource_config.cpu_limit;
}

/**
 * Return the --proc-limit value (0 when not set, else >= 1).  Used by
 * the guest-side resource_limit extension (rlimit_callback) to cap the
 * number of live tracees of this proot instance: fork/clone/vfork past
 * the limit are answered with -EAGAIN.
 */
int resource_config_proc_limit(void)
{
	return resource_config.proc_limit;
}

/**
 * Initialize @tracee->qemu.
 */
static int post_initialize_exe(Tracee *tracee, const Cli *cli UNUSED,
			size_t argc UNUSED, char *const argv[] UNUSED, size_t cursor UNUSED)
{
	char path[PATH_MAX];
	int status;

	/* Nothing else to do ?  */
	if (tracee->qemu == NULL)
		return 0;

	/* Resolve the full guest path to tracee->qemu[0].  */
	status = which(tracee->reconf.tracee, tracee->reconf.paths, path, tracee->qemu[0]);
	if (status < 0)
		return -1;

	/* Actually tracee->qemu[0] has to be a host path from the tracee's
	 * point-of-view, not from the PRoot's point-of-view.  See
	 * translate_execve() for details.  */
	if (tracee->reconf.tracee != NULL) {
		status = detranslate_path(tracee->reconf.tracee, path, NULL);
		if (status < 0)
			return -1;
	}

	tracee->qemu[0] = talloc_strdup(tracee->qemu, path);
	if (tracee->qemu[0] == NULL)
		return -1;

	return 0;
}

/**
 * Initialize @tracee's fields that are mandatory for PRoot but that
 * are not required on the command line, i.e.  "-w" and "-r".
 */
static int pre_initialize_bindings(Tracee *tracee, const Cli *cli,
			size_t argc UNUSED, char *const argv[] UNUSED, size_t cursor)
{
	int status;

	/* Default to "." if no CWD were specified.  Applied directly
	 * (bypassing handle_option_w) so cwd_explicit stays false: this
	 * tells --exec that the client did NOT request a directory and
	 * the child must inherit the supervisor's cwd. */
	if (tracee->fs->cwd == NULL) {
		tracee->fs->cwd = talloc_strdup(tracee->fs, ".");
		if (tracee->fs->cwd == NULL)
			return -1;
		talloc_set_name_const(tracee->fs->cwd, "$cwd");
	}

	 /* The default guest rootfs is "/" if none was specified.  */
	if (get_root(tracee) == NULL) {
		status = handle_option_r(tracee, cli, "/");
		if (status < 0)
			return -1;
	}

	return cursor;
}

const Cli *get_proot_cli(TALLOC_CTX *context UNUSED)
{
	global_tool_name = proot_cli.name;
	return &proot_cli;
}
