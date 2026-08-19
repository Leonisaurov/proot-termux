#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
    size_t mb = argc > 1 ? atoi(argv[1]) : 100;
    char *p = calloc(mb * 1024 * 1024, 1);
    if (!p) { printf("MEMTEST: calloc %zu MB FAILED\n", mb); return 1; }
    for (size_t i = 0; i < mb * 1024 * 1024; i += 4096) p[i] = 1; /* tocar paginas */
    printf("MEMTEST: allocated %zu MB, pid=%d, sleeping\n", mb, getpid());
    fflush(stdout);
    sleep(60);
    return 0;
}
