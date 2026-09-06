#include <sys/types.h>  /* stat(2), opendir(3), */
#include <sys/stat.h>   /* stat(2), chmod(2), */
#include <unistd.h>     /* stat(2), rmdir(2), unlink(2), readlink(2), */
#include <errno.h>      /* errno(2), */
#include <fcntl.h>
#include <dirent.h>     /* readdir(3), opendir(3), */
#include <string.h>     /* strcmp(3), */
#include <stdlib.h>     /* free(3), getenv(3), */
#include <stdio.h>      /* P_tmpdir, */
#include <talloc.h>     /* talloc(3), */

#include "cli/note.h"

static TALLOC_CTX *temp_context;

TALLOC_CTX *get_temp_context()
{
	if (temp_context == NULL)
		temp_context = talloc_new(NULL);
	return temp_context;
}

void free_temp_context()
{
	talloc_free(temp_context);
	temp_context = NULL;
}

/**
 * Return the path to a directory where temporary files should be
 * created.
 */
const char *get_temp_directory()
{
	static const char *temp_directory = NULL;
	char *tmp;

	if (temp_directory != NULL)
		return temp_directory;

	temp_directory = getenv("PROOT_TMP_DIR");
	if (temp_directory == NULL) {
		temp_directory = P_tmpdir;
	}

	tmp = realpath(temp_directory, NULL);
	if (tmp == NULL) {
		/* A parent PRoot may make realpath(3) fail while the
		 * directory itself remains visible and usable in the guest.
		 * Keep the configured spelling only after checking it is a
		 * directory; this mirrors binding startup and avoids turning
		 * a valid nested temp root into a noisy warning. */
		struct stat st;
		if (stat(temp_directory, &st) == 0 && S_ISDIR(st.st_mode))
			return temp_directory;
		note(NULL, WARNING, SYSTEM,
			"can't canonicalize %s", temp_directory);
		return temp_directory;
	}

	temp_directory = talloc_strdup(get_temp_context(), tmp);
	if (temp_directory == NULL)
		temp_directory = tmp;
	else
		free(tmp);

	return temp_directory;
}

/* Remove entries relative to pinned directory descriptors. Never follow a
 * replaced symlink or change the process cwd during a talloc destructor. */
static int clean_temp_fd(int parent_fd)
{
    int scan_fd = openat(parent_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *dir;
    int errors = 0;
    if (scan_fd < 0)
        return -1;
    dir = fdopendir(scan_fd);
    if (dir == NULL) {
        close(scan_fd);
        return -1;
    }
    for (;;) {
        struct dirent *entry;
        int child_fd;
        errno = 0;
        entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0)
                errors++;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        child_fd = openat(parent_fd, entry->d_name,
                          O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child_fd >= 0) {
            /* O_PATH permits opening mode-000 directories. chmod of '.' is
             * relative to the pinned directory, not to the mutable name. */
            if (fchmodat(child_fd, ".", 0700, 0) < 0 || clean_temp_fd(child_fd) != 0)
                errors++;
            close(child_fd);
            if (unlinkat(parent_fd, entry->d_name, AT_REMOVEDIR) < 0)
                errors++;
        } else if (errno == ENOTDIR || errno == ELOOP) {
            if (unlinkat(parent_fd, entry->d_name, 0) < 0)
                errors++;
        } else if (errno != ENOENT) {
            errors++;
        }
    }
    closedir(dir);
    return errors;
}

/* Created directories are immediate children of the configured temp root.
 * Reject lookalike prefixes, traversal, and symlinks before touching contents. */
