#include "WorkqueueBootstrap.h"
#include "ThreadBootstrap.h"
#include "ProcessDebugger.h"

#include <errno.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/arm/thread_status.h>
#include <mach/arm/exception.h>
#include <mach/exc.h>
#include <mach/mig_errors.h>
#include <mach/ndr.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern int _pthread_workqueue_supported(void);
extern int __workq_open(void);
extern mach_port_t mach_reply_port(void);

enum { StateIdentityID = 2407, MaximumCodes = 2, MessageCapacity = 8192 };
enum { StateNoPtrauth = 1, StateKernelSignedPC = 4, WorkqueueReuse = 0x20000, WorkqueueTSDBaseSet = 0x200000 };
enum { DebugSlots = 16, BreakpointControl = 0x1e5, DebugUserMode = 4 };
static _Atomic(uintptr_t) simulatorEntry;
static uintptr_t nativeEntry, guestEntry;
static pthread_once_t bridgeOnce = PTHREAD_ONCE_INIT;
static int bridgeError;
static mach_port_t exceptionPort;
static mach_port_t exceptionPortSet;
static _Atomic(bool) exceptionServerReady;
static mach_msg_type_number_t savedCount;
static exception_mask_t savedMasks[EXC_TYPES_COUNT];
static mach_port_t savedPorts[EXC_TYPES_COUNT];
static exception_behavior_t savedBehaviors[EXC_TYPES_COUNT];
static thread_state_flavor_t savedFlavors[EXC_TYPES_COUNT];
static exception_mask_t bridgeMask = EXC_MASK_BAD_ACCESS;
static bool hardwareMode, breakpointNeedsRestore;
static int hardwareSlot = -1;
static arm_debug_state64_t originalDebugState;
static unsigned hardwareTrapCount, hardwareRejectedCount;

// Keep startup diagnostics independent of stdio/dispatch. The explicitly selected legacy fault mode stays silent.
static char *traceNumber(char *output, uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    char reversed[16];
    unsigned count = 0;
    do { reversed[count++] = digits[value & 15]; value >>= 4; } while (value);
    *output++ = '0'; *output++ = 'x';
    while (count) *output++ = reversed[--count];
    return output;
}

static void hardwareTrace(const char *event, kern_return_t result, uint64_t detail) {
    if (!hardwareMode) return;
    int saved = errno;
    char line[256], *cursor = line;
    const char *prefix = "[LSB workqueue hardware] ";
    while (*prefix) *cursor++ = *prefix++;
    while (*event) *cursor++ = *event++;
    const char *names[] = {" result=", " detail=", " slot=", " native=", " guest=", " handled="};
    uint64_t values[] = {(unsigned)result, detail, (unsigned)hardwareSlot, nativeEntry, guestEntry, hardwareTrapCount};
    for (unsigned i = 0; i < sizeof(values) / sizeof(*values); ++i) {
        const char *name = names[i];
        while (*name) *cursor++ = *name++;
        cursor = traceNumber(cursor, values[i]);
    }
    *cursor++ = '\n';
    (void)write(STDERR_FILENO, line, (size_t)(cursor - line));
    errno = saved;
}

static bool sameDebugState(const arm_debug_state64_t *left, const arm_debug_state64_t *right) {
    // Task-default state is copied verbatim; XNU sanitizes controls only when inheriting them into a new thread.
    return !memcmp(left, right, sizeof(*left));
}

static kern_return_t getTaskDebugState(arm_debug_state64_t *state) {
    mach_msg_type_number_t count = ARM_DEBUG_STATE64_COUNT;
    kern_return_t result = task_get_state(mach_task_self(), ARM_DEBUG_STATE64, (thread_state_t)state, &count);
    return !result && count != ARM_DEBUG_STATE64_COUNT ? KERN_INVALID_ARGUMENT : result;
}

static bool ownsBreakpoint(const arm_debug_state64_t *state) {
    return hardwareSlot >= 0 && hardwareSlot < DebugSlots && state->__bvr[hardwareSlot] == nativeEntry &&
           state->__bcr[hardwareSlot] == BreakpointControl;
}

