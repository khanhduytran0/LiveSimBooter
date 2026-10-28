#include "TaskPortCapture.h"
#include <errno.h>
#include <mach/ndr.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Exported by libSystem; the iPhoneOS SDK omits mach/mach_vm.h.
extern kern_return_t mach_vm_read_overwrite(vm_map_t, mach_vm_address_t, mach_vm_size_t, mach_vm_address_t, mach_vm_size_t *);

#if defined(__arm64__)
#include <mach/arm/thread_status.h>

// XNU osfmk/mach/mach_exc.defs: mach_exception_raise_state_identity is MIG 2407, with two 64-bit codes.
// The arm64 SDK uses a four-byte-packed MIG envelope and ARM_THREAD_STATE64_COUNT natural_t words.
#pragma pack(push, 4)
typedef struct {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t thread, task;
    NDR_record_t ndr;
    exception_type_t exception;
    mach_msg_type_number_t codeCount;
    int64_t codes[2];
    int flavor;
    mach_msg_type_number_t stateCount;
    natural_t state[ARM_THREAD_STATE64_COUNT];
} LSBTaskCaptureException;
typedef struct {
    mach_msg_header_t header;
    NDR_record_t ndr;
    kern_return_t result;
    int flavor;
    mach_msg_type_number_t stateCount;
    natural_t state[ARM_THREAD_STATE64_COUNT];
} LSBTaskCaptureReply;
#pragma pack(pop)
_Static_assert(offsetof(LSBTaskCaptureException, task) == 40 && offsetof(LSBTaskCaptureException, codes) == 68, "MIG exception ports/codes");
_Static_assert(offsetof(LSBTaskCaptureException, state) == 92 && sizeof(LSBTaskCaptureException) == 364, "MIG arm64 exception state");
_Static_assert(offsetof(LSBTaskCaptureReply, flavor) == 36 && sizeof(LSBTaskCaptureReply) == 316, "MIG arm64 reply state");
enum { CaptureMessageID = 2407, CaptureBreakImmediate = 0x4c53 };
static const exception_behavior_t CaptureBehavior = EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES;

static int captureError(kern_return_t status) {
    switch (status) {
        case KERN_SUCCESS: return 0;
        case KERN_INVALID_ARGUMENT: return EINVAL;
        case KERN_PROTECTION_FAILURE: return EACCES;
        case KERN_RESOURCE_SHORTAGE: return ENOMEM;
        case MACH_RCV_TIMED_OUT: return EAGAIN;
        case MACH_RCV_TOO_LARGE: return EMSGSIZE;
        case MACH_SEND_INVALID_DEST: return EPIPE;
        case MACH_SEND_TIMED_OUT: return ETIMEDOUT;
        default: return EIO;
    }
}

static int validOffer(const LSBTaskPortCaptureOffer *offer) {
    if (!offer || !MACH_PORT_VALID(offer->port)) return 0;
    unsigned char combined = 0;
    for (unsigned i = 0; i < sizeof(offer->nonce); ++i) combined |= offer->nonce[i];
    return combined != 0;
}

int LSBTaskPortCaptureCreate(LSBTaskPortCaptureOffer *offer) {
    if (!offer) return EINVAL;
    memset(offer, 0, sizeof(*offer));
    kern_return_t status = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &offer->port);
    if (status) return captureError(status);
    status = mach_port_insert_right(mach_task_self(), offer->port, offer->port, MACH_MSG_TYPE_MAKE_SEND);
    if (status) {
        mach_port_mod_refs(mach_task_self(), offer->port, MACH_PORT_RIGHT_RECEIVE, -1);
        memset(offer, 0, sizeof(*offer));
        return captureError(status);
    }
    do arc4random_buf(offer->nonce, sizeof(offer->nonce)); while (!validOffer(offer));
    return 0;
}

static int kernelTrailer(const mach_msg_header_t *message, size_t capacity) {
    size_t offset = (message->msgh_size + 3u) & ~(size_t)3u;
    if (offset > capacity || capacity - offset < sizeof(mach_msg_audit_trailer_t)) return 0;
    const mach_msg_audit_trailer_t *trailer = (const void *)((const unsigned char *)message + offset);
    const audit_token_t kernel = {{0}};
    return trailer->msgh_trailer_type == MACH_MSG_TRAILER_FORMAT_0 && trailer->msgh_trailer_size == sizeof(*trailer) &&
        !memcmp(&trailer->msgh_audit, &kernel, sizeof(kernel));
}

