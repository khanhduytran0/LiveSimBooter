/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2020 Apple Inc. All rights reserved.
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

// Modified 2026-10-23: adapt simulator image mapping, validation and launch arguments for an iPhone app.
// Modified 2026-10-24: accept MH_DYLIB signing variants while retaining validation for each image's launch role.
// Modified 2026-10-24: refresh ptr_munge in writable startup data and prepare debugger metadata before host-cache removal.
// Modified 2026-10-25: replace consumed main_stack metadata with validated native stack bounds for simulator pthread teardown.
// Modified 2026-10-25: own startup vectors and writable strings in bounded app-image storage that cache mappings cannot replace.
// Modified 2026-10-26: reject resident-image collisions and relocate the main stack when the cache overlaps its native range.
// Mapping sequence based on Apple's prepareSim(), dyldMain.cpp:300-469 at fd8d0c4d52320ebf64db34f3cb280310d905c5ae.
// https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/dyld/dyldMain.cpp

#include "DyldSimLoader.h"
#include "../LiveProcess/StackReservation.h"
#include "HostPreparation.h"
#include "SharedCacheHandoff.h"
#include "SimSyscallHelpers.h"
#include "LazySignatureClient.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <libkern/OSByteOrder.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/fat.h>
#include <pthread.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif

namespace {

struct FileMapping {
    int fd = -1;
    void *bytes = MAP_FAILED;
    size_t size = 0;
    bool releaseBytes() {
        if (bytes == MAP_FAILED) return true;
        if (munmap(bytes, size)) return false;
        bytes = MAP_FAILED;
        return true;
    }
    ~FileMapping() {
        if (bytes != MAP_FAILED) munmap(bytes, size);
        if (fd != -1) close(fd);
    }
};

struct Layout {
    uint64_t sliceOffset = 0;
    uint64_t sliceSize = 0;
    uint64_t preferredAddress = 0;
    uint64_t mappedSize = 0;
    uint64_t entryOffset = 0;
    linkedit_data_command signature = {};
    std::vector<segment_command_64> segments;
};

bool contains(uint64_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= size - offset;
}

template <typename T> T read(const uint8_t *bytes) {
    T value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

bool fail(std::string &error, const char *message) {
    error = message;
    return false;
}

bool systemFailure(std::string &error, const char *operation) {
    error = std::string(operation) + ": " + strerror(errno);
    return false;
}

bool retainHostCacheRequested(const char *const environment[]) {
    static constexpr char prefix[] = "LSB_RETAIN_HOST_CACHE=";
    for (auto entry = environment; entry && *entry; ++entry) {
        if (!strncmp(*entry, prefix, sizeof(prefix) - 1)) return !strcmp(*entry + sizeof(prefix) - 1, "1");
    }
    return true;
}

bool mainStackContains(uintptr_t top, size_t size, uintptr_t address) {
    return size && size < top && address >= top - size && address < top;
}

bool captureMainStack(uintptr_t &top, size_t &size, std::string &error) {
    if (pthread_main_np() != 1) return fail(error, "Simulator handoff must run on the native main thread");
    pthread_t thread = pthread_self();
    top = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(thread));
    size = pthread_get_stacksize_np(thread);
    if (!mainStackContains(top, size, reinterpret_cast<uintptr_t>(__builtin_frame_address(0))))
        return fail(error, "Native main-thread stack bounds do not contain the current stack");
    return true;
}

// This stable prefix ends at sharedRegionSize; newer cache headers append fields beyond it.
struct CacheReservationHeader {
    char magic[16];
    uint32_t mappingOffset, mappingCount;
    uint8_t metadataBeforeSharedRegion[200];
    uint64_t sharedRegionStart, sharedRegionSize;
};
static_assert(offsetof(CacheReservationHeader, sharedRegionStart) == 224 && offsetof(CacheReservationHeader, sharedRegionSize) == 232);
static_assert(sizeof(CacheReservationHeader) == 240);
struct CacheReservation { uintptr_t address = 0; size_t size = 0; };

bool rangesOverlap(uintptr_t first, size_t firstSize, uintptr_t second, size_t secondSize) {
    if (!firstSize || !secondSize) return false;
    if (firstSize > UINTPTR_MAX - first || secondSize > UINTPTR_MAX - second) return true;
    return first < second + secondSize && second < first + firstSize;
}

bool parseCacheReservation(const CacheReservationHeader &header, uint64_t fileSize, CacheReservation &cache, std::string &error) {
    cache = {};
    if (fileSize < sizeof(header) || memcmp(header.magic, "dyld_v1   arm64", sizeof(header.magic)))
        return fail(error, "Simulator cache has a truncated or unsupported arm64 header");
    if (header.mappingOffset < sizeof(header) || !header.mappingCount ||
        !contains(fileSize, header.mappingOffset, uint64_t(header.mappingCount) * 32))
        return fail(error, "Simulator cache mapping table is invalid or truncated");
    if (!header.sharedRegionStart || !header.sharedRegionSize || (header.sharedRegionStart & 4095) || (header.sharedRegionSize & 4095) ||
        header.sharedRegionSize > UINTPTR_MAX - header.sharedRegionStart)
        return fail(error, "Simulator cache reservation is empty, unaligned or overflows the address space");
    cache = {static_cast<uintptr_t>(header.sharedRegionStart), static_cast<size_t>(header.sharedRegionSize)};
    return true;
}

bool readCacheReservation(const char *path, CacheReservation &cache, std::string &error) {
    cache = {};
    FileMapping file;
    file.fd = open(path, O_RDONLY | O_CLOEXEC);
    if (file.fd < 0) return errno == ENOENT ? true : systemFailure(error, "open simulator cache");
    struct stat info = {};
    if (fstat(file.fd, &info)) return systemFailure(error, "stat simulator cache");
    if (!S_ISREG(info.st_mode) || info.st_size < static_cast<off_t>(sizeof(CacheReservationHeader)))
        return fail(error, "Simulator cache is not a regular file with a complete reservation header");
    CacheReservationHeader header = {};
    size_t count = 0;
    while (count < sizeof(header)) {
        ssize_t amount = pread(file.fd, reinterpret_cast<uint8_t *>(&header) + count, sizeof(header) - count, count);
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0) return systemFailure(error, "read simulator cache header");
        if (!amount) return fail(error, "Simulator cache header was truncated while reading");
        count += static_cast<size_t>(amount);
    }
    return parseCacheReservation(header, info.st_size, cache, error);
}

bool readRuntimeCacheReservation(const char *rootPath, CacheReservation &cache, std::string &error) {
    if (!rootPath || rootPath[0] != '/') return fail(error, "An absolute RuntimeRoot path is required");
    std::string path = std::string(rootPath) + "/System/Library/Caches/com.apple.dyld/dyld_sim_shared_cache_arm64";
    return readCacheReservation(path.c_str(), cache, error);
}

