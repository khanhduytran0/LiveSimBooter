#include "ForkHook.h"
#include "ChildKill.h"
#include "ChildWait.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

extern pid_t __fork(void);
extern ssize_t __read_nocancel(int, void *, size_t);
extern ssize_t __write_nocancel(int, const void *, size_t);
extern pid_t LSBPublicFork(void);
extern pid_t LSBPublicVfork(void);
extern pid_t LSBPrivateFork(void);
#ifndef LSB_FORK_HOOK_TESTING
// Bind the deprecated ABI explicitly; guest executables, including Procursus Dash, still import it.
extern pid_t libcVfork(void) __asm("_vfork");
#endif

// libSystem's atfork prepare may own dyld and malloc locks. Force these imports into non-lazy pointer fixups.
static pid_t (*const volatile nativeForkSyscall)(void) = __fork;
static ssize_t (*const volatile readCheckpoint)(int, void *, size_t) = __read_nocancel;
static ssize_t (*const volatile writeCheckpoint)(int, const void *, size_t) = __write_nocancel;

typedef struct {
    LSBForkAttempt attempt;
    int error;
    bool armed, consumed;
} ForkCall;

static pthread_key_t activeCall;
static _Atomic(LSBForkPrepare) provider;
static bool providerHandlesVfork; // Immutable once provider is published.
static pthread_mutex_t providerLock = PTHREAD_MUTEX_INITIALIZER;
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "fork entry requires a lock-free provider lookup");

static int installForkProvider(LSBForkPrepare prepare, bool handlesVfork) {
    if (!prepare) return EINVAL;
    int error = pthread_mutex_lock(&providerLock);
    if (error) return error;
    if (atomic_load_explicit(&provider, memory_order_relaxed)) error = EALREADY;
    else if (!(error = pthread_key_create(&activeCall, NULL))) {
        providerHandlesVfork = handlesVfork;
        atomic_store_explicit(&provider, prepare, memory_order_release);
    }
    pthread_mutex_unlock(&providerLock);
    return error;
}

int LSBInstallForkProvider(LSBForkPrepare prepare) {
    return installForkProvider(prepare, false);
}

#if defined(LSB_FORK_SNAPSHOT_TRIAL)
extern bool LSBForkTrialEnabled(void);
extern int LSBPrepareForkAttempt(LSBForkAttempt *, const LSBForkContext *);
__attribute__((constructor)) static void enableForkTrial(void) {
    if (LSBForkTrialEnabled()) (void)installForkProvider(LSBPrepareForkAttempt, true);
}
#endif

static int configureSocket(int fd) {
    int yes = 1;
    struct timeval timeout = {.tv_sec = 10};
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) || setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))) return errno;
    return 0;
}

// Runs between libc's prepare and parent handlers. Never create an XPC connection, touch malloc, or call cancellation points here.
static int writeCheckpointBytes(int fd, const void *bytes, size_t length) {
    ssize_t count;
    for (size_t sent = 0; sent < length; sent += (size_t)count) {
        do { count = writeCheckpoint(fd, (const char *)bytes + sent, length - sent); } while (count < 0 && errno == EINTR);
        if (count <= 0) return count < 0 ? errno : EPIPE;
    }
    return 0;
}

static int checkpoint(LSBForkAttempt *attempt, const LSBForkContext *libc) {
    int fd = attempt->socket;
    uint8_t request = LSB_FORK_CHECKPOINT;
    int error = writeCheckpointBytes(fd, &request, sizeof(request));
    if (!error && libc) error = writeCheckpointBytes(fd, libc, sizeof(*libc));
    if (!error && attempt->sendPrepared) error = attempt->sendPrepared(attempt->state, fd);
    if (error) return error;
    int32_t status = 0;
    ssize_t count;
    for (size_t received = 0; received < sizeof(status); received += (size_t)count) {
        do { count = readCheckpoint(fd, (char *)&status + received, sizeof(status) - received); } while (count < 0 && errno == EINTR);
        if (count <= 0) return count < 0 ? errno : EPIPE;
    }
    return status >= 0 && status <= ELAST ? status : EPROTO;
}

