#include "ThreadBootstrap.h"
#include "WorkqueueBootstrap.h"

#include <errno.h>
#include <mach/mach.h>
#include <mach/arm/thread_status.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

extern int __bsdthread_register(uintptr_t, uintptr_t, int, void *, size_t, uint64_t);
extern void *__bsdthread_create(void *(*)(void *), void *, void *, void *, unsigned);

#define THREAD_START_CUSTOM 0x01000000u
#define THREAD_START_TSD_BASE_SET 0x10000000u
#define THREAD_START_SUSPENDED 0x20000000u
#define THREAD_STATE_NO_PTRAUTH 0x1u
#define THREAD_STATE_KERNEL_SIGNED_PC 0x4u
#define THREAD_STATE_USER_DIVERSIFIER 0xff000000u

enum { RegistrationMissing, RegistrationNative, RegistrationRedirect, RegistrationUnsupported };
static _Atomic(unsigned) registration;
static uintptr_t simulatorStart;
static unsigned tsdOffset, machSelfOffset;

typedef struct __attribute__((packed)) {
    uint64_t version, dispatchOffset, mainQoS;
    uint32_t tsdOffset, returnToKernelOffset, machSelfOffset;
} RegistrationPrefix;
_Static_assert(sizeof(RegistrationPrefix) == 36 && offsetof(RegistrationPrefix, machSelfOffset) == 32, "Unexpected pthread registration ABI");

int LSBThreadRegister(uintptr_t start, uintptr_t workqueueStart, int pthreadSize, void *data, size_t size, uint64_t dispatchOffset) {
    RegistrationPrefix prefix = {0};
    bool valid = data && size >= sizeof(prefix) && size <= 256 && start && !(start & 3) && pthreadSize == 16384;
    if (valid) {
        memcpy(&prefix, data, sizeof(prefix));
        valid = prefix.version == size && prefix.tsdOffset == 224 && prefix.machSelfOffset == 24;
    }
    int result = __bsdthread_register(start, workqueueStart, pthreadSize, data, size, dispatchOffset);
    int saved = errno;
    unsigned mode = result >= 0 ? RegistrationNative : RegistrationUnsupported;
    if (valid && result == -1 && saved == EINVAL) {
        // XNU rejects registration twice in one process. Keep its TLS/port/QoS setup and replace only each new suspended thread's PC.
        simulatorStart = start;
        tsdOffset = prefix.tsdOffset;
        machSelfOffset = prefix.machSelfOffset;
        mode = RegistrationRedirect;
    }
    LSBWorkqueueRegister(mode == RegistrationRedirect ? workqueueStart : 0);
    atomic_store_explicit(&registration, mode, memory_order_release);
    errno = saved;
    return result;
}

static int machError(kern_return_t error) {
    switch (error) {
        case KERN_RESOURCE_SHORTAGE: return EAGAIN;
        case KERN_INVALID_ARGUMENT: case KERN_INVALID_ADDRESS: return EINVAL;
        case KERN_PROTECTION_FAILURE: case KERN_NO_ACCESS: return EPERM;
        default: return EIO;
    }
}

uintptr_t LSBThreadSignPC(uintptr_t start, uint32_t flags) {
    if (flags & THREAD_STATE_NO_PTRAUTH) return start;
    // Matches arm_thread_state64_set_pc_fptr: string discriminator "pc" is 0x7481 in Apple's arm64e ABI.
    // The shim uses unsigned arm64 function pointers; sign the new PC directly, retaining all other fetched signed state fields.
    uintptr_t modifier = flags & THREAD_STATE_USER_DIVERSIFIER;
    modifier = modifier ? modifier | (UINT64_C(0x7481) << 48) : UINT64_C(0x7481);
#if defined(LSB_THREAD_BOOTSTRAP_TESTING)
    extern uintptr_t LSBTestSignThreadPC(uintptr_t, uintptr_t);
    return LSBTestSignThreadPC(start, modifier);
#elif defined(__arm64__)
    __asm__("pacia %0, %1" : "+r"(start) : "r"(modifier));
    return start;
#else
#error Thread bootstrap requires Darwin arm64.
#endif
}

static void discardThread(mach_port_t port) {
    // Do not let libpthread free storage still referenced by a kernel thread if cleanup unexpectedly fails.
    if (!MACH_PORT_VALID(port)) _exit(126);
    kern_return_t result = thread_terminate(port);
    if (result != KERN_SUCCESS && result != KERN_TERMINATED) _exit(126);
    (void)mach_port_deallocate(mach_task_self(), port);
}

void *LSBThreadCreate(void *(*function)(void *), void *argument, void *stack, void *thread, unsigned flags) {
    unsigned mode = atomic_load_explicit(&registration, memory_order_acquire);
    if (mode == RegistrationMissing) {
        const char *nativeSupport = getenv("LSB_NATIVE_SUPPORT");
        if (nativeSupport && !strcmp(nativeSupport, "1")) mode = RegistrationNative;
    }
    if (mode == RegistrationNative) return __bsdthread_create(function, argument, stack, thread, flags);
    if (mode != RegistrationRedirect || !(flags & THREAD_START_CUSTOM) || !thread || !stack || (flags & THREAD_START_TSD_BASE_SET)) {
        errno = ENOTSUP;
        return (void *)-1;
    }
    int saved = errno;
    void *result = __bsdthread_create(function, argument, stack, thread, flags | THREAD_START_SUSPENDED);
    if (result == (void *)-1) return result;
    uintptr_t portValue = *(uintptr_t *)((unsigned char *)thread + tsdOffset + machSelfOffset);
    mach_port_t port = portValue <= UINT32_MAX ? (mach_port_t)portValue : MACH_PORT_NULL;
    if (!MACH_PORT_VALID(port)) _exit(126);
    arm_thread_state64_t state;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    kern_return_t status = thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&state, &count);
    int error = status ? machError(status) : 0;
    if (!error && (result != thread || count != ARM_THREAD_STATE64_COUNT || state.__x[0] != (uintptr_t)thread || state.__x[1] != port ||
                   state.__x[4] != (uintptr_t)stack || !(state.__x[5] & THREAD_START_TSD_BASE_SET) || (state.__x[5] & THREAD_START_SUSPENDED)))
        error = EPROTO;
    if (!error) {
        state.__pc = LSBThreadSignPC(simulatorStart, state.__pad);
        state.__pad &= ~THREAD_STATE_KERNEL_SIGNED_PC;
        status = thread_set_state(port, ARM_THREAD_STATE64, (thread_state_t)&state, count);
        if (status) error = machError(status);
    }
    if (!error && !(flags & THREAD_START_SUSPENDED)) {
        status = thread_resume(port);
        if (status) error = machError(status);
    }
    if (error) {
        discardThread(port);
        errno = error;
        return (void *)-1;
    }
    errno = saved;
    return result;
}

#ifndef LSB_THREAD_BOOTSTRAP_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBThreadRegister, (const void *)&__bsdthread_register}, {(const void *)&LSBThreadCreate, (const void *)&__bsdthread_create}
};
#endif