bool lowImageRange(const Layout &layout, const CacheReservation &cache, size_t pageSize,
                   uintptr_t &begin, uintptr_t &end, std::string &error) {
    begin = end = 0;
    if (!pageSize || (pageSize & (pageSize - 1)) || !layout.mappedSize || (layout.mappedSize & (pageSize - 1)) ||
        layout.mappedSize > SIZE_MAX || !cache.size || cache.size > UINTPTR_MAX - cache.address)
        return fail(error, "Invalid image size, cache range or VM page alignment");
    // TXM does not grant executable mappings in the host shared region to these app-signed images.
    // PAGEZERO also prevents allocations below 4 GiB; do not replace it or permit a negative image slide.
    uintptr_t first = std::max<uintptr_t>(0x100000000ULL, layout.preferredAddress);
    uintptr_t limit = std::min<uintptr_t>(0x180000000ULL, cache.address);
    if (first > UINTPTR_MAX - (pageSize - 1)) return fail(error, "Image preferred address overflows page alignment");
    first = (first + pageSize - 1) & ~(pageSize - 1);
    limit &= ~(pageSize - 1);
    if (first >= limit || layout.mappedSize > limit - first)
        return fail(error, "Image does not fit below the simulator cache in the executable address range");
    begin = first;
    end = limit;
    return true;
}

bool reserveImage(const Layout &layout, const CacheReservation &cache, vm_address_t &base, std::string &error) {
    base = 0;
    if (!cache.size) {
        kern_return_t result = vm_allocate(mach_task_self(), &base, static_cast<vm_size_t>(layout.mappedSize), VM_FLAGS_ANYWHERE);
        if (result == KERN_SUCCESS) return true;
        error = std::string("vm_allocate(image): ") + mach_error_string(result);
        return false;
    }
    const size_t pageSize = static_cast<size_t>(getpagesize());
    uintptr_t begin = 0, end = 0;
    if (!lowImageRange(layout, cache, pageSize, begin, end, error)) return false;
    // The kernel's minimum map address includes the native executable's ASLR slide. vm_region_64 does not expose that PAGEZERO prefix.
    const mach_header *nativeMain = _dyld_get_image_header(0);
    if (!nativeMain) return fail(error, "Cannot locate the native main image for the low allocation bound");
    uintptr_t nativeAddress = reinterpret_cast<uintptr_t>(nativeMain);
    if (nativeAddress & (pageSize - 1)) return fail(error, "Native main image address is not VM page aligned");
    begin = std::max(begin, nativeAddress);
    if (begin >= end) return fail(error, "Native main image leaves no low executable address range below the simulator cache");
    uintptr_t candidate = begin;
    unsigned collisions = 0;
    while (layout.mappedSize <= end - candidate) {
        vm_address_t occupied = candidate;
        vm_size_t occupiedSize = 0;
        vm_region_basic_info_data_64_t info = {};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t result = vm_region_64(mach_task_self(), &occupied, &occupiedSize, VM_REGION_BASIC_INFO_64,
                                           reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        if (MACH_PORT_VALID(object)) mach_port_deallocate(mach_task_self(), object);
        if (result != KERN_SUCCESS && result != KERN_INVALID_ADDRESS) {
            error = std::string("vm_region_64(image reservation): ") + mach_error_string(result);
            return false;
        }
        uintptr_t gapEnd = end;
        if (result == KERN_SUCCESS) {
            if (count < VM_REGION_BASIC_INFO_COUNT_64 || !occupiedSize || occupiedSize > UINTPTR_MAX - occupied ||
                (occupied & (pageSize - 1)) || (occupiedSize & (pageSize - 1)) || occupied + occupiedSize <= candidate)
                return fail(error, "Invalid VM region while finding an image reservation");
            gapEnd = std::min<uintptr_t>(end, occupied);
            if (occupied <= candidate || layout.mappedSize > gapEnd - candidate) {
                uintptr_t next = occupied + occupiedSize;
                if (next >= end) break;
                candidate = next;
                continue;
            }
        }
        vm_address_t reservation = candidate;
        // FIXED without OVERWRITE fails on occupied pages, even if the VM map changed after the region query.
        result = vm_allocate(mach_task_self(), &reservation, static_cast<vm_size_t>(layout.mappedSize), VM_FLAGS_FIXED);
        if (result == KERN_SUCCESS) { base = reservation; return true; }
        if (result != KERN_NO_SPACE) {
            error = std::string("vm_allocate(low image reservation): ") + mach_error_string(result);
            return false;
        }
        if (++collisions == 32) return fail(error, "Low image reservation changed repeatedly while allocating");
        candidate += pageSize;
    }
    char detail[256];
    snprintf(detail, sizeof(detail), "No free low image range of 0x%llx bytes in 0x%llx-0x%llx below the simulator cache",
             static_cast<unsigned long long>(layout.mappedSize), static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end));
    return fail(error, detail);
}

struct GuardedMainStack {
    void *mapping = MAP_FAILED;
    size_t mappingSize = 0, size = 0;
    uintptr_t top = 0;
    ~GuardedMainStack() { if (mapping != MAP_FAILED) munmap(mapping, mappingSize); }
    GuardedMainStack() = default;
    GuardedMainStack(const GuardedMainStack &) = delete;
    GuardedMainStack &operator=(const GuardedMainStack &) = delete;
};