static kern_return_t installHardwareBreakpoint(void) {
    uint32_t slots = 0;
    size_t size = sizeof(slots);
    if (sysctlbyname("hw.optional.breakpoint", &slots, &size, NULL, 0) || size != sizeof(slots) || !slots || slots > DebugSlots) {
        hardwareTrace("slot-query-failed", KERN_NOT_SUPPORTED, slots);
        return KERN_NOT_SUPPORTED;
    }
    kern_return_t result = getTaskDebugState(&originalDebugState);
    hardwareTrace("task-get", result, slots);
    if (result) return result;
    hardwareSlot = -1;
    for (unsigned i = 0; i < slots; ++i) {
        // Do not reuse somebody else's disabled address/configuration, nor change stepping/watchpoints.
        if (!originalDebugState.__bvr[i] && !(originalDebugState.__bcr[i] & ~(uint64_t)DebugUserMode)) {
            hardwareSlot = (int)i;
            break;
        }
    }
    if (hardwareSlot < 0) { hardwareTrace("no-unused-slot", KERN_RESOURCE_SHORTAGE, slots); return KERN_RESOURCE_SHORTAGE; }
    arm_debug_state64_t desired = originalDebugState;
    desired.__bvr[hardwareSlot] = nativeEntry;
    desired.__bcr[hardwareSlot] = BreakpointControl; // BAS=1111, user mode, enabled, unlinked instruction-address match.
    hardwareTrace("task-set-begin", KERN_SUCCESS, BreakpointControl);
    result = task_set_state(mach_task_self(), ARM_DEBUG_STATE64, (thread_state_t)&desired, ARM_DEBUG_STATE64_COUNT);
    hardwareTrace("task-set", result, BreakpointControl);
    if (result) return result;
    breakpointNeedsRestore = true;
    arm_debug_state64_t actual;
    result = getTaskDebugState(&actual);
    if (!result && (!ownsBreakpoint(&actual) || !sameDebugState(&desired, &actual))) result = KERN_FAILURE;
    hardwareTrace("task-readback", result, BreakpointControl);
    return result;
}

static kern_return_t restoreHardwareBreakpoint(void) {
    if (!breakpointNeedsRestore) return KERN_SUCCESS;
    arm_debug_state64_t current;
    kern_return_t result = getTaskDebugState(&current);
    // Preserve changes to other slots, and never overwrite a slot that a later debugger has taken over.
    if (!result && !ownsBreakpoint(&current)) result = KERN_FAILURE;
    if (!result) {
        current.__bvr[hardwareSlot] = originalDebugState.__bvr[hardwareSlot];
        current.__bcr[hardwareSlot] = originalDebugState.__bcr[hardwareSlot];
        result = task_set_state(mach_task_self(), ARM_DEBUG_STATE64, (thread_state_t)&current, ARM_DEBUG_STATE64_COUNT);
        if (!result) {
            arm_debug_state64_t actual;
            result = getTaskDebugState(&actual);
            if (!result && !sameDebugState(&current, &actual)) result = KERN_FAILURE;
        }
    }
    hardwareTrace("rollback", result, 0);
    if (!result) breakpointNeedsRestore = false;
    return result;
}

// The SDK only exports legacy exc RPCs. These are the public mach_exc.defs wire layouts, with MIG's four-byte packing.
#pragma pack(push, 4)
typedef struct {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t thread, task;
    NDR_record_t ndr;
    exception_type_t exception;
    mach_msg_type_number_t codeCount;
} IdentityPrefix;
typedef struct {
    mach_msg_header_t header;
    NDR_record_t ndr;
    kern_return_t result;
    int flavor;
    mach_msg_type_number_t stateCount;
    natural_t state[THREAD_STATE_MAX];
} StateReply;
#pragma pack(pop)
typedef union { mach_msg_header_t header; uint64_t alignment; unsigned char bytes[MessageCapacity]; } Message;
_Static_assert(sizeof(IdentityPrefix) + MaximumCodes * sizeof(int64_t) + 2 * sizeof(uint32_t) +
               THREAD_STATE_MAX * sizeof(natural_t) + MAX_TRAILER_SIZE <= sizeof(Message), "exception message and audit trailer capacity");
typedef struct {
    mach_port_t thread, task;
    exception_type_t exception;
    int64_t code[MaximumCodes];
    mach_msg_type_number_t codeCount;
    arm_thread_state64_t state;
} ExceptionRequest;
_Static_assert(sizeof(IdentityPrefix) == 68 && offsetof(StateReply, state) == 44, "Unexpected Mach exception wire ABI");
_Static_assert(sizeof(StateReply) < MessageCapacity, "Mach reply buffer too small");