static int exceptionShape(const LSBTaskCaptureException *message, mach_port_t receivePort) {
    return message->header.msgh_id == CaptureMessageID && message->header.msgh_size == sizeof(*message) &&
        message->header.msgh_bits == (MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, MACH_MSG_TYPE_MOVE_SEND) | MACH_MSGH_BITS_COMPLEX) &&
        message->header.msgh_local_port == receivePort && MACH_PORT_VALID(message->header.msgh_remote_port) &&
        message->header.msgh_voucher_port == MACH_PORT_NULL && message->body.msgh_descriptor_count == 2 &&
        message->thread.type == MACH_MSG_PORT_DESCRIPTOR && message->thread.disposition == MACH_MSG_TYPE_MOVE_SEND &&
        message->task.type == MACH_MSG_PORT_DESCRIPTOR && message->task.disposition == MACH_MSG_TYPE_MOVE_SEND &&
        MACH_PORT_VALID(message->thread.name) && MACH_PORT_VALID(message->task.name) &&
        !memcmp(&message->ndr, &NDR_record, sizeof(NDR_record)) && message->exception == EXC_BREAKPOINT &&
        message->codeCount == 2 && message->codes[0] == 1 && message->flavor == ARM_THREAD_STATE64 &&
        message->stateCount == ARM_THREAD_STATE64_COUNT;
}

static int authenticateException(const LSBTaskCaptureException *message, const LSBTaskPortCaptureOffer *offer,
                                 const audit_token_t *expected, arm_thread_state64_t *state) {
    audit_token_t actual;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    kern_return_t status = task_info(message->task.name, TASK_AUDIT_TOKEN, (task_info_t)&actual, &count);
    if (status || count != TASK_AUDIT_TOKEN_COUNT || memcmp(&actual, expected, sizeof(actual))) return EACCES;
    // A read/inspect task port cannot authorize later task-control requests. Do not treat task_info alone as control proof.
    mach_port_t control = MACH_PORT_NULL;
    status = task_get_special_port(message->task.name, TASK_KERNEL_PORT, &control);
    if (status || !MACH_PORT_VALID(control)) return EACCES;
    mach_port_deallocate(mach_task_self(), control);
    memcpy(state, message->state, sizeof(*state));
    uint64_t nonce[2];
    memcpy(nonce, offer->nonce, sizeof(nonce));
    if (state->__x[0] != nonce[0] || state->__x[1] != nonce[1] || state->__x[2] != EINPROGRESS) return EACCES;
    mach_vm_address_t pc = arm_thread_state64_get_pc(*state);
    if (!pc || (pc & 3) || (uint64_t)message->codes[1] != pc) return EPROTO;
    uint32_t instruction = 0;
    mach_vm_size_t bytes = 0;
    status = mach_vm_read_overwrite(message->task.name, pc, sizeof(instruction), (mach_vm_address_t)&instruction, &bytes);
    if (status || bytes != sizeof(instruction) || instruction != (0xd4200000u | (CaptureBreakImmediate << 5))) return EACCES;
    // SDK accessors handle opaque/PAC thread-state representations; never write a raw PC field.
    arm_thread_state64_set_pc_fptr(*state, (void *)(uintptr_t)(pc + 4));
    state->__x[2] = 0;
    return 0;
}

int LSBTaskPortCaptureReceive(const LSBTaskPortCaptureOffer *offer, const audit_token_t *expectedAudit, mach_port_t *task) {
    if (task) *task = MACH_PORT_NULL;
    if (!validOffer(offer) || !expectedAudit || !task) return EINVAL;
    union { mach_msg_header_t header; unsigned char bytes[1024]; } buffer = {0};
    // Without MACH_RCV_LARGE, an oversized message is discarded instead of leaving a permanently readable queue head.
    kern_return_t status = mach_msg(&buffer.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
        MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT), 0, sizeof(buffer), offer->port, 0, MACH_PORT_NULL);
    if (status) return captureError(status);
    LSBTaskCaptureException *request = (void *)&buffer;
    int trusted = kernelTrailer(&buffer.header, sizeof(buffer));
    int shape = trusted && exceptionShape(request, offer->port);
    arm_thread_state64_t state;
    int error = !trusted ? EACCES : !shape ? EPROTO : authenticateException(request, offer, expectedAudit, &state);
    if (shape) {
        LSBTaskCaptureReply reply = {.header = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0),
            .msgh_size = error ? offsetof(LSBTaskCaptureReply, flavor) : sizeof(reply), .msgh_remote_port = request->header.msgh_remote_port,
            .msgh_id = CaptureMessageID + 100}, .ndr = NDR_record, .result = error ? KERN_FAILURE : KERN_SUCCESS};
        if (!error) { reply.flavor = ARM_THREAD_STATE64; reply.stateCount = ARM_THREAD_STATE64_COUNT; memcpy(reply.state, &state, sizeof(state)); }
        status = mach_msg(&reply.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, reply.header.msgh_size, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
        if (!status) request->header.msgh_remote_port = MACH_PORT_NULL;
        else if (!error) error = captureError(status);
    }
    if (!error) { *task = request->task.name; request->task.name = MACH_PORT_NULL; }
    mach_msg_destroy(&request->header);
    return error;
}

