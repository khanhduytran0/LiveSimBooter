#pragma once

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/vm_map.h>

// These callbacks use executable-local code/data and kernel traps only.
namespace lsb {
decltype(::vm_allocate) vm_allocate;
decltype(::vm_deallocate) vm_deallocate;
decltype(::vm_protect) vm_protect;
decltype(::vm_copy) vm_copy;
decltype(::vm_region_recurse_64) vm_region_recurse_64;
decltype(::mach_thread_self) mach_thread_self;
decltype(::mach_port_deallocate) mach_port_deallocate;
decltype(::mach_port_insert_right) mach_port_insert_right;
decltype(::mach_port_allocate) mach_port_allocate;
decltype(::mach_port_construct) mach_port_construct;
decltype(::mach_port_destruct) mach_port_destruct;
decltype(::task_self_trap) task_self_trap;
decltype(::mach_timebase_info) mach_timebase_info;
decltype(::mach_absolute_time) mach_absolute_time;
decltype(::thread_switch) thread_switch;
decltype(::mach_msg) mach_msg;
decltype(::mach_msg_destroy) mach_msg_destroy;
decltype(::task_info) task_info;
decltype(::thread_info) thread_info;
decltype(::task_register_dyld_image_infos) task_register_dyld_image_infos;
decltype(::task_unregister_dyld_image_infos) task_unregister_dyld_image_infos;
decltype(::task_get_dyld_image_infos) task_get_dyld_image_infos;
decltype(::task_register_dyld_shared_cache_image_info) task_register_dyld_shared_cache_image_info;
decltype(::task_register_dyld_set_dyld_state) task_register_dyld_set_dyld_state;
decltype(::task_register_dyld_get_process_state) task_register_dyld_get_process_state;
decltype(::task_dyld_process_info_notify_get) task_dyld_process_info_notify_get;
}
