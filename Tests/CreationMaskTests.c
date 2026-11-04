#include "../ProcessSupport/SpawnTransport.h"
#include "../ProcessSupport/SingleThreadGuard.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

static const char *kernelRelease;
static int releaseError, guardError, blockError, restoreError, restoreInterrupts;
static bool missingQuery, interruptCapture, checkSerialization;
static atomic_uint queries, lookups, maskChanges, guards, blocks, restores;
static atomic_flag nativeMaskActive = ATOMIC_FLAG_INIT;
static mode_t queryResult, observedMask;
static volatile sig_atomic_t deliveredSignals, maskInHandler, previousInHandler;

static int testSysctl(int *name, u_int count, void *output, size_t *length, void *newValue, size_t newLength) {
    assert(count == 2 && name[0] == CTL_KERN && name[1] == KERN_OSRELEASE && !newValue && !newLength);
    if (releaseError) { errno = releaseError; return -1; }
    size_t bytes = strlen(kernelRelease) + 1;
    assert(*length >= bytes);
    memcpy(output, kernelRelease, bytes);
    *length = bytes;
    return 0;
}

static mode_t testGetumask(void) { ++queries; return queryResult; }
static void *testDlsym(void *scope, const char *symbol) {
    assert(scope == RTLD_DEFAULT && !strcmp(symbol, "getumask"));
    ++lookups;
    return missingQuery ? NULL : (void *)&testGetumask;
}
static int testSingleThread(void) { ++guards; return guardError ? guardError : LSBSingleThreadGuard(); }
static int testSigprocmask(int how, const sigset_t *set, sigset_t *previous) {
    if (how == SIG_BLOCK) {
        ++blocks;
        if (blockError) { errno = blockError; return -1; }
    } else {
        assert(how == SIG_SETMASK);
        ++restores;
        if (restoreInterrupts) { --restoreInterrupts; errno = EINTR; return -1; }
        if (restoreError) { errno = restoreError; return -1; }
    }
    return sigprocmask(how, set, previous);
}
static mode_t testUmask(mode_t mask) {
    assert(!atomic_flag_test_and_set(&nativeMaskActive));
    ++maskChanges;
    mode_t previous = umask(mask);
    if (checkSerialization) {
        assert(previous == observedMask);
        observedMask = mask & 0777;
        sched_yield();
    }
    if (interruptCapture) {
        interruptCapture = false;
        assert(!raise(SIGUSR1));
        assert(!deliveredSignals);
    }
    atomic_flag_clear(&nativeMaskActive);
    return previous;
}

#define LSB_CREATION_MASK_TESTING
#define sysctl testSysctl
#define dlsym testDlsym
#define LSBRequireSingleThread testSingleThread
#define sigprocmask testSigprocmask
#define umask testUmask
#include "../Guest/libLiveSimSpawn/CreationMask.c"
#undef sysctl
#undef dlsym
#undef LSBRequireSingleThread
#undef sigprocmask
#undef umask

static void signalHandler(int number) {
    int savedErrno = errno;
    assert(number == SIGUSR1);
    previousInHandler = LSBUmask(077);
    uint32_t current;
    assert(!LSBReadCreationMask(&current));
    maskInHandler = current;
    ++deliveredSignals;
    errno = savedErrno;
}

static void reset(void) {
    umask(027);
    kernelRelease = "25.1.0";
    releaseError = guardError = blockError = restoreError = restoreInterrupts = 0;
    missingQuery = interruptCapture = checkSerialization = false;
    queries = lookups = maskChanges = guards = blocks = restores = 0;
    queryResult = 077;
    deliveredSignals = 0;
    maskInHandler = previousInHandler = -1;
    atomic_flag_clear(&creationMaskLock);
    configurationReady = false;
    trackingFault = 0;
    shadowMask = 0;
    shadowValid = false;
    initializationError = 0;
    nativeGetumask = NULL;
}

static void assertMaskUnchanged(mode_t expected) { assert(umask(expected) == expected); }

