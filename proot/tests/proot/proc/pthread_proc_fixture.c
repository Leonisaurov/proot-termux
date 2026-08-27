#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void *thread_main(void *unused)
{
	char path[128];
	int fd;
	(void) unused;
	snprintf(path, sizeof(path), "/proc/self/task/%ld/maps", (long)gettid());
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return (void *) 1;
	close(fd);
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t thread;
	pid_t child;
	int status;

	if (argc > 1 && strcmp(argv[1], "child") == 0) {
		int fd = open("/proc/self/fd/1", O_RDONLY);
		if (fd >= 0)
			close(fd);
		return 0;
	}
	if (pthread_create(&thread, NULL, thread_main, NULL) != 0)
		return 2;
	if (pthread_join(thread, NULL) != 0)
		return 3;
	child = fork();
	if (child < 0)
		return 6;
	if (child == 0) {
		execl(argv[0], argv[0], "child", (char *) NULL);
		_exit(7);
	}
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return 8;
	puts("PTHREAD_PROC_OK");
	return 0;
}
