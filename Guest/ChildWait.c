#include "ChildWait.h"
#include "CreationMask.h"
#include "TerminalStopWitness.h"
#if defined(LSB_FORK_SNAPSHOT_TRIAL)
#include "../ProcessSupport/ForkDescriptorSlots.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static _Atomic(LSBChildStatus *) childStatus;
static _Atomic(int) childWakeFD = -1;
static _Atomic(dev_t) wakeDevice;
static _Atomic(ino_t) wakeInode;
static _Atomic(int) configurationError = ENOTSUP;
static _Atomic(bool) wait4StatusOnly;
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "Child wait lookup must remain signal-safe");

// XNU drops default-action tty stops for orphaned native process groups. Guest terminal groups have a logical parent
// even though their extension processes were launched independently. Preserve blocked, ignored and caught dispositions.
int LSBTerminalStopSelf(int reason) {
    LSBChildStatus *ledger = atomic_load_explicit(&childStatus, memory_order_acquire);
    if (!ledger || !atomic_load_explicit(&ledger->terminalSessionAndGroup, memory_order_acquire)) return kill(getpid(), SIGSTOP);
    uint32_t old = atomic_load_explicit(&ledger->terminalStop, memory_order_relaxed), token;
    do {
        token = ((old + 32u) & ~31u) | (unsigned)reason;
    } while (!atomic_compare_exchange_weak_explicit(&ledger->terminalStop, &old, token, memory_order_release, memory_order_relaxed));
    int result = kill(getpid(), SIGSTOP);
    uint32_t cleared = token & ~31u;
    atomic_compare_exchange_strong_explicit(&ledger->terminalStop, &token, cleared, memory_order_release, memory_order_relaxed);
    return result;
}

static void defaultTerminalStop(int number) {
    int savedErrno = errno;
    (void)LSBTerminalStopSelf(number);
    errno = savedErrno;
}

static int terminalStopSignal(int number) {
    return number == SIGTSTP || number == SIGTTIN || number == SIGTTOU;
}

static int attachedTerminal(void) {
    LSBChildStatus *ledger = atomic_load_explicit(&childStatus, memory_order_acquire);
    return ledger && atomic_load_explicit(&ledger->terminalSessionAndGroup, memory_order_acquire);
}

static void updateDiscard(const struct sigaction *action) {
    LSBChildStatus *ledger = atomic_load_explicit(&childStatus, memory_order_acquire);
    if (ledger) {
        atomic_store_explicit(&ledger->suppressJobSignal, (action->sa_flags & SA_NOCLDSTOP) != 0, memory_order_release);
        LSBChildStatusSetDiscard(ledger, action->sa_handler == SIG_IGN || (action->sa_flags & SA_NOCLDWAIT));
    }
}

int LSBChildWaitConfigure(LSBChildStatus *ledger, int wakeFD) {
    int error = LSBChildStatusValidate(ledger);
    if (error) return error;
    int flags = fcntl(wakeFD, F_GETFL);
    if (flags < 0) return errno;
    if (!(flags & O_NONBLOCK) || (flags & O_ACCMODE) == O_WRONLY) return EINVAL;
    struct stat metadata;
    if (fstat(wakeFD, &metadata)) return errno;
    if (!S_ISFIFO(metadata.st_mode)) return EINVAL;
    struct sigaction action;
    if (sigaction(SIGCHLD, NULL, &action)) return errno;
    const char *statusOnly = getenv("LSB_WAIT4_STATUS_ONLY");
    atomic_store_explicit(&wait4StatusOnly, statusOnly && !strcmp(statusOnly, "1"), memory_order_relaxed);
    atomic_store_explicit(&wakeDevice, metadata.st_dev, memory_order_relaxed);
    atomic_store_explicit(&wakeInode, metadata.st_ino, memory_order_relaxed);
    atomic_store_explicit(&childWakeFD, wakeFD, memory_order_release);
    atomic_store_explicit(&childStatus, ledger, memory_order_release);
    atomic_store_explicit(&configurationError, 0, memory_order_relaxed);
    updateDiscard(&action);
    if (attachedTerminal()) {
        const int stops[] = {SIGTSTP, SIGTTIN, SIGTTOU};
        for (unsigned i = 0; i < sizeof(stops) / sizeof(stops[0]); ++i) {
            if (sigaction(stops[i], NULL, &action)) return errno;
            if (action.sa_handler == SIG_DFL) {
                action.sa_handler = defaultTerminalStop;
                if (sigaction(stops[i], &action, NULL)) return errno;
            }
        }
    }
    return 0;
}

