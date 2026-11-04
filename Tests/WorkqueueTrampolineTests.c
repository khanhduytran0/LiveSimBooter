#include <assert.h>
#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_statistics.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned char storage[4 * 65536];
static uintptr_t storageBase;
static uintptr_t entry, target;
static mach_vm_address_t reservedAddress;
static mach_vm_size_t reservedSize;
static int configuredPageSize, sharedResult, sharedError, mapError, protectError, unmapError;
static unsigned failedProtection, protectionCalls, allocationCalls, mappingCalls, unmappingCalls, regionCalls, releases;
static unsigned regionFault;
static kern_return_t allocationResult, regionResult;
static bool misplacedAllocation, misplacedMapping, failedDeallocation;
static unsigned deallocationCalls;
static bool placeholderAvailable, failedRestoreProtection, misplacedRestore, failedRestoreVerification;
static unsigned placeholderFault, placeholderQueries, restoreMaps, restoreProtections;
static int restoreMapError;
static bool exitExpected;
static jmp_buf exitJump;
static char operations[64];
static unsigned operationCount;

static void record(char operation) {
    assert(operationCount + 1 < sizeof(operations));
    operations[operationCount++] = operation;
    operations[operationCount] = '\0';
}

static int testSharedRegion(uint64_t *address) {
    assert(address);
    record('S');
    *address = sharedResult ? 0 : 0x180000000;
    errno = sharedError;
    return sharedResult;
}

static int testPageSize(void) { record('P'); return configuredPageSize; }

static kern_return_t testAllocate(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t size, int flags) {
    assert(task == mach_task_self() && address && flags == VM_FLAGS_FIXED);
    assert(*address <= entry && entry - *address < (uintptr_t)configuredPageSize);
    assert(size && !(size % (unsigned)configuredPageSize));
    assert(*address >= (uintptr_t)storage && size <= (uintptr_t)storage + sizeof(storage) - *address);
    assert(entry + 16 <= *address + size);
    reservedAddress = *address;
    reservedSize = size;
    ++allocationCalls;
    record('A');
    if (!allocationResult && misplacedAllocation) *address += 65536;
    return allocationResult;
}

static kern_return_t testDeallocateVM(vm_map_t task, mach_vm_address_t address, mach_vm_size_t size) {
    assert(task == mach_task_self() && misplacedAllocation && address == reservedAddress + 65536 && size == reservedSize);
    ++deallocationCalls;
    record('V');
    return failedDeallocation ? KERN_FAILURE : KERN_SUCCESS;
}

static void *testMap(void *address, size_t size, int protection, int flags, int fd, off_t offset) {
    assert(allocationCalls == 1 && (!allocationResult || (allocationResult == KERN_NO_SPACE && placeholderAvailable)));
    assert((uintptr_t)address == reservedAddress && size == reservedSize);
    if (protection == PROT_NONE) {
        assert(allocationResult == KERN_NO_SPACE && placeholderQueries == 1 && mappingCalls == 1);
        assert(flags == (MAP_FIXED | MAP_ANON | MAP_PRIVATE) && fd == VM_MAKE_TAG(VM_MEMORY_DYLD) && !offset);
        ++restoreMaps;
        record('B');
        if (restoreMapError) { errno = restoreMapError; return MAP_FAILED; }
        if (misplacedRestore) return (unsigned char *)address + 65536;
        memset(address, 0, size);
        return address;
    }
    assert(protection == (PROT_READ | PROT_EXEC) && flags == (MAP_FIXED | MAP_ANON | MAP_PRIVATE) && fd == -1 && !offset);
    ++mappingCalls;
    record('M');
    if (mapError) { errno = mapError; return MAP_FAILED; }
    if (misplacedMapping) return (unsigned char *)address + 65536;
    memset(address, 0, size);
    return address;
}

