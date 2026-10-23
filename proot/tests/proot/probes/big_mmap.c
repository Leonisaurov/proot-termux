/* Reserva un hueco grande de direcciones con mmap(2) anónima.
 * Con las antiguas fijaciones de arch.h (LOADER_ADDRESS 0x2000000000,
 * EXEC_PIC 0x3000000000, INTERP_PIC 0x3f00000000) el espacio quedaba
 * partido y, en kernels Android de 39 bits, una reserva de 256 GiB
 * fallaba con ENOMEM sólo dentro de proot. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
	size_t want = (size_t) 256 * 1024 * 1024 * 1024;
	void *p;

	p = mmap(NULL, want, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		printf("FAIL mmap %zu GiB: %s\n", want / (1024 * 1024 * 1024),
			strerror(errno));
		return 1;
	}
	munmap(p, want);
	printf("OK mmap %zu GiB\n", want / (1024 * 1024 * 1024));
	return 0;
}
