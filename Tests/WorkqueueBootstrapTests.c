#include <assert.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/arm/thread_status.h>
#include <mach/arm/exception.h>
#include <mach/exc.h>
#include <mach/mig_errors.h>
#include <mach/ndr.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static unsigned destroyCalls, getCalls, setCalls, legacyCalls, rpcCalls, signCalls, originalCalls, modRefCalls;
static unsigned expectedBehavior, malformedReply;
static kern_return_t forwardResult, getResult, setResult;
static uintptr_t signedStart;
static uint32_t signedFlags;
static arm_thread_state64_t kernelState;
static arm_debug_state64_t taskDebugState, threadDebugState;
static unsigned taskGetCalls, taskSetCalls, debugGetCalls, failTaskGetCall, corruptTaskGetCall;
static kern_return_t taskGetResult, taskSetResult, debugGetResult;
static uint32_t breakpointSlots;
static int slotQueryResult;
static bool shortDebugCount;
static exception_type_t expectedException = EXC_BAD_ACCESS;
static int64_t expectedCode = KERN_INVALID_ADDRESS;
static unsigned handlerSetCalls, deallocateCalls;
static exception_mask_t restoredMasks[EXC_TYPES_COUNT + 1];
static mach_port_t restoredPorts[EXC_TYPES_COUNT + 1];
static exception_behavior_t restoredBehaviors[EXC_TYPES_COUNT + 1];
static thread_state_flavor_t restoredFlavors[EXC_TYPES_COUNT + 1];
static const char *const *testEnvironment;
static bool overrideEnvironment;
static unsigned environmentReads, allocationCalls;
void LSBProcessDebuggerHandleMessage(mach_msg_header_t *message, size_t capacity) { (void)message; (void)capacity; abort(); }
static const mach_port_t workerPort = 0x170b, handlerPort = 0x220b, replyPort = 0x330b;
static mach_msg_return_t testMachMsg(mach_msg_header_t *, mach_msg_option_t, mach_msg_size_t, mach_msg_size_t,
                                    mach_port_name_t, mach_msg_timeout_t, mach_port_name_t);