static kern_return_t testRegionRecurse(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t *size,
                                      natural_t *depth, vm_region_recurse_info_t output, mach_msg_type_number_t *count) {
    assert(task == mach_task_self() && address && size && depth && !*depth && output && count);
    assert(*address == reservedAddress && *count == VM_REGION_SUBMAP_INFO_V0_COUNT_64 && allocationResult == KERN_NO_SPACE);
    ++placeholderQueries;
    record('Q');
    if (!placeholderAvailable) return KERN_INVALID_ADDRESS;
    vm_region_submap_info_data_64_t information = {0};
    information.user_tag = VM_MEMORY_DYLD;
    if (placeholderFault == 1) information.protection = VM_PROT_READ;
    if (placeholderFault == 2) information.max_protection = VM_PROT_READ;
    if (placeholderFault == 3) information.user_tag = VM_MEMORY_MALLOC;
    if (placeholderFault == 4) information.is_submap = true;
    if (placeholderFault == 5) *depth = 1;
    *address = reservedAddress - (unsigned)configuredPageSize;
    *size = reservedSize + 2 * (unsigned)configuredPageSize;
    if (placeholderFault == 6) { *address = reservedAddress; *size = reservedSize - 1; }
    if (placeholderFault == 7) *address = reservedAddress + (unsigned)configuredPageSize;
    if (placeholderFault == 8 || (placeholderQueries == 2 && failedRestoreVerification)) --*count;
    if (placeholderFault == 11) ++*count;
    if (placeholderFault == 9) *size = reservedSize + (unsigned)configuredPageSize - 1;
    if (placeholderFault == 10) *size = 0;
    memcpy(output, &information, VM_REGION_SUBMAP_INFO_V0_SIZE);
    return KERN_SUCCESS;
}

static kern_return_t testProtectVM(vm_map_t task, mach_vm_address_t address, mach_vm_size_t size,
                                  boolean_t maximum, vm_prot_t protection) {
    assert(task == mach_task_self() && address == reservedAddress && size == reservedSize && maximum == TRUE && protection == VM_PROT_NONE);
    assert(restoreMaps == 1 && !restoreMapError && !misplacedRestore);
    ++restoreProtections;
    record('Z');
    return failedRestoreProtection ? KERN_FAILURE : KERN_SUCCESS;
}

static int testProtect(void *address, size_t size, int protection) {
    assert((uintptr_t)address == reservedAddress && size == reservedSize && mappingCalls == 1);
    ++protectionCalls;
    assert(protectionCalls <= 2);
    assert(protection == (protectionCalls == 1 ? PROT_READ | PROT_WRITE : PROT_READ | PROT_EXEC));
    record(protectionCalls == 1 ? 'W' : 'X');
    if (protectionCalls == failedProtection) { errno = protectError; return -1; }
    return 0;
}

static void assertEncoding(void) {
    uint32_t instructions[2];
    uint64_t destination;
    memcpy(instructions, (const void *)entry, sizeof(instructions));
    memcpy(&destination, (const void *)(entry + sizeof(instructions)), sizeof(destination));
    assert(instructions[0] == 0x58000050u && instructions[1] == 0xd61f0200u && destination == target);
}

static void testInvalidate(void *address, size_t size) {
    assert(protectionCalls == 1 && (uintptr_t)address <= entry && size >= entry + 16 - (uintptr_t)address);
    assert((uintptr_t)address >= reservedAddress && size <= reservedAddress + reservedSize - (uintptr_t)address);
    assertEncoding();
    record('C');
}

