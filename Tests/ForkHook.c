#include "../Guest/ForkHook.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t originalFork(void), originalSyscall(void);
#define fork originalFork
#define __fork originalSyscall
#define LSB_FORK_HOOK_TESTING
#include "../Guest/ForkHook.c"
#undef __fork
#undef fork

enum { Forward, Success, Denied, Malformed, Disconnect, SetupError, NoSyscall, Concurrent };
static _Thread_local int mode, peer = -1, prepareCount, libcCount, nativeCount, kills, waits;
static _Thread_local bool locked;

static pid_t originalSyscall(void) {
    nativeCount++;
    errno = EPERM;
    return -1;
}

static pid_t originalFork(void) {
    assert(!locked);
    libcCount++;
    locked = true;
    if (mode == Disconnect) { close(peer); peer = -1; }
    // vfork and a direct private syscall cannot consume the transaction for ordinary fork.
    assert(forkSyscall() == -1 && errno == EPERM);
    pid_t result = mode == NoSyscall ? originalSyscall() : forkSyscallAt((uintptr_t)originalFork + 0x34);
    locked = false;
    return result;
}

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
    assert(!locked && caller->pc == 0x1234 && caller->sp == 0x3450 && attempt->socket == -1 && !attempt->child);
    if (mode == Forward) return LSB_FORK_NOT_HANDLED;
    if (mode == SetupError) return ENOMEM;
    int fds[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    attempt->socket = fds[0]; attempt->child = 191;
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
    errno = ERANGE;
    LSBForkContext caller = {.pc = 0x1234, .sp = 0x3450};
    pid_t result = LSBForkAt(&caller);
    if (result != (expectedError ? -1 : 191) || errno != (expectedError ? expectedError : ERANGE))
        fprintf(stderr, "mode=%d result=%d errno=%d expectedError=%d\n", testMode, result, errno, expectedError);
    assert(result == (expectedError ? -1 : 191) && errno == (expectedError ? expectedError : ERANGE));
    assert(prepareCount == 1 && libcCount == 1 && nativeCount == (testMode == Forward || testMode == NoSyscall ? 2 : 1));
    assert(kills == cleanup && waits == cleanup);
    if (peer >= 0) {
        uint8_t byte;
        if (request) assert(read(peer, &byte, 1) == 1 && byte == LSB_FORK_CHECKPOINT);
        // If an unconsumed reply was discarded, Darwin can report ECONNRESET after close.
        ssize_t count = read(peer, &byte, 1);
        assert(count == 0 || (count < 0 && errno == ECONNRESET));
        close(peer); peer = -1;
    }
}

static void *concurrent(void *arg) {
    concurrentReply = *(const int *)arg;
    check(Concurrent, concurrentReply, concurrentReply != 0, true);
    return NULL;
}

int main(void) {
    assert(LSBForkAt(NULL) == -1 && errno == EPERM && !prepareCount && nativeCount == 2);
    assert(LSBInstallForkProvider(NULL) == EINVAL && LSBInstallForkProvider(prepare) == 0 && LSBInstallForkProvider(prepare) == EALREADY);
    check(Forward, EPERM, false, false);
    check(Success, 0, false, true);
    check(Denied, EADDRINUSE, true, true);
    check(Malformed, EPROTO, true, true);
    check(Disconnect, EPIPE, true, false);
    check(SetupError, ENOMEM, false, false);
    check(NoSyscall, EPERM, true, false);
    pthread_t threads[2];
    int replies[] = {0, EACCES};
    for (int i = 0; i < 2; ++i) assert(!pthread_create(&threads[i], NULL, concurrent, &replies[i]));
    for (int i = 0; i < 2; ++i) assert(!pthread_join(threads[i], NULL));
    puts("fork entry, libc forwarding, staged-child cleanup, and per-thread rendezvous passed");
}
