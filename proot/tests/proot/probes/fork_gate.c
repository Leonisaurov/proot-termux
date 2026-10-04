/* Probe de --proc-limit: intenta crear @n procesos hijos que permanecen vivos
 * un rato y reporta cuántos forks aceptó el sandbox y con qué errno falló el
 * primero rechazado.  Bash reintenta EAGAIN internamente, por eso el intento se
 * hace en C: el test necesita ver el rechazo, no el reintento. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
	int requested = argc > 1 ? atoi(argv[1]) : 5;
	int created = 0;
	int first_errno = 0;
	int i;

	if (requested < 1 || requested > 64) {
		fprintf(stderr, "usage: %s <n-forks>\n", argv[0]);
		return 2;
	}

	for (i = 0; i < requested; i++) {
		pid_t pid = fork();

		if (pid < 0) {
			first_errno = errno;
			break;
		}
		if (pid == 0) {
			struct timespec nap = { .tv_sec = 2, .tv_nsec = 0 };

			/* El hijo vive lo suficiente para contar como tracee
			 * vivo mientras el padre sigue intentando forks. */
			nanosleep(&nap, NULL);
			_exit(0);
		}
		created++;
	}

	for (i = 0; i < created; i++) {
		int status;

		if (wait(&status) < 0 && first_errno == 0)
			first_errno = errno;
	}

	printf("created=%d requested=%d errno=%d\n", created, requested, first_errno);
	return 0;
}
