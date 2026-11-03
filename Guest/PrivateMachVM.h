#ifndef LSB_PRIVATE_MACH_VM_H
#define LSB_PRIVATE_MACH_VM_H

#include "../ProcessSupport/PrivateMachVM.h"
#include <stdint.h>

// The iOS SDK hides mach_vm.h; the arm64 kernel/libSystem ABI uses these 64-bit addresses and sizes.
extern kern_return_t mach_vm_allocate(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t size, int flags);
extern kern_return_t mach_vm_region(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t *size, vm_region_flavor_t flavor,
                                    vm_region_info_t info, mach_msg_type_number_t *count, mach_port_t *object);
extern kern_return_t mach_vm_protect(vm_map_t task, mach_vm_address_t address, mach_vm_size_t size, boolean_t maximum, vm_prot_t protection);
extern int __shared_region_check_np(uint64_t *address);

#endif
