#ifndef VIRTUAL_NET_HELPER_H
#define VIRTUAL_NET_HELPER_H

/**
 * Helper process entry point.
 * Called from cli.c when proot is invoked with --vnp-helper NAME.
 * Runs the TCP→Unix bridge event loop for exposed ports.
 */
extern int vnp_helper_main(int argc, char *argv[]);

#endif /* VIRTUAL_NET_HELPER_H */
