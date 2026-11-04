#pragma once
#include "SpawnPlan.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#define LSB_FORK_ANCHOR_ENV "LSB_FORK_ANCHOR_FD"
#define LSB_FORK_RECEIVE_ARG "--fork-fds"

// Called in a single-threaded fork bootstrap. Only close aliases of the new, private pipe; a subsequently reused number stays open.
static inline int LSBForkReleaseDescriptorSlots(int anchor, dev_t device, ino_t inode) {
    struct stat expected;
    if (anchor < LSB_MAX_FDS || fcntl(anchor, F_GETFD) != FD_CLOEXEC || fstat(anchor, &expected) ||
        !S_ISFIFO(expected.st_mode) || !inode || expected.st_dev != device || expected.st_ino != inode) return EINVAL;
    for (int fd = 3; fd < LSB_MAX_FDS; ++fd) {
        struct stat current;
        if (fcntl(fd, F_GETFD) != FD_CLOEXEC || fstat(fd, &current)) continue;
        if (S_ISFIFO(current.st_mode) && current.st_dev == expected.st_dev && current.st_ino == expected.st_ino && close(fd)) return errno;
    }
    return close(anchor) ? errno : 0;
}

static inline int LSBForkReserveDescriptorSlots(int *result, struct stat *identity) {
    *result = -1;
    int ends[2];
    if (pipe(ends)) return errno;
    int anchor = fcntl(ends[0], F_DUPFD_CLOEXEC, LSB_MAX_FDS);
    int error = anchor < 0 ? errno : 0;
    close(ends[0]);
    close(ends[1]);
    if (error) return error;
    if (fstat(anchor, identity) || !S_ISFIFO(identity->st_mode) || !identity->st_ino) { close(anchor); return ENOTSUP; }
    for (;;) {
        int fd = fcntl(anchor, F_DUPFD_CLOEXEC, 3);
        if (fd < 0) { error = errno; LSBForkReleaseDescriptorSlots(anchor, identity->st_dev, identity->st_ino); return error; }
        if (fd >= LSB_MAX_FDS) { close(fd); break; }
    }
    *result = anchor;
    return 0;
}
