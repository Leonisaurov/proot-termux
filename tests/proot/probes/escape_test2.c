#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/mount.h>

void test(const char *name, int result, int expected_err) {
    printf("%-50s: ret=%d errno=%d (%s)%s\n", 
           name, result, errno, strerror(errno),
           (result < 0 && errno == expected_err) ? " \xe2\x9c\x85" : "");
}

int main() {
    printf("=== ESCAPE TESTS V2 ===\n");
    
    /* 1. process_vm_readv a PID 1 (host) */
    errno = 0;
    char buf[4];
    struct iovec local = {buf, sizeof(buf)};
    int r = syscall(SYS_process_vm_readv, 1, &local, 1, NULL, 0, 0);
    test("process_vm_readv(PID 1)", r, ESRCH);
    
    /* 2. socket(AF_NETLINK) */
    errno = 0;
    r = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    test("socket(AF_NETLINK, NETLINK_ROUTE)", r, EACCES);
    if (r >= 0) close(r);
    
    /* 3. unshare */
    errno = 0;
    r = unshare(CLONE_NEWNS);
    test("unshare(CLONE_NEWNS)", r, ENOSYS);
    
    /* 4. mount */
    errno = 0;
    mkdir("/tmp/x", 0755);
    r = mount("/proc", "/tmp/x", NULL, MS_BIND, NULL);
    test("mount --bind /proc", r, ENOSYS);
    
    /* 5. ptrace a PID 1 */
    errno = 0;
    r = ptrace(PTRACE_ATTACH, 1, NULL, NULL);
    test("ptrace(PID 1)", r, ESRCH);
    
    printf("=== FIN ===\n");
    return 0;
}