static void testOldKernel(void) {
    reset();
    errno = EDOM;
    initializeCreationMask();
    assert(errno == EDOM && guards == 1 && maskChanges == 2);
    uint32_t result = UINT32_MAX;
    assert(!LSBReadCreationMask(&result) && result == 027 && errno == EDOM);
    assert(!queries && !lookups && guards == 1 && blocks == 2 && restores == 2 && maskChanges == 2);
    assertMaskUnchanged(027);
    assert(LSBUmask(010077) == 027 && errno == EDOM);
    assert(!LSBReadCreationMask(&result) && result == 077 && errno == EDOM && maskChanges == 3);
    assertMaskUnchanged(077);

    reset();
    assert(LSBUmask(022) == 027); // A dependency can call umask before this image's constructor.
    guardError = EBUSY;
    initializeCreationMask();
    assert(!LSBReadCreationMask(&result) && result == 022 && !guards && maskChanges == 1);
    assertMaskUnchanged(022);
}

static void testSignalHandler(void) {
    struct sigaction action = {.sa_handler = signalHandler}, previous;
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGUSR1, &action, &previous));
    for (unsigned variant = 0; variant < 2; ++variant) {
        reset();
        if (variant) initializeCreationMask();
        interruptCapture = true;
        restoreInterrupts = 1;
        errno = EDOM;
        if (variant) assert(LSBUmask(022) == 027);
        else initializeCreationMask();
        assert(errno == EDOM && deliveredSignals == 1 && maskInHandler == 077 && previousInHandler == (variant ? 022 : 027));
        uint32_t result;
        assert(!LSBReadCreationMask(&result) && result == 077 && errno == EDOM);
        assertMaskUnchanged(077);
    }
    assert(!sigaction(SIGUSR1, &previous, NULL));
}

static void testFailureRestoresState(void) {
    for (unsigned variant = 0; variant < 3; ++variant) {
        reset();
        sigset_t before, after;
        assert(!sigprocmask(SIG_SETMASK, NULL, &before));
        uint32_t result = UINT32_MAX;
        if (variant == 0) guardError = EBUSY;
        if (variant == 1) guardError = EIO;
        if (variant == 2) blockError = EPERM;
        errno = EDOM;
        assert(LSBReadCreationMask(&result) == (blockError ? blockError : guardError) && result == UINT32_MAX && errno == EDOM);
        assert(!sigprocmask(SIG_SETMASK, NULL, &after) && !memcmp(&before, &after, sizeof(before)));
        assert(!maskChanges && !queries && restores == (variant == 2 ? 0u : 2u));
        assertMaskUnchanged(027);
    }
    reset();
    initializeCreationMask();
    blockError = EPERM;
    errno = EDOM;
    assert(LSBUmask(077) == 027 && errno == EDOM);
    blockError = 0;
    uint32_t result = UINT32_MAX;
    assert(LSBReadCreationMask(&result) == EPERM && result == UINT32_MAX && errno == EDOM);
    assertMaskUnchanged(077); // A tracking failure does not discard the caller's requested kernel umask.
    assert(LSBReadCreationMask(NULL) == EINVAL && errno == EDOM);

    reset();
    initializeCreationMask();
    sigset_t before;
    assert(!sigprocmask(SIG_SETMASK, NULL, &before));
    restoreError = EIO;
    errno = EDOM;
    assert(LSBUmask(077) == 027 && errno == EDOM);
    assert(!sigprocmask(SIG_SETMASK, &before, NULL)); // Restore the signal state after the injected restoration failure.
    restoreError = 0;
    result = UINT32_MAX;
    assert(LSBReadCreationMask(&result) == EIO && result == UINT32_MAX && errno == EDOM);
    assertMaskUnchanged(077);
}

static void *blockedWorker(void *context) {
    char byte;
    assert(read(*(int *)context, &byte, 1) == 1);
    return NULL;
}

