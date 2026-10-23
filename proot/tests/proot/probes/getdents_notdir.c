/* Llama getdents64(2) sobre un fd que no es un directorio.
 * hidden_files leía el resultado como unsigned, así que el fallo
 * (ENOTDIR, o EINVAL con un buffer corto) se interpretaba como un
 * listado de casi 4 GiB y PRoot copiaba eso en un buffer de stack:
 * SIGSEGV del propio proot. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

struct linux_dirent64 {
	unsigned long long d_ino;
	long long          d_off;
	unsigned short     d_reclen;
	unsigned char      d_type;
	char               d_name[];
};

int main(int argc, char **argv)
{
	char buf[4096];
	int fd;
	long res;

	if (argc < 2) {
		printf("uso: %s <ruta>\n", argv[0]);
		return 2;
	}

	fd = open(argv[1], O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("FAIL open: %s\n", strerror(errno));
		return 1;
	}

	errno = 0;
	res = syscall(SYS_getdents64, fd, buf, sizeof(buf));
	if (res < 0) {
		printf("ENOTDIR-like: %s\n", strerror(errno));
		return 0;
	}
	printf("OK listado: %ld bytes\n", res);
	return 0;
}
