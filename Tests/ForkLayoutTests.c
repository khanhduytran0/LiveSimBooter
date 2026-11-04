#include "../ProcessSupport/ForkLayout.h"
#include <assert.h>
#include <stdio.h>

static void checkGuard(mach_vm_address_t cursor, mach_vm_address_t end) {
    while (cursor < end) {
        mach_vm_address_t start = cursor;
        mach_vm_size_t size = 0;
        natural_t depth = UINT32_MAX;
        vm_region_submap_info_data_64_t info = {};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        assert(!mach_vm_region_recurse(mach_task_self(), &start, &size, &depth, (vm_region_recurse_info_t)&info, &count));
        assert(start <= cursor && start + size > cursor && !info.is_submap && info.user_tag == LSB_FORK_DONOR_GUARD_TAG);
        assert(!info.protection && !info.max_protection && info.inheritance == VM_INHERIT_NONE && !info.pages_resident);
        cursor = start + size;
    }
}

int main(void) {
    vm_map_t self = mach_task_self();
    size_t p = vm_page_size;
    mach_vm_address_t base = 0;
    assert(!mach_vm_map(self, &base, 8*p, 0, VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, FALSE, VM_PROT_READ|VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_COPY));
    *(int *)(uintptr_t)base = 37;
    *(int *)(uintptr_t)(base+3*p) = 19;
    *(int *)(uintptr_t)(base+7*p) = 41;
    assert(!mach_vm_deallocate(self, base+p, 2*p) && !mach_vm_deallocate(self, base+4*p, 3*p));
    assert(LSBForkGuardVacantRanges(base+p, base+7*p));
    checkGuard(base+p, base+3*p);
    checkGuard(base+4*p, base+7*p);
    assert(LSBForkGuardVacantRanges(base, base+7*p)); // Repeat with the first queried address already occupied.
    assert(*(int *)(uintptr_t)base == 37 && *(int *)(uintptr_t)(base+3*p) == 19 && *(int *)(uintptr_t)(base+7*p) == 41);
    assert(!LSBForkGuardVacantRanges(base+p, base));
    assert(!mach_vm_deallocate(self, base, 8*p));
    puts("Fork layout filled only vacant ranges with nonresident, non-inherited guards");
}