static kern_return_t testRegion(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t *size,
                               vm_region_flavor_t flavor, vm_region_info_t output, mach_msg_type_number_t *count, mach_port_t *object) {
    assert(task == mach_task_self() && address && size && flavor == VM_REGION_BASIC_INFO_64 && output && count && object);
    assert(*address == reservedAddress && *count == VM_REGION_BASIC_INFO_COUNT_64 && protectionCalls == 2);
    ++regionCalls;
    record('R');
    if (regionResult) return regionResult;
    vm_region_basic_info_data_64_t information = {0};
    information.protection = VM_PROT_READ | VM_PROT_EXECUTE;
    information.max_protection = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
    if (regionFault == 1) information.protection |= VM_PROT_WRITE;
    if (regionFault == 2) information.protection &= ~VM_PROT_EXECUTE;
    if (regionFault == 3) *address += (unsigned)configuredPageSize;
    *size = regionFault == 4 ? reservedSize - 1 : reservedSize;
    *count = VM_REGION_BASIC_INFO_COUNT_64 - (regionFault == 5);
    *object = 0x1503;
    memcpy(output, &information, sizeof(information));
    return KERN_SUCCESS;
}

static kern_return_t testDeallocate(ipc_space_t task, mach_port_name_t name) {
    assert(task == mach_task_self() && name == 0x1503);
    ++releases;
    record('D');
    return KERN_SUCCESS;
}

static int testUnmap(void *address, size_t size) {
    assert(!allocationResult && allocationCalls == 1 && size == reservedSize);
    assert((uintptr_t)address == reservedAddress || (misplacedMapping && (uintptr_t)address == reservedAddress + 65536));
    ++unmappingCalls;
    record('U');
    if (unmapError) { errno = unmapError; return -1; }
    return 0;
}

static _Noreturn void testExit(int status) {
    assert(status == 126 && exitExpected);
    record('E');
    longjmp(exitJump, 1);
}

#define __shared_region_check_np testSharedRegion
#define getpagesize testPageSize
#define mach_vm_allocate testAllocate
#define mach_vm_deallocate testDeallocateVM
#define mach_vm_region_recurse testRegionRecurse
#define mach_vm_protect testProtectVM
#define mmap testMap
#define mprotect testProtect
#define sys_icache_invalidate testInvalidate
#define mach_vm_region testRegion
#define mach_port_deallocate testDeallocate
#define munmap testUnmap
#define _exit testExit
#include "../Guest/libLiveSimSpawn/WorkqueueTrampoline.c"
#undef __shared_region_check_np
#undef getpagesize
#undef mach_vm_allocate
#undef mach_vm_deallocate
#undef mach_vm_region_recurse
#undef mach_vm_protect
#undef mmap
#undef mprotect
#undef sys_icache_invalidate
#undef mach_vm_region
#undef mach_port_deallocate
#undef munmap
#undef _exit

static void reset(void) {
    memset(storage, 0xa5, sizeof(storage));
    storageBase = ((uintptr_t)storage + 65535) & ~(uintptr_t)65535;
    entry = storageBase + 16384 + 0x78b8 % 16384;
    target = 0x16aae0a74;
    configuredPageSize = 16384;
    sharedResult = -1;
    sharedError = ENOMEM;
    mapError = failedProtection = unmapError = regionFault = 0;
    protectError = EACCES;
    allocationResult = regionResult = KERN_SUCCESS;
    misplacedAllocation = misplacedMapping = failedDeallocation = false;
    deallocationCalls = 0;
    placeholderAvailable = failedRestoreProtection = misplacedRestore = failedRestoreVerification = false;
    placeholderFault = placeholderQueries = restoreMaps = restoreProtections = 0;
    restoreMapError = 0;
    protectionCalls = allocationCalls = mappingCalls = unmappingCalls = regionCalls = releases = 0;
    reservedAddress = reservedSize = 0;
    operationCount = 0;
    operations[0] = '\0';
    exitExpected = false;
}

static int install(void) {
    errno = EDOM;
    int result = LSBWorkqueueTrampolineInstall(entry, target);
    assert(errno == EDOM);
    return result;
}