bool prepareMainStack(const CacheReservation &cache, uintptr_t &top, size_t &size, GuardedMainStack &stack, std::string &error) {
    if (!size || size >= top) return fail(error, "Invalid native main-stack bounds");
    if (size == LSB_NATIVE_ARGUMENT_STACK_SIZE) {
        size_t pageSize = static_cast<size_t>(getpagesize());
        constexpr size_t usableSize = LSB_GUEST_MAIN_STACK_SIZE;
        if (!pageSize || (pageSize & (pageSize - 1)) || (top & (pageSize - 1)) || pageSize > size - usableSize)
            return fail(error, "Invalid preserved native argument-stack bounds");
        uintptr_t guard = top - usableSize - pageSize;
        if (rangesOverlap(guard, usableSize + pageSize, cache.address, cache.size))
            return fail(error, "Kernel argument stack still overlaps simulator cache; refusing to overwrite native process arguments");
        // LC_MAIN forced XNU to relocate the original stack and its recorded argv window. Keep that window in place.
        // The unused lower reservation is demand-zero; a new guard bounds the simulator to the upper 8 MiB.
        // Native pthread metadata remains unchanged so pre-handoff failures can still return through native code.
        if (mprotect(reinterpret_cast<void *>(guard), pageSize, PROT_NONE))
            return systemFailure(error, "guard preserved native argument stack");
        size = usableSize;
        return true;
    }
    if (!rangesOverlap(top - size, size, cache.address, cache.size)) return true;
    size_t pageSize = static_cast<size_t>(getpagesize());
    size_t wanted = std::max(size, size_t(8 * 1024 * 1024));
    if (!pageSize || (pageSize & (pageSize - 1)) || wanted > SIZE_MAX - 3 * pageSize)
        return fail(error, "Simulator main-stack allocation size overflows");
    stack.size = (wanted + pageSize - 1) & ~(pageSize - 1);
    stack.mappingSize = stack.size + 2 * pageSize;
    // A hint is never allowed to replace an existing mapping, including another thread's stack or executable image.
    stack.mapping = mmap(reinterpret_cast<void *>(0x400000000ULL), stack.mappingSize, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (stack.mapping == MAP_FAILED) return systemFailure(error, "reserve simulator main stack");
    uintptr_t base = reinterpret_cast<uintptr_t>(stack.mapping);
    if (base < 0x300000000ULL || stack.mappingSize > UINTPTR_MAX - base || rangesOverlap(base, stack.mappingSize, cache.address, cache.size))
        return fail(error, "Simulator main-stack allocation is not outside the cache in the high address range");
    if (mprotect(reinterpret_cast<void *>(base + pageSize), stack.size, PROT_READ | PROT_WRITE))
        return systemFailure(error, "protect simulator main stack");
    stack.top = base + pageSize + stack.size;
    top = stack.top;
    size = stack.size;
    return true;
}

// Inspect only already loaded, trusted Mach-O headers. __PAGEZERO reserves address space but contains no surviving code/data.
bool cacheOverlapFailure(const char *kind, uintptr_t address, size_t size, const CacheReservation &cache, std::string &error) {
    char detail[320];
    snprintf(detail, sizeof(detail), "Simulator cache 0x%llx-0x%llx overlaps %s at 0x%llx (size 0x%llx)",
             static_cast<unsigned long long>(cache.address), static_cast<unsigned long long>(cache.address + cache.size), kind,
             static_cast<unsigned long long>(address), static_cast<unsigned long long>(size));
    return fail(error, detail);
}

bool validNativeStackExclusion(const segment_command_64 &segment, const uint8_t *command, uintptr_t slide, bool wide = true) {
    if (slide >= LSB_STACK_RESERVATION_MAX_SLIDE || segment.cmdsize != sizeof(segment) + sizeof(section_64) ||
        (wide ? segment.vmaddr != LSB_STACK_RESERVATION_ADDRESS : (segment.vmaddr & 0x3fff)) ||
        segment.vmsize != (wide ? LSB_STACK_RESERVATION_SIZE : 0x4000) ||
        segment.fileoff || segment.filesize || segment.maxprot || segment.initprot || segment.nsects != 1 || segment.flags)
        return false;
    auto section = read<section_64>(command + sizeof(segment));
    return !strncmp(segment.segname, LSB_STACK_RESERVATION_SEGMENT, sizeof(segment.segname)) &&
        !strncmp(section.segname, LSB_STACK_RESERVATION_SEGMENT, sizeof(section.segname)) &&
        !strncmp(section.sectname, LSB_STACK_RESERVATION_SECTION, sizeof(section.sectname)) &&
        section.addr == segment.vmaddr && section.size == segment.vmsize && !section.offset && section.align == 14 &&
        !section.reloff && !section.nreloc && section.flags == S_ZEROFILL && !section.reserved1 && !section.reserved2 && !section.reserved3;
}

bool nativeStackPreservesArguments(uintptr_t top, size_t size, const CacheReservation &cache, std::string &error,
                                   const CacheReservation &exclusion = {}) {
    if (!top || !mainStackContains(top, size, top - 1)) return fail(error, "Invalid native main-stack bounds");
    if (rangesOverlap(top - size, size, cache.address, cache.size))
        return fail(error, "Kernel fallback stack still overlaps simulator cache; refusing to overwrite native process arguments");
    if (rangesOverlap(top - size, size, exclusion.address, exclusion.size))
        return fail(error, "Kernel fallback stack overlaps native exclusion; refusing to release native process arguments");
    return true;
}

bool imageOutsideCache(const mach_header *header, const CacheReservation &cache, std::string &error, const char *kind = "resident image",
                       CacheReservation *nativeStackExclusion = nullptr) {
    if (nativeStackExclusion) *nativeStackExclusion = {};
    if (!header || header->magic != MH_MAGIC_64 || header->sizeofcmds > 16 * 1024 * 1024)
        return fail(error, "Cannot validate the resident image against the simulator cache");
    const uint8_t *commands = reinterpret_cast<const uint8_t *>(header) + sizeof(mach_header_64);
    size_t offset = 0;
    uintptr_t preferredHeader = UINTPTR_MAX;
    for (uint32_t index = 0; index < header->ncmds; ++index) {
        if (!contains(header->sizeofcmds, offset, sizeof(load_command))) return fail(error, "Invalid resident image load commands");
        auto command = read<load_command>(commands + offset);
        if (command.cmdsize < sizeof(command) || !contains(header->sizeofcmds, offset, command.cmdsize))
            return fail(error, "Invalid resident image load-command size");
        if (command.cmd == LC_SEGMENT_64) {
            if (command.cmdsize < sizeof(segment_command_64)) return fail(error, "Truncated resident image segment");
            auto segment = read<segment_command_64>(commands + offset);
            if (!segment.fileoff && segment.filesize >= sizeof(mach_header_64)) preferredHeader = segment.vmaddr;
        }
        offset += command.cmdsize;
    }
    uintptr_t address = reinterpret_cast<uintptr_t>(header);
    if (preferredHeader == UINTPTR_MAX || preferredHeader > address) return fail(error, "Invalid resident image header address");
    uintptr_t slide = address - preferredHeader;
    offset = 0;
    for (uint32_t index = 0; index < header->ncmds; ++index) {
        auto command = read<load_command>(commands + offset);
        if (command.cmd == LC_SEGMENT_64) {
            auto segment = read<segment_command_64>(commands + offset);
            if (nativeStackExclusion && !strncmp(segment.segname, LSB_STACK_RESERVATION_SEGMENT, sizeof(segment.segname))) {
                bool wide = segment.vmsize == LSB_STACK_RESERVATION_SIZE;
                if (nativeStackExclusion->size || !validNativeStackExclusion(segment, commands + offset, slide, wide))
                    return fail(error, "Invalid native stack-exclusion segment");
                if (wide) {
                    // Experimental only: the loader's exact file-empty reservation may be released. Guest images receive no exemption.
                    *nativeStackExclusion = {segment.vmaddr + slide, static_cast<size_t>(segment.vmsize)};
                    offset += command.cmdsize;
                    continue;
                }
                // Default builds retain the linked 16 KiB placeholder, with ordinary collision checks and no deallocation.
            }
            if (segment.vmsize && strncmp(segment.segname, "__PAGEZERO", sizeof(segment.segname))) {
                if (segment.vmaddr > UINTPTR_MAX - slide) { error = std::string(kind) + " segment address overflows"; return false; }
                if (rangesOverlap(segment.vmaddr + slide, segment.vmsize, cache.address, cache.size)) {
                    std::string part = std::string(kind) + " " + std::string(segment.segname, strnlen(segment.segname, sizeof(segment.segname)));
                    return cacheOverlapFailure(part.c_str(), segment.vmaddr + slide, segment.vmsize, cache, error);
                }
            }
        }
        offset += command.cmdsize;
    }
    return true;
}

bool releaseNativeStackExclusion(const CacheReservation &reservation, std::string &error) {
    if (!reservation.address && !reservation.size) return true;
    if (reservation.address < LSB_STACK_RESERVATION_ADDRESS ||
        reservation.address - LSB_STACK_RESERVATION_ADDRESS >= LSB_STACK_RESERVATION_MAX_SLIDE ||
        (reservation.address & 0x3fff) || reservation.size != LSB_STACK_RESERVATION_SIZE)
        return fail(error, "Invalid native stack-exclusion release range");
    // This is the native loader's validated zero-file, maxprot=0 segment, never its kernel-selected argument stack.
    // Remove its Mach-O VM entry explicitly: MAP_FIXED is not assumed to replace every native loader mapping on iOS.
    kern_return_t result = vm_deallocate(mach_task_self(), reservation.address, reservation.size);
    if (result == KERN_SUCCESS) return true;
    char detail[192];
    snprintf(detail, sizeof(detail), "vm_deallocate(native stack exclusion 0x%llx-0x%llx): kern_return=%d",
             static_cast<unsigned long long>(reservation.address), static_cast<unsigned long long>(reservation.address + reservation.size), result);
    return fail(error, detail);
}

bool storageInResidentImage(const void *storage, const void *residentHeader, std::string &error) {
    Dl_info image = {};
    if (!storage || !dladdr(storage, &image) || image.dli_fbase != residentHeader)
        return fail(error, "Simulator handoff storage is not in the validated resident image");
    return true;
}

bool debuggerMetadataOutsideCache(const task_dyld_info_data_t &info, size_t pageSize, const CacheReservation &cache, std::string &error) {
    if (info.all_image_info_format != TASK_DYLD_ALL_IMAGE_INFO_64 || !info.all_image_info_addr ||
        info.all_image_info_size < sizeof(dyld_all_image_infos) || !pageSize || (pageSize & (pageSize - 1)))
        return fail(error, "Invalid native debugger metadata bounds");
    uintptr_t address = info.all_image_info_addr, mask = pageSize - 1;
    if (info.all_image_info_size > UINTPTR_MAX - address || address + info.all_image_info_size > UINTPTR_MAX - mask)
        return fail(error, "Native debugger metadata bounds overflow");
    uintptr_t base = address & ~mask, end = (address + info.all_image_info_size + mask) & ~mask;
    if (rangesOverlap(base, end - base, cache.address, cache.size))
        return fail(error, "Simulator cache overlaps the registered native debugger metadata pages");
    return true;
}

constexpr size_t startupStorageCapacity = 512 * 1024;
struct StartupStorage { uintptr_t words[startupStorageCapacity / sizeof(uintptr_t)]; };
struct StartupVectors { const char *const *argv, *const *envp, *const *apple; };
static StartupStorage startupStorage;

int copyStartupVectors(StartupStorage &storage, size_t capacity, const mach_header *executable, const std::vector<std::string> &arguments,
                       const std::vector<std::string> &environment, const std::vector<std::string> &apple, StartupVectors &vectors) {
    // The guest cache may replace native malloc zones. Only app-image storage may survive the destructive handoff.
    // Preflight the entire layout so an overlong input cannot publish a partial vector or overflow the static buffer.
    if (capacity > sizeof(storage.words)) return EINVAL;
    size_t wordCount = 5; // executable header, argc, and three vector terminators
    if (wordCount > capacity / sizeof(uintptr_t)) return E2BIG;
    for (const auto *group : {&arguments, &environment, &apple}) {
        if (group->size() > capacity / sizeof(uintptr_t) - wordCount) return E2BIG;
        wordCount += group->size();
    }
    size_t used = wordCount * sizeof(uintptr_t);
    for (const auto *group : {&arguments, &environment, &apple}) {
        for (const auto &string : *group) {
            if (string.size() >= capacity - used) return E2BIG;
            used += string.size() + 1;
        }
    }
    storage.words[0] = reinterpret_cast<uintptr_t>(executable);
    storage.words[1] = arguments.size();
    size_t word = 2, stringOffset = wordCount * sizeof(uintptr_t);
    auto append = [&](const std::vector<std::string> &strings) {
        auto vector = reinterpret_cast<const char *const *>(storage.words + word);
        for (const auto &string : strings) {
            char *destination = reinterpret_cast<char *>(storage.words) + stringOffset;
            memcpy(destination, string.c_str(), string.size() + 1);
            storage.words[word++] = reinterpret_cast<uintptr_t>(destination);
            stringOffset += string.size() + 1;
        }
        storage.words[word++] = 0;
        return vector;
    };
    StartupVectors result;
    result.argv = append(arguments);
    result.envp = append(environment);
    result.apple = append(apple);
    vectors = result;
    return 0;
}

int prepareStartupVectors(const mach_header *executable, const char *executablePath, const char *rootPath, int argc,
                          const char *const argv[], const char *const envp[], const char *const apple[],
                          uintptr_t stackTop, size_t stackSize, StartupVectors &vectors) {
    // These temporary transformations are destroyed on return, before either cache mapping or thread/cache detachment.
    std::vector<std::string> arguments, environment, appleStrings;
    size_t remaining = startupStorageCapacity - 5 * sizeof(uintptr_t);
    auto append = [&](std::vector<std::string> &group, const char *text) {
        if (!text) return EINVAL;
        if (remaining <= sizeof(uintptr_t)) return E2BIG;
        size_t length = strnlen(text, remaining - sizeof(uintptr_t));
        if (length == remaining - sizeof(uintptr_t)) return E2BIG;
        remaining -= sizeof(uintptr_t) + length + 1;
        group.emplace_back(text, length);
        return 0;
    };
    if (static_cast<size_t>(argc) > remaining / (sizeof(uintptr_t) + 1)) return E2BIG;
    for (int i = 0; i < argc; ++i) {
        int error = append(arguments, argv[i]);
        if (error) return error;
    }
    for (auto entry = envp; entry && *entry; ++entry) {
        if (strncmp(*entry, "DYLD_ROOT_PATH=", sizeof("DYLD_ROOT_PATH=") - 1) == 0 ||
            strncmp(*entry, "DYLD_SHARED_CACHE_DIR=", sizeof("DYLD_SHARED_CACHE_DIR=") - 1) == 0) continue;
        int error = append(environment, *entry);
        if (error) return error;
    }
    if (strnlen(rootPath, remaining) == remaining || strnlen(executablePath, remaining) == remaining) return E2BIG;
    int error = append(environment, (std::string("DYLD_ROOT_PATH=") + rootPath).c_str());
    if (!error) error = append(environment, (std::string("DYLD_SHARED_CACHE_DIR=") + rootPath + "/System/Library/Caches/com.apple.dyld").c_str());
    if (error) return error;
    // Diagnostic variables remain opt-in. executable_file would identify the host and override executable_path inside dyld.
    for (auto entry = apple; entry && *entry; ++entry) {
        if (strncmp(*entry, "executable_path=", 16) == 0 || strncmp(*entry, "executable_file=", 16) == 0 ||
            strncmp(*entry, "main_stack=", 11) == 0 || strncmp(*entry, "ptr_munge=", 10) == 0) continue;
        error = append(appleStrings, *entry);
        if (error) return error;
    }
    error = append(appleStrings, (std::string("executable_path=") + executablePath).c_str());
    if (error) return error;
    // Native libpthread consumed main_stack. The simulator must use actual bounds, without freeing the app-local pthread object.
    char mainStackEntry[sizeof("main_stack=0x,0x,0x0,0x0") + 2 * sizeof(stackTop) + 2 * sizeof(stackSize)];
    snprintf(mainStackEntry, sizeof(mainStackEntry), "main_stack=0x%llx,0x%llx,0x0,0x0",
             static_cast<unsigned long long>(stackTop), static_cast<unsigned long long>(stackSize));
    error = append(appleStrings, mainStackEntry);
    if (error) return error;
    // Native libpthread consumed the kernel's token too. Its simulator copy needs a fresh, writable nonzero secret.
    uintptr_t ptrMungeToken = 0;
    do { arc4random_buf(&ptrMungeToken, sizeof(ptrMungeToken)); } while (!ptrMungeToken);
    char ptrMungeEntry[sizeof("ptr_munge=0x") + 2 * sizeof(ptrMungeToken)];
    snprintf(ptrMungeEntry, sizeof(ptrMungeEntry), "ptr_munge=0x%llx", static_cast<unsigned long long>(ptrMungeToken));
    error = append(appleStrings, ptrMungeEntry);
    if (error) return error;
    return copyStartupVectors(startupStorage, sizeof(startupStorage.words), executable, arguments, environment, appleStrings, vectors);
}

bool selectSlice(const FileMapping &file, Layout &layout, std::string &error) {
    const auto *bytes = static_cast<const uint8_t *>(file.bytes);
    if (file.size < sizeof(mach_header_64)) return fail(error, "Image is too small for a Mach-O header");
    uint32_t magic = read<uint32_t>(bytes);
    layout.sliceSize = file.size;
    if (magic == MH_MAGIC_64) return true;
    bool fat64 = magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64;
    if (!fat64 && magic != FAT_MAGIC && magic != FAT_CIGAM) return fail(error, "Image is not a 64-bit Mach-O or universal binary");
    bool swapped = magic == FAT_CIGAM || magic == FAT_CIGAM_64;
    auto swap32 = [swapped](uint32_t value) { return swapped ? OSSwapInt32(value) : value; };
    auto swap64 = [swapped](uint64_t value) { return swapped ? OSSwapInt64(value) : value; };
    uint32_t count = swap32(read<fat_header>(bytes).nfat_arch);
    size_t archSize = fat64 ? sizeof(fat_arch_64) : sizeof(fat_arch);
    if (!contains(file.size, sizeof(fat_header), uint64_t(count) * archSize)) return fail(error, "Truncated universal architecture table");
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *entry = bytes + sizeof(fat_header) + i * archSize;
        uint32_t cpu, subtype, alignment;
        uint64_t offset, size;
        if (fat64) {
            auto arch = read<fat_arch_64>(entry);
            cpu = swap32(arch.cputype); subtype = swap32(arch.cpusubtype); alignment = swap32(arch.align);
            offset = swap64(arch.offset); size = swap64(arch.size);
        } else {
            auto arch = read<fat_arch>(entry);
            cpu = swap32(arch.cputype); subtype = swap32(arch.cpusubtype); alignment = swap32(arch.align);
            offset = swap32(arch.offset); size = swap32(arch.size);
        }
        // This host is built as arm64; an arm64e slice has a different ABI even on an arm64e-capable phone.
        if (cpu != CPU_TYPE_ARM64 || subtype != CPU_SUBTYPE_ARM64_ALL) continue;
        if (alignment > 63 || (offset & ((uint64_t(1) << alignment) - 1)) || !contains(file.size, offset, size))
            return fail(error, "Invalid arm64 slice bounds or alignment");
        if (offset < sizeof(fat_header) + uint64_t(count) * archSize || size < sizeof(mach_header_64))
            return fail(error, "Invalid arm64 slice header");
        layout.sliceOffset = offset;
        layout.sliceSize = size;
        return true;
    }
    return fail(error, "Image has no compatible arm64 slice");
}

