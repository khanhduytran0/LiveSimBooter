#include "CreationMask.h"
#include "../../ProcessSupport/SpawnTransport.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

static atomic_flag creationMaskLock = ATOMIC_FLAG_INIT;
static atomic_bool configurationReady;
static atomic_int trackingFault;
static mode_t shadowMask;
static bool shadowValid;
static int initializationError;
static mode_t (*nativeGetumask)(void);

static void lockCreationMask(void) {
    while (atomic_flag_test_and_set_explicit(&creationMaskLock, memory_order_acquire)) {}
}

static void unlockCreationMask(void) { atomic_flag_clear_explicit(&creationMaskLock, memory_order_release); }

static void rememberTrackingFault(int error) {
    int expected = 0;
    atomic_compare_exchange_strong_explicit(&trackingFault, &expected, error ? error : EIO, memory_order_release, memory_order_relaxed);
}

static int blockSignals(sigset_t *previous) {
    sigset_t allSignals;
    if (sigfillset(&allSignals) || sigprocmask(SIG_BLOCK, &allSignals, previous)) return errno ? errno : EIO;
    return 0;
}

static int restoreSignals(const sigset_t *previous) {
    int result;
    do { result = sigprocmask(SIG_SETMASK, previous, NULL); } while (result && errno == EINTR);
    return result ? (errno ? errno : EIO) : 0;
}

static bool kernelHasGetumask(void) {
    // Simulator libraries can export newer syscall stubs than the phone's kernel implements.
    int mib[] = {CTL_KERN, KERN_OSRELEASE};
    char release[64];
    size_t length = sizeof(release);
    if (sysctl(mib, 2, release, &length, NULL, 0) || !length || length > sizeof(release) || release[length - 1]) return false;
    unsigned major = 0;
    size_t index = 0;
    while (index < length && release[index] >= '0' && release[index] <= '9') {
        unsigned digit = (unsigned)(release[index++] - '0');
        if (major > (UINT_MAX - digit) / 10) return false;
        major = major * 10 + digit;
    }
    return index && index < length && release[index] == '.' && major >= 26;
}

#ifndef LSB_CREATION_MASK_TESTING
__attribute__((constructor))
#endif
static void initializeCreationMask(void) {
    if (atomic_load_explicit(&configurationReady, memory_order_acquire)) return;
    int savedErrno = errno;
    // Resolve once during startup, never from the interposed umask, which may run inside a signal handler.
    mode_t (*query)(void) = kernelHasGetumask() ? (mode_t (*)(void))dlsym(RTLD_DEFAULT, "getumask") : NULL;
    sigset_t previous;
    int error = blockSignals(&previous);
    if (error) {
        rememberTrackingFault(error);
        errno = savedErrno;
        return;
    }
    lockCreationMask();
    if (!atomic_load_explicit(&configurationReady, memory_order_relaxed)) {
        nativeGetumask = query;
        // An earlier dependency initializer may already have called the interposed umask.
        if (!nativeGetumask && !shadowValid) {
            initializationError = LSBRequireSingleThread();
            if (!initializationError) {
                shadowMask = umask(0);
                umask(shadowMask);
                shadowValid = true;
            }
        }
        atomic_store_explicit(&configurationReady, true, memory_order_release);
    }
    unlockCreationMask();
    error = restoreSignals(&previous);
    if (error) rememberTrackingFault(error);
    errno = savedErrno;
}

mode_t LSBUmask(mode_t mask) {
    int savedErrno = errno;
    sigset_t previousSignals;
    int error = blockSignals(&previousSignals);
    if (error) {
        // umask has no failure return. Apply it, but never offer a possibly stale shadow to subsequent spawns.
        rememberTrackingFault(error);
        mode_t previous = umask(mask);
        errno = savedErrno;
        return previous;
    }
    lockCreationMask();
    // Same-image references retain the native symbol; there is no dlsym or pthread initialization in this path.
    mode_t previous = umask(mask);
    shadowMask = mask & 0777u;
    shadowValid = true;
    initializationError = 0;
    unlockCreationMask();
    error = restoreSignals(&previousSignals);
    if (error) rememberTrackingFault(error);
    errno = savedErrno;
    return previous;
}

int LSBReadCreationMask(uint32_t *mask) {
    int savedErrno = errno;
    int error = mask ? 0 : EINVAL;
    mode_t value = 0;
    if (!error) {
        initializeCreationMask();
        error = atomic_load_explicit(&trackingFault, memory_order_acquire);
    }
    sigset_t previousSignals;
    if (!error) {
        error = blockSignals(&previousSignals);
        if (!error) {
            lockCreationMask();
            error = atomic_load_explicit(&trackingFault, memory_order_acquire);
            if (!error && nativeGetumask) {
                value = nativeGetumask();
                if (value & ~0777u) error = EPROTO;
            } else if (!error && shadowValid) {
                value = shadowMask;
            } else if (!error) {
                error = initializationError ? initializationError : EBUSY;
            }
            unlockCreationMask();
            int restoreError = restoreSignals(&previousSignals);
            if (restoreError) { rememberTrackingFault(restoreError); error = restoreError; }
        }
    }
    if (!error) *mask = value;
    errno = savedErrno;
    return error;
}

#ifndef LSB_CREATION_MASK_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBUmask, (const void *)&umask}
};
#endif
