#include "WorkqueueTrampoline.h"
#include "../Shared/PrivateMachVM.h"

#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct {
    uint32_t loadTarget, branchTarget;
    uint64_t target;
} WorkqueueTrampoline;
_Static_assert(sizeof(WorkqueueTrampoline) == 16 && offsetof(WorkqueueTrampoline, target) == 8, "ARM64 literal branch layout changed");

static bool coversRange(mach_vm_address_t region, mach_vm_size_t regionSize, uintptr_t base, size_t size) {
    return region <= base && regionSize >= base - region && regionSize - (base - region) >= size;
}

static bool isDetachedPlaceholder(uintptr_t base, size_t size) {
    mach_vm_address_t region = base;
    mach_vm_size_t regionSize = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info = {0};
    // Only v0 fields are needed; newer SDKs request trailing fields that the iOS 26 kernel does not return.
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_V0_COUNT_64;
    kern_return_t result = mach_vm_region_recurse(mach_task_self(), &region, &regionSize, &depth,
                                                 (vm_region_recurse_info_t)&info, &count);
    // XNU vm_shared_region_remove() leaves an anonymous DYLD placeholder with both protections zero, not an empty hole.
    // https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/vm/vm_shared_region.c#L2624-L2660
    // Query only the top-level map: never descend into or replace a surviving shared-region submap or another guard mapping.
    return !result && count == VM_REGION_SUBMAP_INFO_V0_COUNT_64 && depth == 0 && !info.is_submap && info.user_tag == VM_MEMORY_DYLD &&
           info.protection == VM_PROT_NONE && info.max_protection == VM_PROT_NONE && coversRange(region, regionSize, base, size);
}

static void restorePlaceholder(uintptr_t base, size_t size) {
    void *mapping = mmap((void *)base, size, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, VM_MAKE_TAG(VM_MEMORY_DYLD), 0);
    if (mapping == MAP_FAILED || mapping != (void *)base ||
        mach_vm_protect(mach_task_self(), base, size, TRUE, VM_PROT_NONE) || !isDetachedPlaceholder(base, size)) _exit(126);
}

static int installTrampoline(uintptr_t nativeEntry, uintptr_t guestEntry) {
    if (!nativeEntry || !guestEntry || nativeEntry == guestEntry || ((nativeEntry | guestEntry) & 3) ||
        nativeEntry > UINTPTR_MAX - sizeof(WorkqueueTrampoline)) return EINVAL;
    uint64_t sharedRegion = 0;
    // The host permits detachment only for an exact non-production MobileGestalt result. Also verify actual removal here:
    // a retained native cache must never be patched, even if environment metadata requests the wrong mode.
    if (!__shared_region_check_np(&sharedRegion)) return EPERM;
    if (errno != ENOMEM && errno != EINVAL) return errno ? errno : EIO;
    int pageSize = getpagesize();
    if (pageSize < 4096 || pageSize > 65536 || (pageSize & (pageSize - 1))) return ENOTSUP;
    uintptr_t mask = (uintptr_t)pageSize - 1;
    uintptr_t base = nativeEntry & ~mask, end = nativeEntry + sizeof(WorkqueueTrampoline);
    if (end > UINTPTR_MAX - mask) return EOVERFLOW;
    size_t size = ((end + mask) & ~mask) - base;
    mach_vm_address_t reservation = base;
    // Prefer an unoccupied range. A detached kernel placeholder may be carved only for these captured entry pages.
    // This runs before the bridge server or kernel workqueue is started; no guest worker can race the mapping checks.
    kern_return_t result = mach_vm_allocate(mach_task_self(), &reservation, size, VM_FLAGS_FIXED);
    bool carved = result == KERN_NO_SPACE && isDetachedPlaceholder(base, size);
    if (result && !carved) return result == KERN_NO_SPACE ? EADDRINUSE : ENOMEM;
    if (!carved && reservation != base) {
        if (mach_vm_deallocate(mach_task_self(), reservation, size)) _exit(126);
        return EFAULT;
    }
    int error = 0;
    void *mapping = mmap((void *)base, size, PROT_READ | PROT_EXEC, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
    if (mapping == MAP_FAILED) { error = errno; goto discard; }
    if (mapping != (void *)base) {
        if (munmap(mapping, size)) _exit(126);
        error = EFAULT;
        goto discard;
    }
    if (mprotect(mapping, size, PROT_READ | PROT_WRITE)) { error = errno; goto discard; }
    // ldr x16, #8; br x16. No stack, LR, argument or TLS changes; x16 is the ABI's intra-procedure scratch register.
    // The branch reaches unsigned arm64 guest code directly for initial entry, reuse, event delivery and cleanup (-1).
    WorkqueueTrampoline code = {.loadTarget = 0x58000050, .branchTarget = 0xd61f0200, .target = guestEntry};
    memcpy((void *)nativeEntry, &code, sizeof(code));
    sys_icache_invalidate((void *)nativeEntry, sizeof(code));
    if (mprotect(mapping, size, PROT_READ | PROT_EXEC)) { error = errno; goto discard; }
    // On production TXM, an apparently successful executable mmap can still be read-only. Do not open the workqueue
    // unless this development-device mapping really has execute permission and has dropped write permission.
    mach_vm_address_t region = base;
    mach_vm_size_t regionSize = 0;
    vm_region_basic_info_data_64_t info = {0};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    result = mach_vm_region(mach_task_self(), &region, &regionSize, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
    if (MACH_PORT_VALID(object)) mach_port_deallocate(mach_task_self(), object);
    if (result || count != VM_REGION_BASIC_INFO_COUNT_64 || !coversRange(region, regionSize, base, size) ||
        info.protection != (VM_PROT_READ | VM_PROT_EXECUTE)) {
        error = EACCES;
        goto discard;
    }
    return 0;
discard:
    if (carved) restorePlaceholder(base, size);
    else if (munmap((void *)base, size)) _exit(126);
    return error ? error : EIO;
}

int LSBWorkqueueTrampolineInstall(uintptr_t nativeEntry, uintptr_t guestEntry) {
    int saved = errno;
    int error = installTrampoline(nativeEntry, guestEntry);
    errno = saved;
    return error;
}