static int remove_temp_directory2(const char *path)
{
    const char *base = get_temp_directory();
    size_t length = strlen(base);
    const char *name;
    int root_fd, child_fd, result = -1;
    while (length > 1 && base[length - 1] == '/')
        length--;
    if (strncmp(path, base, length) != 0)
        return -1;
    if (length == 1 && base[0] == '/')
        name = path + 1;
    else {
        if (path[length] != '/')
            return -1;
        name = path + length + 1;
    }
    if (name[0] == '\0' || strchr(name, '/') != NULL ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -1;
    root_fd = open(base, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0)
        return -1;
    child_fd = openat(root_fd, name, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child_fd >= 0) {
        if (fchmodat(child_fd, ".", 0700, 0) == 0 && clean_temp_fd(child_fd) == 0)
            result = 0;
        close(child_fd);
        if (unlinkat(root_fd, name, AT_REMOVEDIR) < 0)
            result = -1;
    } else if (errno == ENOTDIR || errno == ELOOP) {
        /* A replaced root symlink may be removed, never traversed. */
        result = unlinkat(root_fd, name, 0);
    }
    close(root_fd);
    if (result < 0)
        note(NULL, WARNING, SYSTEM, "can't remove temporary directory '%s'", path);
    return result;
}

/**
 * Like remove_temp_directory2() but always return 0.
 *
 * Note: this is a talloc destructor.
 */
static int remove_temp_directory(char *path)
{
	(void) remove_temp_directory2(path);
	return 0;
}

/**
 * Remove the file @path.  This function always returns 0.
 *
 * Note: this is a talloc destructor.
 */
static int remove_temp_file(char *path)
{
	int status;

	status = unlink(path);
	if (status < 0)
		note(NULL, ERROR, SYSTEM, "can't remove '%s'", path);

	return 0;
}

/**
 * Create a path name with the following format:
 * "/tmp/@prefix-$PID-XXXXXX".  The returned C string is either
 * auto-freed if @context is NULL.  This function returns NULL if an
 * error occurred.
 */
char *create_temp_name(TALLOC_CTX *context, const char *prefix)
{
	const char *temp_directory = get_temp_directory();
	char *name;

	if (context == NULL)
		context = get_temp_context();

	name = talloc_asprintf(context, "%s/%s-%d-XXXXXX", temp_directory, prefix, getpid());
	if (name == NULL) {
		note(NULL, ERROR, INTERNAL, "can't allocate memory");
		return NULL;
	}

	return name;
}

/** Reserve a unique filesystem name, then remove the reservation so that
 * bind(2) can create a UNIX socket at the returned path. */
char *create_temp_socket_name(TALLOC_CTX *context, const char *prefix,
		size_t max_length)
{
	char *name;
	int fd;

	if (context == NULL)
		context = get_temp_context();
	name = create_temp_name(context, prefix);
	if (name == NULL || strlen(name) > max_length)
		goto error;

	fd = mkstemp(name);
	if (fd < 0) {
		note(NULL, ERROR, SYSTEM, "can't reserve temporary socket name");
		goto error;
	}
	if (close(fd) < 0) {
		note(NULL, ERROR, SYSTEM, "can't close temporary socket reservation");
		(void) unlink(name);
		goto error;
	}
	if (unlink(name) < 0) {
		note(NULL, ERROR, SYSTEM, "can't remove temporary socket reservation");
		goto error;
	}
	return name;

error:
	if (name != NULL)
		talloc_free(name);
	return NULL;
}

/**
 * Create a directory that will be automatically removed either on
 * PRoot termination if @context is NULL, or once its path name
 * (attached to @context) is freed.  This function returns NULL on
 * error, otherwise the absolute path name to the created directory
 * (@prefix-ed).
 */
const char *create_temp_directory(TALLOC_CTX *context, const char *prefix)
{
	char *name;

	name = create_temp_name(context, prefix);
	if (name == NULL)
		return NULL;

	name = mkdtemp(name);
	if (name == NULL) {
		note(NULL, ERROR, SYSTEM, "can't create temporary directory");
		note(NULL, INFO, USER, "Please set PROOT_TMP_DIR env. variable "
			"to an alternate location (with write permission).");
		return NULL;
	}

	talloc_set_destructor(name, remove_temp_directory);

	return name;
}

/**
 * Create a file that will be automatically removed either on PRoot
 * termination if @context is NULL, or once its path name (attached to
 * @context) is freed.  This function returns NULL on error,
 * otherwise the absolute path name to the created file (@prefix-ed).
 */
const char *create_temp_file(TALLOC_CTX *context, const char *prefix)
{
	char *name;
	int fd;

	name = create_temp_name(context, prefix);
	if (name == NULL)
		return NULL;

	fd = mkstemp(name);
	if (fd < 0) {
		note(NULL, ERROR, SYSTEM, "can't create temporary file");
		note(NULL, INFO, USER, "Please set PROOT_TMP_DIR env. variable "
			"to an alternate location (with write permission).");
		return NULL;
	}
	close(fd);

	talloc_set_destructor(name, remove_temp_file);

	return name;
}

/**
 * Like create_temp_file() but returns an open file stream to the
 * created file.  It's up to the caller to close returned stream.
 */
FILE* open_temp_file(TALLOC_CTX *context, const char *prefix)
{
	char *name;
	FILE *file;
	int fd;

	name = create_temp_name(context, prefix);
	if (name == NULL)
		return NULL;

	fd = mkstemp(name);
	if (fd < 0)
		goto error;

	talloc_set_destructor(name, remove_temp_file);

	file = fdopen(fd, "w");
	if (file == NULL)
		goto error;

	return file;

error:
	if (fd >= 0)
		close(fd);
	note(NULL, ERROR, SYSTEM, "can't create temporary file");
	note(NULL, INFO, USER, "Please set PROOT_TMP_DIR env. variable "
		"to an alternate location (with write permission).");
	return NULL;
}
