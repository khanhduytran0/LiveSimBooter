#include <assert.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/arm/thread_status.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned registerCalls, createCalls, getCalls, setCalls, resumeCalls, terminateCalls, deallocateCalls, signCalls;
static unsigned observedFlags, corruptState, stateFlags;
static int registerResult, registerError, createError, exitStatus;
static kern_return_t getError, setError, resumeError, terminateError;
static uintptr_t observedStart, observedWorkqueue, observedModifier, observedSignedPC;
static void *observedData;
static size_t observedSize;
static uint64_t observedDispatch;
static int observedPthreadSize;
static bool invalidPort;
static jmp_buf exitJump;
static unsigned char pthreadStorage[16384] __attribute__((aligned(16384)));
static arm_thread_state64_t initialState, updatedState;
static const mach_port_t testPort = 0x170b;
static uintptr_t observedWorkqueueRegistration;
static const char *nativeSupportMode;
static unsigned environmentReads;
static char *testGetenv(const char *name) {
    assert(!strcmp(name, "LSB_NATIVE_SUPPORT"));
    ++environmentReads;
    return (char *)nativeSupportMode;
}
void LSBWorkqueueRegister(uintptr_t start) { observedWorkqueueRegistration = start; }

static int testRegister(uintptr_t start, uintptr_t workqueue, int size, void *data, size_t bytes, uint64_t dispatch) {
    ++registerCalls;
    observedStart = start;
    observedWorkqueue = workqueue;
    observedPthreadSize = size;
    observedData = data;
    observedSize = bytes;
    observedDispatch = dispatch;
    if (registerResult < 0) errno = registerError;
    return registerResult;
}

static void *testCreate(void *(*function)(void *), void *argument, void *stack, void *thread, unsigned flags) {
    ++createCalls;
    observedFlags = flags;
    if (createError) { errno = createError; return (void *)-1; }
    *(uintptr_t *)((unsigned char *)thread + 248) = invalidPort ? UINT64_MAX : testPort;
    memset(&initialState, 0x3c, sizeof(initialState));
    initialState.__x[0] = (uintptr_t)thread;
    initialState.__x[1] = testPort;
    initialState.__x[2] = (uintptr_t)function;
    initialState.__x[3] = (uintptr_t)argument;
    initialState.__x[4] = (uintptr_t)stack;
    initialState.__x[5] = (flags | 0x10000000) & ~0x20000000;
    initialState.__sp = (uintptr_t)stack;
    initialState.__pc = 0x1e4a538c4;
    initialState.__pad = stateFlags;
    return thread;
}

static kern_return_t testGetState(thread_act_t port, thread_state_flavor_t flavor, thread_state_t state, mach_msg_type_number_t *count) {
    ++getCalls;
    assert(port == testPort && flavor == ARM_THREAD_STATE64 && *count == ARM_THREAD_STATE64_COUNT);
    if (!getError) {
        memcpy(state, &initialState, sizeof(initialState));
        if (corruptState == 1) --*count;
        if (corruptState == 2) ((arm_thread_state64_t *)state)->__x[0] = 0;
        if (corruptState == 3) ((arm_thread_state64_t *)state)->__x[1] = 0;
        if (corruptState == 4) ((arm_thread_state64_t *)state)->__x[4] = 0;
        if (corruptState == 5) ((arm_thread_state64_t *)state)->__x[5] &= ~0x10000000;
        if (corruptState == 6) ((arm_thread_state64_t *)state)->__x[5] |= 0x20000000;
    }
    return getError;
}

static kern_return_t testSetState(thread_act_t port, thread_state_flavor_t flavor, thread_state_t state, mach_msg_type_number_t count) {
    ++setCalls;
    assert(port == testPort && flavor == ARM_THREAD_STATE64 && count == ARM_THREAD_STATE64_COUNT);
    memcpy(&updatedState, state, sizeof(updatedState));
    return setError;
}
static kern_return_t testResume(thread_act_t port) { ++resumeCalls; assert(port == testPort); return resumeError; }
static kern_return_t testTerminate(thread_act_t port) { ++terminateCalls; assert(port == testPort); return terminateError; }
static kern_return_t testDeallocate(ipc_space_t task, mach_port_name_t port) {
    ++deallocateCalls;
    assert(task == mach_task_self() && port == testPort);
    return KERN_SUCCESS;
}
static void testExit(int status) __attribute__((noreturn));
static void testExit(int status) { exitStatus = status; longjmp(exitJump, 1); }
uintptr_t LSBTestSignThreadPC(uintptr_t start, uintptr_t modifier) {
    ++signCalls;
    observedSignedPC = start;
    observedModifier = modifier;
    return start | UINT64_C(0x4321000000000000);
}

