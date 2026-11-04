// initialize_job_control in the pinned Bash duplicates stderr, moves it to a descriptor below 256 and sets FD_CLOEXEC.
// Recreate that alias after starting the observer. Staging fd 255 through spawn itself can exhaust a 256-entry descriptor table.
#pragma once
#include "../../libLiveSimSpawn/FakePTY.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

typedef struct { int32_t source, target; } TrialTerminalAlias; // A zero target needs no reconstruction.

static inline bool trialTerminalFD(int fd, int (*name)(int, char *, size_t)) {
    char path[32];
    return !name(fd, path, sizeof(path)) && !strcmp(path, "/dev/lsbtty");
}

static inline int trialCaptureTerminal(int fd, TrialTerminalAlias *alias) {
    if (fd < 0 || fd >= 256 || !trialTerminalFD(fd, LSBPTYNameR)) return ENOTSUP;
    if (fd <= STDERR_FILENO) return 0;
    if (fcntl(fd, F_GETFD) != FD_CLOEXEC) return ENOTSUP;
    for (int source = STDIN_FILENO; source <= STDERR_FILENO; ++source) {
        if (!trialTerminalFD(source, LSBPTYNameR) || fcntl(source, F_GETFD) != 0) continue;
        *alias = (TrialTerminalAlias){source, fd};
        return 0;
    }
    return ENOTSUP;
}

static inline int trialRestoreTerminal(TrialTerminalAlias alias, int controlFD, int (*name)(int, char *, size_t)) {
    if (!alias.target) return alias.source ? EPROTO : 0;
    if (alias.target <= STDERR_FILENO || alias.target >= 256 || alias.source < 0 || alias.source > STDERR_FILENO ||
        alias.target == controlFD || !trialTerminalFD(alias.source, name)) return EPROTO;
    if (fcntl(alias.target, F_GETFD) != -1 || errno != EBADF) return EBUSY;
    if (dup2(alias.source, alias.target) < 0 || fcntl(alias.target, F_SETFD, FD_CLOEXEC) < 0) return errno;
    return 0;
}
