/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of proot-termux, a fork of PRoot.
 *
 * Upstream: termux/proot (GPL v2 or later)
 * Fork modifications: Copyright (C) 2025-2026 Leonisaurov
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

#include <dirent.h>    /* DIR, struct dirent, opendir, closedir, readdir) */
#include <stdio.h>     /* rename(2), */
#include <stdlib.h>    /* atoi */
#include <unistd.h>    /* symlink(2), symlinkat(2), readlink(2), lstat(2), unlink(2), unlinkat(2)*/
#include <string.h>    /* str*, strrchr, strcat, strcpy, strncpy, strncmp */
#include <sys/types.h> /* lstat(2), */
#include <sys/stat.h>  /* lstat(2), */
#include <errno.h>     /* E*, */
#include <limits.h>    /* PATH_MAX, */

#include "extension/extension.h"
#include "tracee/tracee.h"
#include "tracee/mem.h"
#include "syscall/syscall.h"
#include "syscall/sysnum.h"
#include "path/path.h"
#include "arch.h"
#include "attribute.h"

#define PREFIX ".proot.l2s."
#define DELETED_SUFFIX " (deleted)"

/**
 * Make it so fake hard links look like real hard link with respect to number of links and inode 
 * This function returns -errno if an error occured, otherwise 0.
 */
static int handle_sysexit_end(Tracee *tracee)
{
    word_t sysnum;

    sysnum = get_sysnum(tracee, ORIGINAL);

    switch (sysnum) {

    case PR_lstat64:                   //int lstat(const char *path, struct stat *buf);
    case PR_lstat: {                     //int lstat(const char *path, struct stat *buf);
        word_t result;
        Reg sysarg_stat;
        Reg sysarg_path;
        int status;
        struct stat statl;
        ssize_t size;
        char original[PATH_MAX];
        char intermediate[PATH_MAX];

        /* Override only if it succeed.  */
        result = peek_reg(tracee, CURRENT, SYSARG_RESULT);
        if (result != 0)
            return 0;

        /*for lstat, the link2symlink extension should have already drilled down to the final final and past a fake hard link
          so if the path returned points to a symbolic link, it should be a normal symbolic link*/
        sysarg_path = SYSARG_1;
        size = read_string(tracee, original, peek_reg(tracee, MODIFIED, sysarg_path), PATH_MAX);
        if (size < 0)
            return size;
        if (size >= PATH_MAX)
            return -ENAMETOOLONG;

        /* Check if it is a link */
        status = lstat(original, &statl);
        if (status < 0)  {//shouldn't happen
           return status;
        }

        /* If it is not a link, get out */
        if (!S_ISLNK(statl.st_mode)) {
           return 0;
        }
 
        
        size = readlink(original, intermediate, PATH_MAX);
        if (size < 0)
            return size;

        sysarg_stat = SYSARG_2;

        /* Overwrite the stat struct with the correct size. */
        read_data(tracee, &statl, peek_reg(tracee, ORIGINAL, sysarg_stat), sizeof(statl));
        statl.st_size = (off_t)size;
        status = write_data(tracee, peek_reg(tracee, ORIGINAL,  sysarg_stat), &statl, sizeof(statl));
        if (status < 0)
            return status;

        return 0;
    }

    default:
        return 0;
    }
}

/**
 * Handler for this @extension.  It is triggered each time an @event
 * occurred.  See ExtensionEvent for the meaning of @data1 and @data2.
 */
int fix_symlink_size_callback(Extension *extension, ExtensionEvent event,
                  intptr_t data1 UNUSED, intptr_t data2 UNUSED)
{
    switch (event) {
    case INITIALIZATION: {
        /* List of syscalls handled by this extensions.  */
        static FilteredSysnum filtered_sysnums[] = {
            { PR_lstat,     FILTER_SYSEXIT },
            { PR_lstat64,       FILTER_SYSEXIT },
            FILTERED_SYSNUM_END,
        };
        extension->filtered_sysnums = filtered_sysnums;
        return 0;
    }

    case SYSCALL_EXIT_END: {
        return handle_sysexit_end(TRACEE(extension));
    }

    default:
        return 0;
    }
}
