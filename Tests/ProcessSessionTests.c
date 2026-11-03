#define LSB_PROCESS_SESSION_TESTING
#include "../Guest/ProcessSession.c"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/wait.h>

static pid_t guestIdentity = 12345;
pid_t LSBGuestPID(void) { return guestIdentity; }
static LSBChildStatus terminalLedger;
static bool attached;
LSBChildStatus *LSBGuestChildStatus(void) { return attached ? &terminalLedger : NULL; }
int LSBTransportSetProcessGroup(pid_t pid, pid_t group) {
    if (pid) return ESRCH;
    if (group < 0) return EINVAL;
    atomic_store(&terminalLedger.terminalSessionAndGroup, packSession(123, group ?: guestIdentity));
    return 0;
}

static void configure(const char *enabled, const char *session, const char *group) {
    if (enabled) setenv(LSB_SESSION_ENABLE_ENV, enabled, 1); else unsetenv(LSB_SESSION_ENABLE_ENV);
    if (session) setenv(LSB_SESSION_ID_ENV, session, 1); else unsetenv(LSB_SESSION_ID_ENV);
    if (group) setenv(LSB_PROCESS_GROUP_ENV, group, 1); else unsetenv(LSB_PROCESS_GROUP_ENV);
    initializeSession();
}

static void testNativeFallback(void) {
    configure(NULL, NULL, NULL);
    pid_t sid = 999, pgid = 998;
    assert(LSBProcessSessionSnapshot(&sid, &pgid) == ENOTSUP && sid == 999 && pgid == 998);
    assert(LSBGuestProcessGroup() == getpgrp());
    assert(LSBGuestGetPGID(0) == getpgid(0));
    assert(LSBGuestGetSID(0) == getsid(0));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        pid_t value = LSBGuestSetSID();
        _exit(value == getpid() && getsid(0) == value && getpgrp() == value ? 0 : 1);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void testVirtualSession(void) {
    pid_t nativeSession = getsid(0), nativeGroup = getpgrp();
    configure("1", "100", "101");
    errno = EDOM;
    assert(LSBGuestGetSID(0) == 100 && LSBGuestProcessGroup() == 101 && errno == EDOM);
    assert(LSBGuestGetPGID(guestIdentity) == 101 && LSBGuestGetSID(guestIdentity) == 100);
    assert(LSBGuestSetSID() == guestIdentity && errno == EDOM);
    assert(getsid(0) == nativeSession && getpgrp() == nativeGroup);
    assert(LSBGuestGetSID(0) == guestIdentity && LSBGuestProcessGroup() == guestIdentity);
    pid_t sid, pgid;
    assert(!LSBProcessSessionSnapshot(&sid, &pgid) && sid == guestIdentity && pgid == guestIdentity);
    assert(LSBGuestSetSID() == -1 && errno == EPERM);
    assert(LSBGuestGetSID(guestIdentity + 1) == -1 && errno == ENOTSUP);
    assert(LSBGuestGetPGID(-1) == -1 && errno == EINVAL);
    assert(LSBGuestSetPGID(0, 0) == -1 && errno == ENOTSUP && getpgrp() == nativeGroup);
    assert(LSBGuestSetPGID(0, -1) == -1 && errno == EINVAL);
    assert(LSBGuestSetPGrp() == -1 && errno == ENOTSUP && getpgrp() == nativeGroup);
    // Recreated process after exec inherits the logical session even though its native PID and group are different.
    char inheritedSID[32], inheritedGroup[32];
    snprintf(inheritedSID, sizeof(inheritedSID), "%d", sid);
    snprintf(inheritedGroup, sizeof(inheritedGroup), "%d", pgid);
    configure("1", inheritedSID, inheritedGroup);
    assert(LSBGuestGetSID(0) == guestIdentity && LSBGuestProcessGroup() == guestIdentity);
    assert(LSBGuestSetSID() == -1 && errno == EPERM);
}

static void testInitialAndInvalidState(void) {
    configure("1", NULL, NULL);
    assert(LSBGuestGetSID(0) == getsid(0) && LSBGuestProcessGroup() == getpgrp());
    const char *invalid[] = {NULL, "", "0", "-1", " 1", "12x", "2147483648", "9999999999999999999999"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        configure("1", "100", invalid[i]);
        pid_t sid = 7, pgid = 8;
        assert(LSBProcessSessionSnapshot(&sid, &pgid) == EINVAL && sid == 7 && pgid == 8);
        assert(LSBGuestSetSID() == -1 && errno == EINVAL);
        assert(LSBGuestProcessGroup() == -1 && errno == EINVAL);
    }
    configure("0", "broken", "broken");
    assert(LSBGuestProcessGroup() == getpgrp());
}

static _Atomic(unsigned) sessionWinners;
static void *raceSession(void *unused) {
    (void)unused;
    pid_t result = LSBGuestSetSID();
    if (result == guestIdentity) atomic_fetch_add(&sessionWinners, 1);
    else assert(result == -1 && errno == EPERM);
    for (unsigned i = 0; i < 10000; ++i) {
        pid_t sid, pgid;
        assert(!LSBProcessSessionSnapshot(&sid, &pgid));
        assert(sid == guestIdentity && pgid == guestIdentity);
    }
    return NULL;
}

static void testConcurrentSetSID(void) {
    configure("1", "100", "101");
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_create(&threads[i], NULL, raceSession, NULL));
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_join(threads[i], NULL));
    assert(atomic_load(&sessionWinners) == 1);
}

int main(void) {
    testNativeFallback();
    testVirtualSession();
    testInitialAndInvalidState();
    testConcurrentSetSID();
    LSBChildStatusInitialize(&terminalLedger);
    atomic_store(&terminalLedger.terminalSessionAndGroup, packSession(123, 124));
    attached = true;
    assert(LSBGuestGetSID(0) == 123 && LSBGuestGetPGID(0) == 124);
    assert(!LSBGuestSetPGID(0, 0) && LSBGuestProcessGroup() == guestIdentity);
    assert(LSBGuestSetPGID(500, 500) == -1 && errno == ESRCH);
    LSBChildTicket child;
    assert(!LSBChildStatusReserve(&terminalLedger, &child));
    assert(!LSBChildStatusPublishRunning(&terminalLedger, child, 51, 52));
    assert(LSBGuestGetPGID(51) == 52 && LSBGuestGetPGID(53) == -1 && errno == ESRCH);
    puts("ProcessSession: native fallback, exec inheritance, concurrent setsid, terminal groups and child queries passed");
    return 0;
}
