#include "../Host/ForkSnapshotBroker.h"
#include "../ProcessSupport/ForkSnapshot.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

enum { Parent = 101, Child = 102, ParentToken = 301, ChildToken = 302 };
enum { Start = 0x8000, PageSize = 0x4000 };
static unsigned held, cloned;
static mach_port_t refuseTask;
static uint64_t regionStart, regionSize;
static int cloneResult;

kern_return_t task_suspend2(task_read_t task, task_suspension_token_t *token) {
    assert(task == Parent || task == Child);
    if (task == refuseTask) return KERN_FAILURE;
    assert(held == (task == Parent ? 0u : 1u));
    held++;
    *token = task == Parent ? ParentToken : ChildToken;
    return KERN_SUCCESS;
}

kern_return_t task_resume2(task_suspension_token_t token) {
    assert(held && token == (held == 2 ? ChildToken : ParentToken));
    held--;
    return KERN_SUCCESS;
}

kern_return_t mach_vm_region_recurse(vm_map_read_t task, mach_vm_address_t *address, mach_vm_size_t *size, natural_t *depth,
                                      vm_region_recurse_info_t raw, mach_msg_type_number_t *words) {
    assert(held == 2 && task == Parent && *address == Start && *depth == UINT32_MAX && *words == VM_REGION_SUBMAP_INFO_COUNT_64);
    *address = regionStart;
    *size = regionSize;
    vm_region_submap_info_data_64_t *info = (vm_region_submap_info_data_64_t *)raw;
    info->inheritance = VM_INHERIT_COPY;
    info->protection = VM_PROT_READ;
    info->max_protection = VM_PROT_READ | VM_PROT_WRITE;
    return KERN_SUCCESS;
}

int LSBCloneForkRanges(vm_map_t parent, vm_map_t child, const LSBForkMapRange *source, size_t count, const LSBForkMapRange *occupied, size_t occupiedCount) {
    assert(held == 2 && parent == Parent && child == Child && count == 1 && !occupied && !occupiedCount);
    assert(source->start == Start && source->size == PageSize && source->currentProtection == VM_PROT_READ);
    assert(source->maxProtection == (VM_PROT_READ | VM_PROT_WRITE) && source->inheritance == VM_INHERIT_COPY && !source->immutableImage);
    cloned++;
    return cloneResult;
}

int main(void) {
    LSBForkWireRange range = {Start, PageSize};
    regionStart = Start - PageSize;
    regionSize = PageSize * 2;
    assert(LSBCopyOfferedForkRanges(Parent, Parent, &range, 1) == EINVAL);
    refuseTask = Parent;
    assert(LSBCopyOfferedForkRanges(Parent, Child, &range, 1) == EBUSY && !held);
    refuseTask = Child;
    assert(LSBCopyOfferedForkRanges(Parent, Child, &range, 1) == EBUSY && !held);
    refuseTask = MACH_PORT_NULL;
    assert(!LSBCopyOfferedForkRanges(Parent, Child, &range, 1) && !held && cloned == 1);
    cloneResult = EADDRINUSE;
    assert(LSBCopyOfferedForkRanges(Parent, Child, &range, 1) == EADDRINUSE && !held && cloned == 2);
    regionSize--;
    assert(LSBCopyOfferedForkRanges(Parent, Child, &range, 1) == ENOTSUP && !held && cloned == 2);
    regionStart = Start + PageSize;
    assert(LSBCopyOfferedForkRanges(Parent, Child, &range, 1) == EFAULT && !held && cloned == 2);
    puts("fork snapshot broker checks passed");
    return 0;
}