#define LSB_THREAD_BOOTSTRAP_TESTING
#define __bsdthread_register testRegister
#define __bsdthread_create testCreate
#define thread_get_state testGetState
#define thread_set_state testSetState
#define thread_resume testResume
#define thread_terminate testTerminate
#define mach_port_deallocate testDeallocate
#define _exit testExit
#define getenv testGetenv
#include "../Guest/libLiveSimSpawn/ThreadBootstrap.c"
#undef __bsdthread_register
#undef __bsdthread_create
#undef thread_get_state
#undef thread_set_state
#undef thread_resume
#undef thread_terminate
#undef mach_port_deallocate
#undef _exit
#undef getenv

static void reset(void) {
    nativeSupportMode = NULL;
    environmentReads = 0;
    registerCalls = createCalls = getCalls = setCalls = resumeCalls = terminateCalls = deallocateCalls = signCalls = 0;
    observedFlags = corruptState = stateFlags = 0;
    registerResult = -1;
    registerError = EINVAL;
    createError = exitStatus = 0;
    getError = setError = resumeError = terminateError = KERN_SUCCESS;
    invalidPort = false;
    atomic_store(&registration, RegistrationMissing);
    observedWorkqueueRegistration = 0;
    memset(pthreadStorage, 0, sizeof(pthreadStorage));
    memset(&initialState, 0, sizeof(initialState));
    memset(&updatedState, 0, sizeof(updatedState));
    stateFlags = THREAD_STATE_NO_PTRAUTH;
}

static void registerSimulator(void) {
    uint32_t data[15] = {60, 0, 160, 0, 0, 0, 224, 40, 24, 0, 0, 0, 0, 0, 0};
    assert(LSBThreadRegister(0x12b269a80, 0x12b269a74, 16384, data, sizeof(data), 160) == registerResult);
    assert(registerCalls == 1 && observedStart == 0x12b269a80 && observedWorkqueue == 0x12b269a74 && observedPthreadSize == 16384);
    assert(observedData == data && observedSize == sizeof(data) && observedDispatch == 160);
}

static void *start(void *argument) { return argument; }
static void *create(unsigned extraFlags) {
    return LSBThreadCreate(start, (void *)0xfeed0010, pthreadStorage, pthreadStorage, 0x090008ff | extraFlags);
}

static void testNativeSupport(void) {
    static const char *invalidModes[] = {NULL, "0", "01", "true", ""};
    for (unsigned i = 0; i != sizeof(invalidModes) / sizeof(invalidModes[0]); ++i) {
        reset();
        nativeSupportMode = invalidModes[i];
        initializeThreadFallback();
        assert(create(0) == (void *)-1 && errno == ENOTSUP && !createCalls && environmentReads == 1);
        nativeSupportMode = "1";
        assert(create(0) == (void *)-1 && errno == ENOTSUP && !createCalls && environmentReads == 1);
    }
    for (unsigned suspended = 0; suspended != 2; ++suspended) {
        reset();
        nativeSupportMode = "1";
        unsigned flags = suspended ? THREAD_START_SUSPENDED : 0;
        errno = EDOM;
        assert(LSBThreadCreate(start, NULL, pthreadStorage, pthreadStorage, flags) == pthreadStorage && errno == EDOM);
        assert(createCalls == 1 && observedFlags == flags && !registerCalls && !getCalls && !setCalls && !resumeCalls && !signCalls);
        assert(atomic_load(&registration) == RegistrationMissing && !observedWorkqueueRegistration);
        initializeThreadFallback(); // The earlier pre-constructor create did not consume the final launch policy.
        nativeSupportMode = NULL;
        assert(create(flags) == pthreadStorage && createCalls == 2 && environmentReads == 2);
        registerSimulator();
        assert(create(0) == pthreadStorage && getCalls == 1 && resumeCalls == 1 && environmentReads == 2);
    }
    reset();
    nativeSupportMode = "1";
    createError = EAGAIN;
    assert(create(0) == (void *)-1 && errno == EAGAIN && createCalls == 1 && !getCalls && !terminateCalls);

    reset();
    registerSimulator();
    nativeSupportMode = "1";
    initializeThreadFallback();
    assert(create(0) == pthreadStorage && getCalls == 1 && setCalls == 1 && resumeCalls == 1 && !environmentReads);
    // The environment fallback cannot overwrite an already selected redirect registration.
}

