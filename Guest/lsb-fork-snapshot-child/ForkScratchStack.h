// A fork observer can lend the unused lower part of its native main stack to the donor's stack.
#pragma once
#include "../Shared/Fork/ForkTrialWire.h"
#include "../Shared/Fork/ForkPthreadRegistry.h"
#include "../../ProcessSupport/ForkStackHint.h"
#include <mach/vm_statistics.h>
#include <sys/mman.h>

typedef struct { uintptr_t top, bottom; } TrialStackBounds;
typedef struct {
    TrialStackBounds native, scratch, donor;
    TrialStackBounds *pthreadBounds;
    uintptr_t releaseStart, releaseEnd, landingSP;
    LSBForkContext continuation;
} TrialScratchStack;

typedef struct {
    unsigned char prefix[0xb0];
    TrialStackBounds bounds;
    uintptr_t freeAddress, freeSize;
} TrialMainPthread;
_Static_assert(offsetof(TrialMainPthread, bounds.bottom) == 0xb8 && offsetof(TrialMainPthread, freeAddress) == 0xc0, "native arm64 pthread stack fields");

// The pinned 23B50/24A435 native libpthread getters use the top/bottom pair. _pthread_terminate uses those same bounds for a main
// thread whose pthread object is outside its stack allocation. Keep allocation ownership, the object and its TSD untouched.
static bool trialStackLeaf(uintptr_t address, uintptr_t start, size_t length, vm_prot_t prot) {
    mach_vm_address_t region = address;
    mach_vm_size_t bytes = 0;
    natural_t depth = UINT32_MAX;
    vm_region_submap_info_data_64_t info = {};
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    return !mach_vm_region_recurse(mach_task_self(), &region, &bytes, &depth, (vm_region_recurse_info_t)&info, &count) &&
        count >= VM_REGION_SUBMAP_INFO_V0_COUNT_64 && !info.is_submap && region == start && bytes == length && info.user_tag == VM_MEMORY_STACK &&
        info.protection == prot;
}

static int trialPrepareStackHandoff(TrialScratchStack *s, LSBForkWireRange stack, uintptr_t oldSP) {
    pthread_t self = pthread_self();
    uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);
    size_t bytes = pthread_get_stacksize_np(self);
    if (!pthread_main_np() || !bytes || bytes >= top || oldSP < top - bytes || oldSP >= top) return ENOTSUP;
    if (!stack.size || stack.start < vm_page_size || stack.size > UINTPTR_MAX - stack.start || (stack.start | stack.size) & (vm_page_size - 1)) return EPROTO;
    uintptr_t bottom = top - bytes;
    TrialMainPthread *t = (TrialMainPthread *)self;
    if ((uintptr_t)t > UINTPTR_MAX - sizeof(*self) || ((uintptr_t)t < top && bottom < (uintptr_t)t + sizeof(*self)) ||
        t->bounds.top != top || t->bounds.bottom != bottom || t->freeSize > UINTPTR_MAX - t->freeAddress ||
        ((uintptr_t)t >= t->freeAddress && (uintptr_t)t < t->freeAddress + t->freeSize)) return ENOTSUP;
    *s = (TrialScratchStack){.native = {top, bottom}, .donor = {stack.start + stack.size, stack.start}, .pthreadBounds = &t->bounds};
    return 0;
}

static bool trialPlanScratchStack(TrialScratchStack *s, uintptr_t oldSP) {
    const size_t page = vm_page_size;
    uintptr_t top = s->native.top, bottom = s->native.bottom, donorEnd = s->donor.top, landing = oldSP & ~(page - 1);
    // Keep the live native frames and XNU's p_user_stack argument window. Revisit this upper suffix on the way out to free scratch.
    if ((top | bottom) & (page - 1) || donorEnd <= bottom || donorEnd >= landing || landing - donorEnd < LSBForkStackLandingHeadroom + page ||
        !trialStackLeaf(oldSP, bottom, top - bottom, VM_PROT_READ | VM_PROT_WRITE) || forkPthreadSingleMain()) return false;
    uintptr_t start = bottom;
    // XNU can put a one-page, inaccessible VM_MEMORY_STACK guard immediately below the original leaf. No other adjacent mapping is ours.
    uintptr_t guard = s->donor.bottom - page;
    if (guard < bottom && bottom >= page && trialStackLeaf(bottom - page, bottom - page, page, VM_PROT_NONE)) start -= page;
    s->releaseStart = start < guard ? guard : start;
    s->releaseEnd = donorEnd;
    s->landingSP = landing;
    return true;
}

static int trialAllocateScratchStack(TrialScratchStack *s, uintptr_t hint) {
    size_t page = vm_page_size, size = 1024 * 1024;
    void *mapping = mmap((void *)hint, size + 2 * page, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mapping == MAP_FAILED) return errno;
    uintptr_t bottom = (uintptr_t)mapping + page;
    if (mprotect((void *)bottom, size, PROT_READ | PROT_WRITE)) {
        int error = errno;
        munmap(mapping, size + 2 * page);
        return error;
    }
    s->scratch = (TrialStackBounds){bottom + size, bottom};
    return 0;
}

__attribute__((naked, noreturn)) static void trialPivotStack(uintptr_t sp, TrialStackBounds *fields, const TrialStackBounds *bounds,
                                                            void (*entry)(void *), void *argument) {
    __asm__("ldp x8, x9, [x2]\n stp x8, x9, [x1]\n mov sp, x0\n mov x0, x4\n mov x29, xzr\n mov x30, xzr\n br x3");
}

static int trialReclaimStack(TrialScratchStack *s) {
    uintptr_t sp = (uintptr_t)&sp;
    if (sp < s->scratch.bottom || sp >= s->scratch.top || s->pthreadBounds->bottom != s->scratch.bottom || s->pthreadBounds->top != s->scratch.top) return EFAULT;
    // The retained suffix is briefly a stack again during teardown. A signal handler overflowing it must fault before it reaches the copied donor stack.
    if (mprotect((void *)s->releaseEnd, vm_page_size, PROT_NONE)) return EADDRINUSE;
    if (mach_vm_deallocate(mach_task_self(), s->releaseStart, s->releaseEnd - s->releaseStart)) return EADDRINUSE;
    return 0;
}

__attribute__((noreturn)) static void trialResumeOnDonorStack(TrialScratchStack *s, const LSBForkContext *caller) {
    // Publish pthread's bounds at the SP switch, after observer-side libc calls and before the first donor instruction.
    trialPivotStack(caller->sp, s->pthreadBounds, &s->donor, (void (*)(void *))LSBResumeForkContext, (void *)caller);
}

__attribute__((noreturn, no_stack_protector)) static void trialLeaveScratch(void *argument) {
    TrialScratchStack *s = argument;
    if (munmap((void *)(s->scratch.bottom - vm_page_size), s->scratch.top - s->scratch.bottom + 2 * vm_page_size)) _exit(126);
    trialResumeOnDonorStack(s, &s->continuation);
}

__attribute__((noreturn, no_stack_protector)) static void trialResumeFromScratch(TrialScratchStack *s, const LSBForkContext *caller) {
    s->continuation = *caller;
    TrialStackBounds suffix = {s->native.top, s->releaseEnd + vm_page_size};
    // Use the preserved native suffix for munmap. Nothing calls C on the copied donor stack or writes beneath its saved SP.
    trialPivotStack(s->landingSP, s->pthreadBounds, &suffix, trialLeaveScratch, s);
}