int LSBGuestChildWakeFD(void) {
    int fd = atomic_load_explicit(&childWakeFD, memory_order_acquire);
    if (fd < 0) return -1;
    int saved = errno;
    struct stat metadata;
    bool owned = !fstat(fd, &metadata) && S_ISFIFO(metadata.st_mode) &&
        metadata.st_dev == atomic_load_explicit(&wakeDevice, memory_order_relaxed) && metadata.st_ino == atomic_load_explicit(&wakeInode, memory_order_relaxed);
    errno = saved;
    return owned ? fd : -1;
}
LSBChildStatus *LSBGuestChildStatus(void) { return atomic_load_explicit(&childStatus, memory_order_acquire); }

#ifndef LSB_CHILD_WAIT_TESTING
#if defined(LSB_FORK_SNAPSHOT_TRIAL)
static bool deferredTerminalWitness;
// Only the authenticated fork bootstrap receives the descriptor-anchor environment entry from LiveProcess.
__attribute__((visibility("default"))) int LSBTrialResumeTerminalWitness(void) {
    if (!deferredTerminalWitness) return 0;
    deferredTerminalWitness = false;
    return LSBStartTerminalStopWitness(&LSBGuestChildStatus()->nativeStopEvent);
}
#endif

__attribute__((constructor)) static void initializeChildWait(void) {
    const char *address = getenv(LSB_CHILD_STATUS_ENV), *descriptor = getenv(LSB_CHILD_WAKE_FD_ENV);
    int error = EINVAL;
    if (address && descriptor && *address && *descriptor) {
        char *end;
        errno = 0;
        unsigned long long pointer = strtoull(address, &end, 16);
        if (!errno && !*end && pointer && pointer <= UINTPTR_MAX) {
            errno = 0;
            long fd = strtol(descriptor, &end, 10);
            if (!errno && !*end && fd >= 0 && fd <= INT_MAX) error = LSBChildWaitConfigure((LSBChildStatus *)(uintptr_t)pointer, (int)fd);
        }
    }
    atomic_store_explicit(&configurationError, error, memory_order_relaxed);
    if (!error && attachedTerminal()) {
        // Hosts without getumask need a single thread for their first mask snapshot; constructor ordering is not an ABI.
        uint32_t mask;
        if (LSBReadCreationMask(&mask)) return;
#if defined(LSB_FORK_SNAPSHOT_TRIAL)
        if (getenv(LSB_FORK_ANCHOR_ENV)) { deferredTerminalWitness = true; return; }
#endif
        (void)LSBStartTerminalStopWitness(&LSBGuestChildStatus()->nativeStopEvent);
    }
}
#endif

static pid_t finishWait(pid_t child, int value, int *status, int savedErrno) {
#if defined(LSB_FORK_SNAPSHOT_TRIAL)
    extern void LSBForkTrialDidWait(pid_t child, int status);
    LSBForkTrialDidWait(child, value);
#endif
    if (status) *status = value;
    errno = savedErrno;
    return child;
}