static void waitForSingleThread(void) {
    int guard = EBUSY;
    for (unsigned attempt = 0; attempt < 100 && guard == EBUSY; ++attempt) { usleep(1000); guard = LSBSingleThreadGuard(); }
    assert(!guard);
}

static void testRealThreadGuard(void) {
    reset();
    int gate[2];
    assert(!pipe(gate));
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, blockedWorker, &gate[0]));
    uint32_t result = UINT32_MAX;
    assert(LSBReadCreationMask(&result) == EBUSY && result == UINT32_MAX && !maskChanges && guards == 1);
    assert(LSBUmask(022) == 027);
    assert(!LSBReadCreationMask(&result) && result == 022 && guards == 1 && maskChanges == 1);
    assert(write(gate[1], "x", 1) == 1 && !pthread_join(worker, NULL));
    close(gate[0]); close(gate[1]);
    waitForSingleThread();
}

static void *concurrentWorker(void *context) {
    bool writer = (uintptr_t)context != 0;
    for (unsigned iteration = 0; iteration < 2000; ++iteration) {
        errno = EDOM;
        if (writer) {
            mode_t previous = LSBUmask(iteration & 1 ? 022 : 077);
            assert(previous == 022 || previous == 027 || previous == 077);
        }
        uint32_t result;
        assert(!LSBReadCreationMask(&result) && (result == 022 || result == 027 || result == 077) && errno == EDOM);
    }
    return NULL;
}

static void testConcurrentTracking(void) {
    reset();
    initializeCreationMask();
    checkSerialization = true;
    observedMask = 027;
    pthread_t workers[8];
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_create(&workers[i], NULL, concurrentWorker, (void *)(uintptr_t)(i & 1)));
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_join(workers[i], NULL));
    checkSerialization = false;
    assert(guards == 1 && maskChanges == 8002 && !queries && !lookups);
    uint32_t result;
    assert(!LSBReadCreationMask(&result) && result == observedMask);
    assertMaskUnchanged(result);
    waitForSingleThread();
}

static void testKernelSelection(void) {
    const char *unknown[] = {"", "26", "x26.0", "-26.0", "9999999999999999999999999.0", "24.9.0", "25.1.0"};
    for (unsigned i = 0; i < sizeof(unknown) / sizeof(unknown[0]); ++i) {
        reset();
        kernelRelease = unknown[i];
        uint32_t result;
        assert(!LSBReadCreationMask(&result) && result == 027 && !queries && !lookups && maskChanges == 2);
    }
    reset();
    releaseError = ENOTSUP;
    uint32_t result;
    assert(!LSBReadCreationMask(&result) && result == 027 && !queries && !lookups);
    reset();
    kernelRelease = "26.0.0";
    guardError = EBUSY;
    assert(!LSBReadCreationMask(&result) && result == 077 && queries == 1 && lookups == 1 && !guards && !maskChanges);
    assert(LSBUmask(022) == 027);
    queryResult = 012; // The native query remains authoritative, including calls bypassing the interposer.
    assert(!LSBReadCreationMask(&result) && result == 012 && queries == 2 && lookups == 1);
    assertMaskUnchanged(022);
    reset();
    kernelRelease = "26.1.0";
    missingQuery = true;
    assert(!LSBReadCreationMask(&result) && result == 027 && !queries && lookups == 1 && maskChanges == 2);
    reset();
    kernelRelease = "26.1.0";
    queryResult = (mode_t)-1;
    result = UINT32_MAX;
    errno = EDOM;
    assert(LSBReadCreationMask(&result) == EPROTO && result == UINT32_MAX && errno == EDOM && !maskChanges);
}

int main(void) {
    alarm(30);
    mode_t original = umask(027);
    testOldKernel();
    testSignalHandler();
    testFailureRestoresState();
    testRealThreadGuard();
    testConcurrentTracking();
    testKernelSelection();
    umask(original);
    puts("CreationMask: startup capture, concurrent tracking, signal-handler reentry, failure closure, native query gating, and errno preservation passed");
    return 0;
}