void LSBWorkqueueRegister(uintptr_t start) {
    atomic_store_explicit(&simulatorEntry, start && !(start & 3) ? start : 0, memory_order_release);
}

static bool parseMetadata(uintptr_t *entry, int *features) {
    const char *start = getenv(LSB_HOST_WORKQUEUE_START_ENV), *mask = getenv(LSB_HOST_WORKQUEUE_FEATURES_ENV);
    if (!start || !mask || !*start || !*mask || *start == '-' || *mask == '-') return false;
    char *end;
    int saved = errno;
    errno = 0;
    unsigned long long address = strtoull(start, &end, 0);
    bool valid = !errno && !*end && address && address <= UINTPTR_MAX && !(address & 3);
    errno = 0;
    unsigned long long value = strtoull(mask, &end, 0);
    valid = valid && !errno && !*end && value && value <= INT_MAX;
    errno = saved;
    if (valid) { *entry = (uintptr_t)address; *features = (int)value; }
    return valid;
}

int LSBWorkqueueSupported(void) {
    if (!atomic_load_explicit(&simulatorEntry, memory_order_acquire)) return _pthread_workqueue_supported();
    uintptr_t entry;
    int features;
    // A zero mask makes libdispatch reject initialization; never invent kernel capabilities when native capture was unavailable.
    return parseMetadata(&entry, &features) ? features : 0;
}

static bool parseException(const Message *message, ExceptionRequest *request) {
    const IdentityPrefix *prefix = (const IdentityPrefix *)message;
    size_t size = message->header.msgh_size;
    if (message->header.msgh_id != StateIdentityID || !(message->header.msgh_bits & MACH_MSGH_BITS_COMPLEX) ||
        size < sizeof(*prefix) || size > sizeof(*message) || prefix->body.msgh_descriptor_count != 2 ||
        prefix->thread.type != MACH_MSG_PORT_DESCRIPTOR || prefix->task.type != MACH_MSG_PORT_DESCRIPTOR ||
        prefix->thread.disposition != MACH_MSG_TYPE_PORT_SEND || prefix->task.disposition != MACH_MSG_TYPE_PORT_SEND ||
        !MACH_PORT_VALID(prefix->thread.name) || prefix->task.name != mach_task_self() ||
        (prefix->exception != EXC_BAD_ACCESS && prefix->exception != EXC_BREAKPOINT) ||
        memcmp(&prefix->ndr, &NDR_record, sizeof(NDR_record)) || prefix->codeCount > MaximumCodes) return false;
    size_t stateHeader = sizeof(*prefix) + prefix->codeCount * sizeof(int64_t);
    if (size < stateHeader + 8) return false;
    int flavor;
    mach_msg_type_number_t count;
    memcpy(&flavor, message->bytes + stateHeader, 4);
    memcpy(&count, message->bytes + stateHeader + 4, 4);
    if (flavor != ARM_THREAD_STATE64 || count != ARM_THREAD_STATE64_COUNT || size != stateHeader + 8 + sizeof(request->state)) return false;
    request->thread = prefix->thread.name;
    request->task = prefix->task.name;
    request->exception = prefix->exception;
    request->codeCount = prefix->codeCount;
    memset(request->code, 0, sizeof(request->code));
    memcpy(request->code, message->bytes + sizeof(*prefix), prefix->codeCount * sizeof(int64_t));
    memcpy(&request->state, message->bytes + stateHeader + 8, sizeof(request->state));
    return true;
}

static uintptr_t rawPC(const arm_thread_state64_t *state) {
    uintptr_t pc = state->__pc;
    if (!(state->__pad & StateNoPtrauth)) {
#if defined(LSB_WORKQUEUE_BOOTSTRAP_TESTING)
        pc &= UINT64_C(0x0000ffffffffffff);
#elif defined(__arm64__)
        __asm__("xpaci %0" : "+r"(pc));
#else
#error Workqueue bootstrap requires Darwin arm64.
#endif
    }
    return pc;
}

