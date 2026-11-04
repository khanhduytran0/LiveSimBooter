#include "ChildKill.h"
#include "ChildWait.h"
#include "SpawnShim.h"

#include <crt_externs.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/wait.h>

// Signal dispositions are process-wide. Hold the lock only while the first caller saves them
// or the last caller restores them. Two threads may have shells running at the same time.
static pthread_mutex_t signalLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned signalUsers;
static struct sigaction savedInterrupt, savedQuit;

typedef struct {
    sigset_t originalMask;
    pid_t child;
} ShellCall;

static int enterShellWait(ShellCall *call, sigset_t *childDefaults) {
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    int error = pthread_sigmask(SIG_BLOCK, &block, &call->originalMask);
    if (error) return error;

    pthread_mutex_lock(&signalLock);
    if (!signalUsers) {
        struct sigaction ignore = {.sa_handler = SIG_IGN};
        sigemptyset(&ignore.sa_mask);
        if (sigaction(SIGINT, &ignore, &savedInterrupt)) error = errno;
        else if (sigaction(SIGQUIT, &ignore, &savedQuit)) {
            error = errno;
            sigaction(SIGINT, &savedInterrupt, NULL);
        }
    }
    if (!error) {
        ++signalUsers;
        sigemptyset(childDefaults);
        if (savedInterrupt.sa_handler != SIG_IGN) sigaddset(childDefaults, SIGINT);
        if (savedQuit.sa_handler != SIG_IGN) sigaddset(childDefaults, SIGQUIT);
    }
    pthread_mutex_unlock(&signalLock);
    if (error) pthread_sigmask(SIG_SETMASK, &call->originalMask, NULL);
    return error;
}

static pid_t waitForShell(pid_t child, int *status) {
    pid_t result;
    do { result = LSBWaitPID(child, status, 0); } while (result < 0 && errno == EINTR);
    return result;
}

static void leaveShellWait(void *context) {
    ShellCall *call = context;
    if (call->child > 0) {
        // The waiting thread was canceled. The broker owns the real PID.
        LSBKill(call->child, SIGKILL);
        waitForShell(call->child, NULL);
    }
    pthread_mutex_lock(&signalLock);
    if (!--signalUsers) {
        sigaction(SIGINT, &savedInterrupt, NULL);
        sigaction(SIGQUIT, &savedQuit, NULL);
    }
    pthread_mutex_unlock(&signalLock);
    pthread_sigmask(SIG_SETMASK, &call->originalMask, NULL);
}

// An unusable interpreter is reported as exit 127. Broker, descriptor and process-capacity failures retain their errno.
static int shellLaunchFailure(int error) {
    switch (error) {
        case ENOENT: case ENOTDIR: case ELOOP: case ENAMETOOLONG: case EACCES: case EPERM:
        case ENOEXEC: case EBADARCH: case EBADEXEC: case EBADMACHO: case E2BIG: case ETXTBSY:
            return W_EXITCODE(127, 0);
        default:
            return -error;
    }
}

static int runShell(const char *command, int waitCancelState) {
    char shell[PATH_MAX];
    int error = LSBResolveExecutableForSpawn("/bin/sh", shell);
    if (error == ENOENT || error == ENOTDIR) error = LSBResolveExecutableForSpawn("/var/jb/bin/sh", shell);
    if (!command) return !error;
    if (error) return shellLaunchFailure(error);

    ShellCall call = {0};
    sigset_t defaults;
    error = enterShellWait(&call, &defaults);
    if (error) return -error;

    int status;
    pthread_cleanup_push(leaveShellWait, &call);
    posix_spawnattr_t attributes;
    error = posix_spawnattr_init(&attributes);
    if (!error) {
        error = posix_spawnattr_setsigmask(&attributes, &call.originalMask);
        if (!error) error = posix_spawnattr_setsigdefault(&attributes, &defaults);
        if (!error) error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
        if (!error) {
            char *args[] = {"sh", "-c", "--", (char *)command, NULL};
            error = LSBPosixSpawn(&call.child, shell, NULL, &attributes, args, *_NSGetEnviron());
            status = error ? shellLaunchFailure(error) : 0;
        } else status = -error;
        posix_spawnattr_destroy(&attributes);
    } else status = -error;

    if (!error) {
        // Enable cancellation only once the cleanup handler owns the shell's logical PID.
        pthread_setcancelstate(waitCancelState, NULL);
        if (waitForShell(call.child, &status) < 0) status = -errno;
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        call.child = 0;
    }
    pthread_cleanup_pop(1);
    return status;
}

static int shellStatus(const char *command, bool cancellable) {
    int savedErrno = errno, cancelState;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancelState);
    int status = runShell(command, cancellable ? cancelState : PTHREAD_CANCEL_DISABLE);
    pthread_setcancelstate(cancelState, NULL);
    errno = status < 0 ? -status : savedErrno;
    return status < 0 ? -1 : status;
}

int LSBSystem(const char *command) { return shellStatus(command, true); }
int LSBSystemNoCancel(const char *command) { return shellStatus(command, false); }

#ifndef LSB_SYSTEM_TESTING
// Both 23A343 libsystem_c entry points abort. Use their ABI names since the iOS SDK marks system() unavailable.
extern int darwinSystem(const char *) __asm("_system");
extern int darwinSystemNoCancel(const char *) __asm("_system$NOCANCEL");
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBSystem, (const void *)&darwinSystem}, {(const void *)&LSBSystemNoCancel, (const void *)&darwinSystemNoCancel}
};
#endif
