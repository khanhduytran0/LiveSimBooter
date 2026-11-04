#include "../Guest/ForkHook.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t originalFork(void), libcVfork(void), originalSyscall(void);
#define fork originalFork
#define __fork originalSyscall
#define LSB_FORK_HOOK_TESTING
#include "../Guest/ForkHook.c"
#undef __fork
#undef fork

enum { Forward, Success, Denied, Malformed, Disconnect, SetupError, NoSyscall, Concurrent, WithLibcContext, StaleOffer, SendError };
static _Thread_local int mode, peer = -1, prepareCount, libcCount, nativeCount, kills, waits;
static _Thread_local bool locked, testingVfork;
static LSBForkContext savedLibc;
static _Thread_local unsigned rechecks, sends, ends, releases;

static int sendPreparedState(void *state, int fd) {
    assert(locked && state == &mode && rechecks == 1 && sends == 0);
    sends++;
    if (mode == SendError) return EBUSY;
    return write(fd, "x", 1) == 1 ? 0 : errno;
}

static void endPreparedState(void *state) {
    assert(locked && state == &mode && sends == 1 && ends == 0);
    ends++;
}

static int checkPreparedState(void *state) {
    assert(locked && state == &mode);
    ++rechecks;
    return mode == StaleOffer ? EAGAIN : 0;
}

static void releaseState(void *state) {
    assert(!locked && state == &mode);
    ++releases;
}

static pid_t originalSyscall(void) {
    nativeCount++;
    errno = EPERM;
    return -1;
}

static pid_t simulateLibc(bool isVfork) {
    assert(!locked && isVfork == testingVfork);
    libcCount++;
    locked = true;
    if (mode == Disconnect) { close(peer); peer = -1; }
    // Neither a direct private syscall nor the other public wrapper's return PC can consume this transaction.
    LSBForkContext unrelated = {.pc = 0x1000};
    assert(LSBForkSyscallAt(&unrelated) == -1 && errno == EPERM);
    unrelated.pc = (uintptr_t)(isVfork ? originalFork : libcVfork) + LSB_FORK_LIBC_RETURN_OFFSET;
    assert(LSBForkSyscallAt(&unrelated) == -1 && errno == EPERM);
    LSBForkContext libc = {.pc = (uintptr_t)(isVfork ? libcVfork : originalFork) + LSB_FORK_LIBC_RETURN_OFFSET, .sp = 0x3000, .fp = 0x3010, .fpcr = 0x400000,
                           .x19ToX28[0] = 0x19, .d8ToD15[0] = 0xd8};
    if (mode == WithLibcContext) savedLibc = libc;
    pid_t result = mode == NoSyscall ? originalSyscall() : LSBForkSyscallAt(&libc);
    locked = false;
    return result;
}

static pid_t originalFork(void) { return simulateLibc(false); }
static pid_t libcVfork(void) { return simulateLibc(true); }

int LSBKill(pid_t pid, int signalNumber) {
    assert(!locked && pid == 191 && signalNumber == SIGKILL);
    ++kills;
    return 0;
}

pid_t LSBWaitPID(pid_t pid, int *status, int options) {
    assert(!locked && pid == 191 && !status && !options);
    ++waits;
    return pid;
}

// Make two calls cross the preparation boundary together; each must see only its own socket.
static pthread_mutex_t pairLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pairReady = PTHREAD_COND_INITIALIZER;
static int pairCount;
static _Thread_local int32_t concurrentReply;

static int prepare(LSBForkAttempt *attempt, const LSBForkContext *caller) {
    ++prepareCount;
    assert(attempt->isVfork == testingVfork);
    assert(!locked && caller->pc == 0x1234 && caller->sp == 0x3450 && attempt->socket == -1 && !attempt->child);
    if (mode == Forward) return LSB_FORK_NOT_HANDLED;
    if (mode == SetupError) return ENOMEM;
    int fds[2];
    attempt->state = &mode;
    attempt->checkPrepared = checkPreparedState;
    attempt->release = releaseState;
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    attempt->socket = fds[0]; attempt->child = 191;
    attempt->captureLibcContext = mode == WithLibcContext;
    if (mode == WithLibcContext || mode == SendError || mode == StaleOffer) {
        attempt->sendPrepared = sendPreparedState;
        attempt->endCheckpoint = endPreparedState;
    }
    peer = fds[1];
    if (mode == Disconnect) return 0;
    int32_t reply = mode == Denied ? EADDRINUSE : mode == Malformed ? -13 : mode == Concurrent ? concurrentReply : 0;
    if (mode == Concurrent) {
        pthread_mutex_lock(&pairLock);
        ++pairCount;
        pthread_cond_broadcast(&pairReady);
        while (pairCount != 2) pthread_cond_wait(&pairReady, &pairLock);
        pthread_mutex_unlock(&pairLock);
    }
    assert(write(peer, &reply, sizeof(reply)) == sizeof(reply));
    return 0;
}

