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
static pthread_mutex_t providerLock = PTHREAD_MUTEX_INITIALIZER;
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "fork entry requires a lock-free provider lookup");

int LSBInstallForkProvider(LSBForkPrepare prepare) {
    if (!prepare) return EINVAL;
    int error = pthread_mutex_lock(&providerLock);
    if (error) return error;
    if (atomic_load_explicit(&provider, memory_order_relaxed)) error = EALREADY;
    else if (!(error = pthread_key_create(&activeCall, NULL))) atomic_store_explicit(&provider, prepare, memory_order_release);
    pthread_mutex_unlock(&providerLock);
    return error;
}

#if defined(LSB_FORK_SNAPSHOT_TRIAL)
extern bool LSBForkTrialEnabled(void);
extern int LSBPrepareForkAttempt(LSBForkAttempt *, const LSBForkContext *);
__attribute__((constructor)) static void enableForkTrial(void) {
    if (LSBForkTrialEnabled()) (void)LSBInstallForkProvider(LSBPrepareForkAttempt);
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
static int checkpoint(int fd) {
    uint8_t request = LSB_FORK_CHECKPOINT;
    ssize_t count;
    do { count = writeCheckpoint(fd, &request, sizeof(request)); } while (count < 0 && errno == EINTR);
    if (count != 1) return count < 0 ? errno : EPIPE;
    int32_t status = 0;
    for (size_t received = 0; received < sizeof(status); received += (size_t)count) {
        do { count = readCheckpoint(fd, (char *)&status + received, sizeof(status) - received); } while (count < 0 && errno == EINTR);
        if (count <= 0) return count < 0 ? errno : EPIPE;
    }
    return status >= 0 && status <= ELAST ? status : EPROTO;
}

static pid_t forkSyscallAt(uintptr_t returnPC) {
    // 23A343 libsystem_c: fork+0x30 is BL to the ___fork stub; vfork has its own BL to the same stub.
    // Requiring this exact caller also keeps signal handlers or direct users of __fork from taking the staged operation.
    if (returnPC != (uintptr_t)&fork + 0x34 || !atomic_load_explicit(&provider, memory_order_acquire)) return nativeForkSyscall();
    ForkCall *call = pthread_getspecific(activeCall);
    if (!call || !call->armed || call->consumed) return nativeForkSyscall();
    call->consumed = true;
    if (!call->error) call->error = checkpoint(call->attempt.socket);
    if (call->error) { errno = call->error; return -1; }
    return call->attempt.child;
}

static pid_t forkSyscall(void) { return forkSyscallAt((uintptr_t)__builtin_return_address(0)); }

pid_t LSBForkAt(const LSBForkContext *caller) {
    LSBForkPrepare prepare = atomic_load_explicit(&provider, memory_order_acquire);
    if (!prepare) return fork();
    if (pthread_getspecific(activeCall)) { errno = EDEADLK; return -1; }
    int saved = errno;
    ForkCall call = {.attempt.socket = -1};
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
    pid_t result = fork();
    int resultErrno = result < 0 ? errno : saved;
    // Clearing an allocated key cannot fail. Never leave a live pointer to this stack frame in TSD.
    if (pthread_setspecific(activeCall, NULL)) _exit(126);
    if (call.attempt.socket >= 0) close(call.attempt.socket);
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

#ifndef LSB_FORK_HOOK_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBPublicFork, (const void *)&fork}, {(const void *)&forkSyscall, (const void *)&__fork}
};
#endif