bool parseLayout(const FileMapping &file, Layout &layout, std::string &error, uint32_t expectedType = MH_DYLINKER) {
    if (!selectSlice(file, layout, error)) return false;
    const auto *bytes = static_cast<const uint8_t *>(file.bytes) + layout.sliceOffset;
    auto header = read<mach_header_64>(bytes);
    if (header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_ARM64 || header.cpusubtype != CPU_SUBTYPE_ARM64_ALL)
        return fail(error, "Image must contain a little-endian arm64 Mach-O");
    // The requested role determines validation even when the on-disk image has been re-signed as MH_DYLIB.
    if (header.filetype != expectedType && header.filetype != MH_DYLIB)
        return fail(error, expectedType == MH_EXECUTE ? "Simulator program must be MH_EXECUTE or MH_DYLIB" : "dyld_sim must be MH_DYLINKER or MH_DYLIB");
    if (expectedType == MH_EXECUTE && !(header.flags & MH_PIE))
        return fail(error, "Simulator program must use MH_PIE for allocation at a new address");
    if (!contains(layout.sliceSize, sizeof(header), header.sizeofcmds) || header.ncmds > header.sizeofcmds / sizeof(load_command))
        return fail(error, "Invalid Mach-O load command bounds");

    uint64_t cursor = sizeof(header), commandsEnd = cursor + header.sizeofcmds;
    uint64_t entryValue = 0;
    bool hasEntry = false, entryIsFileOffset = false, hasText = false, hasPlatform = false;
    for (uint32_t i = 0; i < header.ncmds; ++i) {
        if (!contains(commandsEnd, cursor, sizeof(load_command))) return fail(error, "Truncated load command");
        auto command = read<load_command>(bytes + cursor);
        if (command.cmdsize < sizeof(command) || command.cmdsize % 8 || !contains(commandsEnd, cursor, command.cmdsize))
            return fail(error, "Invalid load command size");
        const uint8_t *data = bytes + cursor;
        if (command.cmd == LC_SEGMENT_64) {
            if (command.cmdsize < sizeof(segment_command_64)) return fail(error, "Truncated segment command");
            auto segment = read<segment_command_64>(data);
            if (!contains(command.cmdsize, sizeof(segment), uint64_t(segment.nsects) * sizeof(section_64)))
                return fail(error, "Truncated segment sections");
            if (segment.filesize > segment.vmsize || !contains(layout.sliceSize, segment.fileoff, segment.filesize)
                || segment.vmsize > UINT64_MAX - segment.vmaddr)
                return fail(error, "Invalid segment bounds");
            if ((segment.initprot & ~VM_PROT_ALL) || (segment.maxprot & ~VM_PROT_ALL) || (segment.initprot & ~segment.maxprot))
                return fail(error, "Invalid segment protections");
            if (segment.flags & (SG_HIGHVM | SG_PROTECTED_VERSION_1)) return fail(error, "Unsupported segment flags");
            if (strncmp(segment.segname, SEG_TEXT, sizeof(segment.segname)) == 0) {
                if (hasText || segment.fileoff != 0 || segment.filesize < commandsEnd || !(segment.initprot & VM_PROT_READ))
                    return fail(error, "Invalid __TEXT segment");
                hasText = true;
                layout.preferredAddress = segment.vmaddr;
            }
            if (segment.vmsize) layout.segments.push_back(segment);
        } else if (command.cmd == LC_BUILD_VERSION && expectedType == MH_EXECUTE) {
            if (command.cmdsize < sizeof(build_version_command)) return fail(error, "Truncated build version command");
            auto version = read<build_version_command>(data);
            if (!contains(command.cmdsize, sizeof(version), uint64_t(version.ntools) * sizeof(build_tool_version)))
                return fail(error, "Truncated build tool versions");
            if ((version.platform != PLATFORM_IOSSIMULATOR && version.platform != PLATFORM_IOS) || hasPlatform)
                return fail(error, "Program requires a single iOS or iOS Simulator platform declaration");
            hasPlatform = true;
        } else if (expectedType == MH_EXECUTE && (command.cmd == LC_VERSION_MIN_IPHONEOS || command.cmd == LC_VERSION_MIN_MACOSX
                                                  || command.cmd == LC_VERSION_MIN_TVOS || command.cmd == LC_VERSION_MIN_WATCHOS)) {
            if (command.cmdsize != sizeof(version_min_command)) return fail(error, "Invalid minimum OS version command");
            if (command.cmd != LC_VERSION_MIN_IPHONEOS || hasPlatform)
                return fail(error, "Program requires a single iOS or iOS Simulator platform declaration");
            // For this arm64 image LC_VERSION_MIN_IPHONEOS declares device iOS. The patched dyld_sim handles its compatibility.
            hasPlatform = true;
        } else if (command.cmd == LC_CODE_SIGNATURE) {
            if (command.cmdsize != sizeof(linkedit_data_command) || layout.signature.datasize) return fail(error, "Invalid code signature command");
            layout.signature = read<linkedit_data_command>(data);
            if (!layout.signature.datasize || !contains(layout.sliceSize, layout.signature.dataoff, layout.signature.datasize))
                return fail(error, "Invalid code signature bounds");
        } else if (command.cmd == LC_MAIN) {
            if (command.cmdsize != sizeof(entry_point_command) || hasEntry) return fail(error, "Invalid or duplicate entry point");
            entryValue = read<entry_point_command>(data).entryoff;
            hasEntry = true;
            entryIsFileOffset = true;
        } else if (command.cmd == LC_UNIXTHREAD) {
            size_t stateOffset = sizeof(load_command);
            while (stateOffset < command.cmdsize) {
                if (!contains(command.cmdsize, stateOffset, 8)) return fail(error, "Truncated thread state header");
                uint32_t flavor = read<uint32_t>(data + stateOffset), count = read<uint32_t>(data + stateOffset + 4);
                stateOffset += 8;
                if (!contains(command.cmdsize, stateOffset, uint64_t(count) * 4)) return fail(error, "Truncated thread state");
                // ARM_THREAD_STATE64 on-disk ABI: 68 uint32 words, PC at the 33rd uint64 (no pointer authentication).
                if (flavor == 6) {
                    if (count != 68 || hasEntry) return fail(error, "Invalid or duplicate arm64 entry state");
                    entryValue = read<uint64_t>(data + stateOffset + 32 * sizeof(uint64_t));
                    hasEntry = true;
                }
                stateOffset += size_t(count) * 4;
            }
        }
        cursor += command.cmdsize;
    }
    if (cursor != commandsEnd || !hasText || !hasEntry || !layout.signature.datasize)
        return fail(error, "Image requires valid load commands, __TEXT, an entry point and an embedded code signature");
    if (expectedType == MH_EXECUTE && !hasPlatform)
        return fail(error, "Program is missing an iOS or iOS Simulator platform declaration");
    if (expectedType == MH_EXECUTE && !entryIsFileOffset)
        return fail(error, "Simulator program must use LC_MAIN; legacy LC_UNIXTHREAD may start the program during preparation");

    const uint64_t pageSize = static_cast<uint64_t>(getpagesize());
    std::sort(layout.segments.begin(), layout.segments.end(), [](const auto &a, const auto &b) { return a.vmaddr < b.vmaddr; });
    uint64_t previousEnd = layout.preferredAddress;
    bool entryInCode = false;
    for (const auto &segment : layout.segments) {
        if (segment.vmaddr < layout.preferredAddress) {
            if (strncmp(segment.segname, SEG_PAGEZERO, sizeof(segment.segname)) != 0 || segment.filesize || segment.initprot || segment.maxprot
                || segment.vmaddr + segment.vmsize > layout.preferredAddress)
                return fail(error, "Invalid or overlapping __PAGEZERO segment");
            continue; // The host already protects address zero; do not reserve the executable's entire __PAGEZERO span.
        }
        if (segment.vmaddr < previousEnd || segment.vmaddr % pageSize || segment.vmsize > UINT64_MAX - (pageSize - 1))
            return fail(error, "Segments overlap or are not aligned for the device page size");
        uint64_t roundedSize = (segment.vmsize + pageSize - 1) & ~(pageSize - 1);
        if (roundedSize > UINT64_MAX - segment.vmaddr) return fail(error, "Segment address overflow");
        if (segment.filesize && (layout.sliceOffset + segment.fileoff) % pageSize)
            return fail(error, "Segment file offset is not aligned for the device page size");
        if (segment.filesize < segment.vmsize && segment.filesize % pageSize && (segment.initprot & VM_PROT_EXECUTE))
            return fail(error, "Zero-fill inside a signed executable page is unsupported");
        previousEnd = segment.vmaddr + roundedSize;
        if ((segment.initprot & VM_PROT_EXECUTE) && segment.filesize) {
            uint64_t offset = entryIsFileOffset ? segment.fileoff : segment.vmaddr;
            if (entryValue >= offset && contains(segment.filesize, entryValue - offset, 4)) {
                layout.entryOffset = segment.vmaddr - layout.preferredAddress + entryValue - offset;
                entryInCode = true;
            }
        }
    }
    if (!entryInCode || entryValue % 4) return fail(error, "Entry point is outside executable file-backed segments or is unaligned");
    layout.mappedSize = previousEnd - layout.preferredAddress;
    if (!layout.mappedSize || layout.mappedSize > SIZE_MAX) return fail(error, "Invalid image virtual size");
    return true;
}