static void testDestroy(mach_msg_header_t *header) {
    ++destroyCalls;
    assert(header->msgh_remote_port == MACH_PORT_NULL);
}
static mach_port_t testReplyPort(void) { return replyPort; }
static kern_return_t testModRefs(ipc_space_t task, mach_port_name_t port, mach_port_right_t right, mach_port_delta_t delta) {
    assert(task == mach_task_self() && port == replyPort && right == MACH_PORT_RIGHT_RECEIVE && delta == -1);
    ++modRefCalls;
    return KERN_SUCCESS;
}
static kern_return_t testGet(thread_act_t port, thread_state_flavor_t flavor, thread_state_t state, mach_msg_type_number_t *count) {
    if (flavor == ARM_DEBUG_STATE64) {
        assert(port == workerPort && *count == ARM_DEBUG_STATE64_COUNT);
        ++debugGetCalls;
        memcpy(state, &threadDebugState, sizeof(threadDebugState));
        *count = ARM_DEBUG_STATE64_COUNT - shortDebugCount;
        return debugGetResult;
    }
    assert(port == workerPort && flavor == ARM_THREAD_STATE64 && *count >= ARM_THREAD_STATE64_COUNT);
    ++getCalls;
    memcpy(state, &kernelState, sizeof(kernelState));
    *count = ARM_THREAD_STATE64_COUNT;
    return getResult;
}
static kern_return_t testTaskGet(task_t task, thread_state_flavor_t flavor, thread_state_t state, mach_msg_type_number_t *count) {
    assert(task == mach_task_self() && flavor == ARM_DEBUG_STATE64 && *count == ARM_DEBUG_STATE64_COUNT);
    ++taskGetCalls;
    if (failTaskGetCall == taskGetCalls) return KERN_FAILURE;
    memcpy(state, &taskDebugState, sizeof(taskDebugState));
    if (corruptTaskGetCall == taskGetCalls) ((arm_debug_state64_t *)state)->__wvr[1] ^= 16;
    return taskGetResult;
}
static kern_return_t testTaskSet(task_t task, thread_state_flavor_t flavor, thread_state_t state, mach_msg_type_number_t count) {
    assert(task == mach_task_self() && flavor == ARM_DEBUG_STATE64 && count == ARM_DEBUG_STATE64_COUNT);
    ++taskSetCalls;
    if (!taskSetResult) memcpy(&taskDebugState, state, sizeof(taskDebugState));
    return taskSetResult;
}
static int testSysctl(const char *name, void *value, size_t *size, void *replacement, size_t replacementSize) {
    assert(!strcmp(name, "hw.optional.breakpoint") && *size == sizeof(breakpointSlots) && !replacement && !replacementSize);
    memcpy(value, &breakpointSlots, sizeof(breakpointSlots));
    return slotQueryResult;
}
static kern_return_t testSetException(task_t task, exception_mask_t mask, mach_port_t port,
                                      exception_behavior_t behavior, thread_state_flavor_t flavor) {
    assert(task == mach_task_self() && handlerSetCalls < EXC_TYPES_COUNT + 1);
    restoredMasks[handlerSetCalls] = mask;
    restoredPorts[handlerSetCalls] = port;
    restoredBehaviors[handlerSetCalls] = behavior;
    restoredFlavors[handlerSetCalls++] = flavor;
    return KERN_SUCCESS;
}
static kern_return_t testDeallocate(task_t task, mach_port_t port) {
    assert(task == mach_task_self() && MACH_PORT_VALID(port));
    ++deallocateCalls;
    return KERN_SUCCESS;
}
static kern_return_t testSet(thread_act_t port, thread_state_flavor_t flavor, thread_state_t state, mach_msg_type_number_t count) {
    assert(port == workerPort && flavor == ARM_THREAD_STATE64 && count == ARM_THREAD_STATE64_COUNT);
    ++setCalls;
    memcpy(&kernelState, state, sizeof(kernelState));
    return setResult;
}
static kern_return_t testRaise(mach_port_t port, mach_port_t thread, mach_port_t task, exception_type_t exception,
                              exception_data_t code, mach_msg_type_number_t count) {
    assert(port == handlerPort && thread == workerPort && task == mach_task_self() && exception == expectedException);
    assert(count == 2 && code[0] == expectedCode && (uint32_t)code[1] == 0x21218a74);
    ++legacyCalls;
    kernelState.__pc = 0x100040000;
    return forwardResult;
}
static kern_return_t testRaiseState(mach_port_t port, exception_type_t exception, const exception_data_t code,
                                   mach_msg_type_number_t count, int *flavor, const thread_state_t oldState,
                                   mach_msg_type_number_t oldCount, thread_state_t newState, mach_msg_type_number_t *newCount) {
    assert(port == handlerPort && exception == expectedException && count == 2 && code[0] == expectedCode);
    assert(*flavor == ARM_THREAD_STATE64 && oldCount == ARM_THREAD_STATE64_COUNT && *newCount >= oldCount);
    ++legacyCalls;
    memcpy(newState, oldState, sizeof(kernelState));
    ((arm_thread_state64_t *)newState)->__pc = 0x100050000;
    *newCount = oldCount;
    return forwardResult;
}
static kern_return_t testRaiseIdentity(mach_port_t port, mach_port_t thread, mach_port_t task, exception_type_t exception,
                                      exception_data_t code, mach_msg_type_number_t count, int *flavor, thread_state_t oldState,
                                      mach_msg_type_number_t oldCount, thread_state_t newState, mach_msg_type_number_t *newCount) {
    assert(thread == workerPort && task == mach_task_self());
    return testRaiseState(port, exception, code, count, flavor, oldState, oldCount, newState, newCount);
}
static int testSupported(void) { ++originalCalls; return 0x37; }
static int testOpen(void) { ++originalCalls; return 23; }
static char *testGetenv(const char *name) {
    ++environmentReads;
    if (!overrideEnvironment) return getenv(name);
    size_t length = strlen(name);
    for (const char *const *entry = testEnvironment; entry && *entry; ++entry)
        if (!strncmp(*entry, name, length) && (*entry)[length] == '=') return (char *)*entry + length + 1;
    return NULL;
}
static kern_return_t testAllocate(ipc_space_t task, mach_port_right_t right, mach_port_name_t *port) {
    assert(task == mach_task_self() && right == MACH_PORT_RIGHT_RECEIVE && port);
    ++allocationCalls;
    return KERN_RESOURCE_SHORTAGE; // Stop initialization before any real exception-port or thread mutation.
}
uintptr_t LSBThreadSignPC(uintptr_t start, uint32_t flags) {
    ++signCalls;
    signedStart = start;
    signedFlags = flags;
    return start | (flags & 1 ? 0 : UINT64_C(0x4321000000000000));
}