static void testRegistration(void) {
    reset();
    assert(create(0) == (void *)-1 && errno == ENOTSUP && !createCalls);
    registerSimulator();
    assert(errno == EINVAL && atomic_load(&registration) == RegistrationRedirect);
    assert(observedWorkqueueRegistration == 0x12b269a74);

    reset();
    registerResult = 0x7f;
    errno = EDOM;
    registerSimulator();
    assert(errno == EDOM && atomic_load(&registration) == RegistrationNative);
    assert(observedWorkqueueRegistration == 0);
    assert(create(0) == pthreadStorage && observedFlags == 0x090008ff && !getCalls && !setCalls && !resumeCalls);

    reset();
    registerError = EPERM;
    registerSimulator();
    assert(errno == EPERM && create(0) == (void *)-1 && errno == ENOTSUP && !createCalls);

    uint32_t data[15] = {60, 0, 160, 0, 0, 0, 224, 40, 24};
    for (unsigned field = 0; field < 4; ++field) {
        reset();
        uint32_t bad[15];
        memcpy(bad, data, sizeof(bad));
        if (field == 0) bad[0] = 36;
        if (field == 1) bad[6] = 240;
        if (field == 2) bad[8] = 32;
        assert(LSBThreadRegister(0x12b269a80, 0x12b269a74, field == 3 ? 8192 : 16384, bad, sizeof(bad), 160) == -1);
        assert(errno == EINVAL && registerCalls == 1 && create(0) == (void *)-1 && errno == ENOTSUP && !createCalls);
    }
}

static void testCreateAndPreservation(void) {
    for (unsigned suspended = 0; suspended < 2; ++suspended) {
        reset();
        registerSimulator();
        errno = EDOM;
        assert(create(suspended ? THREAD_START_SUSPENDED : 0) == pthreadStorage && errno == EDOM);
        assert(observedFlags == 0x290008ff && getCalls == 1 && setCalls == 1 && resumeCalls == !suspended && !terminateCalls && !signCalls);
        arm_thread_state64_t expected = initialState;
        expected.__pc = 0x12b269a80;
        assert(!memcmp(&expected, &updatedState, sizeof(expected)));
    }
    for (unsigned diversifier = 0; diversifier < 2; ++diversifier) {
        reset();
        registerSimulator();
        stateFlags = 0xc | (diversifier ? 0x6a000000 : 0);
        assert(create(0) == pthreadStorage);
        assert(signCalls == 1 && observedSignedPC == 0x12b269a80);
        assert(observedModifier == (diversifier ? UINT64_C(0x748100006a000000) : 0x7481));
        arm_thread_state64_t expected = initialState;
        expected.__pc = UINT64_C(0x432100012b269a80);
        expected.__pad &= ~THREAD_STATE_KERNEL_SIGNED_PC;
        assert(!memcmp(&expected, &updatedState, sizeof(expected)));
    }
}

static void testFailureCleanup(void) {
    reset();
    registerSimulator();
    createError = EAGAIN;
    assert(create(0) == (void *)-1 && errno == EAGAIN && !getCalls && !terminateCalls);

    for (unsigned stage = 0; stage < 3; ++stage) {
        reset();
        registerSimulator();
        if (stage == 0) getError = KERN_INVALID_ARGUMENT;
        if (stage == 1) setError = KERN_PROTECTION_FAILURE;
        if (stage == 2) resumeError = KERN_RESOURCE_SHORTAGE;
        assert(create(0) == (void *)-1 && errno == (stage == 0 ? EINVAL : stage == 1 ? EPERM : EAGAIN));
        assert(getCalls == 1 && setCalls == (stage >= 1) && resumeCalls == (stage == 2) && terminateCalls == 1 && deallocateCalls == 1);
    }
    for (unsigned corruption = 1; corruption <= 6; ++corruption) {
        reset();
        registerSimulator();
        corruptState = corruption;
        assert(create(0) == (void *)-1 && errno == EPROTO && !setCalls && !resumeCalls && terminateCalls == 1 && deallocateCalls == 1);
    }
    reset();
    registerSimulator();
    assert(create(THREAD_START_TSD_BASE_SET) == (void *)-1 && errno == ENOTSUP && !createCalls);
    assert(LSBThreadCreate(start, NULL, pthreadStorage, pthreadStorage, 0) == (void *)-1 && errno == ENOTSUP && !createCalls);

    reset();
    registerSimulator();
    invalidPort = true;
    if (!setjmp(exitJump)) { (void)create(0); assert(!"Invalid port must fail closed"); }
    assert(exitStatus == 126 && !getCalls && !resumeCalls);
    reset();
    registerSimulator();
    getError = KERN_INVALID_ARGUMENT;
    terminateError = KERN_FAILURE;
    if (!setjmp(exitJump)) { (void)create(0); assert(!"Failed termination must fail closed"); }
    assert(exitStatus == 126 && terminateCalls == 1 && !deallocateCalls);
}

int main(void) {
    testRegistration();
    testNativeSupport();
    testCreateAndPreservation();
    testFailureCleanup();
    puts("Thread bootstrap tests passed");
    return 0;
}
