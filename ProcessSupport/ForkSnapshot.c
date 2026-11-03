#include "ForkSnapshot.h"
#include "PrivateMachVM.h"
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>

static int vmError(kern_return_t kr) {
    switch (kr) {
        case KERN_SUCCESS: return 0;
        case KERN_NO_SPACE: return EADDRINUSE;
        case KERN_INVALID_ADDRESS: return EFAULT;
        case KERN_PROTECTION_FAILURE: return EACCES;
        case KERN_RESOURCE_SHORTAGE: return ENOMEM;
        case KERN_INVALID_TASK: return ESRCH;
        default: return EIO;
    }
}

// region_recurse returns the next leaf even when the requested address is a hole.
static int checkRange(vm_map_t task, const LSBForkMapStep *step, const LSBForkMapRange *expected) {
    uint64_t end = step->start + step->size;
    for (uint64_t pos = step->start; pos < end;) {
        mach_vm_address_t start = pos;
        mach_vm_size_t size = 0;
        natural_t depth = UINT32_MAX;
        vm_region_submap_info_data_64_t info = {0};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        kern_return_t kr = mach_vm_region_recurse(task, &start, &size, &depth, (vm_region_recurse_info_t)&info, &count);
        if (kr == KERN_INVALID_ADDRESS) return expected ? EFAULT : 0;
        if (kr != KERN_SUCCESS) return vmError(kr);
        if (count < VM_REGION_SUBMAP_INFO_V0_COUNT_64 || info.is_submap || !size || start > UINT64_MAX - size || start + size <= pos) return ENOTSUP;
        if (!expected) return start < end ? EADDRINUSE : 0;
        if (start > pos) return EFAULT;
        if (info.protection != expected->currentProtection || info.max_protection != expected->maxProtection ||
            info.inheritance != expected->inheritance) return ENOTSUP;
        pos = start + size;
    }
    return 0;
}

static int applyStep(vm_map_t parent, vm_map_t child, const LSBForkMapStep *step, const LSBForkMapRange *range) {
    if (step->action == LSB_FORK_MAP_SKIP || step->action == LSB_FORK_MAP_REUSE) return 0;
    mach_vm_address_t actual = step->start;
    kern_return_t kr;
    if (step->action == LSB_FORK_MAP_GUARD) {
        kr = mach_vm_map(child, &actual, step->size, 0, VM_FLAGS_FIXED, MACH_PORT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_NONE, range->inheritance);
    } else {
        vm_prot_t current = VM_PROT_NONE, maximum = VM_PROT_NONE;
        kr = mach_vm_remap(child, &actual, step->size, 0, VM_FLAGS_FIXED, parent, step->start, step->action == LSB_FORK_MAP_COPY,
                           &current, &maximum, range->inheritance);
    }
    if (kr != KERN_SUCCESS) return vmError(kr);
    if (actual != step->start) return EFAULT;
    return checkRange(child, step, range);
}

int LSBCloneForkRanges(vm_map_t parent, vm_map_t child, const LSBForkMapRange *source, size_t sourceCount, const LSBForkMapRange *occupied, size_t childCount) {
    if (!MACH_PORT_VALID(parent) || !MACH_PORT_VALID(child) || parent == child) return EINVAL;
    task_vm_info_data_t vm = {0};
    mach_msg_type_number_t words = TASK_VM_INFO_COUNT;
    kern_return_t kr = task_info(child, TASK_VM_INFO, (task_info_t)&vm, &words);
    if (kr != KERN_SUCCESS) return vmError(kr);
    if (words < TASK_VM_INFO_REV2_COUNT || vm.page_size <= 0) return ENOTSUP;
    size_t count = 0;
    int error = LSBPlanForkMappings(source, sourceCount, occupied, childCount, vm.min_address, vm.max_address, vm.page_size, NULL, 0, &count);
    if (error || !count) return error;
    LSBForkMapStep *steps = calloc(count, sizeof(*steps));
    if (!steps) return ENOMEM;
    error = LSBPlanForkMappings(source, sourceCount, occupied, childCount, vm.min_address, vm.max_address, vm.page_size, steps, count, &count);
    for (size_t i = 0; !error && i < count; ++i) {
        const LSBForkMapStep *step = &steps[i];
        const LSBForkMapRange *range = &source[step->sourceIndex];
        error = checkRange(parent, step, range);
        if (step->action == LSB_FORK_MAP_SKIP) continue;
        if (!error) error = checkRange(child, step, step->action == LSB_FORK_MAP_REUSE ? range : NULL);
    }
    for (size_t i = 0; !error && i < count; ++i) error = applyStep(parent, child, &steps[i], &source[steps[i].sourceIndex]);
    free(steps);
    return error;
}