static void check(int testMode, int expectedError, bool cleanup, bool request) {
    mode = testMode;
    prepareCount = libcCount = nativeCount = kills = waits = 0;
    rechecks = sends = ends = releases = 0;
    errno = ERANGE;
    LSBForkContext caller = {.pc = 0x1234, .sp = 0x3450};
    pid_t result = testingVfork ? LSBVforkAt(&caller) : LSBForkAt(&caller);
    if (result != (expectedError ? -1 : 191) || errno != (expectedError ? expectedError : ERANGE))
        fprintf(stderr, "mode=%d result=%d errno=%d expectedError=%d\n", testMode, result, errno, expectedError);
    assert(result == (expectedError ? -1 : 191) && errno == (expectedError ? expectedError : ERANGE));
    assert(prepareCount == 1 && libcCount == 1 && nativeCount == (testMode == Forward || testMode == NoSyscall ? 3 : 2));
    assert(kills == cleanup && waits == cleanup);
    assert(releases == (mode != Forward && mode != SetupError));
    assert(rechecks == (releases && mode != NoSyscall));
    assert(sends == (mode == WithLibcContext || mode == SendError) && ends == sends);
    if (peer >= 0) {
        uint8_t byte;
        if (request) assert(read(peer, &byte, 1) == 1 && byte == LSB_FORK_CHECKPOINT);
        if (mode == WithLibcContext) {
            LSBForkContext libc;
            assert(recv(peer, &libc, sizeof(libc), MSG_WAITALL) == sizeof(libc));
            assert(!memcmp(&libc, &savedLibc, sizeof(libc)));
            assert(read(peer, &byte, 1) == 1 && byte == 'x');
        }
        // If an unconsumed reply was discarded, Darwin can report ECONNRESET after close.
        ssize_t count = read(peer, &byte, 1);
        assert(count == 0 || (count < 0 && errno == ECONNRESET));
        close(peer); peer = -1;
    }
}

static void *concurrent(void *arg) {
    concurrentReply = *(const int *)arg;
    testingVfork = concurrentReply != 0;
    check(Concurrent, concurrentReply, concurrentReply != 0, true);
    return NULL;
}

int main(void) {
    assert(LSBForkAt(NULL) == -1 && errno == EPERM && !prepareCount && nativeCount == 3);
    testingVfork = true;
    assert(LSBVforkAt(NULL) == -1 && errno == EPERM && !prepareCount && nativeCount == 6);
    // Exercise the public fork-only provider in a fresh process. Provider registration is immutable.
    pid_t legacy = fork();
    assert(legacy >= 0);
    if (!legacy) {
        assert(LSBInstallForkProvider(prepare) == 0);
        assert(LSBVforkAt(NULL) == -1 && errno == EPERM && !prepareCount && nativeCount == 9);
        testingVfork = false;
        check(Success, 0, false, true);
        _exit(0);
    }
    int status;
    assert(waitpid(legacy, &status, 0) == legacy && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(LSBInstallForkProvider(NULL) == EINVAL && installForkProvider(prepare, true) == 0 && LSBInstallForkProvider(prepare) == EALREADY);
    check(Forward, EPERM, false, false);
    check(WithLibcContext, 0, false, true);
    check(StaleOffer, EAGAIN, true, false);
    check(NoSyscall, EPERM, true, false);
    testingVfork = false;
    check(Forward, EPERM, false, false);
    check(Success, 0, false, true);
    check(WithLibcContext, 0, false, true);
    check(StaleOffer, EAGAIN, true, false);
    check(SendError, EBUSY, true, true);
    check(Denied, EADDRINUSE, true, true);
    check(Malformed, EPROTO, true, true);
    check(Disconnect, EPIPE, true, false);
    check(SetupError, ENOMEM, false, false);
    check(NoSyscall, EPERM, true, false);
    pthread_t threads[2];
    int replies[] = {0, EACCES};
    for (int i = 0; i < 2; ++i) assert(!pthread_create(&threads[i], NULL, concurrent, &replies[i]));
    for (int i = 0; i < 2; ++i) assert(!pthread_join(threads[i], NULL));
    puts("fork/vfork entries, exact libc calls, legacy forwarding, staged-child cleanup, and per-thread rendezvous passed");
}
