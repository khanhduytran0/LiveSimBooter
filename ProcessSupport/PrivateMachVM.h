#pragma once
#include <mach/mach.h>

// Native libSystem Mach VM interfaces. The iPhone SDK omits mach_vm.h, but exports these LP64 entry points.
#ifdef __cplusplus
extern "C" {
#endif
kern_return_t mach_vm_map(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t size, mach_vm_offset_t mask, int flags,
                          mem_entry_name_port_t object, memory_object_offset_t offset, boolean_t copy,
                          vm_prot_t currentProtection, vm_prot_t maximumProtection, vm_inherit_t inheritance);
kern_return_t mach_vm_deallocate(vm_map_t task, mach_vm_address_t address, mach_vm_size_t size);
kern_return_t mach_vm_region_recurse(vm_map_read_t task, mach_vm_address_t *address, mach_vm_size_t *size, natural_t *depth,
                                     vm_region_recurse_info_t info, mach_msg_type_number_t *count);
kern_return_t mach_vm_remap(vm_map_t target, mach_vm_address_t *address, mach_vm_size_t size, mach_vm_offset_t mask, int flags,
                            vm_map_t source, mach_vm_address_t sourceAddress, boolean_t copy, vm_prot_t *current, vm_prot_t *maximum, vm_inherit_t inheritance);
#ifdef __cplusplus
}
#endif