static uintptr_t rawSP(const arm_thread_state64_t *state) {
    uintptr_t sp = state->__sp;
    if (!(state->__pad & StateNoPtrauth)) {
#if defined(LSB_WORKQUEUE_BOOTSTRAP_TESTING)
        sp &= UINT64_C(0x0000ffffffffffff);
#else
        __asm__("xpacd %0" : "+r"(sp));
#endif
    }
    return sp;
}

static bool redirectException(ExceptionRequest *request) {
    arm_thread_state64_t *state = &request->state;
    if (!nativeEntry || !guestEntry || rawPC(state) != nativeEntry) return false;
    bool breakpoint = request->exception == EXC_BREAKPOINT;
    if (breakpoint) {
        // Do not rely on code[1] being an address: the authenticated state PC and inherited owned slot identify our trap.
        if (!hardwareMode || !breakpointNeedsRestore || !request->codeCount || request->codeCount > MaximumCodes ||
            request->code[0] != EXC_ARM_BREAKPOINT) return false;
    } else if (request->exception != EXC_BAD_ACCESS || request->codeCount != 2 ||
               (request->code[0] != KERN_INVALID_ADDRESS && request->code[0] != KERN_PROTECTION_FAILURE) ||
               (uint64_t)request->code[1] != nativeEntry) return false;
    // Validate the pinned kernel's initial/reused workqueue upcall shape, including the -1 cleanup sentinel.
    uint64_t flags = state->__x[4];
    int64_t events = (int64_t)state->__x[5];
    uintptr_t stack = rawSP(state);
    if (!state->__x[0] || (state->__x[0] & 15) || state->__x[1] != request->thread || !state->__x[2] ||
        state->__x[2] > stack || stack > state->__x[0] || (stack & 15) ||
        !(flags & (WorkqueueReuse | WorkqueueTSDBaseSet)) || events < -1 || events > 16) return false;
    if (breakpoint) {
        arm_debug_state64_t debug;
        mach_msg_type_number_t count = ARM_DEBUG_STATE64_COUNT;
        kern_return_t result = thread_get_state(request->thread, ARM_DEBUG_STATE64, (thread_state_t)&debug, &count);
        if (result || count != ARM_DEBUG_STATE64_COUNT || !ownsBreakpoint(&debug)) {
            if (hardwareRejectedCount < 8) {
                ++hardwareRejectedCount;
                hardwareTrace("thread-slot-rejected", result ? result : KERN_FAILURE, request->thread);
            }
            return false;
        }
        // Every kernel reuse returns to nativeEntry again. Keep the inherited slot enabled on this worker and future workers.
        if (hardwareTrapCount != UINT_MAX) ++hardwareTrapCount;
        if (hardwareTrapCount <= 8) {
            hardwareTrace("redirect", KERN_SUCCESS, request->thread);
            hardwareTrace("upcall-flags", KERN_SUCCESS, flags);
            hardwareTrace("upcall-events", KERN_SUCCESS, (uint64_t)events);
        }
    }
    state->__pc = LSBThreadSignPC(guestEntry, state->__pad);
    state->__pad &= ~StateKernelSignedPC;
    return true;
}

static void append(Message *message, size_t *size, const void *data, size_t count) {
    memcpy(message->bytes + *size, data, count);
    *size += count;
}