#define LSB_WORKQUEUE_BOOTSTRAP_TESTING
#define mach_msg testMachMsg
#define mach_msg_destroy testDestroy
#define mach_reply_port testReplyPort
#define mach_port_mod_refs testModRefs
#define thread_get_state testGet
#define thread_set_state testSet
#define task_get_state testTaskGet
#define task_set_state testTaskSet
#define sysctlbyname testSysctl
#define task_set_exception_ports testSetException
#define mach_port_deallocate testDeallocate
#define exception_raise testRaise
#define exception_raise_state testRaiseState
#define exception_raise_state_identity testRaiseIdentity
#define _pthread_workqueue_supported testSupported
#define __workq_open testOpen
#define getenv testGetenv
#define mach_port_allocate testAllocate
#include "../Guest/WorkqueueBootstrap.c"
#undef mach_msg
#undef mach_msg_destroy
#undef mach_reply_port
#undef mach_port_mod_refs
#undef thread_get_state
#undef thread_set_state
#undef task_get_state
#undef task_set_state
#undef sysctlbyname
#undef task_set_exception_ports
#undef mach_port_deallocate
#undef exception_raise
#undef exception_raise_state
#undef exception_raise_state_identity
#undef _pthread_workqueue_supported
#undef __workq_open
#undef getenv
#undef mach_port_allocate

static void reset(void) {
    destroyCalls = getCalls = setCalls = legacyCalls = rpcCalls = signCalls = originalCalls = modRefCalls = 0;
    malformedReply = 0;
    forwardResult = getResult = setResult = KERN_SUCCESS;
    memset(&kernelState, 0, sizeof(kernelState));
    memset(savedPorts, 0, sizeof(savedPorts));
    savedCount = 0;
    taskGetCalls = taskSetCalls = debugGetCalls = failTaskGetCall = corruptTaskGetCall = 0;
    taskGetResult = taskSetResult = debugGetResult = KERN_SUCCESS;
    breakpointSlots = 6;
    slotQueryResult = 0;
    shortDebugCount = false;
    hardwareMode = breakpointNeedsRestore = false;
    hardwareSlot = -1;
    hardwareTrapCount = hardwareRejectedCount = 0;
    handlerSetCalls = deallocateCalls = 0;
    bridgeMask = EXC_MASK_BAD_ACCESS;
    expectedException = EXC_BAD_ACCESS;
    expectedCode = KERN_INVALID_ADDRESS;
    memset(&taskDebugState, 0, sizeof(taskDebugState));
    memset(&threadDebugState, 0, sizeof(threadDebugState));
    nativeEntry = 0x2321218a74;
    guestEntry = 0x12b269a74;
    LSBWorkqueueRegister(guestEntry);
}

static Message requestMessage(void) {
    Message message = {0};
    IdentityPrefix prefix = {0};
    prefix.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
    prefix.header.msgh_remote_port = replyPort;
    prefix.header.msgh_id = StateIdentityID;
    prefix.body.msgh_descriptor_count = 2;
    prefix.thread = (mach_msg_port_descriptor_t){.name = workerPort, .disposition = MACH_MSG_TYPE_PORT_SEND, .type = MACH_MSG_PORT_DESCRIPTOR};
    prefix.task = (mach_msg_port_descriptor_t){.name = mach_task_self(), .disposition = MACH_MSG_TYPE_PORT_SEND, .type = MACH_MSG_PORT_DESCRIPTOR};
    prefix.ndr = NDR_record;
    prefix.exception = EXC_BAD_ACCESS;
    prefix.codeCount = 2;
    int64_t code[] = {KERN_INVALID_ADDRESS, (int64_t)nativeEntry};
    int flavor = ARM_THREAD_STATE64;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    arm_thread_state64_t state;
    memset(&state, 0x3c, sizeof(state));
    state.__pc = nativeEntry;
    state.__pad = StateNoPtrauth;
    state.__x[0] = 0x104008000;
    state.__x[1] = workerPort;
    state.__x[2] = 0x104000000;
    state.__x[3] = 0;
    state.__x[4] = WorkqueueTSDBaseSet | 0x40000;
    state.__x[5] = 0;
    state.__sp = 0x104008000;
    size_t size = 0;
    append(&message, &size, &prefix, sizeof(prefix));
    append(&message, &size, code, sizeof(code));
    append(&message, &size, &flavor, 4);
    append(&message, &size, &count, 4);
    append(&message, &size, &state, sizeof(state));
    message.header.msgh_size = (mach_msg_size_t)size;
    assert(size == 364);
    return message;
}