static void successfulInstallTests(void) {
    const unsigned pageSizes[] = {4096, 16384, 65536};
    for (unsigned index = 0; index < sizeof(pageSizes) / sizeof(*pageSizes); ++index) {
        for (unsigned tail = 4; tail <= 20; tail += 4) {
            reset();
            configuredPageSize = (int)pageSizes[index];
            entry = storageBase + 2 * (unsigned)configuredPageSize - tail;
            assert(!install());
            assertEncoding();
            assert(reservedSize == (tail < 16 ? 2u : 1u) * (unsigned)configuredPageSize);
            assert(allocationCalls == 1 && mappingCalls == 1 && protectionCalls == 2 && regionCalls == 1 && releases == 1 && !unmappingCalls);
            assert(strstr(operations, "AMWCXR") && operations[operationCount - 1] == 'D');
            for (uintptr_t cursor = reservedAddress; cursor < reservedAddress + reservedSize; ++cursor)
                if (cursor < entry || cursor >= entry + 16) assert(!*(const unsigned char *)cursor);
        }
    }
    reset();
    sharedError = EINVAL;
    assert(!install());
}

static void validationTests(void) {
    for (unsigned fault = 0; fault < 7; ++fault) {
        reset();
        if (fault == 0) entry = 0;
        if (fault == 1) target = 0;
        if (fault == 2) entry |= 1;
        if (fault == 3) target |= 1;
        if (fault == 4) target = entry;
        if (fault == 5) entry = UINTPTR_MAX - 3;
        if (fault == 6) entry = UINTPTR_MAX - 19;
        assert(install() && !allocationCalls && !mappingCalls && !unmappingCalls);
    }
    const int pageSizes[] = {0, -1, 2048, 8193, 131072};
    for (unsigned index = 0; index < sizeof(pageSizes) / sizeof(*pageSizes); ++index) {
        reset();
        configuredPageSize = pageSizes[index];
        assert(install() && !allocationCalls && !mappingCalls && !unmappingCalls);
    }
    reset();
    sharedResult = 0;
    sharedError = 0;
    assert(install() && !allocationCalls && !mappingCalls && !unmappingCalls);
    reset();
    sharedError = EPERM;
    assert(install() && !allocationCalls && !mappingCalls && !unmappingCalls);
}

static void failureTests(void) {
    reset();
    allocationResult = KERN_NO_SPACE;
    assert(install() && allocationCalls == 1 && !mappingCalls && !unmappingCalls);
    reset();
    misplacedAllocation = true;
    assert(install() == EFAULT && allocationCalls == 1 && deallocationCalls == 1 && !mappingCalls && !unmappingCalls);
    reset();
    misplacedMapping = true;
    assert(install() == EFAULT && mappingCalls == 1 && !protectionCalls && unmappingCalls == 2);
    reset();
    mapError = EACCES;
    assert(install() == EACCES && mappingCalls == 1 && !protectionCalls && unmappingCalls == 1);
    for (unsigned failure = 1; failure <= 2; ++failure) {
        reset();
        failedProtection = failure;
        assert(install() == EACCES && protectionCalls == failure && !regionCalls && unmappingCalls == 1);
    }
    reset();
    regionResult = KERN_INVALID_ADDRESS;
    assert(install() && regionCalls == 1 && !releases && unmappingCalls == 1);
    for (unsigned fault = 1; fault <= 5; ++fault) {
        reset();
        regionFault = fault;
        assert(install() && regionCalls == 1 && releases == 1 && unmappingCalls == 1);
    }
    reset();
    mapError = EACCES;
    unmapError = EBUSY;
    exitExpected = true;
    if (!setjmp(exitJump)) { (void)install(); assert(!"Unsafe cleanup failure must terminate before any worker can start"); }
    assert(unmappingCalls == 1 && operations[operationCount - 1] == 'E');
    reset();
    misplacedAllocation = failedDeallocation = exitExpected = true;
    if (!setjmp(exitJump)) { (void)install(); assert(!"Unsafe reservation cleanup failure must terminate"); }
    assert(deallocationCalls == 1 && operations[operationCount - 1] == 'E');
}

