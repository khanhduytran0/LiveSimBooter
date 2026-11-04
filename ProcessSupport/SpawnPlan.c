#include "SpawnPlan.h"
#include "OpenFDs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint32_t supportedFlags = POSIX_SPAWN_RESETIDS | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF |
                                       POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT;

int LSBPlanAddString(LSBSpawnPlan *plan, const char *value, uint32_t *offset) {
    if (!plan || !value || !offset || plan->stringBytes > LSB_STRING_BYTES) return EINVAL;
    size_t remaining = LSB_STRING_BYTES - plan->stringBytes;
    size_t length = strnlen(value, remaining);
    if (length == remaining) return E2BIG;
    *offset = plan->stringBytes;
    memcpy(plan->strings + plan->stringBytes, value, length + 1);
    plan->stringBytes += (uint32_t)length + 1;
    return 0;
}

static bool validString(const LSBSpawnPlan *plan, uint32_t offset, size_t limit, bool required) {
    if (offset >= plan->stringBytes || (offset && plan->strings[offset - 1])) return false;
    size_t available = plan->stringBytes - offset;
    size_t length = strnlen(plan->strings + offset, available);
    return length < available && length < limit && (!required || length != 0);
}

static bool validFD(int fd) { return fd >= 0 && fd < INT_MAX - LSB_MAX_FDS - 1; }

int LSBValidateSpawnPlan(const LSBSpawnPlan *plan) {
    if (!plan || plan->version != LSB_PLAN_VERSION || !plan->argc || plan->argc > LSB_MAX_VECTOR || plan->envc > LSB_MAX_VECTOR ||
        plan->actionCount > LSB_MAX_ACTIONS || plan->fdCount > LSB_MAX_FDS || !plan->stringBytes || plan->stringBytes > LSB_STRING_BYTES)
        return EINVAL;
    if (!validString(plan, plan->path, LSB_PATH_BYTES, true) || !validString(plan, plan->cwd, LSB_PATH_BYTES, true) ||
        plan->strings[plan->cwd] != '/' || (plan->creationMask & ~0777u)) return EINVAL;
    if (plan->spawnFlags & ~supportedFlags) return ENOTSUP;
    if (plan->processGroup < 0) return EINVAL;
    for (uint32_t i = 0; i < plan->argc; ++i) if (!validString(plan, plan->argv[i], LSB_STRING_BYTES, false)) return EINVAL;
    for (uint32_t i = 0; i < plan->envc; ++i) if (!validString(plan, plan->envp[i], LSB_STRING_BYTES, false)) return EINVAL;
    for (uint32_t i = 0; i < plan->fdCount; ++i) {
        if (!validFD(plan->fds[i].number) || (plan->fds[i].flags & ~FD_CLOEXEC)) return EINVAL;
        for (uint32_t j = 0; j < i; ++j) if (plan->fds[i].number == plan->fds[j].number) return EINVAL;
    }
    for (uint32_t i = 0; i < plan->actionCount; ++i) {
        const LSBFileAction *action = &plan->actions[i];
        if (action->kind != LSB_ACTION_CHDIR && !validFD(action->fd)) return EINVAL;
        switch (action->kind) {
            case LSB_ACTION_CLOSE: case LSB_ACTION_FCHDIR: case LSB_ACTION_INHERIT: break;
            case LSB_ACTION_DUP2: if (!validFD(action->target)) return EINVAL; break;
            case LSB_ACTION_OPEN:
                if (action->mode & ~07777u) return EINVAL;
                if (!validString(plan, action->path, LSB_PATH_BYTES, false)) return EINVAL;
                break;
            case LSB_ACTION_CHDIR: if (!validString(plan, action->path, LSB_PATH_BYTES, false)) return EINVAL; break;
            default: return ENOTSUP;
        }
    }
    return 0;
}

static void closeReceived(int fds[LSB_MAX_FDS], uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (fds[i] >= 0) close(fds[i]);
        fds[i] = -1;
    }
}

static bool containsFD(const int *fds, uint32_t count, int fd) {
    for (uint32_t i = 0; i < count; ++i) if (fds[i] == fd) return true;
    return false;
}

static bool explicitlyInherited(const LSBSpawnPlan *plan, int fd) {
    for (uint32_t i = 0; i < plan->actionCount; ++i) {
        const LSBFileAction *action = &plan->actions[i];
        if (action->kind == LSB_ACTION_DUP2 && action->target == fd) return true;
        if ((action->kind == LSB_ACTION_OPEN || action->kind == LSB_ACTION_INHERIT) && action->fd == fd) return true;
    }
    return false;
}

static int replayActions(const LSBSpawnPlan *plan) {
    for (uint32_t i = 0; i < plan->actionCount; ++i) {
        const LSBFileAction *action = &plan->actions[i];
        switch (action->kind) {
            case LSB_ACTION_CLOSE: if (close(action->fd)) return errno; break;
            case LSB_ACTION_DUP2: if (dup2(action->fd, action->target) < 0) return errno; break;
            case LSB_ACTION_INHERIT: {
                int flags = fcntl(action->fd, F_GETFD);
                if (flags < 0 || fcntl(action->fd, F_SETFD, flags & ~FD_CLOEXEC)) return errno;
                break;
            }
            case LSB_ACTION_OPEN: {
                int opened = open(plan->strings + action->path, action->flags, (mode_t)action->mode);
                if (opened < 0) return errno;
                if (opened != action->fd) {
                    int error = dup2(opened, action->fd) < 0 ? errno : 0;
                    close(opened);
                    if (error) return error;
                }
                break;
            }
            case LSB_ACTION_CHDIR: if (chdir(plan->strings + action->path)) return errno; break;
            case LSB_ACTION_FCHDIR: if (fchdir(action->fd)) return errno; break;
            default: return ENOTSUP;
        }
    }
    return 0;
}

