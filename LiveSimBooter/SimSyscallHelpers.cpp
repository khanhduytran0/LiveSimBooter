/*
 * Copyright (c) 2004-2025 Apple Inc. All rights reserved.
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

// Modified 2026-10-23: replace host-cache callbacks with app-local runtime, BSD syscall and Mach implementations.
// Modified 2026-10-24: forward simulator image notifications to an app-resident debugger hook and support relocated process info.
// Modified 2026-10-24: publish compact image snapshots before notifying debuggers using modern dyld introspection.
// Helper-table ordering adapted from dyldMain.cpp at apple-oss-distributions/dyld fd8d0c4d52320ebf64db34f3cb280310d905c5ae.
#include "SimSyscallHelpers.h"
#include "CompactImageInfo.h"
#include "LocalRuntime.h"
#include "LocalPOSIX.h"
#include "LazySignatureClient.h"
#include "LocalMach.h"
#include "../ProcessSupport/SlidableCache.h"

#include <errno.h>
#include <limits.h>
#include <mach-o/dyld_images.h>

#if defined(LSB_DEBUGGER_NOTIFICATION_TESTING)
void LSBTestDebuggerNotification(dyld_image_mode mode, uint32_t count, const dyld_image_info *images);
#endif

extern "C" __attribute__((noinline, used, visibility("default")))
void LSBDebuggerImageNotification(dyld_image_mode mode, uint32_t count, const dyld_image_info *images) {
    // LLDB reads these arguments at the function entry. Keep the call and its ABI observable in optimized builds.
    __asm volatile("" : : "r"(mode), "r"(count), "r"(images) : "memory");
#if defined(LSB_DEBUGGER_NOTIFICATION_TESTING)
    LSBTestDebuggerNotification(mode, count, images);
#endif
}

namespace {

dyld_all_image_infos sProcessInfo = {};
dyld_all_image_infos *sActiveProcessInfo = &sProcessInfo;
char sDyldPath[PATH_MAX] = {};
bool sProcessInfoInitialized = false;

bool compareAndSwapPointer(void *oldValue, void *newValue, void *volatile *address) {
    return __atomic_compare_exchange_n(address, &oldValue, newValue, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

void memoryBarrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
dyld_all_image_infos *getProcessInfo() { return sActiveProcessInfo; }

// Keep ABI slots for disabled tracing and publish debugger state without depending on host libraries.
void coreSymbolicationNotification(void *, uint64_t, const char *, const mach_header *) {}
void debuggerLog(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    lsb::vlog(format, arguments);
    va_end(arguments);
}
#include "SlidableCacheImplementation.h"

void publishDebuggerSnapshot(uint64_t state = 0) {
    int error = lsb::publishCompactImageInfo(sActiveProcessInfo, state);
    if (error) {
        debuggerLog("[dyld_sim] Cannot publish debugger image snapshot: error=%d array images=%u capacity=%zu.\n",
                    error, sActiveProcessInfo->infoArrayCount, lsb::kCompactImageInfoMaxImages);
        lsb::exitProcess(126);
    }
}
void notifyMainCalled() { publishDebuggerSnapshot(0x50); }
void notifyBeforeInitializers() { publishDebuggerSnapshot(0x40); }
void notifyImageChanges(bool unloading, unsigned count, const mach_header *headers[], const char *paths[]) {
    // Version 18 delegates breakpoint notifications here. dyld_sim has already updated the canonical image list.
    if (!count) return;
    publishDebuggerSnapshot();
    dyld_image_info images[64];
    for (unsigned offset = 0; offset < count;) {
        unsigned chunk = count - offset;
        if (chunk > 64) chunk = 64;
        for (unsigned index = 0; index < chunk; ++index) {
            images[index] = { headers[offset + index], paths[offset + index], 0 };
        }
        LSBDebuggerImageNotification(unloading ? dyld_image_removing : dyld_image_adding, chunk, images);
        offset += chunk;
    }
}
bool tracingEnabled(uint32_t) { return false; }
int trace(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t) { return 0; }
uint64_t traceString(uint32_t, uint64_t, const char *) { return 0; }

const SimSyscallHelpers sHelpers = {
    18,
    &lsb::open,
    &lsb::close,
    &lsb::pread,
    &lsb::write,
    &slidableCacheMmap,
    &lsb::munmap,
    &lsb::madvise,
    &LSBLazySignatureDyldStat,
    &lsb::fcntl,
    &lsb::ioctl,
    &lsb::issetugid,
    &lsb::getcwd,
    &lsb::realpath,
    &lsb::vm_allocate,
    &lsb::vm_deallocate,
    &lsb::vm_protect,
    &lsb::vlog,
    &lsb::vlog,
    &lsb::pthread_mutex_lock,
    &lsb::pthread_mutex_unlock,
    &lsb::mach_thread_self,
    &lsb::mach_port_deallocate,
    &lsb::task_self_trap,
    &lsb::mach_timebase_info,
    &compareAndSwapPointer,
    &memoryBarrier,
    &getProcessInfo,
    &lsb::errnoAddress,
    &lsb::mach_absolute_time,
    &lsb::thread_switch,
    &lsb::opendir,
    &lsb::readdir_r,
    &lsb::closedir,
    &coreSymbolicationNotification,
    &coreSymbolicationNotification,
    &lsb::proc_regionfilename,
    &lsb::getpid,
    &lsb::mach_port_insert_right,
    &lsb::mach_port_allocate,
    &lsb::mach_msg,
    &lsb::abort_with_payload,
    &lsb::task_register_dyld_image_infos,
    &lsb::task_unregister_dyld_image_infos,
    &lsb::task_get_dyld_image_infos,
    &lsb::task_register_dyld_shared_cache_image_info,
    &lsb::task_register_dyld_set_dyld_state,
    &lsb::task_register_dyld_get_process_state,
    &lsb::task_info,
    &lsb::thread_info,
    &tracingEnabled,
    &trace,
    &traceString,
    &lsb::amfi_check_dyld_policy_self,
    &notifyMainCalled,
    &notifyImageChanges,
    &lsb::mach_msg_destroy,
    &lsb::mach_port_construct,
    &lsb::mach_port_destruct,
    &lsb::fstat,
    &lsb::vm_copy,
    &lsb::task_dyld_process_info_notify_get,
    &lsb::fsgetpath,
    &lsb::getattrlistbulk,
    &lsb::getattrlist,
    &lsb::getfsstat,
    &notifyBeforeInitializers,
    &lsb::sysctlbyname,
};

} // namespace

const SimSyscallHelpers *LSBGetSimSyscallHelpers() { return &sHelpers; }

void LSBSetSimProcessInfo(dyld_all_image_infos *info) { sActiveProcessInfo = info; }

bool LSBInitializeSimProcessInfo(const mach_header *dyldHeader, const char *dyldPath) {
    if (sProcessInfoInitialized) { *lsb::errnoAddress() = EBUSY; return false; }
    if (!dyldHeader || !dyldPath) { *lsb::errnoAddress() = EINVAL; return false; }
    if (lsb::copyString(sDyldPath, dyldPath, sizeof(sDyldPath)) >= sizeof(sDyldPath)) {
        *lsb::errnoAddress() = ENAMETOOLONG;
        return false;
    }
    sProcessInfo.version = 17;
    sProcessInfo.notification = &LSBDebuggerImageNotification;
    sProcessInfo.processDetachedFromSharedRegion = true;
    sProcessInfo.dyldImageLoadAddress = dyldHeader;
    sProcessInfo.dyldVersion = "dyld_sim (LiveSimBooter)";
    sProcessInfo.dyldAllImageInfosAddress = &sProcessInfo;
    sProcessInfo.dyldPath = sDyldPath;
    sProcessInfo.platform = PLATFORM_IOSSIMULATOR;
    sProcessInfoInitialized = true;
    return true;
}

int LSBPrepareSlidableCacheData(const char *runtimeRoot, uint64_t actualBase, const uint8_t cacheUUID[16], bool trace) {
    return prepareSlidableData(runtimeRoot, actualBase, cacheUUID, trace);
}

const LSBSlidableDataHeader *LSBPreparedSlidableCacheData() {
    return sSlidableCache.enabled ? &sSlidableCache.backing : nullptr;
}