static void metadataTests(void) {
    reset();
    LSBWorkqueueRegister(0);
    assert(LSBWorkqueueSupported() == 0x37 && originalCalls == 1);
    assert(LSBWorkqueueOpen() == 23 && originalCalls == 2);
    LSBWorkqueueRegister(guestEntry);
    unsetenv(LSB_HOST_WORKQUEUE_START_ENV);
    unsetenv(LSB_HOST_WORKQUEUE_FEATURES_ENV);
    assert(!LSBWorkqueueSupported());
    setenv(LSB_HOST_WORKQUEUE_START_ENV, "0x2321218a74", 1);
    setenv(LSB_HOST_WORKQUEUE_FEATURES_ENV, "0xff", 1);
    errno = EDOM;
    assert(LSBWorkqueueSupported() == 0xff && errno == EDOM && originalCalls == 2);
    const char *invalid[] = {"", "-1", "0", "0x2321218a75", "0x2321218a74junk", "18446744073709551616"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        setenv(LSB_HOST_WORKQUEUE_START_ENV, invalid[i], 1);
        assert(!LSBWorkqueueSupported());
    }
    setenv(LSB_HOST_WORKQUEUE_START_ENV, "0x2321218a74", 1);
    setenv(LSB_HOST_WORKQUEUE_FEATURES_ENV, "0x80000000", 1);
    assert(!LSBWorkqueueSupported());
}

static void parseTests(void) {
    reset();
    Message original = requestMessage();
    ExceptionRequest request;
    assert(parseException(&original, &request));
    assert(request.thread == workerPort && request.task == mach_task_self() && request.code[1] == (int64_t)nativeEntry);
    for (unsigned variation = 0; variation < 11; ++variation) {
        Message message = original;
        IdentityPrefix *prefix = (IdentityPrefix *)&message;
        switch (variation) {
            case 0: message.header.msgh_id = 2403; break;
            case 1: message.header.msgh_bits &= ~MACH_MSGH_BITS_COMPLEX; break;
            case 2: --message.header.msgh_size; break;
            case 3: prefix->body.msgh_descriptor_count = 1; break;
            case 4: prefix->thread.type = MACH_MSG_OOL_DESCRIPTOR; break;
            case 5: prefix->task.disposition = MACH_MSG_TYPE_COPY_SEND; break;
            case 6: prefix->task.name = MACH_PORT_NULL; break;
            case 7: prefix->codeCount = 3; break;
            case 8: prefix->ndr.int_rep ^= 1; break;
            case 9: *(uint32_t *)(message.bytes + 84) = ARM_EXCEPTION_STATE64; break;
            case 10: *(uint32_t *)(message.bytes + 88) = 67; break;
        }
        assert(!parseException(&message, &request));
    }
}

static void redirectTests(void) {
    reset();
    Message message = requestMessage();
    ExceptionRequest original;
    assert(parseException(&message, &original));
    for (unsigned mode = 0; mode < 4; ++mode) {
        ExceptionRequest request = original;
        if (mode == 1) { request.state.__x[4] = WorkqueueReuse; request.state.__x[5] = -1; }
        if (mode == 2) { request.state.__x[4] = WorkqueueReuse; request.state.__x[5] = 16; }
        if (mode == 3) {
            request.state.__pad = 0x6a00000c;
            request.state.__pc |= UINT64_C(0x4321000000000000);
            request.state.__sp |= UINT64_C(0x4311000000000000);
        }
        arm_thread_state64_t expected = request.state;
        assert(redirectException(&request));
        expected.__pc = guestEntry | (expected.__pad & StateNoPtrauth ? 0 : UINT64_C(0x4321000000000000));
        expected.__pad &= ~StateKernelSignedPC;
        assert(!memcmp(&request.state, &expected, sizeof(expected)) && signedStart == guestEntry);
    }
    assert(signCalls == 4 && signedFlags == 0x6a00000c);
    for (unsigned variation = 0; variation < 11; ++variation) {
        ExceptionRequest request = original;
        switch (variation) {
            case 0: ++request.state.__pc; break;
            case 1: ++request.code[1]; break;
            case 2: request.code[0] = KERN_NO_ACCESS; break;
            case 3: request.codeCount = 1; break;
            case 4: request.state.__x[1] = 0; break;
            case 5: request.state.__x[0] |= 1; break;
            case 6: request.state.__x[2] = request.state.__sp + 16; break;
            case 7: request.state.__sp = request.state.__x[0] + 16; break;
            case 8: request.state.__x[4] = 0; break;
            case 9: request.state.__x[5] = 17; break;
            case 10: request.state.__x[5] = -2; break;
        }
        assert(!redirectException(&request));
    }
    StateReply reply;
    handleMessage(&message, &reply);
    assert(destroyCalls == 1 && reply.result == KERN_SUCCESS && reply.header.msgh_remote_port == replyPort);
    assert(reply.header.msgh_id == 2507 && reply.header.msgh_size == 316 && reply.stateCount == ARM_THREAD_STATE64_COUNT);
    arm_thread_state64_t replyState;
    memcpy(&replyState, reply.state, sizeof(replyState));
    assert(replyState.__pc == guestEntry);
}

