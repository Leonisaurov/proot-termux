/* Probe de --ashmem-memfd: usa memfd_create de punta a punta y reporta lo que
 * percibe el guest (fd, tamano tras fstat, contenido releido).  La extension
 * reescribe memfd_create a openat("/dev/ashmem") cuando el kernel no lo
 * soporta y arregla el st_size de esos fds, asi que este probe debe imprimir lo
 * mismo con y sin la flag. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef __NR_memfd_create
#    define __NR_memfd_create 279  /* ARM64/asm-generic */
#endif

int main(void)
{
	const char *payload = "proot-memfd";
	char buf[64] = { 0 };
	struct stat statbuf;
	int fd;
	ssize_t n;

	fd = (int) syscall(__NR_memfd_create, "proot-memfd-test", 0);
	if (fd < 0) {
		printf("memfd_errno=%d\n", errno);
		return 1;
	}

	if (write(fd, payload, strlen(payload)) != (ssize_t) strlen(payload)) {
		printf("write_errno=%d\n", errno);
		return 1;
	}

	if (lseek(fd, 0, SEEK_SET) < 0) {
		printf("lseek_errno=%d\n", errno);
		return 1;
	}

	if (fstat(fd, &statbuf) < 0) {
		printf("fstat_errno=%d\n", errno);
		return 1;
	}

	n = read(fd, buf, sizeof(buf) - 1);
	printf("fd=%d size=%lld read=%zd content=%s\n",
	       fd, (long long) statbuf.st_size, n, n > 0 ? buf : "");

	close(fd);
	return 0;
}
