/* SPDX-License-Identifier: MIT */
/* OS-only portability wrapper around the pinned upstream SimBricks transport. */
#ifndef __linux__
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

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
            close(fd);
            return -1;
        }
    }
    return fd;
}
#define accept4 openurma_accept4
#endif

#include "openurma-simbricks-if-impl.c"
