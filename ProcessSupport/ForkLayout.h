#pragma once

#include "PrivateMachVM.h"
#include <mach/vm_statistics.h>
#include <stdbool.h>
#include <stdint.h>

// In 23A343 arm64 libsystem_malloc, mvm_aslr_init seeds the first heap address at
// 0x15fd00000 - ((malloc_entropy[1] & 127) << 23). Leave the observer's native PIE and this bootstrap heap
// room to grow. The donor still has 448 MiB below the native shared region for loose signed file mappings.
#define LSB_FORK_DONOR_FLOOR UINT64_C(0x164000000)
#define LSB_FORK_DONOR_GUARD_TAG VM_MEMORY_APPLICATION_SPECIFIC_14

static inline bool LSBForkGuardVacantRanges(mach_vm_address_t cursor, mach_vm_address_t end) {
    if (cursor > end) return false;
    const vm_map_t task = mach_task_self();
    // This is an allocation preference. Bound the map walk even if a native thread changes the layout.
    for (unsigned visits = 0; cursor < end && visits < 1024; ++visits) {
        mach_vm_address_t start = cursor;
        mach_vm_size_t size = 0;
        vm_region_submap_info_data_64_t info = {};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        natural_t depth = UINT32_MAX;
        kern_return_t kr = mach_vm_region_recurse(task, &start, &size, &depth, (vm_region_recurse_info_t)&info, &count);
        if (kr == KERN_SUCCESS) {
            if (count < VM_REGION_SUBMAP_INFO_V0_COUNT_64 || info.is_submap || !size || size > UINT64_MAX - start || start + size <= cursor) return false;
            if (start <= cursor) {
                cursor = start + size < end ? start + size : end;
                continue;
            }
        } else if (kr == KERN_INVALID_ADDRESS) start = end;
        else return false;
        mach_vm_address_t limit = start < end ? start : end;
        mach_vm_address_t address = cursor;
        kr = mach_vm_map(task, &address, limit - cursor, 0, VM_FLAGS_FIXED | VM_MAKE_TAG((unsigned)LSB_FORK_DONOR_GUARD_TAG),
                         MACH_PORT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_NONE, VM_INHERIT_NONE);
        if (kr == KERN_NO_SPACE) continue; // Re-query: FIXED without OVERWRITE leaves any new occupant intact.
        if (kr != KERN_SUCCESS || address != cursor) return false;
        cursor = limit;
    }
    return cursor == end;
}

static inline void LSBPreferForkDonorLayout(void) {
    task_vm_info_data_t vm = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count) != KERN_SUCCESS || count < TASK_VM_INFO_REV2_COUNT ||
        vm.min_address >= LSB_FORK_DONOR_FLOOR || vm.max_address < LSB_FORK_DONOR_FLOOR) return;
    // No physical pages are committed. Completed reservations remain with the native task and never enter the guest fork offer.
    (void)LSBForkGuardVacantRanges(vm.min_address, LSB_FORK_DONOR_FLOOR);
}