bool mapImage(const char *path, LSBMappedImage &image, std::string &error, uint32_t expectedType, const CacheReservation &cache) {
    FileMapping file;
    file.fd = open(path, O_RDONLY | O_CLOEXEC);
    if (file.fd == -1) return systemFailure(error, "open(image)");
    struct stat info = {};
    if (fstat(file.fd, &info) == -1) return systemFailure(error, "fstat(image)");
    if (!S_ISREG(info.st_mode) || info.st_size <= 0 || uint64_t(info.st_size) > SIZE_MAX) return fail(error, "Image is not a nonempty regular file");
    file.size = static_cast<size_t>(info.st_size);
    file.bytes = mmap(nullptr, file.size, PROT_READ, MAP_PRIVATE, file.fd, 0);
    if (file.bytes == MAP_FAILED) return systemFailure(error, "mmap(image file)");
    Layout layout;
    if (!parseLayout(file, layout, error, expectedType)) return false;

    fsignatures_t signature = {};
    signature.fs_file_start = static_cast<off_t>(layout.sliceOffset);
    signature.fs_blob_start = reinterpret_cast<void *>(uintptr_t(layout.signature.dataoff));
    signature.fs_blob_size = layout.signature.datasize;
    if (fcntl(file.fd, F_ADDFILESIGS_RETURN, &signature) == -1 && LSBLazySignatureRetry(file.fd, &signature))
        return systemFailure(error, "fcntl(F_ADDFILESIGS_RETURN)");
    // F_ADDFILESIGS_RETURN replaces fs_file_start with the covered length relative to the slice.
    if (signature.fs_file_start < 0 || uint64_t(signature.fs_file_start) < layout.signature.dataoff)
        return fail(error, "Code signature does not cover the image up to LC_CODE_SIGNATURE.dataoff");

    // Layout owns its commands; retaining this whole-file view can consume the last contiguous low gap for a large executable.
    if (!file.releaseBytes()) return systemFailure(error, "munmap(image parse view)");
    vm_address_t base = 0;
    if (!reserveImage(layout, cache, base, error)) return false;
    image.header = reinterpret_cast<const mach_header *>(base);
    image.mappedSize = static_cast<size_t>(layout.mappedSize);
    // Keep unused gaps inaccessible; file mappings replace only pages inside this reservation.
    if (mprotect(reinterpret_cast<void *>(base), image.mappedSize, PROT_NONE) == -1) return systemFailure(error, "mprotect(image reservation)");
    const size_t pageSize = static_cast<size_t>(getpagesize());
    for (const auto &segment : layout.segments) {
        if (segment.vmaddr < layout.preferredAddress) continue;
        auto *address = reinterpret_cast<void *>(base + segment.vmaddr - layout.preferredAddress);
        if (segment.filesize) {
            off_t offset = static_cast<off_t>(layout.sliceOffset + segment.fileoff);
            void *mapped = mmap(address, static_cast<size_t>(segment.filesize), segment.initprot, MAP_FIXED | MAP_PRIVATE, file.fd, offset);
            if (mapped == MAP_FAILED) {
                error = std::string("mmap(") + std::string(segment.segname, strnlen(segment.segname, 16)) + "): " + strerror(errno);
                return false;
            }
            if (mapped != address) return fail(error, "mmap returned an unexpected segment address");
        }
        size_t filePages = (static_cast<size_t>(segment.filesize) + pageSize - 1) & ~(pageSize - 1);
        if (segment.vmsize > segment.filesize && segment.filesize % pageSize) {
            // mmap rounds to a page, which can include unrelated bytes after this segment in the file.
            auto *lastPage = static_cast<uint8_t *>(address) + filePages - pageSize;
            if (mprotect(lastPage, pageSize, PROT_READ | PROT_WRITE) == -1) return systemFailure(error, "mprotect(image partial page)");
            size_t tailSize = static_cast<size_t>(std::min<uint64_t>(filePages, segment.vmsize) - segment.filesize);
            memset(static_cast<uint8_t *>(address) + segment.filesize, 0, tailSize);
            if (mprotect(lastPage, pageSize, segment.initprot) == -1) return systemFailure(error, "mprotect(image partial page restore)");
        }
        if (segment.vmsize > filePages) {
            auto *zeroFill = static_cast<uint8_t *>(address) + filePages;
            if (mprotect(zeroFill, static_cast<size_t>(segment.vmsize - filePages), segment.initprot) == -1)
                return systemFailure(error, "mprotect(image zero-fill)");
        }
    }
    image.slide = base - layout.preferredAddress;
    image.entry = base + layout.entryOffset;
    return true;
}

