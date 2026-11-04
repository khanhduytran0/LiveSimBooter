#include "ForkSnapshotBroker.h"
#include "../ProcessSupport/ForkSnapshot.h"
#include "../ProcessSupport/PrivateMachVM.h"
#include <errno.h>
#include <stdlib.h>

static int describeForkRange(mach_port_t task, LSBForkWireRange offered, LSBForkMapRange *result) {
    mach_vm_address_t start = offered.start;
    mach_vm_size_t size = 0;
    natural_t depth = UINT32_MAX;
    vm_region_submap_info_data_64_t info = {0};
    mach_msg_type_number_t words = VM_REGION_SUBMAP_INFO_COUNT_64;
    kern_return_t kr = mach_vm_region_recurse(task, &start, &size, &depth, (vm_region_recurse_info_t)&info, &words);
    if (kr == KERN_INVALID_ADDRESS || start > offered.start) return EFAULT;
    if (kr != KERN_SUCCESS) return EIO;
    if (words < VM_REGION_SUBMAP_INFO_V0_COUNT_64 || info.is_submap || !size || size > UINT64_MAX - start || offered.start >= start + size ||
        (info.max_protection & ~VM_PROT_ALL) ||
        (info.protection & ~info.max_protection) || info.inheritance > VM_INHERIT_NONE) return ENOTSUP;

    // No verified immutable provenance is available for a guest's offered address. The destination must be vacant unless inheritance is NONE.
    *result = (LSBForkMapRange){ .start = offered.start, .size = offered.size < start + size - offered.start ? offered.size : start + size - offered.start,
        .currentProtection = info.protection, .maxProtection = info.max_protection, .inheritance = info.inheritance, .dirty = true };
    return 0;
}

int LSBCopyOfferedForkRanges(mach_port_t parent, mach_port_t child, const LSBForkWireRange *offered, size_t count) {
    if (!MACH_PORT_VALID(parent) || !MACH_PORT_VALID(child) || parent == child || !offered || !count || count > LSB_MAX_FORK_RANGES) return EINVAL;
    // The parent may have other live threads even though the accepting child's caller is blocked in its synchronous RPC.
    // These task-specific send-once tokens balance only our own suspension. Losing the broker also releases its holds.
    task_suspension_token_t parentHold = MACH_PORT_NULL, childHold = MACH_PORT_NULL;
    if (task_suspend2(parent, &parentHold) != KERN_SUCCESS) return EBUSY;
    int error = task_suspend2(child, &childHold) == KERN_SUCCESS ? 0 : EBUSY;
    // A contiguous offer can span many differently protected VM leaves. Preserve each leaf's kernel-reported attributes.
    LSBForkMapRange *source = NULL;
    size_t capacity = 0, used = 0, total = 0;
    const size_t page = 16384; // Guest transport alignment.
    for (size_t i = 0; !error && i < count; ++i) {
        LSBForkWireRange remaining = offered[i];
        if (!remaining.start || !remaining.size || ((remaining.start | remaining.size) & (page - 1)) ||
            remaining.size > UINT64_MAX - remaining.start || remaining.size > LSB_MAX_FORK_BYTES - total) { error = EINVAL; break; }
        total += remaining.size;
        while (!error && remaining.size) {
            if (used == capacity) {
                size_t next = capacity ? capacity * 2 : count;
                LSBForkMapRange *larger = realloc(source, next * sizeof(*source));
                if (!larger) { error = ENOMEM; break; }
                source = larger;
                capacity = next;
            }
            error = describeForkRange(parent, remaining, &source[used]);
            if (error) break;
            size_t length = source[used].size;
            if (length & (page - 1)) { error = ENOTSUP; break; }
            ++used;
            remaining.start += length;
            remaining.size -= length;
        }
    }
    if (!error) error = LSBCloneForkRanges(parent, child, source, used, NULL, 0);
    free(source);
    if (MACH_PORT_VALID(childHold) && task_resume2(childHold) != KERN_SUCCESS && !error) error = ESRCH;
    if (task_resume2(parentHold) != KERN_SUCCESS && !error) error = ESRCH;
    return error;
}