static kern_return_t forwardMach64(mach_port_t port, unsigned behavior, ExceptionRequest *request, int *flavor,
                                   thread_state_t state, mach_msg_type_number_t *count) {
    Message message = {0};
    bool identity = behavior != EXCEPTION_STATE, hasState = behavior != EXCEPTION_DEFAULT;
    if (request->codeCount > MaximumCodes || *count > THREAD_STATE_MAX) return KERN_INVALID_ARGUMENT;
    mach_port_t replyPort = mach_reply_port();
    if (!MACH_PORT_VALID(replyPort)) return KERN_RESOURCE_SHORTAGE;
    message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    message.header.msgh_remote_port = port;
    message.header.msgh_local_port = replyPort;
    message.header.msgh_id = 2404 + (int)behavior;
    size_t size = sizeof(mach_msg_header_t);
    if (identity) {
        message.header.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
        mach_msg_body_t body = {.msgh_descriptor_count = 2};
        mach_msg_port_descriptor_t thread = {.name = request->thread, .disposition = MACH_MSG_TYPE_COPY_SEND, .type = MACH_MSG_PORT_DESCRIPTOR};
        mach_msg_port_descriptor_t task = {.name = request->task, .disposition = MACH_MSG_TYPE_COPY_SEND, .type = MACH_MSG_PORT_DESCRIPTOR};
        append(&message, &size, &body, sizeof(body));
        append(&message, &size, &thread, sizeof(thread));
        append(&message, &size, &task, sizeof(task));
    }
    append(&message, &size, &NDR_record, sizeof(NDR_record));
    append(&message, &size, &request->exception, 4);
    append(&message, &size, &request->codeCount, 4);
    append(&message, &size, request->code, request->codeCount * 8);
    if (hasState) {
        append(&message, &size, flavor, 4);
        append(&message, &size, count, 4);
        append(&message, &size, state, *count * sizeof(natural_t));
    }
    message.header.msgh_size = (mach_msg_size_t)size;
    kern_return_t result = mach_msg(&message.header, MACH_SEND_MSG | MACH_RCV_MSG, (mach_msg_size_t)size,
                                    sizeof(message), replyPort, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    (void)mach_port_mod_refs(mach_task_self(), replyPort, MACH_PORT_RIGHT_RECEIVE, -1);
    if (result) return result;
    StateReply *reply = (StateReply *)&message;
    if ((message.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) || message.header.msgh_remote_port || message.header.msgh_voucher_port) {
        mach_msg_destroy(&message.header);
        return MIG_TYPE_ERROR;
    }
    if (message.header.msgh_id != 2504 + (int)behavior || message.header.msgh_size < sizeof(mig_reply_error_t) ||
        memcmp(&reply->ndr, &NDR_record, sizeof(NDR_record))) return MIG_REPLY_MISMATCH;
    if (reply->result) return message.header.msgh_size == sizeof(mig_reply_error_t) ? reply->result : MIG_TYPE_ERROR;
    if (!hasState) return message.header.msgh_size == sizeof(mig_reply_error_t) ? KERN_SUCCESS : MIG_TYPE_ERROR;
    if (message.header.msgh_size < offsetof(StateReply, state) || reply->stateCount > THREAD_STATE_MAX ||
        message.header.msgh_size != offsetof(StateReply, state) + reply->stateCount * sizeof(natural_t)) return MIG_TYPE_ERROR;
    *flavor = reply->flavor;
    *count = reply->stateCount;
    memcpy(state, reply->state, *count * sizeof(natural_t));
    return KERN_SUCCESS;
}

static bool supportedBehavior(exception_behavior_t value, thread_state_flavor_t flavor) {
    unsigned base = (unsigned)value & ~(unsigned)MACH_EXCEPTION_CODES;
    return base == EXCEPTION_DEFAULT || ((base == EXCEPTION_STATE || base == EXCEPTION_STATE_IDENTITY) && flavor > 0 && flavor != THREAD_STATE_NONE);
}

static kern_return_t forwardException(ExceptionRequest *request) {
    for (unsigned i = 0; i < savedCount; ++i) {
        if (!(savedMasks[i] & (1u << request->exception)) || !MACH_PORT_VALID(savedPorts[i])) continue;
        unsigned behavior = (unsigned)savedBehaviors[i] & ~(unsigned)MACH_EXCEPTION_CODES;
        if (!supportedBehavior(savedBehaviors[i], savedFlavors[i])) return KERN_FAILURE;
        thread_state_data_t state;
        mach_msg_type_number_t count = THREAD_STATE_MAX;
        int flavor = savedFlavors[i];
        kern_return_t result = KERN_SUCCESS;
        if (behavior != EXCEPTION_DEFAULT) result = thread_get_state(request->thread, flavor, state, &count);
        if (result) return result;
        if ((unsigned)savedBehaviors[i] & (unsigned)MACH_EXCEPTION_CODES) {
            result = forwardMach64(savedPorts[i], behavior, request, &flavor, state, &count);
        } else {
            exception_data_type_t codes[MaximumCodes] = {(int)request->code[0], (int)request->code[1]};
            thread_state_data_t updated;
            mach_msg_type_number_t updatedCount = THREAD_STATE_MAX;
            if (behavior == EXCEPTION_DEFAULT)
                result = exception_raise(savedPorts[i], request->thread, request->task, request->exception, codes, request->codeCount);
            else if (behavior == EXCEPTION_STATE)
                result = exception_raise_state(savedPorts[i], request->exception, codes, request->codeCount, &flavor, state, count, updated, &updatedCount);
            else
                result = exception_raise_state_identity(savedPorts[i], request->thread, request->task, request->exception,
                                                         codes, request->codeCount, &flavor, state, count, updated, &updatedCount);
            if (!result && behavior != EXCEPTION_DEFAULT) {
                if (updatedCount > THREAD_STATE_MAX) return MIG_TYPE_ERROR;
                count = updatedCount;
                memcpy(state, updated, count * sizeof(natural_t));
            }
        }
        if (!result && behavior != EXCEPTION_DEFAULT) result = thread_set_state(request->thread, flavor, state, count);
        // A DEFAULT handler (including LLDB) can change registers with thread_set_state. Never overwrite its changes with the old request.
        if (!result) {
            count = ARM_THREAD_STATE64_COUNT;
            result = thread_get_state(request->thread, ARM_THREAD_STATE64, (thread_state_t)&request->state, &count);
            if (!result && count != ARM_THREAD_STATE64_COUNT) result = KERN_INVALID_ARGUMENT;
        }
        return result;
    }
    return KERN_FAILURE;
}

static void handleMessage(Message *message, StateReply *reply) {
    memset(reply, 0, sizeof(*reply));
    reply->header.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(message->header.msgh_bits), 0);
    reply->header.msgh_remote_port = message->header.msgh_remote_port;
    reply->header.msgh_id = message->header.msgh_id + 100;
    reply->header.msgh_size = sizeof(mig_reply_error_t);
    reply->ndr = NDR_record;
    reply->result = MIG_BAD_ARGUMENTS;
    ExceptionRequest request;
    if (parseException(message, &request)) {
        reply->result = redirectException(&request) ? KERN_SUCCESS : forwardException(&request);
        if (!reply->result) {
            reply->flavor = ARM_THREAD_STATE64;
            reply->stateCount = ARM_THREAD_STATE64_COUNT;
            memcpy(reply->state, &request.state, sizeof(request.state));
            reply->header.msgh_size = offsetof(StateReply, state) + sizeof(request.state);
        }
    }
    // Retain the reply send-once right, but release both received identity send rights and any voucher.
    message->header.msgh_remote_port = MACH_PORT_NULL;
    mach_msg_destroy(&message->header);
}