pid_t LSBWaitPID(pid_t selector, int *status, int options) {
    int savedErrno = errno;
    if (options & ~(WNOHANG | WUNTRACED | WCONTINUED)) { errno = EINVAL; return -1; }
    LSBChildStatus *ledger = atomic_load_explicit(&childStatus, memory_order_acquire);
    if (!ledger) { errno = atomic_load_explicit(&configurationError, memory_order_relaxed); return -1; }
    pid_t group = 0;
    if (!selector) {
        group = (pid_t)(uint32_t)atomic_load_explicit(&ledger->terminalSessionAndGroup, memory_order_acquire);
        if (!group) group = getpgrp();
    }
    int wakeFD = LSBGuestChildWakeFD();
    for (;;) {
        pid_t child;
        int value;
        int error = LSBChildStatusTryWaitOptions(ledger, selector, group, options, &child, &value);
        if (!error) return finishWait(child, value, status, savedErrno);
        if (error != EAGAIN) { errno = error; return -1; }
        if (options & WNOHANG) { errno = savedErrno; return 0; }
        // Wake bytes are hints, never ownership of an exit. A bounded rescan handles another waiter draining the pipe.
        struct pollfd descriptor = {.fd = wakeFD, .events = POLLIN};
        int result = poll(&descriptor, 1, 100);
        if (result < 0) {
            error = errno;
            if (error == EINTR && !LSBChildStatusTryWaitOptions(ledger, selector, group, options, &child, &value))
                return finishWait(child, value, status, savedErrno);
            errno = error;
            return -1;
        }
        if (descriptor.revents & POLLNVAL) { errno = EBADF; return -1; }
        if (descriptor.revents & (POLLIN | POLLHUP)) {
            char hints[64];
            ssize_t count = read(wakeFD, hints, sizeof(hints));
            if (count > 0 || (count < 0 && (errno == EAGAIN || errno == EINTR))) continue;
            error = count == 0 ? EPIPE : errno;
            if (!LSBChildStatusTryWaitOptions(ledger, selector, group, options, &child, &value)) return finishWait(child, value, status, savedErrno);
            errno = error;
            return -1;
        }
        if (descriptor.revents & POLLERR) { errno = EIO; return -1; }
    }
}

pid_t LSBWait(int *status) { return LSBWaitPID(-1, status, 0); }

pid_t LSBWait4(pid_t selector, int *status, int options, struct rusage *usage) {
    if (usage && !atomic_load_explicit(&wait4StatusOnly, memory_order_relaxed)) { errno = ENOTSUP; return -1; }
    pid_t result = LSBWaitPID(selector, status, options);
    // Explicit compatibility mode for callers such as Go os/exec that always request rusage. These zeros mean uncollected,
    // not measured usage; the child's exit/signal status still comes from the broker's real terminal event.
    if (result > 0 && usage) memset(usage, 0, sizeof(*usage));
    return result;
}

int LSBSigaction(int number, const struct sigaction *action, struct sigaction *oldAction) {
    // action and oldAction may alias; the kernel can overwrite the caller's input with the previous disposition.
    struct sigaction requested;
    if (action && (number == SIGCHLD || terminalStopSignal(number))) {
        requested = *action;
        if (terminalStopSignal(number) && requested.sa_handler == SIG_DFL && attachedTerminal()) requested.sa_handler = defaultTerminalStop;
        action = &requested;
    }
    int result = sigaction(number, action, oldAction);
    if (!result && oldAction && terminalStopSignal(number) && oldAction->sa_handler == defaultTerminalStop) oldAction->sa_handler = SIG_DFL;
    if (!result && number == SIGCHLD && action) updateDiscard(&requested);
    return result;
}

sig_t LSBSignal(int number, sig_t handler) {
    if (terminalStopSignal(number) && handler == SIG_DFL && attachedTerminal()) handler = defaultTerminalStop;
    sig_t result = signal(number, handler);
    if (terminalStopSignal(number) && result == defaultTerminalStop) result = SIG_DFL;
    int savedErrno = errno;
    if (result != SIG_ERR && number == SIGCHLD) {
        struct sigaction action;
        if (!sigaction(number, NULL, &action)) updateDiscard(&action);
    }
    errno = savedErrno;
    return result;
}

#ifndef LSB_CHILD_WAIT_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBWaitPID, (const void *)&waitpid}, {(const void *)&LSBWait, (const void *)&wait},
    {(const void *)&LSBWait4, (const void *)&wait4},
    {(const void *)&LSBSigaction, (const void *)&sigaction}, {(const void *)&LSBSignal, (const void *)&signal}
};
#endif
