#pragma once

#include "GuestImagePlacement.h"
#include "PrivateMachVM.h"
#include <mach/vm_statistics.h>

// Only the fork observer uses this tag. An untouched, inaccessible VM reservation costs no resident pages.
#define LSB_FORK_IMAGE_GUARD_TAG VM_MEMORY_APPLICATION_SPECIFIC_16

static inline int LSBReserveForkImageGuard(uint64_t cacheBase) {
    task_vm_info_data_t vm = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count) != KERN_SUCCESS || count < TASK_VM_INFO_REV2_COUNT) return EIO;
    uint64_t address;
    int error = LSBGuestImageAddress(cacheBase, LSB_NATIVE_SUPPORT_CACHE_SPAN, 0, LSB_GUEST_MAIN_IMAGE_CAPACITY, vm.page_size,
                                     vm.max_address, LSBGuestMainImage, &address);
    if (error) return error;
    mach_vm_address_t dest = address;
    kern_return_t kr = mach_vm_map(mach_task_self(), &dest, LSB_GUEST_MAIN_IMAGE_CAPACITY, 0, VM_FLAGS_FIXED | VM_MAKE_TAG((unsigned)LSB_FORK_IMAGE_GUARD_TAG),
                                   MACH_PORT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_NONE, VM_INHERIT_NONE);
    if (kr == KERN_NO_SPACE) return EADDRINUSE;
    if (kr != KERN_SUCCESS || dest != address) return EFAULT;
    return 0;
}

// iOS can split one anonymous reservation into multiple 128 MiB leaves. Validate them all before releasing any.
static inline int LSBCheckForkGuard(uint64_t address, uint64_t end, unsigned tag) {
    if (address >= end) return EINVAL;
    for (mach_vm_address_t cursor = address; cursor < end;) {
        mach_vm_address_t start = cursor;
        mach_vm_size_t size = 0;
        natural_t depth = UINT32_MAX;
        vm_region_submap_info_data_64_t info = {};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        kern_return_t kr = mach_vm_region_recurse(mach_task_self(), &start, &size, &depth, (vm_region_recurse_info_t)&info, &count);
        if (kr != KERN_SUCCESS || count < VM_REGION_SUBMAP_INFO_V0_COUNT_64 || start != cursor || !size || size > end - cursor ||
            info.is_submap || info.user_tag != tag || info.protection != VM_PROT_NONE || info.max_protection != VM_PROT_NONE ||
            info.inheritance != VM_INHERIT_NONE) return EADDRINUSE;
        cursor += size;
    }
    return 0;
}

// Match the observer's own adjacent executable and the entire tagged guard before deallocating anything.
static inline int LSBReleaseForkImageGuard(uintptr_t bootstrapHeader, uint64_t offeredHeader) {
    if (bootstrapHeader < LSB_GUEST_MAIN_IMAGE_CAPACITY || bootstrapHeader - LSB_GUEST_MAIN_IMAGE_CAPACITY != offeredHeader) return EPROTO;
    int error = LSBCheckForkGuard(offeredHeader, bootstrapHeader, LSB_FORK_IMAGE_GUARD_TAG);
    if (error) return error;
    return mach_vm_deallocate(mach_task_self(), offeredHeader, LSB_GUEST_MAIN_IMAGE_CAPACITY) == KERN_SUCCESS ? 0 : EIO;
}