static void *exceptionServer(void *unused) {
    (void)unused;
    for (;;) {
        Message message;
        mach_msg_option_t options = MACH_RCV_MSG | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
            MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT);
        kern_return_t status = mach_msg(&message.header, options, 0, sizeof(message), exceptionPortSet, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (status == MACH_RCV_INTERRUPTED) continue;
        if (status) _exit(126);
        if (message.header.msgh_local_port != exceptionPort) {
            LSBProcessDebuggerHandleMessage(&message.header, sizeof(message));
            continue;
        }
        StateReply reply;
        handleMessage(&message, &reply);
        status = mach_msg(&reply.header, MACH_SEND_MSG, reply.header.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (status == MACH_SEND_INVALID_DEST || status == MACH_SEND_TIMED_OUT || status == MACH_SEND_INTERRUPTED) mach_msg_destroy(&reply.header);
        else if (status) _exit(126);
    }
}

static void restoreHandlers(void) {
    if (task_set_exception_ports(mach_task_self(), bridgeMask, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE)) _exit(126);
    for (unsigned i = 0; i < savedCount; ++i) {
        if (task_set_exception_ports(mach_task_self(), savedMasks[i], savedPorts[i], savedBehaviors[i], savedFlavors[i])) _exit(126);
        if (MACH_PORT_VALID(savedPorts[i])) (void)mach_port_deallocate(mach_task_self(), savedPorts[i]);
    }
    savedCount = 0;
}

static void initializeBridge(void) {
    guestEntry = atomic_load_explicit(&simulatorEntry, memory_order_acquire);
    int features;
    bridgeError = ENOTSUP;
    if (!guestEntry || !parseMetadata(&nativeEntry, &features) || nativeEntry == guestEntry) return;
    const char *hardware = getenv(LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV);
    hardwareMode = !hardware || !strcmp(hardware, "1");
    bridgeMask = EXC_MASK_BAD_ACCESS | (hardwareMode ? EXC_MASK_BREAKPOINT : 0);
    hardwareTrace("initialize", KERN_SUCCESS, bridgeMask);
    bool ownsSend = false;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &exceptionPort)) return;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &exceptionPortSet)) goto releasePort;
    if (mach_port_move_member(mach_task_self(), exceptionPort, exceptionPortSet)) goto releasePort;
    if (mach_port_insert_right(mach_task_self(), exceptionPort, exceptionPort, MACH_MSG_TYPE_MAKE_SEND)) goto releasePort;
    ownsSend = true;
    savedCount = EXC_TYPES_COUNT;
    if (task_swap_exception_ports(mach_task_self(), bridgeMask, exceptionPort, EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES,
                                  ARM_THREAD_STATE64, savedMasks, &savedCount, savedPorts, savedBehaviors, savedFlavors)) goto releasePort;
    for (unsigned i = 0; i < savedCount; ++i) {
        if (MACH_PORT_VALID(savedPorts[i]) && !supportedBehavior(savedBehaviors[i], savedFlavors[i])) goto restore;
    }
    // The handoff has no preexisting native workers. XNU machine_thread_inherit_taskwide copies this default only into future threads.
    if (hardwareMode && installHardwareBreakpoint()) goto restore;
    pthread_t thread;
    int result = pthread_create(&thread, NULL, exceptionServer, NULL);
    if (result) { bridgeError = result; goto restore; }
    // Prevent our server's own faults from going straight back to itself when a prior debugger handler is available.
    mach_port_t server = pthread_mach_thread_np(thread);
    for (unsigned i = 0; i < savedCount; ++i) {
        if (MACH_PORT_VALID(savedPorts[i]) && thread_set_exception_ports(server, savedMasks[i], savedPorts[i], savedBehaviors[i], savedFlavors[i]))
            _exit(126);
    }
    (void)pthread_detach(thread);
    bridgeError = 0;
    atomic_store_explicit(&exceptionServerReady, true, memory_order_release);
    hardwareTrace("ready", KERN_SUCCESS, bridgeMask);
    return;