static void hardwareInstallationTests(void) {
    reset();
    hardwareMode = true;
    taskDebugState.__bvr[0] = 0x100001000;
    taskDebugState.__bcr[0] = BreakpointControl;
    taskDebugState.__bvr[1] = 0x100002000; // Disabled but owned by another debugger: cannot borrow it.
    taskDebugState.__bcr[2] = 0x1e4; // A disabled configured slot is not ours either.
    taskDebugState.__bcr[3] = DebugUserMode;
    taskDebugState.__wvr[0] = 0x100003000;
    taskDebugState.__wcr[0] = 0x1ff;
    taskDebugState.__mdscr_el1 = 1;
    arm_debug_state64_t before = taskDebugState, expected = before;
    expected.__bvr[3] = nativeEntry;
    expected.__bcr[3] = BreakpointControl;
    assert(installHardwareBreakpoint() == KERN_SUCCESS);
    assert(hardwareSlot == 3 && breakpointNeedsRestore && taskGetCalls == 2 && taskSetCalls == 1);
    assert(!memcmp(&expected, &taskDebugState, sizeof(expected)));
    // Rollback restores only our slot, preserving a later change to somebody else's breakpoint/watchpoint state.
    taskDebugState.__bvr[0] += 4;
    taskDebugState.__wvr[1] = 0x100004000;
    before.__bvr[0] += 4;
    before.__wvr[1] = 0x100004000;
    assert(restoreHardwareBreakpoint() == KERN_SUCCESS && !breakpointNeedsRestore);
    assert(!memcmp(&before, &taskDebugState, sizeof(before)));
    assert(restoreHardwareBreakpoint() == KERN_SUCCESS && taskSetCalls == 2);

    for (unsigned failure = 0; failure < 8; ++failure) {
        reset();
        switch (failure) {
            case 0: slotQueryResult = -1; break;
            case 1: breakpointSlots = 0; break;
            case 2: breakpointSlots = DebugSlots + 1; break;
            case 3: taskGetResult = KERN_FAILURE; break;
            case 4:
                for (unsigned i = 0; i < breakpointSlots; ++i) taskDebugState.__bcr[i] = BreakpointControl;
                break;
            case 5: taskSetResult = KERN_PROTECTION_FAILURE; break;
            case 6: failTaskGetCall = 2; break;
            case 7: corruptTaskGetCall = 2; break;
        }
        before = taskDebugState;
        assert(installHardwareBreakpoint() != KERN_SUCCESS);
        assert(breakpointNeedsRestore == (failure >= 6));
        assert(restoreHardwareBreakpoint() == KERN_SUCCESS);
        assert(!memcmp(&before, &taskDebugState, sizeof(before)));
    }
    reset();
    assert(!installHardwareBreakpoint());
    taskDebugState.__bvr[hardwareSlot] += 4; // A later debugger took this slot: refuse to overwrite it.
    before = taskDebugState;
    assert(restoreHardwareBreakpoint() == KERN_FAILURE);
    assert(!memcmp(&before, &taskDebugState, sizeof(before)));
}

