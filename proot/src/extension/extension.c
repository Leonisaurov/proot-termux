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

#include <assert.h>     /* assert(3), */
#include <talloc.h>     /* talloc_*, */
#include <sys/queue.h>  /* LIST_*, */
#include <strings.h>    /* bzero(3), */

#include "extension/extension.h"
#include "cli/note.h"
#include "build.h"

#include "compat.h"

static void rebuild_extension_cache(Extensions *extensions);

/**
 * Remove an @extension from its tracee's list, then send it the
 * "REMOVED" event.
 *
 * Note: this is a Talloc destructor.
 */
static int remove_extension(Extension *extension)
{
	Extensions *extensions = extension->owner;
	/* LIST_REMOVE unlinks the node before REMOVED is delivered.  Clear the
	 * optional cache at the same boundary so callbacks cannot observe a
	 * dangling extension during teardown. */
	if (extensions != NULL) {
		if (extensions->net_policy == extension)
			extensions->net_policy = NULL;
		if (extensions->proc_isolation == extension)
			extensions->proc_isolation = NULL;
		if (extensions->virtual_net == extension)
			extensions->virtual_net = NULL;
		if (extensions->resource_limit == extension)
			extensions->resource_limit = NULL;
		if (extensions->fake_id0 == extension)
			extensions->fake_id0 = NULL;
	}
	LIST_REMOVE(extension, link);
	if (extensions != NULL)
		rebuild_extension_cache(extensions);
	if (extension->callback != NULL)
		extension->callback(extension, REMOVED, 0, 0);

	bzero(extension, sizeof(Extension));
	return 0;
}

static void cache_extension(Extensions *extensions, Extension *extension)
{
	if (extension->callback == net_policy_callback)
		extensions->net_policy = extension;
	else if (extension->callback == hpc_callback)
		extensions->proc_isolation = extension;
	else if (extension->callback == vnp_callback)
		extensions->virtual_net = extension;
	else if (extension->callback == rlimit_callback)
		extensions->resource_limit = extension;
	else if (extension->callback == fake_id0_callback)
		extensions->fake_id0 = extension;
}

static void rebuild_extension_cache(Extensions *extensions)
{
	Extension *extension;

	extensions->net_policy = NULL;
	extensions->proc_isolation = NULL;
	extensions->virtual_net = NULL;
	extensions->resource_limit = NULL;
	extensions->fake_id0 = NULL;
	LIST_FOREACH(extension, extensions, link)
		cache_extension(extensions, extension);
}

/**
 * Allocate a new extension for the given @callback then attach it to
 * its @tracee.  This function returns NULL on error, otherwise the
 * new extension.
 */
static Extension *new_extension(Tracee *tracee, extension_callback_t callback)
{
	Extension *extension;

	/* Lazy allocation of the list head. */
	if (tracee->extensions == NULL) {
		tracee->extensions = talloc_zero(tracee, Extensions);
		if (tracee->extensions == NULL)
			return NULL;
	}

	/* Allocate a new extension. */
	extension = talloc_zero(tracee->extensions, Extension);
	if (extension == NULL)
		return NULL;
	extension->callback = callback;
	extension->owner = tracee->extensions;

	/* Attach it to its tracee. */
	LIST_INSERT_HEAD(tracee->extensions, extension, link);
	cache_extension(tracee->extensions, extension);
	talloc_set_destructor(extension, remove_extension);

	return extension;
}

/**
 * Retrieve from @tracee->extensions the extension for the given
 * @callback.
 */
Extension *get_extension(Tracee *tracee, extension_callback_t callback)
{
	Extension *extension;

	if (tracee->extensions == NULL)
		return NULL;

	/* These callbacks are looked up from path/syscall handlers.  A NULL cache
	 * is also the inactive fast path; no list walk is needed in either case. */
	if (callback == net_policy_callback)
		return tracee->extensions->net_policy;
	if (callback == hpc_callback)
		return tracee->extensions->proc_isolation;
	if (callback == vnp_callback)
		return tracee->extensions->virtual_net;
	if (callback == rlimit_callback)
		return tracee->extensions->resource_limit;
	if (callback == fake_id0_callback)
		return tracee->extensions->fake_id0;

	LIST_FOREACH(extension, tracee->extensions, link) {
		if (extension->callback == callback)
			return extension;
	}

	return NULL;
}

/**
 * Initialize a new extension for the given @callback then attach it
 * to its @tracee.  The parameter @cli is its argument that was passed
 * to the command-line interface.  This function return -1 if an error
 * occurred, otherwise 0.
 */
int initialize_extension(Tracee *tracee, extension_callback_t callback, const char *cli)
{
	Extension *extension;
	int status;

	if (callback == NULL) {
		note(tracee, WARNING, INTERNAL, "can't initialize a NULL extension callback");
		return -1;
	}

	extension = new_extension(tracee, callback);
	if (extension == NULL) {
		note(tracee, WARNING, INTERNAL, "can't create a new extension");
		return -1;
	}

	/* Extension lifecycle:
	 * - INITIALIZATION: called right after allocation.
	 * - REMOVED: called during teardown (talloc destructor).
	 * - Other events: called during tracee execution.
	 *
	 * Remove the new extension if its initialization has failed.  */
	status = extension->callback(extension, INITIALIZATION, (intptr_t) cli, 0);
	if (status < 0) {
		TALLOC_FREE(extension);
		return status;
	}

	return 0;
}

/**
 * Rebuild a new list of extensions for this @child from its @parent.
 * The inheritance model is controlled by the @parent.
 */
void inherit_extensions(Tracee *child, Tracee *parent, word_t clone_flags)
{
	Extension *parent_extension;
	Extension *child_extension;
	int status;

	if (parent->extensions == NULL)
		return;

	/* Sanity check.  */
	assert(child->extensions == NULL || clone_flags == CLONE_RECONF);

	LIST_FOREACH(parent_extension, parent->extensions, link) {
		/* Ask the parent how this extension is
		 * inheritable.  */
		status = parent_extension->callback(parent_extension, INHERIT_PARENT,
						(intptr_t)child, clone_flags);

		/* Not inheritable.  */
		if (status < 0)
			continue;

		/* Inheritable...  */
		child_extension = new_extension(child, parent_extension->callback);
		if (child_extension == NULL) {
			note(parent, WARNING, INTERNAL,
				"can't create a new extension for pid %d", child->pid);
			continue;
		}

		if (status == 0) {
			/* ... with a shared config or ...  */
			child_extension->config =
				talloc_reference(child_extension, parent_extension->config);
		}
		else {
			/* ... with another inheritance model.  */
			child_extension->callback(child_extension, INHERIT_CHILD,
						(intptr_t)parent_extension, clone_flags);
		}
	}
}
