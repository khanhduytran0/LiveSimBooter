#include "ProcessSession.h"
#include "ProcessIdentity.h"
#include "ChildWait.h"
#include "../ProcessSupport/SpawnTransport.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Atomic(uint64_t) sessionAndGroup;
static _Atomic(int) sessionError = ENOTSUP;
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "Session metadata must remain lock-free");

static uint64_t packSession(pid_t session, pid_t group) { return ((uint64_t)(uint32_t)session << 32) | (uint32_t)group; }

static pid_t parseSessionPID(const char *text) {
    if (!text || !*text) return 0;
    uint32_t value = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || value > (INT_MAX - (unsigned)(*p - '0')) / 10u) return 0;
        value = value * 10 + (unsigned)(*p - '0');
    }
    return (pid_t)value;
}

__attribute__((constructor)) static void initializeSession(void) {
    int savedErrno = errno;
    const char *enabled = getenv(LSB_SESSION_ENABLE_ENV);
    atomic_store_explicit(&sessionAndGroup, 0, memory_order_relaxed);
    atomic_store_explicit(&sessionError, ENOTSUP, memory_order_release);
    if (!enabled || strcmp(enabled, "1")) { errno = savedErrno; return; }
    const char *sessionText = getenv(LSB_SESSION_ID_ENV), *groupText = getenv(LSB_PROCESS_GROUP_ENV);
    pid_t session = 0, group = 0;
    int error = 0;
    if (sessionText || groupText) {
        session = parseSessionPID(sessionText);
        group = parseSessionPID(groupText);
        if (session <= 0 || group <= 0) error = EINVAL;
    } else {
        // The initial launchd guest establishes the metadata; every child then inherits the guest values explicitly.
        session = getsid(0);
        if (session < 0) error = errno;
        group = getpgrp();
        if (group < 0 && !error) error = errno;
        if ((!session || !group) && !error) error = EINVAL;
    }
    if (!error) atomic_store_explicit(&sessionAndGroup, packSession(session, group), memory_order_release);
    atomic_store_explicit(&sessionError, error, memory_order_release);
    errno = savedErrno;
}

static uint64_t terminalIdentity(void) {
    LSBChildStatus *ledger = LSBGuestChildStatus();
    return ledger ? atomic_load_explicit(&ledger->terminalSessionAndGroup, memory_order_acquire) : 0;
}

int LSBProcessSessionSnapshot(pid_t *session, pid_t *group) {
    if (!session || !group) return EINVAL;
    uint64_t value = terminalIdentity();
    if (!value) {
        int error = atomic_load_explicit(&sessionError, memory_order_acquire);
        if (error) return error;
        value = atomic_load_explicit(&sessionAndGroup, memory_order_acquire);
    }
    *session = (pid_t)(uint32_t)(value >> 32);
    *group = (pid_t)(uint32_t)value;
    return 0;
}

pid_t LSBGuestProcessGroup(void) {
    pid_t session, group;
    int error = LSBProcessSessionSnapshot(&session, &group);
    if (error == ENOTSUP) return getpgrp();
    if (error) { errno = error; return -1; }
    return group;
}

static pid_t sessionQuery(pid_t pid, int wantSession) {
    pid_t session, group;
    int error = LSBProcessSessionSnapshot(&session, &group);
    if (error == ENOTSUP) return wantSession ? getsid(pid) : getpgid(pid);
    if (error) { errno = error; return -1; }
    if (pid < 0) { errno = EINVAL; return -1; }
    if (pid && pid != LSBGuestPID()) {
        if (!terminalIdentity() || wantSession) { errno = ENOTSUP; return -1; }
        LSBChildObservation child;
        error = LSBChildStatusObserve(LSBGuestChildStatus(), pid, &child);
        if (error) { errno = error == ECHILD ? ESRCH : error; return -1; }
        return child.group;
    }
    return wantSession ? session : group;
}

pid_t LSBGuestGetPGID(pid_t pid) { return sessionQuery(pid, 0); }
pid_t LSBGuestGetSID(pid_t pid) { return sessionQuery(pid, 1); }

pid_t LSBGuestSetSID(void) {
    uint64_t terminal = terminalIdentity();
    if (terminal) {
        // Leaving the fake controlling terminal needs a separate broker detach operation.
        errno = (pid_t)(uint32_t)terminal == LSBGuestPID() ? EPERM : ENOTSUP;
        return -1;
    }
    int error = atomic_load_explicit(&sessionError, memory_order_acquire);
    if (error == ENOTSUP) return setsid();
    if (error) { errno = error; return -1; }
    pid_t pid = LSBGuestPID();
    if (pid <= 0) { errno = EINVAL; return -1; }
    uint64_t previous = atomic_load_explicit(&sessionAndGroup, memory_order_acquire);
    for (;;) {
        if ((pid_t)(uint32_t)previous == pid) { errno = EPERM; return -1; }
        if (atomic_compare_exchange_weak_explicit(&sessionAndGroup, &previous, packSession(pid, pid),
                                                 memory_order_acq_rel, memory_order_acquire)) return pid;
    }
}

int LSBGuestSetPGID(pid_t pid, pid_t group) {
    if (terminalIdentity()) {
        int saved = errno;
        int error = LSBTransportSetProcessGroup(pid, group);
        errno = error ?: saved;
        return error ? -1 : 0;
    }
    int error = atomic_load_explicit(&sessionError, memory_order_acquire);
    if (error == ENOTSUP) return setpgid(pid, group);
    // Changing groups needs broker metadata and group signaling support. Fail explicitly instead of changing only the native process.
    errno = error ? error : (pid < 0 || group < 0 ? EINVAL : ENOTSUP);
    return -1;
}

pid_t LSBGuestSetPGrp(void) {
    if (terminalIdentity()) return LSBGuestSetPGID(0, 0);
    int error = atomic_load_explicit(&sessionError, memory_order_acquire);
    if (error == ENOTSUP) return setpgrp();
    errno = error ? error : ENOTSUP;
    return -1;
}

#ifndef LSB_PROCESS_SESSION_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBGuestProcessGroup, (const void *)&getpgrp},
    {(const void *)&LSBGuestGetPGID, (const void *)&getpgid},
    {(const void *)&LSBGuestGetSID, (const void *)&getsid},
    {(const void *)&LSBGuestSetSID, (const void *)&setsid},
    {(const void *)&LSBGuestSetPGID, (const void *)&setpgid},
    {(const void *)&LSBGuestSetPGrp, (const void *)&setpgrp}
};
#endif
