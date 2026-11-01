/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
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

// Modified 2026-10-23: adapt shared-region removal to executable-local arm64 syscalls and simulator handoff.
// Modified 2026-10-24: switch to app-owned main-thread storage before cache removal and restore host TLS on failure.
// Modified 2026-10-24: invoke the prepared simulator main wrapper and preserve its return status without host cleanup.
// Modified 2026-10-24: transfer debugger notifications and preserve registered image metadata across cache removal.
// Modified 2026-10-26: support a one-way switch to a guarded stack before dyld_sim replaces the native stack mapping.
// Modified 2026-10-26: reserve the full simulator cache span so loose images cannot occupy gaps within its bounds.
// Modified 2026-10-27: optionally retain the native cache after validating a disjoint simulator reservation below it.
// Shared-region removal follows Apple's deallocateExistingSharedCache() at fd8d0c4d52320ebf64db34f3cb280310d905c5ae.
// https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/dyld/SharedCacheRuntime.cpp#L1230-L1243
#include "SharedCacheHandoff.h"
#include "DebuggerBridge.h"
#include "LocalPOSIX.h"
#include "LocalRuntime.h"
#include "LocalMach.h"
#include "SimSyscallHelpers.h"

#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#if defined(LSB_SHARED_CACHE_HANDOFF_TESTING)
// Compile-time substitutes: the host tests must never detach their real shared region or terminate the test process.
int LSBTestSharedRegionCheck(uint64_t *address);
[[noreturn]] void LSBTestExit(int status);
static inline int sharedRegionCheck(uint64_t *address) { return LSBTestSharedRegionCheck(address); }
[[noreturn]] static inline void exitProcess(int status) { LSBTestExit(status); }
#else
#if !defined(__arm64__)
#error Shared-cache handoff requires Darwin arm64.
#endif

static_assert(SYS_shared_region_check_np == 294 && SYS_exit == 1, "Unexpected Darwin syscall numbers");

__attribute__((always_inline, no_stack_protector)) static inline int sharedRegionCheck(uint64_t *address) {
    register uintptr_t result __asm("x0") = reinterpret_cast<uintptr_t>(address);
    register uintptr_t number __asm("x16") = SYS_shared_region_check_np;
    // Darwin reports a positive errno in x0 with carry set. Avoid libc's errno accessor after removing the cache.
    __asm volatile("svc #0x80\n\tcsel %0, %0, xzr, cs" : "+r"(result), "+r"(number) : : "x1", "cc", "memory");
    return static_cast<int>(result);
}

[[noreturn]] __attribute__((always_inline, no_stack_protector)) static inline void exitProcess(int status) {
    for (;;) {
        register uintptr_t result __asm("x0") = static_cast<uintptr_t>(status);
        register uintptr_t number __asm("x16") = SYS_exit;
        __asm volatile("svc #0x80" : "+r"(result), "+r"(number) : : "x1", "cc", "memory");
    }
}
#endif

__attribute__((no_stack_protector)) static void writeHex(char *output, uintptr_t value, unsigned digits) {
    static constexpr char hexadecimal[] = "0123456789abcdef";
    for (unsigned index = digits; index; --index) { output[index - 1] = hexadecimal[value & 15]; value >>= 4; }
}

