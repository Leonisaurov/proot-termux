#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void usage(const char *name)
{
    fprintf(stderr, "usage: %s server|client 4|6 accept|accept4 port\n", name);
    exit(64);
}

int main(int argc, char **argv)
{
    int is_server;
    int family;
    int use_accept4;
    int port;
    int listener;
    int result;
    int one = 1;
    struct sockaddr_storage address;
    socklen_t address_length;

    if (argc != 5)
        usage(argv[0]);
    is_server = strcmp(argv[1], "server") == 0;
    family = strcmp(argv[2], "6") == 0 ? AF_INET6 : AF_INET;
    use_accept4 = strcmp(argv[3], "accept4") == 0;
    port = atoi(argv[4]);
    if ((!is_server && strcmp(argv[1], "client") != 0) ||
        (family != AF_INET && family != AF_INET6) || port < 1 || port > 65535)
        usage(argv[0]);

    listener = socket(family, SOCK_STREAM, 0);
    if (listener < 0) {
        printf("SOCKET_ERR:%d:%s\n", errno, strerror(errno));
        return 1;
    }
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&address, 0, sizeof(address));
    if (family == AF_INET) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)&address;
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons((uint16_t)port);
        inet_pton(AF_INET, "127.0.0.1", &ipv4->sin_addr);
        address_length = sizeof(*ipv4);
    } else {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)&address;
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons((uint16_t)port);
        inet_pton(AF_INET6, "::1", &ipv6->sin6_addr);
        address_length = sizeof(*ipv6);
    }

    if (is_server) {
        if (bind(listener, (struct sockaddr *)&address, address_length) < 0 ||
            listen(listener, 1) < 0) {
            printf("BIND_ERR:%d:%s\n", errno, strerror(errno));
            close(listener);
            return 1;
        }
        setvbuf(stdout, NULL, _IOLBF, 0);
        printf("READY:%d:%s:%d\n", family, use_accept4 ? "accept4" : "accept", port);
        address_length = sizeof(address);
#ifdef __linux__
        if (use_accept4)
            result = accept4(listener, (struct sockaddr *)&address, &address_length, 0);
        else
#endif
            result = accept(listener, (struct sockaddr *)&address, &address_length);
        if (result < 0) {
            printf("ACCEPT_ERR:%d:%s\n", errno, strerror(errno));
            close(listener);
            return 2;
        }
        printf("ACCEPT_OK:%d:%u\n", address.ss_family, (unsigned)address_length);
        close(result);
    } else {
        result = connect(listener, (struct sockaddr *)&address, address_length);
        if (result < 0) {
            printf("CONNECT_ERR:%d:%s\n", errno, strerror(errno));
            close(listener);
            return 3;
        }
        printf("CLIENT_OK:%d\n", family);
    }
    close(listener);
    return 0;
}