bool mapFile(const char *path, LSBMappedImage *image, char *error, size_t errorSize, uint32_t expectedType, const char *rootPath = nullptr) {
    if (!path || !image) {
        if (error && errorSize) snprintf(error, errorSize, "A path and output image are required");
        return false;
    }
    *image = {};
    std::string message;
    CacheReservation cache;
    bool success = (!rootPath || readRuntimeCacheReservation(rootPath, cache, message)) && mapImage(path, *image, message, expectedType, cache);
    if (!success) LSBUnloadImage(image);
    if (error && errorSize) {
        if (success) error[0] = '\0';
        else snprintf(error, errorSize, "%s: %s", expectedType == MH_DYLINKER ? "dyld_sim" : "Simulator executable", message.c_str());
    }
    return success;
}

} // namespace

bool LSBMapDyldSim(const char *path, LSBMappedImage *image, char *error, size_t errorSize) {
    return mapFile(path, image, error, errorSize, MH_DYLINKER);
}

bool LSBMapSimExecutable(const char *path, LSBMappedImage *image, char *error, size_t errorSize) {
    return mapFile(path, image, error, errorSize, MH_EXECUTE);
}

bool LSBMapDyldSimForRuntime(const char *path, const char *rootPath, LSBMappedImage *image, char *error, size_t errorSize) {
    return mapFile(path, image, error, errorSize, MH_DYLINKER, rootPath ? rootPath : "");
}

