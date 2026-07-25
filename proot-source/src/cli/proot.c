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
#include <stdio.h>     /* printf(3), fflush(3), */
#include <unistd.h>    /* write(2), */

#include "cli/cli.h"
#include "cli/note.h"
#include "extension/extension.h"
#include "extension/sysvipc/sysvipc.h"
#include "extension/virtual_net/virtual_net.h"
#include "extension/virtual_net/virtual_net_internal.h"
#include "extension/proc_isolation/proc_isolation.h"
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
		HpcConfig *config = talloc_get_type_abort(ext->config, HpcConfig);
		if (config != NULL) {
			config->flags |= flag;
			VERBOSE(tracee, 2, "proc_isolation: added flag 0x%x (now 0x%x)", flag, config->flags);
			return 0;
		}
	}

	status = initialize_extension(tracee, hpc_callback, (void *)(uintptr_t)flag);
	if (status < 0)
		note(tracee, WARNING, INTERNAL, "proc_isolation not initialized");

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

	/* Default to "." if no CWD were specified.  */
	if (tracee->fs->cwd == NULL) {
		status = handle_option_w(tracee, cli, ".");
		if (status < 0)
			return -1;
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