restore:
    if (restoreHardwareBreakpoint()) _exit(126);
    restoreHandlers();
releasePort:
    if (MACH_PORT_VALID(exceptionPortSet)) (void)mach_port_mod_refs(mach_task_self(), exceptionPortSet, MACH_PORT_RIGHT_PORT_SET, -1);
    exceptionPortSet = MACH_PORT_NULL;
    (void)mach_port_mod_refs(mach_task_self(), exceptionPort, MACH_PORT_RIGHT_RECEIVE, -1);
    if (ownsSend) (void)mach_port_deallocate(mach_task_self(), exceptionPort);
    exceptionPort = MACH_PORT_NULL;
}

static int ensureExceptionServer(void) {
    if (atomic_load_explicit(&exceptionServerReady, memory_order_acquire)) return 0;
    if (!atomic_load_explicit(&simulatorEntry, memory_order_acquire)) return ENOTSUP;
    pthread_once(&bridgeOnce, initializeBridge);
    return bridgeError;
}

int LSBWorkqueueAddProcessControlPort(mach_port_t port) {
    mach_port_type_t type = 0;
    if (!MACH_PORT_VALID(port) || port == exceptionPort || mach_port_type(mach_task_self(), port, &type) || !(type & MACH_PORT_TYPE_RECEIVE))
        return EINVAL;
    // Inserted-library constructors can precede libdispatch's lazy __workq_open. Start the same guest server now, after guest TLS is ready.
    int error = ensureExceptionServer();
    if (error) return error;
    return mach_port_move_member(mach_task_self(), port, exceptionPortSet) ? EIO : 0;
}

int LSBWorkqueueOpen(void) {
    if (!atomic_load_explicit(&simulatorEntry, memory_order_acquire)) return __workq_open();
    int error = ensureExceptionServer();
    // libpthread ignores __workq_open's return value, so returning an error here would let it launch an unredirected worker.
    if (error) _exit(126);
    return __workq_open();
}

#ifndef LSB_WORKQUEUE_BOOTSTRAP_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBWorkqueueSupported, (const void *)&_pthread_workqueue_supported}, {(const void *)&LSBWorkqueueOpen, (const void *)&__workq_open}
};
#endif