bool LSBMapSimExecutableForRuntime(const char *path, const char *rootPath, LSBMappedImage *image, char *error, size_t errorSize) {
    return mapFile(path, image, error, errorSize, MH_EXECUTE, rootPath ? rootPath : "");
}

void LSBUnloadImage(LSBMappedImage *image) {
    if (!image) return;
    if (image->header) vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(image->header), image->mappedSize);
    *image = {};
}

int LSBEnterDyldSim(const LSBMappedImage *image, const mach_header *executable, const char *executablePath, const char *dyldPath, const char *rootPath,
                   int argc, const char *const argv[], const char *const envp[], const char *const apple[]) {
    if (!image || !image->entry || !executable || !executablePath || !dyldPath || !rootPath || argc < 1 || !argv) return 1;
    static bool entered = false;
    static LSBDyldSimLaunch launch = {};
    if (entered) { fprintf(stderr, "dyld_sim can only be entered once per process\n"); return 1; }
    const bool traceHandoff = LSBTraceHandoffRequested(envp);
    uintptr_t stackTop = 0;
    size_t stackSize = 0;
    std::string stackError;
    if (!captureMainStack(stackTop, stackSize, stackError)) {
        fprintf(stderr, "Cannot prepare simulator main stack: %s\n", stackError.c_str());
        return 1;
    }
    CacheReservation cache;
    GuardedMainStack relocatedStack;
    CacheReservation nativeStackExclusion;
    Dl_info resident = {};
    if (!readRuntimeCacheReservation(rootPath, cache, stackError)) {
        fprintf(stderr, "Cannot prepare simulator cache reservation: %s\n", stackError.c_str());
        return 1;
    }
    if (cache.size) {
        bool safe = dladdr(reinterpret_cast<const void *>(&LSBEnterDyldSim), &resident) && resident.dli_fbase;
        if (!safe) fail(stackError, "Cannot locate the native loader image");
        if (safe) safe = imageOutsideCache(static_cast<const mach_header *>(resident.dli_fbase), cache, stackError,
                                          "native loader image", &nativeStackExclusion);
        if (safe && nativeStackExclusion.size) safe = nativeStackPreservesArguments(stackTop, stackSize, cache, stackError, nativeStackExclusion);
        if (safe && rangesOverlap(reinterpret_cast<uintptr_t>(image->header), image->mappedSize, cache.address, cache.size))
            safe = cacheOverlapFailure("mapped dyld_sim", reinterpret_cast<uintptr_t>(image->header), image->mappedSize, cache, stackError);
        if (safe) safe = imageOutsideCache(executable, cache, stackError, "simulator executable");
        if (safe && rangesOverlap(reinterpret_cast<uintptr_t>(&startupStorage), sizeof(startupStorage), cache.address, cache.size))
            safe = cacheOverlapFailure("native startup storage", reinterpret_cast<uintptr_t>(&startupStorage), sizeof(startupStorage), cache, stackError);
        if (safe && rangesOverlap(reinterpret_cast<uintptr_t>(&launch), sizeof(launch), cache.address, cache.size))
            safe = cacheOverlapFailure("native launch storage", reinterpret_cast<uintptr_t>(&launch), sizeof(launch), cache, stackError);
        if (!safe) {
            fprintf(stderr, "Cannot use simulator cache: %s\n", stackError.c_str());
            return 1;
        }
    }
    bool argumentStack = stackSize == LSB_NATIVE_ARGUMENT_STACK_SIZE;
    if (argumentStack && !mainStackContains(stackTop, LSB_GUEST_MAIN_STACK_SIZE, reinterpret_cast<uintptr_t>(__builtin_frame_address(0)))) {
        fprintf(stderr, "Native preparation exceeded the preserved argument stack's usable bounds\n");
        return 1;
    }
    if (!prepareMainStack(cache, stackTop, stackSize, relocatedStack, stackError)) {
        fprintf(stderr, "Cannot prepare simulator main stack: %s\n", stackError.c_str());
        return 1;
    }
    if (traceHandoff && relocatedStack.top)
        fprintf(stderr, "Relocated simulator main stack: top 0x%llx, size 0x%llx; cache reservation 0x%llx-0x%llx\n",
                static_cast<unsigned long long>(stackTop), static_cast<unsigned long long>(stackSize),
                static_cast<unsigned long long>(cache.address), static_cast<unsigned long long>(cache.address + cache.size));
    else if (traceHandoff && (nativeStackExclusion.size || argumentStack))
        fprintf(stderr, "Preserved native argument stack: top 0x%llx, size 0x%llx; outside simulator cache\n",
                static_cast<unsigned long long>(stackTop), static_cast<unsigned long long>(stackSize));
    if (!LSBInitializeSimProcessInfo(image->header, dyldPath)) {
        fprintf(stderr, "Cannot initialize simulator process info: %s\n", strerror(*LSBGetSimSyscallHelpers()->errnoAddress()));
        return 1;
    }

    // dyld_sim reconstructs KernelArgs at argv - 2 words; all three vectors and their strings must survive cache mapping.
    StartupVectors startup = {};
    int startupError = prepareStartupVectors(executable, executablePath, rootPath, argc, argv, envp, apple, stackTop, stackSize, startup);
    if (startupError) {
        fprintf(stderr, "Cannot prepare simulator startup data: %s\n", strerror(startupError));
        errno = startupError;
        return 1;
    }

    using Entry = LSBDyldSimLaunch::Entry;
    auto entry = reinterpret_cast<Entry>(image->entry);
#if __has_feature(ptrauth_calls)
    entry = reinterpret_cast<Entry>(ptrauth_sign_unauthenticated(reinterpret_cast<void *>(image->entry), ptrauth_key_function_pointer, 0));
#endif
    // Capture the real AMFI result and materialize arguments before the cache disappears.
    LSBPrepareHostPolicy();
    lsb::MainThreadBootstrap thread = {};
    int threadError = LSBPrepareHostMainThread(&thread);
    if (threadError) {
        fprintf(stderr, "Cannot prepare simulator main thread: %s\n", strerror(threadError));
        return 1;
    }
    const SimSyscallHelpers *helpers = LSBGetSimSyscallHelpers();
    if (cache.size) {
        size_t pageSize = static_cast<size_t>(getpagesize());
        task_dyld_info_data_t nativeInfo = {};
        mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
        kern_return_t result = task_info(mach_task_self(), TASK_DYLD_INFO, reinterpret_cast<task_info_t>(&nativeInfo), &count);
        // TLS, the helper table and simulator/debugger snapshots are static members of the already checked app image.
        // The kernel-registered native metadata pages can survive at their old address when publication is unavailable.
        bool valid = result == KERN_SUCCESS && count >= TASK_DYLD_INFO_COUNT && pageSize && !(pageSize & (pageSize - 1)) &&
                     !(cache.address & (pageSize - 1)) && !(cache.size & (pageSize - 1)) && helpers && helpers->getProcessInfo;
        if (!valid || !storageInResidentImage(reinterpret_cast<const void *>(thread.simulatorTSD), resident.dli_fbase, stackError) ||
            !storageInResidentImage(helpers, resident.dli_fbase, stackError) ||
            !storageInResidentImage(helpers->getProcessInfo(), resident.dli_fbase, stackError) ||
            !debuggerMetadataOutsideCache(nativeInfo, pageSize, cache, stackError)) {
            fprintf(stderr, "Cannot preserve simulator handoff storage: %s\n", stackError.empty() ? "invalid metadata or cache alignment" : stackError.c_str());
            return 1;
        }
    }
    int debuggerError = LSBPrepareHostDebugger(executable, executablePath);
    if (debuggerError) {
        fprintf(stderr, "Cannot prepare debugger image metadata: %s\n", strerror(debuggerError));
        return 1;
    }
    launch = {entry, argc, startup.argv, startup.envp, startup.apple, executable, image->header, image->slide, helpers, thread,
              relocatedStack.top, cache.address, cache.size, retainHostCacheRequested(startup.envp)};
    entered = true;
    if (traceHandoff) {
        fprintf(stderr, "%s host cache, then entering dyld_sim at %p (header %p, slide 0x%llx)\n",
                launch.retainHostCache ? "Retaining" : "Detaching", reinterpret_cast<void *>(image->entry), image->header,
                static_cast<unsigned long long>(image->slide));
        fflush(stderr);
    }
    int workerError = LSBValidateHostThreadCount();
    if (workerError) {
        fprintf(stderr, "Refusing host-cache detachment with native workers: %s\n", strerror(workerError));
        return 1;
    }
    if (!releaseNativeStackExclusion(nativeStackExclusion, stackError)) {
        fprintf(stderr, "Cannot release native stack exclusion: %s\n", stackError.c_str());
        return 1;
    }
    int detachError = LSBDetachHostCacheAndEnter(&launch);
    // The handoff returns only when detachment failed and host libraries are still mapped.
    fprintf(stderr, "Host shared-cache detach failed: %s\n", strerror(detachError));
    return 1;
}
