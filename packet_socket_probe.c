/*
 * Minimal probe for WAMR sock_open_packet abstract API.
 *
 * Expects CAP_NET_RAW (or root) for a successful open. Without capability,
 * reaching EPERM / Operation not permitted proves the WAMR API path reaches
 * the kernel permission check.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __wasi__
#include <wasi_socket_ext.h>
#else
/* Host fallback: not used for the wasm probe binary. */
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
static int
sock_open_packet(const char *ifname, int protocol, int flags)
{
    (void)ifname;
    (void)protocol;
    (void)flags;
    errno = ENOSYS;
    return -1;
}
#ifndef ETH_P_ALL
#define ETH_P_ALL 0x0003
#endif
#endif

static void
print_errno(const char *label)
{
    printf("%s: errno=%d (%s)\n", label, errno, strerror(errno));
}

int
main(void)
{
    const char *ifname = "lo";
    int protocol = ETH_P_ALL;
    int flags = 0;
    int fd;

    printf("packet socket probe: ifname=%s protocol=0x%04x flags=%d\n", ifname,
           protocol, flags);

    errno = 0;
    fd = sock_open_packet(ifname, protocol, flags);
    printf("sock_open_packet returned %d\n", fd);

    if (fd < 0) {
        print_errno("sock_open_packet failed");
        /* Without CAP_NET_RAW this is the expected success criterion for path
         * verification: kernel rejected the AF_PACKET open. */
        if (errno == EPERM) {
            printf("reached kernel permission check (EPERM) — API path OK\n");
            return 0;
        }
        return 1;
    }

    printf("open succeeded (CAP_NET_RAW or root present)\n");

    errno = 0;
    if (close(fd) != 0) {
        print_errno("close failed");
        return 1;
    }
    printf("close succeeded\n");
    return 0;
}