static void placeholderTests(void) {
    reset();
    allocationResult = KERN_NO_SPACE;
    placeholderAvailable = true;
    assert(!install());
    assertEncoding();
    assert(placeholderQueries == 1 && !restoreMaps && !restoreProtections && !unmappingCalls);
    assert(strstr(operations, "AQMWCXR"));
    reset();
    allocationResult = KERN_RESOURCE_SHORTAGE;
    placeholderAvailable = true;
    assert(install() && !placeholderQueries && !mappingCalls); // Only an address collision permits examining a placeholder.
    for (unsigned fault = 1; fault <= 11; ++fault) {
        reset();
        allocationResult = KERN_NO_SPACE;
        placeholderAvailable = true;
        placeholderFault = fault;
        assert(install() == EADDRINUSE && placeholderQueries == 1 && !mappingCalls && !restoreMaps && !unmappingCalls);
    }
    for (unsigned failure = 0; failure < 4; ++failure) {
        reset();
        allocationResult = KERN_NO_SPACE;
        placeholderAvailable = true;
        if (failure == 0) mapError = EACCES;
        if (failure == 1 || failure == 2) failedProtection = failure;
        if (failure == 3) regionFault = 1;
        assert(install() == EACCES && restoreMaps == 1 && restoreProtections == 1 && placeholderQueries == 2 && !unmappingCalls);
        assert(strstr(operations, "BZQ") && operations[operationCount - 1] == 'Q');
    }
    for (unsigned failure = 0; failure < 4; ++failure) {
        reset();
        allocationResult = KERN_NO_SPACE;
        placeholderAvailable = true;
        mapError = EACCES;
        if (failure == 0) restoreMapError = EBUSY;
        if (failure == 1) misplacedRestore = true;
        if (failure == 2) failedRestoreProtection = true;
        if (failure == 3) failedRestoreVerification = true;
        exitExpected = true;
        if (!setjmp(exitJump)) { (void)install(); assert(!"Unsafe placeholder restoration must terminate before any worker starts"); }
        assert(restoreMaps == 1 && !unmappingCalls && operations[operationCount - 1] == 'E');
    }
}

// Optional native execution uses an independent anonymous page, never the real process's registered pthread entry.
__attribute__((naked)) static uint64_t trampolineTarget(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    __asm__("add x0, x0, x1\n\teor x0, x0, x2\n\tadd x0, x0, x3\n\teor x0, x0, x4\n\tadd x0, x0, x5\n\tret");
}

static void executeTrampoline(void) {
    size_t size = (size_t)getpagesize();
    void *memory = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(memory != MAP_FAILED);
    assert(!mprotect(memory, size, PROT_READ | PROT_WRITE));
    const WorkqueueTrampoline code = {.loadTarget = 0x58000050, .branchTarget = 0xd61f0200, .target = (uintptr_t)&trampolineTarget};
    memcpy(memory, &code, sizeof(code));
    sys_icache_invalidate(memory, sizeof(code));
    assert(!mprotect(memory, size, PROT_READ | PROT_EXEC));
    uint64_t (*invoke)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) = memory;
    for (uint64_t value = 0; value < 1000; ++value) {
        uint64_t expected = ((((value + 7) ^ 13) + 23) ^ 47) + 101;
        assert(invoke(value, 7, 13, 23, 47, 101) == expected);
    }
    assert(!munmap(memory, size));
    puts("Workqueue trampoline: native ARM64 branch execution preserves six arguments and returns through the original LR");
}

int main(int argc, char **argv) {
    successfulInstallTests();
    validationTests();
    failureTests();
    placeholderTests();
    puts("Workqueue trampoline: protection order, register-neutral code, mapping ownership, boundaries, verification and cleanup passed");
    if (argc == 2 && !strcmp(argv[1], "--execute")) executeTrampoline();
    else assert(argc == 1);
    return 0;
}
