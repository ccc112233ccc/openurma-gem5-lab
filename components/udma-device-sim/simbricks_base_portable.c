// SPDX-License-Identifier: Apache-2.0
// Compile the pinned SimBricks base transport unchanged on Linux. macOS lacks
// Linux's accept4 and MAP_POPULATE, so this translation unit supplies only
// equivalent OS compatibility shims; queue and synchronization semantics stay
// in the upstream source.
#ifndef __linux__
#include <fcntl.h>
#include <sys/socket.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x4000
#endif

static int openurma_accept4(int socket_fd, struct sockaddr *address,
                            socklen_t *address_len, int flags)
{
    int fd = accept(socket_fd, address, address_len);
    if (fd < 0) {
        return fd;
    }
    if ((flags & SOCK_NONBLOCK) != 0) {
        int current = fcntl(fd, F_GETFL, 0);
        if (current < 0 || fcntl(fd, F_SETFL, current | O_NONBLOCK) < 0) {
            return -1;
        }
    }
    return fd;
}
#define accept4 openurma_accept4
#endif

#include <simbricks/base/if.c>
