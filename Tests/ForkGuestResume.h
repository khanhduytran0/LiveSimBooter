#pragma once
#include "ForkLibcCheckpoint.h"
#include "ForkPthreadRegistry.h"
#include "../Guest/PrivateMachVM.h"
#include <crt_externs.h>
#include "../ProcessSupport/ChildStatus.h"
#include <sys/mman.h>

extern uintptr_t __stack_chk_guard;

// Validate the program's original vectors in inherited memory before publishing their addresses to libc.
static inline int trialCheckVector(TrialOffer *offer, uint64_t vector, int expected) {
    if (!vector || (vector & 7)) return EPROTO;
    size_t budget = LSB_STRING_BYTES;
    for (unsigned i = 0; i <= LSB_MAX_VECTOR; ++i) {
        uintptr_t slot, string;
        if (__builtin_add_overflow(vector, (uintptr_t)i * sizeof(string), &slot) || trialSnapshotCovers(offer, slot, sizeof(string)) ||
            trialReadLocal(slot, &string, sizeof(string))) return EFAULT;
        if (!string) return expected < 0 || i == (unsigned)expected ? 0 : EPROTO;
        if (expected >= 0 && i >= (unsigned)expected) return EPROTO;
        for (;;) {
            char block[128];
            size_t bytes = vm_page_size - (string & (vm_page_size - 1));
            if (bytes > sizeof(block)) bytes = sizeof(block);
            if (bytes > budget) bytes = budget;
            if (!bytes) return E2BIG;
            if (trialSnapshotCovers(offer, string, bytes) || trialReadLocal(string, block, bytes)) return EFAULT;
            const char *end = memchr(block, 0, bytes);
            budget -= end ? (size_t)(end - block + 1) : bytes;
            if (end) break;
            string += bytes;
        }
    }
    return E2BIG;
}

// These entries describe rights and private storage of this native task. Carry the caller's ordinary environment across unchanged.
static inline int trialRestoreProgramVars(const TrialOffer *offer) {
    static char *environment[LSB_MAX_VECTOR + 1];
    static const char *const privateKeys[] = {"LSB_BROKER_PORT", LSB_CHILD_STATUS_ENV, LSB_CHILD_WAKE_FD_ENV, "LSB_GUEST_PID", "LSB_GUEST_PPID"};
    char **source = (char **)(uintptr_t)offer->envp;
    unsigned i = 0;
    while (i < LSB_MAX_VECTOR && source[i]) {
        environment[i] = source[i];
        for (unsigned k = 0; k < sizeof(privateKeys) / sizeof(privateKeys[0]); ++k) {
            size_t length = strlen(privateKeys[k]);
            if (strncmp(source[i], privateKeys[k], length) || source[i][length] != '=') continue;
            char **own = *_NSGetEnviron();
            while (*own && (strncmp(*own, privateKeys[k], length) || (*own)[length] != '=')) ++own;
            if (!*own) return ENOENT;
            environment[i] = *own;
            break;
        }
        ++i;
    }
    if (source[i]) return E2BIG;
    environment[i] = NULL;
    *_NSGetArgc() = (int)offer->argc;
    *_NSGetArgv() = (char **)(uintptr_t)offer->argv;
    *_NSGetEnviron() = environment;
    return 0;
}

typedef struct { vm_address_t page; vm_prot_t protection; } TrialGuardPage;
static inline int trialPrepareGuard(uintptr_t guard, uintptr_t libcPC, TrialGuardPage *page) {
    *page = (TrialGuardPage){};
    if (__stack_chk_guard == guard) return 0;
    Dl_info owner, libc;
    if (!guard || !dladdr(&__stack_chk_guard, &owner) || !dladdr((const void *)libcPC, &libc) || owner.dli_fbase != libc.dli_fbase) return ENOTSUP;
    vm_address_t target = (uintptr_t)&__stack_chk_guard & ~(vm_page_size - 1);
    vm_address_t start = target;
    vm_size_t size = 0;
    natural_t depth = UINT32_MAX;
    vm_region_submap_info_data_64_t info = {};
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    if (vm_region_recurse_64(mach_task_self(), &start, &size, &depth, (vm_region_recurse_info_t)&info, &count) ||
        count < VM_REGION_SUBMAP_INFO_V0_COUNT_64 || info.is_submap || start > target || size <= target - start ||
        info.inheritance != VM_INHERIT_COPY || (info.protection & VM_PROT_EXECUTE) || !(info.protection & VM_PROT_READ) ||
        info.share_mode == SM_SHARED || info.share_mode == SM_TRUESHARED) return ENOTSUP;
    if (!(info.protection & VM_PROT_WRITE) &&
        vm_protect(mach_task_self(), target, vm_page_size, FALSE, info.protection | VM_PROT_WRITE | VM_PROT_COPY)) return EACCES;
    page->page = target;
    page->protection = info.protection;
    return 0;
}

// Switch the guest's stack guard only on a no-return path. Frames copied from the parent already contain this value.
__attribute__((noreturn, unused, no_stack_protector)) static void trialReturnToGuest(const TrialOffer *offer, TrialGuardPage page) {
    if (page.page) {
        __stack_chk_guard = offer->guard;
        if (!(page.protection & VM_PROT_WRITE) && vm_protect(mach_task_self(), page.page, vm_page_size, FALSE, page.protection)) _exit(126);
    }
    // Its worker can now use child-side pthread state and the inherited stack guard.
    if (LSBTrialResumeTerminalWitness()) _exit(126);
    LSBResumeForkContext(&offer->caller);
}