uintptr_t LSBTrialLibcForkReturnPC(bool isVfork) {
    // 23A343 libsystem_c: fork and vfork each branch to the ___fork stub at entry + 0x30.
    return (uintptr_t)(isVfork ? &libcVfork : &fork) + LSB_FORK_LIBC_RETURN_OFFSET;
}

pid_t LSBForkSyscallAt(const LSBForkContext *libc) {
    bool isVfork = libc->pc == LSBTrialLibcForkReturnPC(true);
    if ((!isVfork && libc->pc != LSBTrialLibcForkReturnPC(false)) || !atomic_load_explicit(&provider, memory_order_acquire)) return nativeForkSyscall();
    ForkCall *call = pthread_getspecific(activeCall);
    // A signal handler, a different libc wrapper, or a second private call cannot consume this transaction.
    if (!call || !call->armed || call->consumed || call->attempt.isVfork != isVfork) return nativeForkSyscall();
    call->consumed = true;
    if (!call->error && call->attempt.checkPrepared) call->error = call->attempt.checkPrepared(call->attempt.state);
    if (!call->error) {
        call->error = checkpoint(&call->attempt, call->attempt.captureLibcContext ? libc : NULL);
        if (call->attempt.endCheckpoint) call->attempt.endCheckpoint(call->attempt.state);
    }
    if (call->error) { errno = call->error; return -1; }
    return call->attempt.child;
}

static pid_t forkAt(const LSBForkContext *caller, bool isVfork) {
    LSBForkPrepare prepare = atomic_load_explicit(&provider, memory_order_acquire);
    pid_t (*libcFork)(void) = isVfork ? libcVfork : fork;
    if (!prepare || (isVfork && !providerHandlesVfork)) return libcFork();
    if (pthread_getspecific(activeCall)) { errno = EDEADLK; return -1; }
    int saved = errno;
    ForkCall call = {.attempt = {.socket = -1, .isVfork = isVfork}};
    int error = pthread_setspecific(activeCall, &call);
    if (error) { errno = error; return -1; }
    call.error = prepare(&call.attempt, caller);
    bool forward = call.error == LSB_FORK_NOT_HANDLED && call.attempt.socket == -1 && call.attempt.child == 0;
    if (!forward) {
        if (call.error < 0 || call.error > ELAST || (!call.error && (call.attempt.child <= 0 || call.attempt.socket < 0))) call.error = EPROTO;
        if (!call.error) call.error = configureSocket(call.attempt.socket);
        call.armed = true;
    }
    errno = saved;
    pid_t result = libcFork();
    int resultErrno = result < 0 ? errno : saved;
    // Clearing an allocated key cannot fail. Never leave a live pointer to this stack frame in TSD.
    if (pthread_setspecific(activeCall, NULL)) _exit(126);
    if (call.attempt.socket >= 0) close(call.attempt.socket);
    if (call.attempt.release) call.attempt.release(call.attempt.state);
    if (result != 0 && call.attempt.child > 0 && (!call.consumed || call.error || result != call.attempt.child)) {
        // These are logical guest PIDs. Reap only the child that the provider explicitly created for this call.
        if (!LSBKill(call.attempt.child, SIGKILL) || errno == ESRCH)
            while (LSBWaitPID(call.attempt.child, NULL, 0) < 0 && errno == EINTR) {}
    }
#if defined(LSB_FORK_SNAPSHOT_TRIAL)
    extern void LSBForkTrialDidCommit(pid_t child);
    if (call.consumed && !call.error && result > 0 && result == call.attempt.child) LSBForkTrialDidCommit(result);
#endif
    errno = call.consumed && call.error ? call.error : resultErrno;
    return result;
}

pid_t LSBForkAt(const LSBForkContext *caller) { return forkAt(caller, false); }
pid_t LSBVforkAt(const LSBForkContext *caller) { return forkAt(caller, true); }

#ifndef LSB_FORK_HOOK_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBPublicFork, (const void *)&fork}, {(const void *)&LSBPublicVfork, (const void *)&libcVfork},
    {(const void *)&LSBPrivateFork, (const void *)&__fork}
};
#endif