extern "C" [[noreturn]] __attribute__((noinline, no_stack_protector, visibility("hidden")))
void LSBRunDyldSimOnPreparedStack(const LSBDyldSimLaunch *launch) {
    if (launch->cacheSize) {
        void *address = reinterpret_cast<void *>(launch->cacheAddress);
        // Private cache mappings replace individual segments with MAP_FIXED and leave these inaccessible gaps owned.
        // This must follow the stack switch: the native stack may occupy part of the declared cache reservation.
        void *mapping = lsb::mmap(address, launch->cacheSize, PROT_NONE, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
        if (mapping == MAP_FAILED || mapping != address) {
            static char failed[] = "[dyld_sim] Cache reservation mmap failed: errno=0x00000000 address=0x0000000000000000 size=0x0000000000000000; exiting.\n";
            static constexpr char misplaced[] = "[dyld_sim] Cache reservation returned the wrong address; exiting before dyld entry.\n";
            // One bounded attempt; stderr failure must not prevent the raw exit or enter host cleanup.
            if (mapping == MAP_FAILED) {
                // App-local TLS and fixed-buffer formatting remain safe after the native cache has disappeared.
                writeHex(failed + sizeof("[dyld_sim] Cache reservation mmap failed: errno=0x") - 1,
                         static_cast<unsigned>(*lsb::errnoAddress()), 8);
                writeHex(failed + sizeof("[dyld_sim] Cache reservation mmap failed: errno=0x00000000 address=0x") - 1, launch->cacheAddress, 16);
                writeHex(failed + sizeof("[dyld_sim] Cache reservation mmap failed: errno=0x00000000 address=0x0000000000000000 size=0x") - 1,
                         launch->cacheSize, 16);
                (void)lsb::write(2, failed, sizeof(failed) - 1);
            } else (void)lsb::write(2, misplaced, sizeof(misplaced) - 1);
            exitProcess(126);
        }
    }
    uintptr_t startGlue = 0;
    LSBDyldSimLaunch::Main main = launch->entry(launch->argc, launch->argv, launch->envp, launch->apple, launch->executable,
                                              launch->dyld, launch->slide, launch->helpers, &startGlue);
    if (!main) exitProcess(1);
    // This loader accepts LC_MAIN executables. A legacy crt1 entry (startGlue == 0) requires a different stack ABI.
    if (startGlue != 1) exitProcess(126);
    int status = main(launch->argc, launch->argv, launch->envp, launch->apple);
    // start_sim normally calls simulator exit(). If it returns, never enter unmapped host libc cleanup.
    exitProcess(status);
}

// A new C frame begins after changing SP; no prologue, epilogue, authenticated return or old frame survives this bridge.
extern "C" [[noreturn]] __attribute__((naked, noinline, no_stack_protector, visibility("hidden")))
void LSBSwitchToSimulatorStack(const LSBDyldSimLaunch *, uintptr_t) {
    __asm volatile("mov sp, x1\n\tmov x29, xzr\n\tmov x30, xzr\n\tb _LSBRunDyldSimOnPreparedStack");
}

__attribute__((noinline, no_stack_protector)) int LSBDetachHostCacheAndEnter(const LSBDyldSimLaunch *launch) {
    if (!launch || !launch->thread.hostTSD || !launch->thread.simulatorTSD || (launch->stackTop & 15)) return EINVAL;
    if ((!launch->cacheAddress != !launch->cacheSize) || (launch->cacheAddress & 4095) || (launch->cacheSize & 4095) ||
        launch->cacheSize > UINTPTR_MAX - launch->cacheAddress) return EINVAL;
    if (lsb::currentTSDBase() != launch->thread.hostTSD) return EINVAL;
    uint64_t existingCacheAddress = 0;
    int error = sharedRegionCheck(&existingCacheAddress);
    if (error && error != EINVAL && error != ENOMEM) return error;
    bool hasCache = !error;
    const LSBSlidableDataHeader *backing = LSBPreparedSlidableCacheData();
    const bool slidable = backing && backing->actualBase == launch->cacheAddress && backing->cacheSpan == launch->cacheSize;
    if (backing && !slidable) return EINVAL;
    if (slidable && !launch->retainHostCache) return EPERM;
    if (launch->retainHostCache) {
        if (!hasCache || !existingCacheAddress) return ENOTSUP;
        if (slidable) {
            // MAP_FIXED is safe only inside a range we acquired without replacing existing entries, before changing TLS.
            vm_address_t address = launch->cacheAddress;
            kern_return_t result = lsb::vm_allocate(lsb::task_self_trap(), &address, launch->cacheSize, VM_FLAGS_FIXED);
            if (result != KERN_SUCCESS) return EADDRINUSE;
            if (address != launch->cacheAddress) {
                lsb::vm_deallocate(lsb::task_self_trap(), address, launch->cacheSize);
                return EADDRINUSE;
            }
        } else if (launch->cacheAddress + launch->cacheSize > existingCacheAddress) return EADDRINUSE;
    }

    error = lsb::beginDebuggerHandoff();
    if (error) {
        if (slidable) lsb::vm_deallocate(lsb::task_self_trap(), launch->cacheAddress, launch->cacheSize);
        return error;
    }

    // From this switch onward, even a mapped host libSystem cannot safely use our fresh pthread state.
    if (!lsb::setTSDBase(launch->thread.simulatorTSD)) {
        if (!lsb::setTSDBase(launch->thread.hostTSD)) exitProcess(127);
        lsb::cancelDebuggerHandoff();
        if (slidable) lsb::vm_deallocate(lsb::task_self_trap(), launch->cacheAddress, launch->cacheSize);
        return EIO;
    }
    bool detachCache = hasCache && !launch->retainHostCache;
    if (detachCache) {
        error = sharedRegionCheck(nullptr);
        if (error) {
            if (!lsb::setTSDBase(launch->thread.hostTSD)) exitProcess(127);
            lsb::cancelDebuggerHandoff();
            return error;
        }
    }

    if (lsb::finishDebuggerHandoff(detachCache)) exitProcess(126);

    // The descriptor and all pointed-to startup storage must live outside the stack that cache mappings may replace.
    if (launch->stackTop) LSBSwitchToSimulatorStack(launch, launch->stackTop);
    LSBRunDyldSimOnPreparedStack(launch);
}