static void hardwareRedirectTests(void) {
    reset();
    hardwareMode = true;
    assert(!installHardwareBreakpoint());
    threadDebugState = taskDebugState;
    arm_debug_state64_t before = taskDebugState;
    Message message = requestMessage();
    ((IdentityPrefix *)&message)->exception = EXC_BREAKPOINT;
    int64_t code = EXC_ARM_BREAKPOINT;
    memcpy(message.bytes + sizeof(IdentityPrefix), &code, sizeof(code));
    ExceptionRequest original;
    assert(parseException(&message, &original) && original.exception == EXC_BREAKPOINT);
    for (unsigned iteration = 0; iteration < 3; ++iteration) {
        ExceptionRequest request = original;
        if (iteration == 1) { request.state.__x[4] = WorkqueueReuse; request.state.__x[5] = -1; }
        if (iteration == 2) { request.codeCount = 1; request.code[1] = 0; } // The state PC, not code[1], identifies the entry.
        arm_thread_state64_t expected = request.state;
        expected.__pc = guestEntry;
        assert(redirectException(&request) && !memcmp(&expected, &request.state, sizeof(expected)));
    }
    assert(hardwareTrapCount == 3 && taskSetCalls == 1 && setCalls == 0 && debugGetCalls == 3);
    assert(!memcmp(&before, &taskDebugState, sizeof(before)) && !memcmp(&before, &threadDebugState, sizeof(before)));
    for (unsigned failure = 0; failure < 10; ++failure) {
        ExceptionRequest request = original;
        hardwareMode = breakpointNeedsRestore = true;
        threadDebugState = before;
        debugGetResult = KERN_SUCCESS;
        shortDebugCount = false;
        switch (failure) {
            case 0: hardwareMode = false; break;
            case 1: breakpointNeedsRestore = false; break;
            case 2: request.code[0] = EXC_ARM_BREAKPOINT + 1; break;
            case 3: request.codeCount = 0; break;
            case 4: request.state.__pc += 4; break;
            case 5: request.state.__x[1] += 1; break;
            case 6: threadDebugState.__bcr[hardwareSlot] &= ~UINT64_C(1); break;
            case 7: debugGetResult = KERN_FAILURE; break;
            case 8: shortDebugCount = true; break;
            case 9: threadDebugState.__bvr[hardwareSlot] += 4; break;
        }
        arm_thread_state64_t unchanged = request.state;
        assert(!redirectException(&request) && !memcmp(&unchanged, &request.state, sizeof(unchanged)));
    }
    hardwareMode = breakpointNeedsRestore = true;
    debugGetResult = KERN_SUCCESS;
    shortDebugCount = false;
    threadDebugState = before;
    StateReply reply;
    handleMessage(&message, &reply);
    assert(reply.result == KERN_SUCCESS && destroyCalls == 1 && hardwareTrapCount == 4);
}

static void handlerRollbackTests(void) {
    reset();
    bridgeMask = EXC_MASK_BAD_ACCESS | EXC_MASK_BREAKPOINT;
    savedCount = 2;
    savedMasks[0] = EXC_MASK_BAD_ACCESS;
    savedMasks[1] = EXC_MASK_BREAKPOINT;
    savedPorts[0] = handlerPort;
    savedPorts[1] = handlerPort + 1;
    savedBehaviors[0] = EXCEPTION_DEFAULT;
    savedBehaviors[1] = EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES;
    savedFlavors[0] = THREAD_STATE_NONE;
    savedFlavors[1] = ARM_THREAD_STATE64;
    restoreHandlers();
    assert(savedCount == 0 && handlerSetCalls == 3 && deallocateCalls == 2);
    assert(restoredMasks[0] == bridgeMask && restoredPorts[0] == MACH_PORT_NULL);
    assert(restoredBehaviors[0] == EXCEPTION_DEFAULT && restoredFlavors[0] == THREAD_STATE_NONE);
    for (unsigned i = 0; i < 2; ++i) {
        assert(restoredMasks[i + 1] == savedMasks[i] && restoredPorts[i + 1] == savedPorts[i]);
        assert(restoredBehaviors[i + 1] == savedBehaviors[i] && restoredFlavors[i + 1] == savedFlavors[i]);
    }
}

