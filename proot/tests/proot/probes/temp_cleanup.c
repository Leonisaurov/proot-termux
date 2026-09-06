#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include "path/temp.c"

void note(const Tracee *tracee, Severity severity, Origin origin, const char *message, ...)
{
    (void)tracee; (void)severity; (void)origin; (void)message;
}

int main(int argc, char **argv)
{
    char base[PATH_MAX], outside[PATH_MAX], sentinel[PATH_MAX], name[PATH_MAX];
    char cwd[PATH_MAX], after[PATH_MAX];
    TALLOC_CTX *context = talloc_new(NULL);
    const char *temporary;
    int fd;
    struct stat st;
    assert(argc == 2 && context != NULL);
    assert(snprintf(base, sizeof(base), "%s/runtime", argv[1]) < (int)sizeof(base));
    assert(snprintf(outside, sizeof(outside), "%s-escape", base) < (int)sizeof(outside));
    assert(snprintf(sentinel, sizeof(sentinel), "%s/keep", outside) < (int)sizeof(sentinel));
    assert(mkdir(base, 0700) == 0 && mkdir(outside, 0750) == 0);
    assert(chmod(outside, 0750) == 0); /* Do not depend on the caller's umask. */
    assert(setenv("PROOT_TMP_DIR", base, 1) == 0);
    fd = open(sentinel, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && close(fd) == 0);
    assert(getcwd(cwd, sizeof(cwd)) != NULL);

    temporary = create_temp_directory(context, "owned");
    assert(temporary != NULL);
    assert(rmdir(temporary) == 0);
    assert(symlink(outside, temporary) == 0);
    talloc_free(context);
    assert(stat(sentinel, &st) == 0); /* Must not follow the substituted root. */
    assert(stat(outside, &st) == 0 && (st.st_mode & 0777) == 0750);
    assert(getcwd(after, sizeof(after)) != NULL && strcmp(cwd, after) == 0);

    context = talloc_new(NULL);
    temporary = create_temp_directory(context, "normal");
    assert(temporary != NULL);
    assert(snprintf(name, sizeof(name), "%s/nested", temporary) < (int)sizeof(name));
    assert(mkdir(name, 0000) == 0);
    assert(snprintf(name, sizeof(name), "%s/link", temporary) < (int)sizeof(name));
    assert(symlink(outside, name) == 0);
    assert(strlen(temporary) < sizeof(name));
    strcpy(name, temporary);
    talloc_free(context);
    assert(lstat(name, &st) < 0 && errno == ENOENT);
    assert(stat(sentinel, &st) == 0);
    assert(getcwd(after, sizeof(after)) != NULL && strcmp(cwd, after) == 0);
    puts("temporary cleanup confinement and restricted directories: PASS");
    return 0;
}
