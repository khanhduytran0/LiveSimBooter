#include "../Guest/Shared/Fork/ForkAllocator.h"
#include <assert.h>

static void lockMallocForTest(void);
static void unlockMallocForTest(void);
static int checkZoneForTest(const ForkAllocatorState *state, ForkAllocatorVisit visit, void *context);
#define _malloc_fork_prepare lockMallocForTest
#define _malloc_fork_parent unlockMallocForTest
#define forkAllocatorRecheck checkZoneForTest
#include "../Guest/libLiveSimSpawn/ForkTrial.c"

static const uint64_t newRegion = 0x165900000, extent = 1u << 20;
static const audit_token_t broker = {{1, 2, 3, 4, 5, 6, 7, 8}};
static int phase, brokerError, registryError, observerStatus;

int LSBTransportOfferForkSocket(const audit_token_t *child, const LSBForkWireRange *ranges, size_t count, audit_token_t *token) {
    assert(phase == 0 && child && ranges && count);
    phase = 1;
    *token = broker;
    return brokerError;
}

static void lockMallocForTest(void) { assert(phase == 1); phase = 2; }
static void unlockMallocForTest(void) { assert(phase == 2); phase = 3; }

static int checkZoneForTest(const ForkAllocatorState *state, ForkAllocatorVisit visit, void *context) {
    assert(phase == 2 && state);
    return registryError ? registryError : visit(context, newRegion, extent);
}

pid_t LSBWaitPID(pid_t pid, int *status, int options) {
    assert(pid > 0 && !status && !options);
    return waitpid(pid, &observerStatus, 0);
}

static void checkOffer(AllocatorAttempt *state, int expectedError, bool expectedGrowth) {
    int sockets[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        close(sockets[0]);
        audit_token_t actual = {{0}};
        ssize_t n = recv(sockets[1], &actual, sizeof(actual), MSG_WAITALL);
        _exit(expectedError ? n != 0 : n != sizeof(actual) || memcmp(&actual, &broker, sizeof(actual)));
    }
    close(sockets[1]);
    phase = 0;
    LSBForkAttempt attempt = {.child = pid, .socket = sockets[0], .state = state};
    bool grew = !expectedGrowth;
    audit_token_t child = {{0}};
    assert(registerAllocatorOffer(&attempt, &child, &grew) == expectedError && grew == expectedGrowth);
    assert(phase == (brokerError ? 1 : 3));
    assert(!reapRejectedObserver(&attempt) && attempt.child == 0 && attempt.socket == -1);
    assert(WIFEXITED(observerStatus) && !WEXITSTATUS(observerStatus));
    errno = 0;
    assert(waitpid(pid, NULL, WNOHANG) == -1 && errno == ECHILD);
}

static void checkReadyAfterInventory(void) {
    int sockets[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    LSBForkAttempt attempt = {.socket = sockets[0], .child = 41};
    TrialReady sent = {.version = TrialWireVersion, .logicalPID = 41}, received = {};
    assert(!trialTransfer(sockets[1], &sent, sizeof(sent), true) && !receiveObserverReady(&attempt, &received, 0));
    // A locally failed inventory still consumes and validates the ready record. No offer or broker token is delivered.
    assert(!trialTransfer(sockets[1], &sent, sizeof(sent), true) && receiveObserverReady(&attempt, &received, EFAULT) == EFAULT);
    ++sent.logicalPID;
    assert(!trialTransfer(sockets[1], &sent, sizeof(sent), true) && receiveObserverReady(&attempt, &received, EFAULT) == ESRCH);
    ++sent.version;
    assert(!trialTransfer(sockets[1], &sent, sizeof(sent), true) && receiveObserverReady(&attempt, &received, EFAULT) == EPROTO);
    assert(!shutdown(sockets[1], SHUT_WR) && receiveObserverReady(&attempt, &received, EFAULT) == EPIPE);
    assert(!close(sockets[0]));
    assert(read(sockets[1], &received, sizeof(received)) == 0 && !close(sockets[1]));
}

int main(void) {
    checkReadyAfterInventory();
    unsigned placements = 0;
    for (unsigned i = 1; i <= 8; ++i) assert(retryPlacement(&placements, true) == (i < 8));
    placements = 0;
    for (unsigned i = 1; i <= 4; ++i) assert(retryPlacement(&placements, false) == (i < 4));
    placements = 3;
    assert(retryPlacement(&placements, true) && !retryPlacement(&placements, false));

    AllocatorAttempt state = {};
    state.offer.rangeCount = 1;
    state.offer.allocatorMask = 1;
    state.offer.ranges[0] = (LSBForkWireRange){newRegion - extent, extent};
    checkOffer(&state, EAGAIN, true);
    assert(state.missingStart == newRegion && state.missingBytes == extent);
    // A new inventory includes the added region. A registry change can also return EAGAIN, without a coverage miss.
    state.offer.ranges[0].size += extent;
    registryError = EAGAIN;
    checkOffer(&state, EAGAIN, false);
    registryError = 0;
    checkOffer(&state, 0, false);
    brokerError = EAGAIN;
    checkOffer(&state, EAGAIN, false);
    puts("Fork offer bounds placement retries, rejects a stale heap before delivering the broker token, and reaps the observer");
    return 0;
}