static void initializationPolicyTests(void) {
    static const struct { const char *first, *second; bool enabled; } cases[] = {
        {NULL, NULL, true}, {"0", NULL, false}, {"1", NULL, true}, {"", NULL, false}, {"01", NULL, false},
        {"true", NULL, false}, {"1=extra", NULL, false}, {"1", "0", true}, {"0", "1", false}, {"", "1", false}
    };
    reset();
    overrideEnvironment = true;
    testEnvironment = NULL;
    environmentReads = allocationCalls = 0;
    LSBWorkqueueRegister(0);
    assert(LSBWorkqueueOpen() == 23 && LSBWorkqueueSupported() == 0x37);
    assert(!environmentReads && !allocationCalls); // Native/pre-registration calls must not read guest environment or TLS.
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        reset();
        const char *entries[7] = {LSB_HOST_WORKQUEUE_START_ENV "=0x2321218a74", LSB_HOST_WORKQUEUE_FEATURES_ENV "=0xff",
                                 LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV "_EXTRA=0", LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV};
        char first[96], second[96];
        unsigned count = 4;
        if (cases[i].first) {
            snprintf(first, sizeof(first), LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV "=%s", cases[i].first);
            entries[count++] = first;
        }
        if (cases[i].second) {
            snprintf(second, sizeof(second), LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV "=%s", cases[i].second);
            entries[count++] = second;
        }
        entries[count] = NULL;
        testEnvironment = entries;
        environmentReads = allocationCalls = 0;
        LSBWorkqueueRegister(0x140001000);
        initializeBridge();
        assert(hardwareMode == cases[i].enabled);
        assert(bridgeMask == (EXC_MASK_BAD_ACCESS | (cases[i].enabled ? EXC_MASK_BREAKPOINT : 0)));
        assert(environmentReads == 3 && allocationCalls == 1 && bridgeError == ENOTSUP);
        assert(!taskGetCalls && !taskSetCalls); // Default selection precedes allocation but does not arm through a failed setup.
    }
    overrideEnvironment = false;
    testEnvironment = NULL;
    LSBWorkqueueRegister(0);
}

static mach_msg_return_t testMachMsg(mach_msg_header_t *header, mach_msg_option_t options, mach_msg_size_t sendSize,
                                    mach_msg_size_t receiveSize, mach_port_name_t receivePort, mach_msg_timeout_t timeout, mach_port_name_t notify) {
    assert(options == (MACH_SEND_MSG | MACH_RCV_MSG) && receiveSize == MessageCapacity && receivePort == replyPort);
    assert(timeout == MACH_MSG_TIMEOUT_NONE && notify == MACH_PORT_NULL && header->msgh_remote_port == handlerPort);
    assert(header->msgh_local_port == replyPort && header->msgh_id == 2404 + (int)expectedBehavior && header->msgh_size == sendSize);
    ++rpcCalls;
    bool identity = expectedBehavior != EXCEPTION_STATE, hasState = expectedBehavior != EXCEPTION_DEFAULT;
    assert(!!(header->msgh_bits & MACH_MSGH_BITS_COMPLEX) == identity);
    size_t offset = sizeof(mach_msg_header_t);
    Message *message = (Message *)header;
    if (identity) {
        IdentityPrefix *prefix = (IdentityPrefix *)message;
        assert(prefix->body.msgh_descriptor_count == 2);
        assert(prefix->thread.name == workerPort && prefix->thread.disposition == MACH_MSG_TYPE_COPY_SEND);
        assert(prefix->task.name == mach_task_self() && prefix->task.disposition == MACH_MSG_TYPE_COPY_SEND);
        offset += sizeof(mach_msg_body_t) + 2 * sizeof(mach_msg_port_descriptor_t);
    }
    assert(!memcmp(message->bytes + offset, &NDR_record, sizeof(NDR_record)));
    offset += sizeof(NDR_record);
    assert(*(int32_t *)(message->bytes + offset) == expectedException && *(uint32_t *)(message->bytes + offset + 4) == 2);
    int64_t firstCode;
    memcpy(&firstCode, message->bytes + offset + 8, 8);
    assert(firstCode == expectedCode);
    int64_t address;
    memcpy(&address, message->bytes + offset + 16, 8);
    assert(address == (int64_t)nativeEntry);
    offset += 24;
    assert(sendSize == offset + (hasState ? 8 + sizeof(kernelState) : 0));
    memset(message, 0, sizeof(*message));
    StateReply *reply = (StateReply *)message;
    reply->header.msgh_id = 2504 + (int)expectedBehavior;
    reply->header.msgh_size = sizeof(mig_reply_error_t);
    reply->ndr = NDR_record;
    reply->result = forwardResult;
    if (hasState && !forwardResult) {
        reply->flavor = ARM_THREAD_STATE64;
        reply->stateCount = ARM_THREAD_STATE64_COUNT;
        arm_thread_state64_t replyState = kernelState;
        replyState.__pc = 0x100060000;
        memcpy(reply->state, &replyState, sizeof(replyState));
        reply->header.msgh_size = offsetof(StateReply, state) + sizeof(kernelState);
    } else if (!forwardResult) kernelState.__pc = 0x100070000;
    if (malformedReply == 1) ++reply->header.msgh_id;
    if (malformedReply == 2) --reply->header.msgh_size;
    if (malformedReply == 3) reply->stateCount = THREAD_STATE_MAX + 1;
    return KERN_SUCCESS;
}

