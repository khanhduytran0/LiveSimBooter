#include "../ProcessSupport/ForkImageGuard.h"
#include <assert.h>
#include <stdio.h>

static mach_vm_size_t guardLeafSize(mach_vm_address_t address) {
    mach_vm_address_t start = address;
    mach_vm_size_t size;
    natural_t depth = UINT32_MAX;
    vm_region_submap_info_data_64_t info = {};
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    assert(mach_vm_region_recurse(mach_task_self(), &start, &size, &depth, (vm_region_recurse_info_t)&info, &count) == KERN_SUCCESS);
    assert(start == address && !info.is_submap && info.user_tag == LSB_FORK_IMAGE_GUARD_TAG);
    return size;
}

int main(void) {
    const mach_vm_size_t capacity = LSB_GUEST_MAIN_IMAGE_CAPACITY;
    mach_vm_address_t main = UINT64_C(0x800000000);
    assert(mach_vm_map(mach_task_self(), &main, capacity, 0, VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, FALSE,
                       VM_PROT_NONE, VM_PROT_NONE, VM_INHERIT_NONE) == KERN_SUCCESS);
    assert(main >= LSB_SLIDABLE_CACHE_PREFERRED_BASE + LSB_GUEST_MAIN_IMAGE_OFFSET);
    const uint64_t base = main - LSB_GUEST_MAIN_IMAGE_OFFSET + LSB_SLIDABLE_CACHE_PREFERRED_BASE, bootstrap = main + capacity;
    // An existing range, even one with the expected address, size and permissions, belongs to its original owner.
    assert(LSBReserveForkImageGuard(base) == EADDRINUSE);
    assert(LSBReleaseForkImageGuard(bootstrap, main) == EADDRINUSE);
    assert(LSBReserveForkImageGuard(base) == EADDRINUSE);
    assert(mach_vm_deallocate(mach_task_self(), main, capacity) == KERN_SUCCESS);

    assert(!LSBReserveForkImageGuard(base));
    assert(LSBReserveForkImageGuard(base) == EADDRINUSE);
    assert(LSBReleaseForkImageGuard(bootstrap + vm_page_size, main) == EPROTO);
    assert(LSBReleaseForkImageGuard(bootstrap, main + vm_page_size) == EPROTO);
    mach_vm_address_t tail = bootstrap - vm_page_size;
    assert(vm_behavior_set(mach_task_self(), tail, vm_page_size, VM_BEHAVIOR_RANDOM) == KERN_SUCCESS);
    assert(guardLeafSize(main) < capacity && guardLeafSize(tail) == vm_page_size);
    assert(!LSBReleaseForkImageGuard(bootstrap, main));
    assert(LSBReleaseForkImageGuard(bootstrap, main) == EADDRINUSE);

    // An incomplete guard and one with a foreign final leaf must leave the earlier tagged pages in place.
    assert(!LSBReserveForkImageGuard(base));
    assert(mach_vm_deallocate(mach_task_self(), tail, vm_page_size) == KERN_SUCCESS);
    assert(LSBReleaseForkImageGuard(bootstrap, main) == EADDRINUSE && guardLeafSize(main));
    assert(mach_vm_map(mach_task_self(), &tail, vm_page_size, 0, VM_FLAGS_FIXED, MACH_PORT_NULL, 0, FALSE,
                       VM_PROT_NONE, VM_PROT_NONE, VM_INHERIT_NONE) == KERN_SUCCESS);
    assert(LSBReleaseForkImageGuard(bootstrap, main) == EADDRINUSE && guardLeafSize(main));
    assert(mach_vm_deallocate(mach_task_self(), main, capacity) == KERN_SUCCESS);
    puts("Fork image guard: occupied slot, split guard, missing or foreign tail, mismatched offer and one-shot release passed");
    return 0;
}
