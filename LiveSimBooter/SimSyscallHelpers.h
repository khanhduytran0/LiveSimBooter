/*
 * Copyright (c) 2004-2013 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

// Modified 2026-10-23: extract the version 18 callback ABI for LiveSimBooter and add layout checks.
// Modified 2026-10-24: expose the app-resident debugger notifier and relocated process-info selection.
#pragma once

// ABI adapted from dyldSyscallInterface.h at apple-oss-distributions/dyld fd8d0c4d52320ebf64db34f3cb280310d905c5ae.
// Keep every slot, including disabled tracing/monitor callbacks: dyld_sim accesses these by offset.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/dyld_images.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <sys/attr.h>
#include <sys/fcntl.h>
#include <sys/fsgetpath.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

struct SimSyscallHelpers {
    uintptr_t version;
    decltype(&::open) open;
    decltype(&::close) close;
    decltype(&::pread) pread;
    decltype(&::write) write;
    decltype(&::mmap) mmap;
    decltype(&::munmap) munmap;
    decltype(&::madvise) madvise;
    decltype(&::stat) stat;
    decltype(&::fcntl) fcntl;
    decltype(&::ioctl) ioctl;
    decltype(&::issetugid) issetugid;
    decltype(&::getcwd) getcwd;
    decltype(&::realpath) realpath;
    decltype(&::vm_allocate) vm_allocate;
    decltype(&::vm_deallocate) vm_deallocate;
    decltype(&::vm_protect) vm_protect;
    void (*vlog)(const char *, va_list);
    void (*vwarn)(const char *, va_list);
    decltype(&::pthread_mutex_lock) pthread_mutex_lock;
    decltype(&::pthread_mutex_unlock) pthread_mutex_unlock;
    decltype(&::mach_thread_self) mach_thread_self;
    decltype(&::mach_port_deallocate) mach_port_deallocate;
    decltype(&::task_self_trap) task_self_trap;
    decltype(&::mach_timebase_info) mach_timebase_info;
    bool (*OSAtomicCompareAndSwapPtrBarrier)(void *, void *, void *volatile *);
    void (*OSMemoryBarrier)();
    dyld_all_image_infos *(*getProcessInfo)();
    int *(*errnoAddress)();
    decltype(&::mach_absolute_time) mach_absolute_time;
    // Version 2.
    decltype(&::thread_switch) thread_switch;
    // Version 3.
    decltype(&::opendir) opendir;
    int (*readdir_r)(DIR *, dirent *, dirent **);
    decltype(&::closedir) closedir;
    // Version 4.
    void (*coresymbolication_load_notifier)(void *, uint64_t, const char *, const mach_header *);
    void (*coresymbolication_unload_notifier)(void *, uint64_t, const char *, const mach_header *);
    // Version 5. Private APIs absent from public SDK headers have explicit ABI signatures.
    int (*proc_regionfilename)(int, uint64_t, void *, uint32_t);
    decltype(&::getpid) getpid;
    decltype(&::mach_port_insert_right) mach_port_insert_right;
    decltype(&::mach_port_allocate) mach_port_allocate;
    decltype(&::mach_msg) mach_msg;
    // Version 6.
    void (*abort_with_payload)(uint32_t, uint64_t, void *, uint32_t, const char *, uint64_t);
    // Version 7.
    decltype(&::task_register_dyld_image_infos) task_register_dyld_image_infos;
    decltype(&::task_unregister_dyld_image_infos) task_unregister_dyld_image_infos;
    decltype(&::task_get_dyld_image_infos) task_get_dyld_image_infos;
    decltype(&::task_register_dyld_shared_cache_image_info) task_register_dyld_shared_cache_image_info;
    decltype(&::task_register_dyld_set_dyld_state) task_register_dyld_set_dyld_state;
    decltype(&::task_register_dyld_get_process_state) task_register_dyld_get_process_state;
    decltype(&::task_info) task_info;
    decltype(&::thread_info) thread_info;
    // Version 8.
    bool (*kdebug_is_enabled)(uint32_t);
    int (*kdebug_trace)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
    // Version 9.
    uint64_t (*kdebug_trace_string)(uint32_t, uint64_t, const char *);
    // Version 10.
    int (*amfi_check_dyld_policy_self)(uint64_t, uint64_t *);
    // Version 11.
    void (*notifyMonitorOfMainCalled)();
    void (*notifyMonitorOfImageListChanges)(bool, unsigned, const mach_header *[], const char *[]);
    // Version 12.
    decltype(&::mach_msg_destroy) mach_msg_destroy;
    decltype(&::mach_port_construct) mach_port_construct;
    decltype(&::mach_port_destruct) mach_port_destruct;
    // Version 13.
    decltype(&::fstat) fstat;
    decltype(&::vm_copy) vm_copy;
    // Version 14.
    decltype(&::task_dyld_process_info_notify_get) task_dyld_process_info_notify_get;
    // Version 15.
    decltype(&::fsgetpath) fsgetpath;
    // Version 16.
    decltype(&::getattrlistbulk) getattrlistbulk;
    // Version 17.
    decltype(&::getattrlist) getattrlist;
    decltype(&::getfsstat) getfsstat;
    void (*notifyMonitorOfDyldBeforeInitializers)();
    // Version 18.
    decltype(&::sysctlbyname) sysctlbyname;
};

// The pinned ABI contains the version word followed by 67 function pointers.
static_assert(sizeof(SimSyscallHelpers) == 68 * sizeof(uintptr_t), "dyld_sim syscall table size changed");
static_assert(offsetof(SimSyscallHelpers, getProcessInfo) == 27 * sizeof(uintptr_t), "dyld_sim process-info slot changed");
static_assert(offsetof(SimSyscallHelpers, sysctlbyname) == 67 * sizeof(uintptr_t), "dyld_sim v18 slot changed");

const SimSyscallHelpers *LSBGetSimSyscallHelpers();

// Call once before entering dyld_sim. The debugger bridge publishes this state or recreates the kernel-registered metadata pages after detachment.
bool LSBInitializeSimProcessInfo(const mach_header *dyldHeader, const char *dyldPath);

// Switch to persistent writable metadata after debugger publication. The caller supplies a non-null, initialized structure.
void LSBSetSimProcessInfo(dyld_all_image_infos *info);
extern "C" void LSBDebuggerImageNotification(dyld_image_mode mode, uint32_t count, const dyld_image_info *images);

// Native startup only: retain immutable data backing before guest TLS. Zero selects the ordinary cache path.
int LSBPrepareSlidableCacheData(const char *runtimeRoot, uint64_t actualBase, const uint8_t cacheUUID[16], bool trace);
#include "../ProcessSupport/SlidableCache.h"
const LSBSlidableDataHeader *LSBPreparedSlidableCacheData();