static int restoreExceptionPorts(thread_t thread, const LSBTaskPortCaptureOffer *offer, mach_msg_type_number_t savedCount,
                                 const exception_mask_t *savedMasks, const mach_port_t *savedPorts,
                                 const exception_behavior_t *savedBehaviors, const thread_state_flavor_t *savedFlavors) {
    exception_mask_t masks[EXC_TYPES_COUNT];
    mach_port_t ports[EXC_TYPES_COUNT];
    exception_behavior_t behaviors[EXC_TYPES_COUNT];
    thread_state_flavor_t flavors[EXC_TYPES_COUNT];
    mach_msg_type_number_t count = EXC_TYPES_COUNT;
    kern_return_t status = thread_get_exception_ports(thread, EXC_MASK_BREAKPOINT, masks, &count, ports, behaviors, flavors);
    if (status) return captureError(status);
    int stillInstalled = count == 1 && masks[0] == EXC_MASK_BREAKPOINT && ports[0] == offer->port &&
        behaviors[0] == CaptureBehavior && flavors[0] == ARM_THREAD_STATE64;
    for (unsigned i = 0; i < count; ++i) if (MACH_PORT_VALID(ports[i])) mach_port_deallocate(mach_task_self(), ports[i]);
    // A debugger may replace the handler during capture. Preserve that newer registration instead of overwriting it.
    if (!stillInstalled) return 0;
    if (!savedCount) return captureError(thread_set_exception_ports(thread, EXC_MASK_BREAKPOINT, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE));
    for (unsigned i = 0; i < savedCount; ++i) {
        status = thread_set_exception_ports(thread, savedMasks[i], savedPorts[i], savedBehaviors[i], savedFlavors[i]);
        if (status) return captureError(status);
    }
    return 0;
}

int LSBTaskPortCapturePerform(const LSBTaskPortCaptureOffer *offer) {
    if (!validOffer(offer)) return EINVAL;
    thread_t thread = mach_thread_self();
    exception_mask_t masks[EXC_TYPES_COUNT];
    mach_port_t ports[EXC_TYPES_COUNT];
    exception_behavior_t behaviors[EXC_TYPES_COUNT];
    thread_state_flavor_t flavors[EXC_TYPES_COUNT];
    mach_msg_type_number_t count = EXC_TYPES_COUNT;
    kern_return_t status = thread_swap_exception_ports(thread, EXC_MASK_BREAKPOINT, offer->port, CaptureBehavior, ARM_THREAD_STATE64,
        masks, &count, ports, behaviors, flavors);
    if (status) { mach_port_deallocate(mach_task_self(), thread); return captureError(status); }
    uint64_t nonce[2];
    memcpy(nonce, offer->nonce, sizeof(nonce));
    register uint64_t first __asm("x0") = nonce[0], second __asm("x1") = nonce[1], result __asm("x2") = EINPROGRESS;
    __asm__ volatile("brk #0x4c53" : "+r"(first), "+r"(second), "+r"(result) : : "memory");
    int error = restoreExceptionPorts(thread, offer, count, masks, ports, behaviors, flavors);
    for (unsigned i = 0; i < count; ++i) if (MACH_PORT_VALID(ports[i])) mach_port_deallocate(mach_task_self(), ports[i]);
    mach_port_deallocate(mach_task_self(), thread);
    return error ? error : (int)result;
}
#else
int LSBTaskPortCaptureCreate(LSBTaskPortCaptureOffer *offer) { if (offer) memset(offer, 0, sizeof(*offer)); return ENOTSUP; }
int LSBTaskPortCaptureReceive(const LSBTaskPortCaptureOffer *offer, const audit_token_t *expected, mach_port_t *task) {
    (void)offer; (void)expected; if (task) *task = MACH_PORT_NULL; return ENOTSUP;
}
int LSBTaskPortCapturePerform(const LSBTaskPortCaptureOffer *offer) { (void)offer; return ENOTSUP; }
#endif