static void forwardingTests(void) {
    for (unsigned breakpoint = 0; breakpoint < 2; ++breakpoint)
    for (unsigned wide = 0; wide < 2; ++wide) for (unsigned behavior = 1; behavior <= 3; ++behavior) {
        reset();
        Message message = requestMessage();
        ExceptionRequest request;
        assert(parseException(&message, &request));
        expectedException = request.exception = breakpoint ? EXC_BREAKPOINT : EXC_BAD_ACCESS;
        expectedCode = request.code[0] = breakpoint ? EXC_ARM_BREAKPOINT : KERN_INVALID_ADDRESS;
        kernelState = request.state;
        savedCount = 1;
        savedMasks[0] = 1u << expectedException;
        savedPorts[0] = handlerPort;
        savedBehaviors[0] = behavior | (wide ? MACH_EXCEPTION_CODES : 0);
        savedFlavors[0] = ARM_THREAD_STATE64;
        expectedBehavior = behavior;
        assert(forwardException(&request) == KERN_SUCCESS);
        uintptr_t expectedPC = wide ? (behavior == EXCEPTION_DEFAULT ? 0x100070000 : 0x100060000) :
                                      (behavior == EXCEPTION_DEFAULT ? 0x100040000 : 0x100050000);
        assert(request.state.__pc == expectedPC && getCalls == (behavior == EXCEPTION_DEFAULT ? 1u : 2u));
        assert(setCalls == (behavior != EXCEPTION_DEFAULT) && rpcCalls == wide && legacyCalls == !wide && modRefCalls == wide);
        if (wide) for (unsigned malformed = 1; malformed <= (behavior == EXCEPTION_DEFAULT ? 2u : 3u); ++malformed) {
            malformedReply = malformed;
            assert(forwardException(&request) != KERN_SUCCESS);
        }
        malformedReply = 0;
        forwardResult = KERN_FAILURE;
        assert(forwardException(&request) == KERN_FAILURE);
    }
    reset();
    Message message = requestMessage();
    ExceptionRequest request;
    assert(parseException(&message, &request));
    assert(forwardException(&request) == KERN_FAILURE);
    assert(!supportedBehavior(EXCEPTION_IDENTITY_PROTECTED, ARM_THREAD_STATE64));
    assert(!supportedBehavior(EXCEPTION_DEFAULT | MACH_EXCEPTION_BACKTRACE_PREFERRED, ARM_THREAD_STATE64));
    assert(!supportedBehavior(EXCEPTION_STATE, THREAD_STATE_NONE));
}

static void processControlPortSetTests(void) {
    mach_port_t port;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port));
    assert(!mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND));
    LSBWorkqueueRegister(0);
    assert(LSBWorkqueueAddProcessControlPort(port) == ENOTSUP); // A native caller must never initialize a guest worker.
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &exceptionPortSet));
    atomic_store(&exceptionServerReady, true);
    assert(LSBWorkqueueAddProcessControlPort(MACH_PORT_NULL) == EINVAL);
    assert(LSBWorkqueueAddProcessControlPort(mach_task_self()) == EINVAL); // A send-only task capability is not a receive endpoint.
    assert(!LSBWorkqueueAddProcessControlPort(port));
    mach_msg_header_t message = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0), .msgh_size = sizeof(message),
                                .msgh_remote_port = port, .msgh_id = 42};
    assert(!mach_msg(&message, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(message), 0, MACH_PORT_NULL, 10, MACH_PORT_NULL));
    union { mach_msg_header_t header; unsigned char bytes[256]; } received;
    assert(!mach_msg(&received.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(received), exceptionPortSet, 10, MACH_PORT_NULL));
    assert(received.header.msgh_local_port == port && received.header.msgh_id == 42);
    mach_msg_destroy(&received.header);
    assert(!mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, -1));
    assert(!mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(!mach_port_mod_refs(mach_task_self(), exceptionPortSet, MACH_PORT_RIGHT_PORT_SET, -1));
    exceptionPortSet = MACH_PORT_NULL;
    atomic_store(&exceptionServerReady, false);
}

int main(void) {
    metadataTests();
    parseTests();
    redirectTests();
    hardwareInstallationTests();
    hardwareRedirectTests();
    handlerRollbackTests();
    initializationPolicyTests();
    forwardingTests();
    processControlPortSetTests();
    puts("WorkqueueBootstrap tests passed");
    return 0;
}