int LSBApplySpawnPlanPreservingFD(const LSBSpawnPlan *plan, int receivedFDs[LSB_MAX_FDS], int preservedFD) {
    if (!receivedFDs) return EINVAL;
    uint32_t count = plan && plan->fdCount < LSB_MAX_FDS ? plan->fdCount : LSB_MAX_FDS;
    int error = LSBValidateSpawnPlan(plan);
    if (error) { closeReceived(receivedFDs, count); return error; }
    int limit = getdtablesize(), highest = 2;
    for (uint32_t i = 0; i < count; ++i) if (plan->fds[i].number > highest) highest = plan->fds[i].number;
    for (uint32_t i = 0; i < plan->actionCount; ++i) {
        const LSBFileAction *action = &plan->actions[i];
        if (action->kind != LSB_ACTION_CHDIR && action->fd > highest) highest = action->fd;
        if (action->kind == LSB_ACTION_DUP2 && action->target > highest) highest = action->target;
    }
    if (preservedFD < -1 || (preservedFD >= 0 && (preservedFD <= highest || containsFD(receivedFDs, count, preservedFD)))) {
        closeReceived(receivedFDs, count); return EINVAL;
    }
    if (preservedFD >= 0 && fcntl(preservedFD, F_GETFD) < 0) { error = errno; closeReceived(receivedFDs, count); return error; }
    if (highest >= limit - (int)count) { closeReceived(receivedFDs, count); return EMFILE; }
    int staged[LSB_MAX_FDS];
    for (uint32_t i = 0; i < LSB_MAX_FDS; ++i) staged[i] = -1;
    for (uint32_t i = 0; i < count; ++i) {
        staged[i] = fcntl(receivedFDs[i], F_DUPFD_CLOEXEC, highest + 1);
        if (staged[i] < 0) { error = errno; closeReceived(staged, count); closeReceived(receivedFDs, count); return error; }
    }
    closeReceived(receivedFDs, count);
    // Imports are staged above every destination, including those used by later ordered file actions.
    LSBOpenFDScan scan;
    pid_t nativePID = getpid(); // This plan is applied by the native worker before guest startup.
    LSBBeginOpenFDScan(&scan, nativePID, limit);
    for (int fd; (fd = LSBNextOpenFD(&scan)) >= 0;)
        if (fd != preservedFD && !containsFD(staged, count, fd)) close(fd);
    for (uint32_t i = 0; i < count; ++i) {
        if (dup2(staged[i], plan->fds[i].number) < 0 || fcntl(plan->fds[i].number, F_SETFD, plan->fds[i].flags)) {
            error = errno;
            closeReceived(staged, count);
            return error;
        }
    }
    closeReceived(staged, count);
    if (chdir(plan->strings + plan->cwd)) return errno;
    umask((mode_t)plan->creationMask);
    error = replayActions(plan);
    if (error) return error;
    // Darwin applies close-on-exec after actions. CLOEXEC_DEFAULT additionally retains only action destinations/INHERIT descriptors.
    LSBBeginOpenFDScan(&scan, nativePID, limit);
    for (int fd; (fd = LSBNextOpenFD(&scan)) >= 0;) {
        if (fd == preservedFD) continue;
        int flags = fcntl(fd, F_GETFD);
        if (flags >= 0 && ((flags & FD_CLOEXEC) || ((plan->spawnFlags & POSIX_SPAWN_CLOEXEC_DEFAULT) && !explicitlyInherited(plan, fd)))) close(fd);
    }
    if (plan->spawnFlags & POSIX_SPAWN_RESETIDS) {
        if (setegid(getgid()) || seteuid(getuid())) return errno;
    }
    // Extensions keep their launch session/group unless an explicit request can be applied by the kernel.
    if ((plan->spawnFlags & POSIX_SPAWN_SETPGROUP) && setpgid(0, plan->processGroup)) return errno;
    for (int number = 1; number < NSIG; ++number) {
        if (number == SIGKILL || number == SIGSTOP) continue;
        uint32_t bit = 1u << (number - 1);
        struct sigaction action = {0};
        action.sa_handler = (plan->signalIgnored & bit) && !(plan->signalDefault & bit) ? SIG_IGN : SIG_DFL;
        sigemptyset(&action.sa_mask);
        if (sigaction(number, &action, NULL)) return errno;
    }
    sigset_t mask = (sigset_t)plan->signalMask;
    return sigprocmask(SIG_SETMASK, &mask, NULL) ? errno : 0;
}

int LSBApplySpawnPlan(const LSBSpawnPlan *plan, int receivedFDs[LSB_MAX_FDS]) {
    return LSBApplySpawnPlanPreservingFD(plan, receivedFDs, -1);
}
