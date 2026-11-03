// Host tests use synthetic Mach-O bytes and a fake prepare entry. No simulator code is signed or executed.
#include "../LiveSimBooter/SimSyscallHelpers.h"
#include "../LiveSimBooter/LazySignatureClient.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <libkern/OSByteOrder.h>
#include <mach-o/fat.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <vector>

static bool rejectSegmentMapping = false;
static bool rejectSignature = false;
static int lazySignatureError = ENOTSUP;
static unsigned lazySignatureRetries;
static int sourceFD = -1, signatureCalls = 0, deallocations = 0, fileUnmaps = 0;
static void *sourceMapping = MAP_FAILED;
static off_t registeredSlice = -1;
static uintptr_t registeredSignature = 0;
static unsigned randomCalls = 0;
static unsigned stackReservations = 0, stackReleases = 0;
static void *reservedStack = MAP_FAILED;
static bool rejectStackProtection = false, stackUsedFixedMapping = false;
static bool sourceViewLive = false, allocatedWithParseView = false, reservationUsedOverwrite = false;
static bool stripSegmentExecute = false;
static std::function<kern_return_t(vm_address_t *, vm_size_t, int)> allocationHook;
static std::function<kern_return_t(vm_address_t, vm_size_t)> deallocationHook;
static std::function<kern_return_t(vm_address_t *, vm_size_t *, mach_msg_type_number_t *, mach_port_t *)> regionHook;
static constexpr uintptr_t freshPtrMungeToken = 0xa739c450e16b28df;

static void testArc4RandomBuf(void *buffer, size_t size) {
    if (size != sizeof(uintptr_t)) { fprintf(stderr, "Unexpected pointer token size\n"); exit(1); }
    uintptr_t value = randomCalls++ ? freshPtrMungeToken : 0;
    memcpy(buffer, &value, size);
}

static int testFcntl(int, int command, ...) {
    if (command != F_ADDFILESIGS_RETURN) { errno = EINVAL; return -1; }
    va_list arguments;
    va_start(arguments, command);
    auto *signature = va_arg(arguments, fsignatures_t *);
    va_end(arguments);
    ++signatureCalls;
    registeredSlice = signature->fs_file_start;
    registeredSignature = reinterpret_cast<uintptr_t>(signature->fs_blob_start);
    if (rejectSignature) { errno = EACCES; return -1; }
    signature->fs_file_start = static_cast<off_t>(registeredSignature);
    return 0;
}

int LSBLazySignatureRetry(int fd, fsignatures_t *signature) {
    ++lazySignatureRetries;
    if (fd < 0 || !signature) return EINVAL;
    if (!lazySignatureError) signature->fs_file_start = static_cast<off_t>(reinterpret_cast<uintptr_t>(signature->fs_blob_start));
    return lazySignatureError;
}

static void *testMmap(void *address, size_t size, int protection, int flags, int fd, off_t offset) {
    if ((flags & MAP_FIXED) && rejectSegmentMapping) { errno = EACCES; return MAP_FAILED; }
    if ((flags & MAP_FIXED) && stripSegmentExecute) protection &= ~PROT_EXEC;
    void *result = ::mmap(address, size, protection, flags, fd, offset);
    if (address == reinterpret_cast<void *>(0x400000000ULL) && protection == PROT_NONE) {
        ++stackReservations;
        stackUsedFixedMapping |= flags & MAP_FIXED;
        reservedStack = result;
    }
    if (!(flags & (MAP_FIXED | MAP_ANON))) { sourceFD = fd; sourceMapping = result; sourceViewLive = result != MAP_FAILED; }
    return result;
}

static int testMunmap(void *address, size_t size) {
    if (address == reservedStack && address != MAP_FAILED) ++stackReleases;
    int result = ::munmap(address, size);
    if (address == sourceMapping) { ++fileUnmaps; if (!result) sourceViewLive = false; }
    return result;
}

static kern_return_t testAllocate(vm_map_t task, vm_address_t *address, vm_size_t size, int flags) {
    allocatedWithParseView |= sourceViewLive;
    reservationUsedOverwrite |= flags & VM_FLAGS_OVERWRITE;
    return allocationHook ? allocationHook(address, size, flags) : ::vm_allocate(task, address, size, flags);
}

static kern_return_t testRegion(vm_map_t task, vm_address_t *address, vm_size_t *size, vm_region_flavor_t flavor,
                               vm_region_info_t info, mach_msg_type_number_t *count, mach_port_t *object) {
    return regionHook ? regionHook(address, size, count, object) : ::vm_region_64(task, address, size, flavor, info, count, object);
}

static int testMprotect(void *address, size_t size, int protection) {
    if (rejectStackProtection) { errno = EACCES; return -1; }
    return ::mprotect(address, size, protection);
}

static kern_return_t testDeallocate(vm_map_t task, vm_address_t address, vm_size_t size) {
    ++deallocations;
    return deallocationHook ? deallocationHook(address, size) : ::vm_deallocate(task, address, size);
}

#define fcntl testFcntl
#define mmap testMmap
#define munmap testMunmap
#define mprotect testMprotect
#define vm_deallocate testDeallocate
#define vm_allocate testAllocate
#define vm_region_64 testRegion
#define arc4random_buf testArc4RandomBuf
#include "../LiveSimBooter/DyldSimLoader.cpp"
#undef arc4random_buf
#undef vm_deallocate
#undef vm_allocate
#undef vm_region_64
#undef munmap
#undef mprotect
#undef mmap
#undef fcntl

static unsigned checks = 0;
static void check(bool condition, const char *message) {
    ++checks;
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

static std::string expectedMainStack() {
    char value[96];
    snprintf(value, sizeof(value), "main_stack=0x%llx,0x%llx,0x0,0x0",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()))),
             static_cast<unsigned long long>(pthread_get_stacksize_np(pthread_self())));
    return value;
}

static void checkAndConsumeMainStack(const char *value) {
    check(std::string(value) == expectedMainStack(), "main_stack uses real native bounds and zero allocation metadata");
    memset(const_cast<char *>(value) + 11, 0, strlen(value) - 11);
    check(std::string(value) == "main_stack=", "simulator can consume and erase its own main stack metadata");
}

static void mainStackBoundsTest() {
    check(mainStackContains(0x18000, 0x8000, 0x10000), "native stack includes its bottom address");
    check(mainStackContains(0x18000, 0x8000, 0x17fff), "native stack includes its final byte");
    check(!mainStackContains(0x18000, 0x8000, 0x18000), "native stack excludes its top address");
    check(!mainStackContains(0x18000, 0x8000, 0xffff), "native stack rejects an address below its allocation");
    check(!mainStackContains(0x18000, 0, 0x17000), "native stack size must be nonzero");
    check(!mainStackContains(0, 0x8000, 0), "native stack top must be nonzero");
    check(!mainStackContains(0x18000, SIZE_MAX, 0x17000), "native stack subtraction cannot wrap");
    check(!mainStackContains(0x18000, 0x18000, 0x17000), "native stack cannot extend to the null page");
}

static void nativeStackExclusionTests() {
    struct Fixture { mach_header_64 header; segment_command_64 text, guard; section_64 section; } fixture = {};
    fixture.header.magic = MH_MAGIC_64;
    fixture.header.ncmds = 2;
    fixture.header.sizeofcmds = sizeof(fixture) - sizeof(fixture.header);
    fixture.text.cmd = fixture.guard.cmd = LC_SEGMENT_64;
    fixture.text.cmdsize = sizeof(fixture.text);
    strcpy(fixture.text.segname, "__TEXT");
    fixture.text.vmaddr = reinterpret_cast<uintptr_t>(&fixture);
    fixture.text.vmsize = fixture.text.filesize = sizeof(fixture);
    fixture.text.initprot = fixture.text.maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
    fixture.guard.cmdsize = sizeof(fixture.guard) + sizeof(fixture.section);
    strcpy(fixture.guard.segname, LSB_STACK_RESERVATION_SEGMENT);
    fixture.guard.vmaddr = LSB_STACK_RESERVATION_ADDRESS;
    fixture.guard.vmsize = LSB_STACK_RESERVATION_SIZE;
    fixture.guard.nsects = 1;
    strcpy(fixture.section.segname, LSB_STACK_RESERVATION_SEGMENT);
    strcpy(fixture.section.sectname, LSB_STACK_RESERVATION_SECTION);
    fixture.section.addr = fixture.guard.vmaddr;
    fixture.section.size = fixture.guard.vmsize;
    fixture.section.align = 14;
    fixture.section.flags = S_ZEROFILL;
    auto header = reinterpret_cast<const mach_header *>(&fixture.header);
    const CacheReservation cache = {0x110000000, 0x100000};
    std::string error;
    CacheReservation reserved;
    check(!imageOutsideCache(header, cache, error), "guest image cannot use the native stack-exclusion name to bypass collision checks");
    check(imageOutsideCache(header, cache, error, "native loader image", &reserved) && reserved.address == fixture.guard.vmaddr && reserved.size == fixture.guard.vmsize,
          "only exact native file-empty reservation is exempt from cache collision checks");
    auto placeholder = fixture;
    placeholder.text.vmaddr = reinterpret_cast<uintptr_t>(&placeholder);
    placeholder.guard.vmsize = placeholder.section.size = 0x4000;
    check(imageOutsideCache(reinterpret_cast<const mach_header *>(&placeholder), cache, error, "native loader image", &reserved) && !reserved.size,
          "default tiny placeholder uses ordinary resident checks and never requests guard release");
    check(!imageOutsideCache(reinterpret_cast<const mach_header *>(&placeholder), {placeholder.guard.vmaddr, 0x4000}, error,
                             "native loader image", &reserved), "tiny placeholder receives no cache-collision exemption");
    check(!imageOutsideCache(header, {fixture.text.vmaddr, sizeof(fixture)}, error, "native loader image", &reserved),
          "native text still cannot overlap cache when it also has a valid exclusion");
    for (unsigned invalid = 0; invalid < 10; ++invalid) {
        auto altered = fixture;
        switch (invalid) {
            case 0: altered.guard.filesize = 1; break;
            case 1: altered.guard.fileoff = 1; break;
            case 2: altered.guard.initprot = VM_PROT_READ; break;
            case 3: altered.guard.maxprot = VM_PROT_READ; break;
            case 4: altered.guard.vmsize = 0x4000; break;
            case 5: altered.guard.vmaddr += 0x4000; break;
            case 6: altered.guard.nsects = 2; break;
            case 7: altered.section.flags = S_REGULAR; break;
            case 8: altered.section.nreloc = 1; break;
            case 9: altered.section.reserved3 = 1; break;
        }
        check(!validNativeStackExclusion(altered.guard, reinterpret_cast<const uint8_t *>(&altered.guard), 0),
              "malformed, readable, file-backed or unprepared reservations are rejected");
    }
    check(validNativeStackExclusion(fixture.guard, reinterpret_cast<const uint8_t *>(&fixture.guard), LSB_STACK_RESERVATION_MAX_SLIDE - 0x4000),
          "last permitted native ASLR page is supported");
    check(!validNativeStackExclusion(fixture.guard, reinterpret_cast<const uint8_t *>(&fixture.guard), LSB_STACK_RESERVATION_MAX_SLIDE),
          "unexpected native ASLR geometry fails closed");
    check(nativeStackPreservesArguments(0x108000000, 0x800000, cache, error), "kernel fallback stack below cache preserves argv window");
    check(!nativeStackPreservesArguments(0x10c000000, 0x800000, cache, error,
                                        {LSB_STACK_RESERVATION_ADDRESS, LSB_STACK_RESERVATION_SIZE}),
          "stack in the guard's below-cache prefix cannot be released even though it does not overlap cache");
    check(!nativeStackPreservesArguments(0x110004000, 0x800000, cache, error), "fallback inside cache fails before old argv can be overwritten");
    check(!nativeStackPreservesArguments(0x1000, 0x2000, cache, error), "fallback stack underflow is rejected");
    unsigned releases = 0;
    kern_return_t releaseResult = KERN_SUCCESS;
    const CacheReservation owned = {LSB_STACK_RESERVATION_ADDRESS + 0x4000, LSB_STACK_RESERVATION_SIZE};
    deallocationHook = [&](vm_address_t address, vm_size_t size) {
        ++releases;
        check(address == owned.address && size == owned.size, "release touches only the exact slid, file-empty native guard");
        return releaseResult;
    };
    check(releaseNativeStackExclusion({}, error) && !releases, "legacy native image without reservation performs no VM mutation");
    check(releaseNativeStackExclusion(owned, error) && releases == 1, "owned guard is released once before cache reservation");
    releaseResult = KERN_PROTECTION_FAILURE;
    check(!releaseNativeStackExclusion(owned, error) && releases == 2 && error.find("kern_return=2") != std::string::npos,
          "guard release failure is reported numerically before host detachment");
    for (CacheReservation bad : {CacheReservation{owned.address + 1, owned.size}, {owned.address, 0x4000},
                                 {LSB_STACK_RESERVATION_ADDRESS - 0x4000, owned.size},
                                 {LSB_STACK_RESERVATION_ADDRESS + LSB_STACK_RESERVATION_MAX_SLIDE, owned.size}})
        check(!releaseNativeStackExclusion(bad, error) && releases == 2, "malformed release cannot deallocate unrelated memory");
    deallocationHook = {};

}

static CacheReservationHeader cacheHeader() {
    CacheReservationHeader header = {};
    memcpy(header.magic, "dyld_v1   arm64", sizeof(header.magic));
    header.mappingOffset = sizeof(header);
    header.mappingCount = 1;
    header.sharedRegionStart = 0x108000000;
    header.sharedRegionSize = 0xffc08000;
    return header;
}

static void cacheReservationTests() {
    auto header = cacheHeader();
    CacheReservation cache;
    std::string error;
    constexpr size_t fileSize = sizeof(header) + 32;
    check(parseCacheReservation(header, fileSize, cache, error), "valid arm64 cache reservation is accepted");
    check(cache.address == header.sharedRegionStart && cache.size == header.sharedRegionSize, "cache reservation keeps its declared span including gaps");
    for (size_t truncated : {size_t(0), sizeof(header) - 1, fileSize - 1}) {
        check(!parseCacheReservation(header, truncated, cache, error), "truncated header or mapping table is rejected");
        check(!cache.address && !cache.size, "failed parsing clears reservation output");
    }
    for (unsigned mutation = 0; mutation < 9; ++mutation) {
        auto bad = header;
        switch (mutation) {
            case 0: bad.magic[0] = 'x'; break;
            case 1: bad.mappingOffset = sizeof(header) - 1; break;
            case 2: bad.mappingCount = 0; break;
            case 3: bad.mappingCount = UINT32_MAX; break;
            case 4: bad.sharedRegionStart = 0; break;
            case 5: bad.sharedRegionSize = 0; break;
            case 6: ++bad.sharedRegionStart; break;
            case 7: ++bad.sharedRegionSize; break;
            case 8: bad.sharedRegionStart = UINTPTR_MAX - 4095; bad.sharedRegionSize = 4096; break;
        }
        check(!parseCacheReservation(bad, fileSize, cache, error), "malformed cache header fails before handoff");
    }
    check(rangesOverlap(0x1000, 0x1000, 0x1fff, 1), "range overlap includes final byte");
    check(!rangesOverlap(0x1000, 0x1000, 0x2000, 0x1000), "adjacent half-open ranges do not overlap");
    check(!rangesOverlap(0x1000, 0, 0x1000, 0x1000), "absent cache is an empty range");
    check(rangesOverlap(UINTPTR_MAX - 1, 3, 0x1000, 0x1000), "overflowing image ranges fail closed");
    char path[] = "/tmp/lsb-cache-header-XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0 && write(fd, &header, sizeof(header)) == sizeof(header), "write cache header fixture");
    check(ftruncate(fd, fileSize) == 0, "size cache fixture for mapping table");
    check(readCacheReservation(path, cache, error) && cache.address == header.sharedRegionStart, "read only the cache prefix from a regular file");
    check(ftruncate(fd, sizeof(header) - 1) == 0 && !readCacheReservation(path, cache, error), "short file fails before prefix read");
    close(fd);
    unlink(path);
    check(readCacheReservation(path, cache, error) && !cache.size, "ENOENT preserves the loose-library launch path");
    check(!readCacheReservation("/tmp", cache, error), "non-regular cache path is rejected");
    Dl_info resident = {};
    check(dladdr(reinterpret_cast<const void *>(&cacheReservationTests), &resident) && resident.dli_fbase, "resolve actual resident loader module");
    auto nativeHeader = static_cast<const mach_header *>(resident.dli_fbase);
    check(imageOutsideCache(nativeHeader, {0x1000, 0x1000}, error), "resident image validation excludes inaccessible PAGEZERO reservation");
    check(!imageOutsideCache(nativeHeader, {reinterpret_cast<uintptr_t>(resident.dli_fbase), 4096}, error), "cache overlapping resident code is rejected");
    check(!imageOutsideCache(nativeHeader, {reinterpret_cast<uintptr_t>(resident.dli_fbase), 4096}, error, "native loader image") &&
          error.find("native loader image __TEXT at 0x") != std::string::npos, "native collision identifies the image, segment and actual address");
    check(!imageOutsideCache(nativeHeader, {reinterpret_cast<uintptr_t>(resident.dli_fbase), 4096}, error, "simulator executable") &&
          error.find("simulator executable __TEXT at 0x") != std::string::npos, "executable collision is distinct from a native loader collision");
    cacheOverlapFailure("mapped dyld_sim", 0x108001000, 0x4000, {0x108000000, 0x8000}, error);
    check(error.find("0x108000000-0x108008000 overlaps mapped dyld_sim at 0x108001000 (size 0x4000)") != std::string::npos,
          "dyld_sim collision includes both ranges");
    check(storageInResidentImage(&startupStorage, resident.dli_fbase, error), "app-local startup storage belongs to the validated image");
    check(!storageInResidentImage(nullptr, resident.dli_fbase, error), "missing surviving storage is rejected");
    check(!storageInResidentImage(&resident, resident.dli_fbase, error), "native stack storage cannot masquerade as app-local static storage");
    check(!storageInResidentImage(&startupStorage, reinterpret_cast<void *>(0x1234), error), "storage from another image is rejected");
    task_dyld_info_data_t debugger = {};
    debugger.all_image_info_format = TASK_DYLD_ALL_IMAGE_INFO_64;
    debugger.all_image_info_addr = 0x200083ff0;
    debugger.all_image_info_size = sizeof(dyld_all_image_infos);
    constexpr size_t metadataPage = 0x4000;
    check(debuggerMetadataOutsideCache(debugger, metadataPage, {0x108000000, 0x7e194000}, error), "native debugger metadata can survive outside the cache");
    check(!debuggerMetadataOutsideCache(debugger, metadataPage, {0x200080000, metadataPage}, error), "metadata first-page overlap is rejected");
    check(!debuggerMetadataOutsideCache(debugger, metadataPage, {0x200084000, metadataPage}, error), "metadata final-page overlap is rejected");
    check(debuggerMetadataOutsideCache(debugger, metadataPage, {0x20007c000, metadataPage}, error), "adjacent reservation preserves metadata pages");
    for (unsigned mutation = 0; mutation < 5; ++mutation) {
        auto invalid = debugger;
        size_t page = metadataPage;
        switch (mutation) {
            case 0: invalid.all_image_info_format = TASK_DYLD_ALL_IMAGE_INFO_32; break;
            case 1: invalid.all_image_info_size = 1; break;
            case 2: invalid.all_image_info_addr = UINTPTR_MAX - 7; break;
            case 3: invalid.all_image_info_addr = 0; break;
            case 4: page = 3; break;
        }
        check(!debuggerMetadataOutsideCache(invalid, page, {0x108000000, 0x7e194000}, error), "invalid native debugger metadata fails before detachment");
    }
}

static void lowImageReservationTests() {
    constexpr uintptr_t lowFloor = 0x100000000ULL;
    const size_t pageSize = static_cast<size_t>(getpagesize());
    Layout layout;
    layout.preferredAddress = lowFloor;
    layout.mappedSize = 2 * pageSize;
    CacheReservation cache = {0x108000000ULL, 0x7e194000ULL};
    uintptr_t begin = 0, end = 0;
    std::string error;
    check(lowImageRange(layout, cache, pageSize, begin, end, error) && begin == lowFloor && end == cache.address,
          "full runtime cache reserves images only in the remaining low 128 MiB");
    check(lowImageRange(layout, {0x200000000ULL, pageSize}, pageSize, begin, end, error) && end == 0x180000000ULL,
          "a higher cache never permits image code in the host executable shared region");
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
        Layout bad;
        bad.preferredAddress = layout.preferredAddress;
        bad.mappedSize = layout.mappedSize;
        CacheReservation badCache = cache;
        size_t alignment = pageSize;
        switch (mutation) {
            case 0: alignment = 3; break;
            case 1: bad.mappedSize = 0; break;
            case 2: ++bad.mappedSize; break;
            case 3: badCache.size = UINTPTR_MAX; break;
            case 4: bad.preferredAddress = UINTPTR_MAX; break;
            case 5: badCache.address = lowFloor + pageSize; break;
            case 6: bad.preferredAddress = cache.address; break;
            case 7: badCache.size = 0; break;
        }
        begin = end = 1;
        check(!lowImageRange(bad, badCache, alignment, begin, end, error) && !begin && !end,
              "invalid or overflowing low image ranges fail with cleared output");
    }

    // Model a VM change between query and allocation. The second query observes the new occupied page.
    const uintptr_t first = reinterpret_cast<uintptr_t>(_dyld_get_image_header(0));
    layout.preferredAddress = first;
    bool raced = false;
    unsigned allocations = 0, queries = 0;
    regionHook = [&](vm_address_t *address, vm_size_t *size, mach_msg_type_number_t *, mach_port_t *object) {
        ++queries;
        *object = MACH_PORT_NULL;
        if (*address < first + 2 * pageSize) { *address = first; *size = 2 * pageSize; }
        else if (raced && *address < first + 3 * pageSize) { *address = first + 2 * pageSize; *size = pageSize; }
        else { *address = first + 5 * pageSize; *size = pageSize; }
        return KERN_SUCCESS;
    };
    allocationHook = [&](vm_address_t *address, vm_size_t size, int flags) {
        ++allocations;
        check(size == layout.mappedSize && flags == VM_FLAGS_FIXED, "constrained allocation uses fixed reservation without overwrite");
        if (!raced) { check(*address == first + 2 * pageSize, "first free gap is used"); raced = true; return KERN_NO_SPACE; }
        check(*address == first + 3 * pageSize, "collision retry advances to a still-free complete gap");
        return KERN_SUCCESS;
    };
    vm_address_t base = 99;
    check(reserveImage(layout, cache, base, error) && base == first + 3 * pageSize && allocations == 2 && queries == 3,
          "allocation race is retried safely after another VM query");

    queries = allocations = 0;
    regionHook = [&](vm_address_t *address, vm_size_t *size, mach_msg_type_number_t *, mach_port_t *) {
        ++queries;
        if (*address >= first + 2 * pageSize) return KERN_INVALID_ADDRESS;
        *address = first + pageSize;
        *size = pageSize;
        return KERN_SUCCESS;
    };
    allocationHook = [&](vm_address_t *address, vm_size_t, int flags) {
        ++allocations;
        check(*address == first + 2 * pageSize && flags == VM_FLAGS_FIXED, "skip a gap smaller than the complete image span");
        return KERN_SUCCESS;
    };
    check(reserveImage(layout, cache, base, error) && allocations == 1 && queries == 2, "insufficient gaps are skipped without probing occupied pages");
    allocations = 0;
    regionHook = [&](vm_address_t *address, vm_size_t *size, mach_msg_type_number_t *, mach_port_t *object) {
        *object = MACH_PORT_NULL;
        *address = first;
        *size = cache.address - first;
        return KERN_SUCCESS;
    };
    check(!reserveImage(layout, cache, base, error) && !base && !allocations && error.find("No free low image range") != std::string::npos,
          "occupied low window fails before attempting any replacement");

    regionHook = [](vm_address_t *, vm_size_t *, mach_msg_type_number_t *, mach_port_t *) { return KERN_INVALID_ADDRESS; };
    allocationHook = [&](vm_address_t *, vm_size_t, int flags) { ++allocations; check(flags == VM_FLAGS_FIXED, "empty-map retry remains fixed"); return KERN_NO_SPACE; };
    check(!reserveImage(layout, cache, base, error) && !base && allocations == 32 && error.find("changed repeatedly") != std::string::npos,
          "persistent allocation races have a bounded retry count");
    allocations = 0;
    allocationHook = [&](vm_address_t *address, vm_size_t, int) { ++allocations; check(*address == first, "end-of-map gap starts at lower bound"); return KERN_SUCCESS; };
    check(reserveImage(layout, cache, base, error) && base == first && allocations == 1, "end of the VM map is a usable empty gap");
    allocationHook = [](vm_address_t *, vm_size_t, int) { return KERN_PROTECTION_FAILURE; };
    check(!reserveImage(layout, cache, base, error) && !base && error.find("vm_allocate(low image reservation)") != std::string::npos,
          "non-collision allocation errors fail without retrying elsewhere");
    regionHook = [](vm_address_t *, vm_size_t *, mach_msg_type_number_t *, mach_port_t *) { return KERN_FAILURE; };
    check(!reserveImage(layout, cache, base, error) && !base && error.find("vm_region_64") != std::string::npos,
          "region query failure is not mistaken for a free range");
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
        regionHook = [&, mutation](vm_address_t *address, vm_size_t *size, mach_msg_type_number_t *count, mach_port_t *) {
            *size = pageSize;
            if (mutation == 0) *size = 0;
            if (mutation == 1) { *address = UINTPTR_MAX - pageSize + 1; *size = 2 * pageSize; }
            if (mutation == 2) *count = 0;
            if (mutation == 3) { *address = first - pageSize; }
            return KERN_SUCCESS;
        };
        check(!reserveImage(layout, cache, base, error) && !base && error.find("Invalid VM region") != std::string::npos,
              "malformed or stale region boundaries fail instead of wrapping or looping");
    }
    regionHook = {};
    allocationHook = {};

    // Exercise the kernel contract with real pages. A second reservation cannot erase an existing low image.
    cache = {0x180000000ULL, pageSize};
    check(reserveImage(layout, cache, base, error), error.c_str());
    auto *bytes = reinterpret_cast<unsigned char *>(base);
    bytes[0] = 0x39;
    bytes[layout.mappedSize - 1] = 0xa7;
    Layout occupied;
    occupied.preferredAddress = base;
    occupied.mappedSize = layout.mappedSize;
    vm_address_t other = 99;
    check(!reserveImage(occupied, {base + layout.mappedSize, pageSize}, other, error) && !other,
          "a window containing only an existing image cannot be allocated");
    check(bytes[0] == 0x39 && bytes[layout.mappedSize - 1] == 0xa7, "failed reservation leaves the existing image bytes intact");
    regionHook = [](vm_address_t *, vm_size_t *, mach_msg_type_number_t *, mach_port_t *) { return KERN_INVALID_ADDRESS; };
    check(!reserveImage(occupied, {base + layout.mappedSize, pageSize}, other, error) && !other,
          "kernel rejects an occupied reservation even when the preceding query incorrectly reported an empty gap");
    check(bytes[0] == 0x39 && bytes[layout.mappedSize - 1] == 0xa7, "a real fixed allocation collision cannot overwrite existing bytes");
    regionHook = {};
    check(::vm_deallocate(mach_task_self(), base, layout.mappedSize) == KERN_SUCCESS, "release real low reservation fixture");
    check(!reservationUsedOverwrite, "no image reservation requests VM_FLAGS_OVERWRITE");
}

static void expectGuard(void *address) {
    pid_t child = fork();
    check(child >= 0, "fork guard-page probe");
    if (!child) {
        signal(SIGBUS, [](int) { _exit(73); });
        signal(SIGSEGV, [](int) { _exit(73); });
        *static_cast<volatile unsigned char *>(address) = 1;
        _exit(1);
    }
    int status = 0;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 73, "stack guard page rejects writes");
}

static void guardedMainStackTests() {
    uintptr_t nativeTop = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
    size_t nativeSize = pthread_get_stacksize_np(pthread_self());
    std::string error;
    uintptr_t top = nativeTop;
    size_t size = nativeSize;
    unsigned before = stackReservations;
    for (CacheReservation cache : {CacheReservation{}, CacheReservation{0x120000000, 0x1000000}}) {
        GuardedMainStack stack;
        check(prepareMainStack(cache, top, size, stack, error) && stack.mapping == MAP_FAILED, "non-overlapping cache keeps the native stack");
        check(top == nativeTop && size == nativeSize && stackReservations == before, "legacy path does not reserve new memory");
    }
    CacheReservation overlap = {nativeTop - nativeSize, nativeSize};
    unsigned released = stackReleases;
    {
        GuardedMainStack stack;
        check(prepareMainStack(overlap, top, size, stack, error), "overlapping cache reserves a separate stack");
        check(top == stack.top && size == stack.size && size >= nativeSize && size >= 8 * 1024 * 1024, "relocated usable bounds preserve native capacity");
        check(reinterpret_cast<uintptr_t>(stack.mapping) >= 0x300000000ULL && !(top & 15), "new stack is high and ABI aligned");
        check(!rangesOverlap(reinterpret_cast<uintptr_t>(stack.mapping), stack.mappingSize, overlap.address, overlap.size), "guards also avoid cache reservation");
        const char *arguments[] = {"/RuntimeRoot/bin/df", nullptr};
        StartupVectors startup = {};
        check(!prepareStartupVectors(nullptr, arguments[0], "/RuntimeRoot", 1, arguments, nullptr, nullptr, top, size, startup),
              "prepare relocated pthread stack metadata");
        char metadata[96];
        snprintf(metadata, sizeof(metadata), "main_stack=0x%llx,0x%llx,0x0,0x0",
                 static_cast<unsigned long long>(top), static_cast<unsigned long long>(size));
        bool found = false;
        for (const char *const *item = startup.apple; *item; ++item) found |= !strcmp(*item, metadata);
        check(found, "main_stack publishes only new usable bounds and zero pthread allocation metadata");
        randomCalls = 0;
        *reinterpret_cast<volatile unsigned char *>(top - size) = 42;
        *reinterpret_cast<volatile unsigned char *>(top - 1) = 43;
        expectGuard(stack.mapping);
        expectGuard(reinterpret_cast<void *>(top));
        check(!stackUsedFixedMapping, "stack allocator never uses MAP_FIXED");
    }
    check(stackReleases == released + 1, "RAII releases the whole guarded reservation before a failed handoff returns");
    for (bool protectionFailure : {true, false}) {
        top = nativeTop; size = nativeSize; released = stackReleases;
        {
            GuardedMainStack stack;
            rejectStackProtection = protectionFailure;
            auto blocked = protectionFailure ? overlap : CacheReservation{0x1000, UINTPTR_MAX - 0x1000};
            check(!prepareMainStack(blocked, top, size, stack, error), "protection failure or allocation inside cache fails closed");
            check(top == nativeTop && size == nativeSize && !stack.top, "failed allocation never publishes new usable bounds");
            rejectStackProtection = false;
        }
        check(stackReleases == released + 1, "failed stack preparation releases its reservation");
    }
    top = UINTPTR_MAX; size = SIZE_MAX - 1; before = stackReservations;
    GuardedMainStack stack;
    check(!prepareMainStack({0x1000, 0x1000}, top, size, stack, error) && stackReservations == before, "allocation-size overflow fails before mmap");
}

static void preservedArgumentStackTests() {
    constexpr size_t reserved = LSB_NATIVE_ARGUMENT_STACK_SIZE, usable = LSB_GUEST_MAIN_STACK_SIZE;
    size_t pageSize = static_cast<size_t>(getpagesize());
    void *mapping = ::mmap(nullptr, reserved, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    check(mapping != MAP_FAILED, "allocate demand-zero native argument-stack fixture");
    uintptr_t top = reinterpret_cast<uintptr_t>(mapping) + reserved;
    uintptr_t guard = top - usable - pageSize;
    auto *arguments = reinterpret_cast<char *>(top - 4096);
    strcpy(arguments, "LSB:1234 SpringBoard");
    size_t size = reserved;
    std::string error;
    GuardedMainStack stack;
    unsigned before = stackReservations;
    check(!prepareMainStack({guard, pageSize}, top, size, stack, error) && size == reserved,
          "overlapping preserved argv stack fails before shrinking its bounds or mapping a cache");
    check(!strcmp(arguments, "LSB:1234 SpringBoard"), "collision does not alter native argv");
    rejectStackProtection = true;
    check(!prepareMainStack({}, top, size, stack, error) && size == reserved, "failed guard does not publish smaller stack bounds");
    rejectStackProtection = false;
    check(prepareMainStack({}, top, size, stack, error) && size == usable && stack.mapping == MAP_FAILED,
          "preserved kernel stack exposes only its upper 8 MiB without another stack allocation");
    check(top == reinterpret_cast<uintptr_t>(mapping) + reserved && stackReservations == before,
          "native argument stack keeps its original kernel-recorded top");
    check(!strcmp(arguments, "LSB:1234 SpringBoard"), "native argv survives the usable stack adjustment");
    *reinterpret_cast<volatile unsigned char *>(top - usable) = 42;
    expectGuard(reinterpret_cast<void *>(guard));
    check(!::munmap(mapping, reserved), "release native argument-stack fixture");
}

static void startupStorageBoundsTest() {
    StartupStorage storage;
    memset(&storage, 0xa5, sizeof(storage));
    std::vector<std::string> arguments = {"program", ""}, environment = {"KEEP=value"}, apple = {"main_stack=123", "ptr_munge=456"};
    size_t required = (5 + arguments.size() + environment.size() + apple.size()) * sizeof(uintptr_t);
    for (const auto *group : {&arguments, &environment, &apple})
        for (const auto &string : *group) required += string.size() + 1;
    StartupVectors vectors = {};
    for (size_t capacity : {size_t(0), size_t(5 * sizeof(uintptr_t) - 1), required - 1}) {
        check(copyStartupVectors(storage, capacity, nullptr, arguments, environment, apple, vectors) == E2BIG, "startup storage rejects overflow");
        check(!vectors.argv && !vectors.envp && !vectors.apple, "overflow does not publish any partial vector");
        auto bytes = reinterpret_cast<const unsigned char *>(&storage);
        check(std::all_of(bytes, bytes + sizeof(storage), [](unsigned char value) { return value == 0xa5; }), "overflow does not change storage");
    }
    check(copyStartupVectors(storage, sizeof(storage) + 1, nullptr, arguments, environment, apple, vectors) == EINVAL,
          "startup storage capacity cannot exceed the actual static allocation");
    check(!copyStartupVectors(storage, required, nullptr, arguments, environment, apple, vectors), "startup vectors fit at the exact byte boundary");
    auto bytes = reinterpret_cast<const unsigned char *>(&storage);
    check(std::all_of(bytes + required, bytes + sizeof(storage), [](unsigned char value) { return value == 0xa5; }), "exact fit preserves following bytes");
    check(vectors.argv == reinterpret_cast<const char *const *>(storage.words + 2), "startup kernel prefix remains contiguous with argv");
    check(vectors.envp == vectors.argv + 3 && vectors.apple == vectors.envp + 2, "all three vectors are contiguous including terminators");
    for (auto *group : {&arguments, &environment, &apple}) {
        for (auto &string : *group) std::fill(string.begin(), string.end(), '!');
        std::vector<std::string>().swap(*group);
    }
    check(!strcmp(vectors.argv[0], "program") && !strcmp(vectors.argv[1], "") && !strcmp(vectors.envp[0], "KEEP=value"),
          "owned startup strings survive destruction of all staging allocations");
    memset(const_cast<char *>(vectors.apple[0]) + 11, 0, 3);
    memset(const_cast<char *>(vectors.apple[1]) + 10, 0, 3);
    check(!strcmp(vectors.apple[0], "main_stack=") && !strcmp(vectors.apple[1], "ptr_munge="), "owned startup secrets remain writable");
    arguments.assign(startupStorageCapacity / sizeof(uintptr_t), "");
    StartupVectors unchanged = vectors;
    check(copyStartupVectors(storage, sizeof(storage), nullptr, arguments, environment, apple, vectors) == E2BIG,
          "pointer-table overflow is rejected even for empty strings");
    check(vectors.argv == unchanged.argv && vectors.envp == unchanged.envp && vectors.apple == unchanged.apple,
          "failed replacement leaves previously published vectors unchanged");
}

using Bytes = std::vector<uint8_t>;
static constexpr uint64_t preferred = 0x100000000, entryOffset = 1024;
static const size_t page = static_cast<size_t>(getpagesize());

template <typename T> static void put(Bytes &bytes, size_t offset, const T &value) {
    check(offset <= bytes.size() && sizeof(value) <= bytes.size() - offset, "fixture write bounds");
    memcpy(bytes.data() + offset, &value, sizeof(value));
}

template <typename T> static void edit(Bytes &bytes, size_t offset, const std::function<void(T &)> &change) {
    T value = read<T>(bytes.data() + offset);
    change(value);
    put(bytes, offset, value);
}

static constexpr size_t textCommand = sizeof(mach_header_64);
static constexpr size_t dataCommand = textCommand + sizeof(segment_command_64);
static constexpr size_t linkeditCommand = dataCommand + sizeof(segment_command_64);
static constexpr size_t signatureCommand = linkeditCommand + sizeof(segment_command_64);
static constexpr size_t entryCommand = signatureCommand + sizeof(linkedit_data_command);

static Bytes thin(bool mainEntry = false) {
    Bytes bytes(3 * page);
    segment_command_64 segment = {};
    segment.cmd = LC_SEGMENT_64;
    segment.cmdsize = sizeof(segment);
    strcpy(segment.segname, SEG_TEXT);
    segment.vmaddr = preferred;
    segment.vmsize = segment.filesize = page;
    segment.initprot = segment.maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
    put(bytes, textCommand, segment);
    strcpy(segment.segname, SEG_DATA);
    segment.vmaddr += page;
    segment.fileoff = page;
    segment.vmsize = 2 * page;
    segment.initprot = segment.maxprot = VM_PROT_READ | VM_PROT_WRITE;
    put(bytes, dataCommand, segment);
    strcpy(segment.segname, SEG_LINKEDIT);
    segment.vmaddr = preferred + 3 * page;
    segment.fileoff = 2 * page;
    segment.vmsize = page;
    segment.initprot = segment.maxprot = VM_PROT_READ;
    put(bytes, linkeditCommand, segment);
    linkedit_data_command signature = {LC_CODE_SIGNATURE, sizeof(signature), static_cast<uint32_t>(bytes.size() - 64), 64};
    put(bytes, signatureCommand, signature);
    uint32_t entrySize;
    if (mainEntry) {
        entry_point_command command = {LC_MAIN, sizeof(command), entryOffset, 0};
        put(bytes, entryCommand, command);
        entrySize = sizeof(command);
    } else {
        entrySize = sizeof(load_command) + 8 + 68 * sizeof(uint32_t);
        load_command command = {LC_UNIXTHREAD, entrySize};
        put(bytes, entryCommand, command);
        put<uint32_t>(bytes, entryCommand + 8, 6);
        put<uint32_t>(bytes, entryCommand + 12, 68);
        put<uint64_t>(bytes, entryCommand + 16 + 32 * sizeof(uint64_t), preferred + entryOffset);
    }
    mach_header_64 header = {MH_MAGIC_64, CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL, MH_DYLINKER, 5,
                             static_cast<uint32_t>(entryCommand + entrySize - sizeof(header)), 0, 0};
    put(bytes, 0, header);
    return bytes;
}

static constexpr size_t buildCommand = entryCommand + sizeof(entry_point_command);
static constexpr size_t pagezeroCommand = buildCommand + sizeof(build_version_command);

static Bytes executable() {
    Bytes bytes = thin(true);
    build_version_command build = {LC_BUILD_VERSION, sizeof(build), PLATFORM_IOSSIMULATOR, 0x110000, 0x110000, 0};
    put(bytes, buildCommand, build);
    segment_command_64 pagezero = {};
    pagezero.cmd = LC_SEGMENT_64;
    pagezero.cmdsize = sizeof(pagezero);
    strcpy(pagezero.segname, SEG_PAGEZERO);
    pagezero.vmsize = preferred;
    put(bytes, pagezeroCommand, pagezero);
    edit<mach_header_64>(bytes, 0, [](auto &header) {
        header.filetype = MH_EXECUTE;
        header.flags = MH_PIE;
        header.ncmds += 2;
        header.sizeofcmds += sizeof(build_version_command) + sizeof(segment_command_64);
    });
    return bytes;
}

static Bytes dylibVariant(Bytes bytes) {
    edit<mach_header_64>(bytes, 0, [](auto &header) { header.filetype = MH_DYLIB; });
    return bytes;
}

static Bytes universal(bool is64, bool swapped = true, const Bytes &slice = thin()) {
    Bytes bytes(page + slice.size());
    auto s32 = [swapped](uint32_t value) { return swapped ? OSSwapInt32(value) : value; };
    auto s64 = [swapped](uint64_t value) { return swapped ? OSSwapInt64(value) : value; };
    fat_header header = {is64 ? FAT_MAGIC_64 : FAT_MAGIC, 1};
    header.magic = s32(header.magic);
    header.nfat_arch = s32(header.nfat_arch);
    put(bytes, 0, header);
    uint32_t alignment = 0;
    while ((size_t(1) << alignment) < page) ++alignment;
    if (is64) {
        fat_arch_64 arch = {};
        arch.cputype = s32(CPU_TYPE_ARM64); arch.cpusubtype = s32(CPU_SUBTYPE_ARM64_ALL);
        arch.offset = s64(page); arch.size = s64(slice.size()); arch.align = s32(alignment);
        put(bytes, sizeof(header), arch);
    } else {
        fat_arch arch = {};
        arch.cputype = s32(CPU_TYPE_ARM64); arch.cpusubtype = s32(CPU_SUBTYPE_ARM64_ALL);
        arch.offset = s32(page); arch.size = s32(slice.size()); arch.align = s32(alignment);
        put(bytes, sizeof(header), arch);
    }
    memcpy(bytes.data() + page, slice.data(), slice.size());
    return bytes;
}

static void parse(const Bytes &bytes, const char *expectedError = nullptr, uint64_t sliceOffset = 0, uint32_t expectedType = MH_DYLINKER) {
    FileMapping file;
    file.bytes = const_cast<uint8_t *>(bytes.data());
    file.size = bytes.size();
    Layout layout;
    std::string error;
    bool valid = parseLayout(file, layout, error, expectedType);
    file.bytes = MAP_FAILED; // The fixture owns these bytes; FileMapping must not unmap its vector.
    if (expectedError) {
        check(!valid, "malformed fixture rejected");
        if (error.find(expectedError) == std::string::npos) fprintf(stderr, "Unexpected error: %s\n", error.c_str());
        check(error.find(expectedError) != std::string::npos, expectedError);
    } else {
        check(valid, error.c_str());
        check(layout.sliceOffset == sliceOffset && layout.sliceSize == 3 * page, "selected architecture slice");
        check(layout.entryOffset == entryOffset && layout.mappedSize == 4 * page, "entry and mapped extent");
    }
}

static void parserTests() {
    parse(thin()); parse(thin(true));
    parse(dylibVariant(thin())); parse(dylibVariant(thin(true)));
    parse(universal(true, true, dylibVariant(thin())), nullptr, page);
    parse(universal(false), nullptr, page); parse(universal(true), nullptr, page);
    parse(universal(false, false), nullptr, page); parse(universal(true, false), nullptr, page);
    auto bad = thin(); bad.resize(4); parse(bad, "too small");
    bad = thin(); put<uint32_t>(bad, 0, 0); parse(bad, "not a 64-bit");
    bad = thin(); edit<mach_header_64>(bad, 0, [](auto &h) { h.ncmds = UINT32_MAX; }); parse(bad, "load command bounds");
    bad = thin(); edit<mach_header_64>(bad, 0, [](auto &h) { h.sizeofcmds = UINT32_MAX; }); parse(bad, "load command bounds");
    bad = thin(); edit<mach_header_64>(bad, 0, [](auto &h) { h.cputype = CPU_TYPE_X86_64; }); parse(bad, "little-endian arm64");
    bad = thin(); edit<mach_header_64>(bad, 0, [](auto &h) { h.cpusubtype = CPU_SUBTYPE_ARM64E; }); parse(bad, "little-endian arm64");
    bad = thin(); edit<mach_header_64>(bad, 0, [](auto &h) { h.filetype = MH_EXECUTE; }); parse(bad, "MH_DYLINKER");
    bad = thin(); edit<load_command>(bad, textCommand, [](auto &c) { c.cmdsize = 9; }); parse(bad, "command size");
    bad = thin(); edit<load_command>(bad, textCommand, [](auto &c) { c.cmdsize = 8; }); parse(bad, "Truncated segment");
    bad = thin(); edit<segment_command_64>(bad, textCommand, [](auto &s) { s.nsects = 1; }); parse(bad, "segment sections");
    bad = thin(); edit<segment_command_64>(bad, dataCommand, [](auto &s) { s.filesize = UINT64_MAX; }); parse(bad, "segment bounds");
    bad = thin(); edit<segment_command_64>(bad, dataCommand, [](auto &s) { s.vmaddr++; }); parse(bad, "not aligned");
    bad = thin(); edit<segment_command_64>(bad, dataCommand, [](auto &s) { s.vmaddr = preferred; }); parse(bad, "overlap");
    bad = thin(); edit<segment_command_64>(bad, dataCommand, [](auto &s) { s.fileoff++; }); parse(bad, "file offset");
    bad = thin(); edit<load_command>(bad, signatureCommand, [](auto &c) { c.cmd = LC_UUID; }); parse(bad, "embedded code signature");
    bad = thin(); edit<linkedit_data_command>(bad, signatureCommand, [](auto &c) { c.datasize = UINT32_MAX; }); parse(bad, "signature bounds");
    bad = thin(true); edit<entry_point_command>(bad, entryCommand, [](auto &c) { c.entryoff += page; }); parse(bad, "outside executable");
    bad = thin(true); edit<entry_point_command>(bad, entryCommand, [](auto &c) { c.entryoff++; }); parse(bad, "unaligned");
    bad = thin(); put<uint32_t>(bad, entryCommand + 12, 69); parse(bad, "Truncated thread state");
    bad = thin(); put<uint32_t>(bad, entryCommand + 12, 67); parse(bad, "Invalid or duplicate arm64 entry");
    bad = universal(false); put<uint32_t>(bad, 4, OSSwapInt32(UINT32_MAX)); parse(bad, "architecture table");
    bad = universal(true); edit<fat_arch_64>(bad, 8, [](auto &a) { a.cputype = OSSwapInt32(CPU_TYPE_X86_64); }); parse(bad, "no compatible");
    bad = universal(true); edit<fat_arch_64>(bad, 8, [](auto &a) { a.size = OSSwapInt64(UINT64_MAX); }); parse(bad, "slice bounds");
    bad = universal(false); edit<fat_arch>(bad, 8, [](auto &a) { a.offset = OSSwapInt32(page + 1); }); parse(bad, "slice bounds");
}

static void executableParserTests() {
    parse(executable(), nullptr, 0, MH_EXECUTE);
    parse(dylibVariant(executable()), nullptr, 0, MH_EXECUTE);
    parse(universal(true, true, dylibVariant(executable())), nullptr, page, MH_EXECUTE);
    parse(universal(false, true, executable()), nullptr, page, MH_EXECUTE);
    parse(universal(true, true, executable()), nullptr, page, MH_EXECUTE);
    parse(thin(), "MH_EXECUTE", 0, MH_EXECUTE);
    auto bad = executable();
    edit<mach_header_64>(bad, 0, [](auto &header) { header.flags &= ~MH_PIE; });
    parse(bad, "MH_PIE", 0, MH_EXECUTE);
    parse(dylibVariant(bad), "MH_PIE", 0, MH_EXECUTE);
    bad = executable();
    edit<build_version_command>(bad, buildCommand, [](auto &build) { build.platform = PLATFORM_IOS; });
    parse(bad, nullptr, 0, MH_EXECUTE);
    parse(dylibVariant(bad), nullptr, 0, MH_EXECUTE);
    for (uint32_t platform : {PLATFORM_MACOS, PLATFORM_TVOS, PLATFORM_WATCHOS, PLATFORM_MACCATALYST, PLATFORM_TVOSSIMULATOR, 0, 99}) {
        edit<build_version_command>(bad, buildCommand, [=](auto &build) { build.platform = platform; });
        parse(bad, "single iOS or iOS Simulator", 0, MH_EXECUTE);
    }
    // Legacy device binaries keep their original load command rather than acquiring LC_BUILD_VERSION.
    auto legacy = executable();
    const size_t legacyPagezero = buildCommand + sizeof(version_min_command);
    memmove(legacy.data() + legacyPagezero, legacy.data() + pagezeroCommand, sizeof(segment_command_64));
    version_min_command minimum = {LC_VERSION_MIN_IPHONEOS, sizeof(minimum), 0x70000, 0};
    put(legacy, buildCommand, minimum);
    edit<mach_header_64>(legacy, 0, [](auto &header) { header.sizeofcmds -= sizeof(build_version_command) - sizeof(version_min_command); });
    parse(legacy, nullptr, 0, MH_EXECUTE);
    parse(dylibVariant(legacy), nullptr, 0, MH_EXECUTE);
    for (uint32_t command : {LC_VERSION_MIN_MACOSX, LC_VERSION_MIN_TVOS, LC_VERSION_MIN_WATCHOS}) {
        bad = legacy;
        edit<load_command>(bad, buildCommand, [=](auto &load) { load.cmd = command; });
        parse(bad, "single iOS or iOS Simulator", 0, MH_EXECUTE);
    }
    bad = executable();
    edit<load_command>(bad, buildCommand, [](auto &command) { command.cmd = LC_VERSION_MIN_IPHONEOS; });
    parse(bad, "Invalid minimum OS", 0, MH_EXECUTE);
    for (auto bytes : {executable(), legacy}) {
        auto header = read<mach_header_64>(bytes.data());
        put(bytes, sizeof(header) + header.sizeofcmds, minimum);
        ++header.ncmds;
        header.sizeofcmds += sizeof(minimum);
        put(bytes, 0, header);
        parse(bytes, "single iOS or iOS Simulator", 0, MH_EXECUTE);
    }
    bad = executable();
    edit<load_command>(bad, buildCommand, [](auto &command) { command.cmd = LC_UUID; });
    parse(bad, "iOS Simulator", 0, MH_EXECUTE);
    bad = executable();
    edit<segment_command_64>(bad, pagezeroCommand, [](auto &segment) { segment.vmsize += page; });
    parse(bad, "overlapping __PAGEZERO", 0, MH_EXECUTE);
    bad = thin(); // A valid legacy thread entry must not run the program during dyld_sim preparation.
    size_t legacyBuildCommand = sizeof(mach_header_64) + read<mach_header_64>(bad.data()).sizeofcmds;
    build_version_command build = {LC_BUILD_VERSION, sizeof(build), PLATFORM_IOSSIMULATOR, 0x110000, 0x110000, 0};
    put(bad, legacyBuildCommand, build);
    edit<mach_header_64>(bad, 0, [](auto &header) {
        header.filetype = MH_EXECUTE;
        header.flags = MH_PIE;
        ++header.ncmds;
        header.sizeofcmds += sizeof(build_version_command);
    });
    parse(bad, "LC_MAIN", 0, MH_EXECUTE);
    parse(dylibVariant(bad), "LC_MAIN", 0, MH_EXECUTE);
}

static void mappingFailure(const Bytes &bytes, bool afterReservation, bool isExecutable = false) {
    char path[] = "/tmp/LiveSimBooter-test-XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "create temporary fixture");
    check(write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()), "write fixture");
    close(fd);
    sourceMapping = MAP_FAILED; sourceFD = -1; signatureCalls = deallocations = fileUnmaps = 0;
    rejectSegmentMapping = afterReservation;
    LSBMappedImage image = {};
    char error[256];
    bool success = isExecutable ? LSBMapSimExecutable(path, &image, error, sizeof(error)) : LSBMapDyldSim(path, &image, error, sizeof(error));
    unlink(path);
    check(!success && error[0], "mapping failure reports an error");
    check(!image.header && !image.mappedSize && !image.slide && !image.entry, "failure resets output image");
    check(sourceFD >= 0 && ::fcntl(sourceFD, F_GETFD) == -1 && errno == EBADF, "failure closes source descriptor");
    check(fileUnmaps == 1, "failure unmaps source view exactly once");
    check(deallocations == (afterReservation ? 1 : 0), "failure releases reserved image exactly once");
    check(signatureCalls == (afterReservation ? 1 : 0), "signature registration occurs after parsing");
    if (afterReservation) {
        check(registeredSlice == static_cast<off_t>(page), "signature registration preserves universal slice offset");
        check(registeredSignature == 3 * page - 64, "signature blob offset remains relative to slice");
    }
    rejectSegmentMapping = false;
}

static void partialPageMapping(bool asDylib = false) {
    auto bytes = asDylib ? dylibVariant(thin()) : thin();
    edit<segment_command_64>(bytes, dataCommand, [](auto &s) { s.filesize = page - 16; });
    std::fill(bytes.begin() + page, bytes.begin() + 2 * page, 0xA5);
    parse(bytes);
    char path[] = "/tmp/LiveSimBooter-test-XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "create partial-page fixture");
    check(write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()), "write partial-page fixture");
    close(fd);
    LSBMappedImage image = {};
    char error[256];
    bool mapped = LSBMapDyldSim(path, &image, error, sizeof(error));
    unlink(path);
    check(mapped, error);
    check(image.header && image.header->filetype == (asDylib ? MH_DYLIB : MH_DYLINKER), "dyld mapping retains signed header type");
    auto *data = reinterpret_cast<const uint8_t *>(image.header) + page;
    check(std::all_of(data, data + page - 16, [](uint8_t value) { return value == 0xA5; }), "mapped data retains file-backed bytes");
    check(std::all_of(data + page - 16, data + 2 * page, [](uint8_t value) { return value == 0; }), "partial and full zero-fill pages are zero");
    check(image.entry == reinterpret_cast<uintptr_t>(image.header) + entryOffset, "mapped entry resolves relative to actual base");
    check(image.slide == reinterpret_cast<uintptr_t>(image.header) - preferred, "mapped slide resolves from preferred base");
    int previousDeallocations = deallocations;
    LSBUnloadImage(&image);
    check(!image.header && !image.entry && deallocations == previousDeallocations + 1, "successful image unload resets and releases image");
    LSBUnloadImage(&image);
    check(deallocations == previousDeallocations + 1, "unload of cleared image is harmless");
}

static void executableMapping(const Bytes &bytes, uint64_t sliceOffset) {
    char path[] = "/tmp/LiveSimBooter-test-XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "create executable fixture");
    check(write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()), "write executable fixture");
    close(fd);
    sourceMapping = MAP_FAILED; sourceFD = -1; signatureCalls = deallocations = fileUnmaps = 0;
    LSBMappedImage image = {};
    char error[256];
    bool mapped = LSBMapSimExecutable(path, &image, error, sizeof(error));
    unlink(path);
    check(mapped, error);
    auto originalHeader = read<mach_header_64>(bytes.data() + sliceOffset);
    check(image.header && image.header->filetype == originalHeader.filetype, "mapped simulator executable retains its signed Mach-O header");
    check(image.mappedSize == 4 * page, "PAGEZERO is excluded from executable allocation");
    check(image.entry == reinterpret_cast<uintptr_t>(image.header) + entryOffset, "executable entry resolves relative to mapped header");
    check(image.slide == reinterpret_cast<uintptr_t>(image.header) - preferred, "executable slide resolves relative to preferred base");
    check(signatureCalls == 1 && registeredSlice == static_cast<off_t>(sliceOffset), "executable registers selected slice signature");
    check(registeredSignature == 3 * page - 64, "executable signature blob offset remains relative to slice");
    check(fileUnmaps == 1 && sourceFD >= 0 && ::fcntl(sourceFD, F_GETFD) == -1 && errno == EBADF, "executable closes source mapping and descriptor");
    LSBUnloadImage(&image);
    check(!image.header && !image.mappedSize && !image.slide && !image.entry && deallocations == 1, "executable unload releases and clears image");
}

static void runtimeImageMappingTests() {
    char directory[] = "/tmp/LiveSimBooter-runtime-XXXXXX";
    check(mkdtemp(directory), "create runtime mapping fixture");
    std::string root = directory;
    std::vector<std::string> directories = {root + "/System", root + "/System/Library", root + "/System/Library/Caches",
                                            root + "/System/Library/Caches/com.apple.dyld"};
    for (const auto &path : directories) check(!mkdir(path.c_str(), 0700), "create cache fixture parent");
    std::string cachePath = directories.back() + "/dyld_sim_shared_cache_arm64";
    auto header = cacheHeader();
    header.sharedRegionStart = 0x108000000ULL;
    header.sharedRegionSize = 2 * page;
    auto writeCache = [&] {
        int fd = open(cachePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        check(fd >= 0 && write(fd, &header, sizeof(header)) == sizeof(header) && !ftruncate(fd, sizeof(header) + 32), "write runtime cache header");
        close(fd);
    };
    writeCache();
    std::string imagePath = root + "/image";
    auto writeImage = [&](const Bytes &bytes) {
        int fd = open(imagePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        check(fd >= 0 && write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()), "write runtime image fixture");
        close(fd);
    };
    for (bool isExecutable : {false, true}) {
        writeImage(isExecutable ? executable() : thin());
        sourceMapping = MAP_FAILED; sourceFD = -1; signatureCalls = deallocations = fileUnmaps = 0;
        LSBMappedImage image = {};
        char error[512];
        auto map = [&](const char *runtime) {
            return isExecutable ? LSBMapSimExecutableForRuntime(imagePath.c_str(), runtime, &image, error, sizeof(error)) :
                                  LSBMapDyldSimForRuntime(imagePath.c_str(), runtime, &image, error, sizeof(error));
        };
        check(map(root.c_str()), error);
        uintptr_t address = reinterpret_cast<uintptr_t>(image.header);
        check(address >= 0x100000000ULL && image.mappedSize <= header.sharedRegionStart - address,
              "runtime-aware entry maps the entire image below the cache");
        check(fileUnmaps == 1 && !sourceViewLive && !allocatedWithParseView, "source parse view is released exactly once before image allocation");
        check(sourceFD >= 0 && ::fcntl(sourceFD, F_GETFD) == -1 && errno == EBADF, "runtime mapping closes its source descriptor");
        LSBUnloadImage(&image);
        check(deallocations == 1, "runtime mapped image owns exactly one reservation");
        check(!map(nullptr) && !image.header && strstr(error, "absolute RuntimeRoot"), "runtime-aware API rejects a missing root without mapping");
        check(strstr(error, isExecutable ? "Simulator executable:" : "dyld_sim:"), "mapping errors identify the failing image kind");
    }

    // An image that cannot fit fails after parsing without retaining either the source map or an image reservation.
    header.sharedRegionStart = 0x100000000ULL + page;
    writeCache();
    LSBMappedImage image = {};
    char error[512];
    sourceMapping = MAP_FAILED; sourceFD = -1; signatureCalls = deallocations = fileUnmaps = 0;
    check(!LSBMapSimExecutableForRuntime(imagePath.c_str(), root.c_str(), &image, error, sizeof(error)) && strstr(error, "does not fit"),
          "insufficient executable window reports an explicit allocation bound failure");
    check(!image.header && !image.mappedSize && fileUnmaps == 1 && !sourceViewLive && deallocations == 0,
          "low reservation failure clears image output and releases only the source view");
    check(sourceFD >= 0 && ::fcntl(sourceFD, F_GETFD) == -1 && errno == EBADF, "low reservation failure closes the image descriptor");

    // Select a vacant window in this macOS process the way the broker selects a window for the device session.
    vm_size_t windowSize = LSB_SLIDABLE_CACHE_PREFERRED_BASE + LSB_NATIVE_SUPPORT_CACHE_SPAN;
    vm_address_t window = LSB_SLIDABLE_CACHE_PREFERRED_BASE;
    check(vm_allocate(mach_task_self(), &window, windowSize, VM_FLAGS_ANYWHERE) == KERN_SUCCESS &&
          window >= LSB_SLIDABLE_CACHE_PREFERRED_BASE, "locate a free window for the high mapping tests");
    check(vm_deallocate(mach_task_self(), window, windowSize) == KERN_SUCCESS, "release the probe for the actual image reservations");
    configuredCacheBase = window + LSB_SLIDABLE_CACHE_PREFERRED_BASE;
    configuredNativeSupport = true;
    header.sharedRegionStart = LSB_SLIDABLE_CACHE_PREFERRED_BASE;
    header.sharedRegionSize = LSB_NATIVE_SUPPORT_CACHE_SPAN;
    header.uuid[0] = 1;
    writeCache();
    auto mapMain = [&](LSBMappedImage &output) { return LSBMapSimExecutableForRuntime(imagePath.c_str(), root.c_str(), &output, error, sizeof(error)); };
    uintptr_t mainAddress = window + LSB_GUEST_MAIN_IMAGE_OFFSET, bootstrapAddress = window + LSB_FORK_BOOTSTRAP_IMAGE_OFFSET;
    stripSegmentExecute = true;
    check(!mapMain(image) && strstr(error, "requires protection") && !image.header, "a successful high mmap without execute permission releases the reservation");
    stripSegmentExecute = false;
    check(mapMain(image), error);
    check(reinterpret_cast<uintptr_t>(image.header) == mainAddress && image.slide == window && image.entry == mainAddress + entryOffset,
          "native-support executable and cache share the window slide, including the executable entry point");
    std::string protectionError;
    check(!checkExecutableMapping(mainAddress, 2 * page, VM_PROT_READ | VM_PROT_EXECUTE, protectionError),
          "the execute permission check reaches the second VM region when a request crosses the text/data boundary");
    auto *data = reinterpret_cast<uint8_t *>(mainAddress + page);
    data[page] = 93;
    LSBMappedImage second = {};
    check(!mapMain(second) && strstr(error, "Cannot reserve guest executable") && !second.header && !reservationUsedOverwrite,
          "a second ordinary executable cannot replace the first reservation or fall back to a different address");
    check(LSBMapForkBootstrapForRuntime(imagePath.c_str(), root.c_str(), &second, error, sizeof(error)), error);
    check(reinterpret_cast<uintptr_t>(second.header) == bootstrapAddress && second.slide == bootstrapAddress - preferred &&
          second.entry == bootstrapAddress + entryOffset && data[page] == 93, "the bootstrap has a separate slot and preserves the first image's data");
    LSBUnloadImage(&second);
    LSBUnloadImage(&image);
    writeImage(thin());
    check(LSBMapDyldSimForRuntime(imagePath.c_str(), root.c_str(), &image, error, sizeof(error)), error);
    check(reinterpret_cast<uintptr_t>(image.header) + image.mappedSize <= LSB_SLIDABLE_CACHE_PREFERRED_BASE, "dyld_sim still uses its low reservation");
    LSBUnloadImage(&image);
    configuredNativeSupport = false;
    header.sharedRegionSize = LSB_SLIDABLE_CACHE_SPAN;
    writeCache();
    writeImage(executable());
    check(mapMain(image), error);
    check(reinterpret_cast<uintptr_t>(image.header) + image.mappedSize <= LSB_SLIDABLE_CACHE_PREFERRED_BASE, "original slidable profile keeps low executables");
    LSBUnloadImage(&image);
    configuredCacheBase = 0;
    check(!unlink(cachePath.c_str()), "remove cache fixture");
    check(LSBMapSimExecutableForRuntime(imagePath.c_str(), root.c_str(), &image, error, sizeof(error)),
          "runtime without a cache retains loose-library image mapping");
    LSBUnloadImage(&image);
    unlink(imagePath.c_str());
    for (auto path = directories.rbegin(); path != directories.rend(); ++path) check(!rmdir(path->c_str()), "remove cache fixture directory");
    check(!rmdir(root.c_str()), "remove runtime fixture");
}

static SimSyscallHelpers fakeHelpers = {};
static std::function<void(unsigned)> preparationAllocationHook;
void LSBPrepareHostPolicy() { if (preparationAllocationHook) preparationAllocationHook(0); }
static const mach_header *debuggerExecutable;
static std::string debuggerExecutablePath;
static bool checkedFinalThreadCount;
int LSBValidateHostThreadCount() { checkedFinalThreadCount = true; return 0; }
int LSBPrepareHostDebugger(const mach_header *executable, const char *executablePath) {
    debuggerExecutable = executable;
    debuggerExecutablePath = executablePath;
    if (preparationAllocationHook) preparationAllocationHook(2);
    return 0;
}
int LSBPrepareHostMainThread(lsb::MainThreadBootstrap *bootstrap) {
    *bootstrap = {0x1000, 0x2000};
    if (preparationAllocationHook) preparationAllocationHook(1);
    return 0;
}
static const mach_header *initializedHeader = nullptr;
static std::string initializedPath;
const SimSyscallHelpers *LSBGetSimSyscallHelpers() { return &fakeHelpers; }
int LSBPrepareSlidableCacheData(const char *, uint64_t actualBase, const uint8_t *, bool, LSBSlidableCacheProfileKind) { return actualBase ? ENOTSUP : 0; }
int LSBPrepareNativeSupportState(uintptr_t, size_t, char *, size_t) { return ENOTSUP; }
uintptr_t LSBOverrideNativeSupportSymbol(const char *, const char *, uintptr_t address) { return address; }
int LSBPrepareNativeSupportBindings(const char *, uint64_t, LSBNativeSupportOverrideCallback, char *, size_t) { return ENOTSUP; }
int LSBActivateNativeSupportState(void) { return ENOTSUP; }
namespace lsb {
uintptr_t currentTSDBase() { return 0x1000; }
[[noreturn]] void exitProcess(int status) { _exit(status); }
}
const LSBSlidableDataHeader *LSBPreparedSlidableCacheData() { return nullptr; }
bool LSBInitializeSimProcessInfo(const mach_header *header, const char *path) {
    initializedHeader = header; initializedPath = path; return true;
}

using FakeMain = int (*)(int, const char *const[], const char *const[], const char *const[]);
static unsigned entryCalls = 0, mainCalls = 0;
static mach_header fakeExecutable = {}, fakeDyld = {};
static int fakeMain(int, const char *const[], const char *const[], const char *const[]) { ++mainCalls; return 0; }
static FakeMain fakeEntry(int argc, const char *const argv[], const char *const envp[], const char *const apple[], const mach_header *executable,
                          const mach_header *dyld, uintptr_t slide, const SimSyscallHelpers *helpers, uintptr_t *startGlue) {
    ++entryCalls;
    auto *kernelArgs = reinterpret_cast<const uintptr_t *>(argv) - 2;
    check(kernelArgs[0] == reinterpret_cast<uintptr_t>(executable) && kernelArgs[1] == 2, "kernel-style prefix before argv");
    check(argc == 2 && std::string(argv[0]) == "/RuntimeRoot/bin/df" && std::string(argv[1]) == "arg" && !argv[2], "copied argv and terminator");
    check(envp == argv + argc + 1, "environment follows argv contiguously");
    check(std::string(envp[0]) == "KEEP=value" && std::string(envp[3]) == "DYLD_ROOT_PATH=/RuntimeRoot", "root override preserves other environment");
    check(std::string(envp[4]) == "DYLD_SHARED_CACHE_DIR=/RuntimeRoot/System/Library/Caches/com.apple.dyld", "cache directory override");
    size_t environmentCount = 0, diagnosticSettings = 0;
    for (; envp[environmentCount]; ++environmentCount) {
        std::string value = envp[environmentCount];
        if (value.rfind("DYLD_PRINT_", 0) == 0) {
            ++diagnosticSettings;
            check(value == "DYLD_PRINT_TO_STDERR=0" || value == "DYLD_PRINT_TO_FILE=/old.log", "explicit diagnostic settings are preserved");
        }
    }
    check(diagnosticSettings == 2 && environmentCount == 5, "no default simulator diagnostics are injected");
    check(std::string(envp[1]) == "DYLD_PRINT_TO_STDERR=0" && std::string(envp[2]) == "DYLD_PRINT_TO_FILE=/old.log", "diagnostic output is unchanged");
    check(apple == envp + environmentCount + 1, "apple vector follows expanded environment contiguously");
    unsigned executablePaths = 0, preservedApple = 0, mainStacks = 0, ptrMungeTokens = 0;
    for (const char *const *item = apple; *item; ++item) {
        std::string value = *item;
        check(value.rfind("executable_file=", 0) != 0, "host executable file identity is removed");
        if (value.rfind("executable_path=", 0) == 0) {
            ++executablePaths;
            check(value == "executable_path=/RuntimeRoot/bin/df", "apple executable path names mapped df");
        } else if (value.rfind("main_stack=", 0) == 0) {
            ++mainStacks;
            checkAndConsumeMainStack(*item);
        } else if (value.rfind("ptr_munge=", 0) == 0) {
            ++ptrMungeTokens;
            check(value.rfind("ptr_munge=0x", 0) == 0, "token includes the prefix required by Apple's pthread parser");
            char *end = nullptr;
            auto token = strtoull(*item + 10, &end, 16);
            check(token == freshPtrMungeToken && end && !*end, "simulator receives a fresh nonzero hexadecimal ptr_munge token");
            memset(const_cast<char *>(*item) + 10, 0, value.size() - 10);
            check(std::string(*item) == "ptr_munge=", "simulator can consume and erase its own token storage");
        } else {
            ++preservedApple;
            check(value == "stack_guard=0x1234", "unrelated apple value is preserved");
        }
    }
    check(executablePaths == 1 && preservedApple == 1, "apple has one replacement executable path and preserved values");
    check(mainStacks == 1, "all consumed or stale main_stack entries are replaced exactly once");
    check(ptrMungeTokens == 1 && randomCalls == 2, "all inherited tokens are replaced and zero random values are retried");
    check(executable == &fakeExecutable && dyld == &fakeDyld && slide == 42 && helpers == &fakeHelpers, "entry ABI image and helper arguments");
    check(initializedHeader == dyld && initializedPath == "/RuntimeRoot/usr/lib/dyld_sim", "process info initialized before entry");
    check(debuggerExecutable == executable && debuggerExecutablePath == "/RuntimeRoot/bin/df", "debugger metadata receives mapped executable and path");
    check(startGlue && *startGlue == 0, "start glue output initialized");
    *startGlue = 123;
    return fakeMain;
}

struct HandoffComplete {};
int LSBDetachHostCacheAndEnter(const LSBDyldSimLaunch *launch) {
    check(checkedFinalThreadCount, "native worker guard runs before destructive cache detachment");
    check(launch->thread.hostTSD == 0x1000 && launch->thread.simulatorTSD == 0x2000, "handoff receives prepared thread storage");
    uintptr_t nativeTop = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
    check(!mainStackContains(nativeTop, pthread_get_stacksize_np(pthread_self()), reinterpret_cast<uintptr_t>(launch)),
          "launch descriptor survives replacement of the native stack");
    check(!launch->stackTop, "absent cache keeps the existing handoff stack");
    check(!launch->cacheAddress && !launch->cacheSize, "absent cache does not reserve an address range");
    check(launch->retainHostCache, "normal loader handoff retains the native cache without an explicit override");
    uintptr_t base = reinterpret_cast<uintptr_t>(startupStorage.words);
    auto inStorage = [&](const void *pointer, size_t length) {
        uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
        return address >= base && contains(sizeof(startupStorage.words), address - base, length);
    };
    check(inStorage(reinterpret_cast<const uintptr_t *>(launch->argv) - 2, 2 * sizeof(uintptr_t)), "kernel argument prefix lives in app-image storage");
    for (const char *const *vector : {launch->argv, launch->envp, launch->apple}) {
        for (const char *const *item = vector;; ++item) {
            check(inStorage(item, sizeof(*item)), "every vector entry and terminator lives in app-image storage");
            if (!*item) break;
            check(inStorage(*item, 1), "every startup string starts in app-image storage, outside native malloc zones");
            size_t available = sizeof(startupStorage.words) - (reinterpret_cast<uintptr_t>(*item) - base);
            check(memchr(*item, 0, available) != nullptr, "every complete startup string and terminator fits inside app-image storage");
        }
    }
    // The real implementation never returns after entry. Model that boundary without detaching this test process's cache.
    uintptr_t startGlue = 0;
    auto main = launch->entry(launch->argc, launch->argv, launch->envp, launch->apple, launch->executable,
                              launch->dyld, launch->slide, launch->helpers, &startGlue);
    check(main == fakeMain && startGlue == 123, "handoff receives the returned main and start glue");
    throw HandoffComplete{};
}

struct TerminalStartupFixture {
    std::vector<std::string> arguments, environment, apple, expectedEnvironment, expectedApple;
    std::vector<std::pair<uintptr_t, size_t>> inputRanges;
    unsigned prepared = 0;

    explicit TerminalStartupFixture(bool large) {
        arguments = {"/RuntimeRoot/bin/fish", "-i", "-N", "-P", "-C",
                     "status job-control none; set -g fish_use_posix_spawn 1; set -g fish_greeting; function fish_prompt; printf 'fish> '; end"};
        environment = {"TERM=xterm-256color", "HOME=/RuntimeRoot/home/mobile", "PATH=/RuntimeRoot/bin:/RuntimeRoot/usr/bin",
                       "LSB_FAKE_PTY_STATE=/RuntimeRoot/.terminal-state", "DYLD_INSERT_LIBRARIES=/RuntimeRoot/usr/lib/libLiveSimSpawn.dylib",
                       "DYLD_ROOT_PATH=/old", "DYLD_SHARED_CACHE_DIR=/old-cache"};
        apple = {"executable_path=/app/LiveProcess", "executable_file=0x123,0x456", "stack_guard=0x1234",
                 "main_stack=0x100000,0x10000", "main_stack=", "ptr_munge=", "ptr_munge=0x1234"};
        if (large) {
            // Force repeated vector reallocations and both short-string and heap-backed storage.
            for (unsigned i = 0; i < 240; ++i) {
                arguments.emplace_back(i % 4 == 0 ? "" : std::string(i % 4 == 1 ? 15 : 257 + i, 'a' + i % 26));
                environment.emplace_back("FILL_" + std::to_string(i) + "=" + std::string(i % 4 == 0 ? 0 : 23 + i * 7, 'A' + i % 26));
                apple.emplace_back("extra_" + std::to_string(i) + "=" + std::string(i % 4 == 0 ? 0 : 17 + i * 5, '0' + i % 10));
            }
            environment.emplace_back("DYLD_ROOT_PATH=/second-old");
            environment.emplace_back("DYLD_SHARED_CACHE_DIR=/second-old-cache");
            apple.emplace_back("executable_path=/second-old-app");
            apple.emplace_back("executable_file=0x789,0xabc");
            apple.emplace_back("main_stack=0x180000,0x1000,0x170000,0x11000");
            apple.emplace_back("ptr_munge=0xffff");
        }
        for (const auto &item : environment)
            if (item.rfind("DYLD_ROOT_PATH=", 0) != 0 && item.rfind("DYLD_SHARED_CACHE_DIR=", 0) != 0) expectedEnvironment.push_back(item);
        expectedEnvironment.emplace_back("DYLD_ROOT_PATH=/RuntimeRoot");
        expectedEnvironment.emplace_back("DYLD_SHARED_CACHE_DIR=/RuntimeRoot/System/Library/Caches/com.apple.dyld");
        for (const auto &item : apple)
            if (item.rfind("executable_path=", 0) != 0 && item.rfind("executable_file=", 0) != 0 &&
                item.rfind("main_stack=", 0) != 0 && item.rfind("ptr_munge=", 0) != 0)
                expectedApple.push_back(item);
        expectedApple.emplace_back("executable_path=/RuntimeRoot/bin/fish");
        expectedApple.emplace_back(expectedMainStack());
        expectedApple.emplace_back("ptr_munge=0xa739c450e16b28df");
    }

    std::vector<const char *> pointers(std::vector<std::string> &strings) {
        std::vector<const char *> result;
        for (auto &item : strings) {
            result.push_back(item.data());
            inputRanges.emplace_back(reinterpret_cast<uintptr_t>(item.data()), item.size() + 1);
        }
        result.push_back(nullptr);
        return result;
    }

    void checkVector(const char *const actual[], const std::vector<std::string> &expected, const char *message) const {
        for (size_t i = 0; i < expected.size(); ++i) {
            check(actual[i] != nullptr, "startup vector has no premature null entry");
            check(strcmp(actual[i], expected[i].c_str()) == 0, message);
            uintptr_t pointer = reinterpret_cast<uintptr_t>(actual[i]);
            bool aliasesInput = false;
            for (const auto &range : inputRanges) aliasesInput |= pointer >= range.first && pointer - range.first < range.second;
            check(!aliasesInput, "startup strings own storage separate from every original vector");
        }
        check(actual[expected.size()] == nullptr, "startup vector has its final null terminator");
    }
};

static TerminalStartupFixture *terminalFixture;
static FakeMain terminalEntry(int argc, const char *const argv[], const char *const envp[], const char *const apple[], const mach_header *executable,
                              const mach_header *dyld, uintptr_t slide, const SimSyscallHelpers *helpers, uintptr_t *startGlue) {
    ++entryCalls;
    auto &fixture = *terminalFixture;
    check(fixture.prepared == 7, "all native preparation allocation stages completed before entry");
    check(argc == static_cast<int>(fixture.arguments.size()), "terminal argument count survives native preparation");
    const auto *kernelArgs = reinterpret_cast<const uintptr_t *>(argv) - 2;
    check(kernelArgs[0] == reinterpret_cast<uintptr_t>(executable) && kernelArgs[1] == static_cast<uintptr_t>(argc), "terminal kernel-style prefix");
    fixture.checkVector(argv, fixture.arguments, "terminal argument contents survive native allocation");
    auto reconstructedEnvironment = argv + kernelArgs[1] + 1;
    check(reconstructedEnvironment == envp, "dyld reconstructs the exact supplied environment pointer");
    fixture.checkVector(envp, fixture.expectedEnvironment, "terminal environment contents and override order survive native allocation");
    auto reconstructedApple = reconstructedEnvironment;
    while (*reconstructedApple) ++reconstructedApple;
    ++reconstructedApple;
    check(reconstructedApple == apple, "dyld finds the exact supplied apple vector after scanning environment");
    fixture.checkVector(apple, fixture.expectedApple, "terminal apple contents and replacement order survive native allocation");
    check(randomCalls == 2, "terminal startup retries zero before generating a fresh pointer token");
    size_t tokenIndex = fixture.expectedApple.size() - 1;
    checkAndConsumeMainStack(apple[tokenIndex - 1]);
    char *token = const_cast<char *>(apple[tokenIndex]);
    check(strncmp(token, "ptr_munge=0x", 12) == 0 && !apple[tokenIndex + 1], "fresh pointer token is the final apple entry");
    size_t tokenLength = strlen(token);
    memset(token + 10, 0, tokenLength - 10);
    check(strcmp(token, "ptr_munge=") == 0, "terminal pointer token remains writable for libpthread consumption");
    check(apple[tokenIndex - 1] && !apple[tokenIndex + 1], "token erasure preserves adjacent vector entries and final terminator");
    check(executable == &fakeExecutable && dyld == &fakeDyld && slide == 42 && helpers == &fakeHelpers, "terminal entry retains image and helper ABI");
    check(debuggerExecutable == executable && debuggerExecutablePath == "/RuntimeRoot/bin/fish", "terminal debugger preparation receives the original path");
    check(startGlue && *startGlue == 0, "terminal start glue is initialized");
    *startGlue = 123;
    return fakeMain;
}

static void terminalEntryTest(bool large) {
    // Each child starts before the parent's one permitted handoff; this is host-test isolation, not guest fork support.
    pid_t child = fork();
    check(child >= 0, "create isolated terminal startup test");
    if (child == 0) {
        unsigned initialChecks = checks;
        TerminalStartupFixture fixture(large);
        terminalFixture = &fixture;
        auto inputArguments = fixture.arguments, inputEnvironment = fixture.environment, inputApple = fixture.apple;
        auto argv = fixture.pointers(inputArguments), envp = fixture.pointers(inputEnvironment), apple = fixture.pointers(inputApple);
        preparationAllocationHook = [&](unsigned stage) {
            check(stage < 3 && fixture.prepared == (1u << stage) - 1, "native allocation hooks run in handoff order");
            fixture.prepared |= 1u << stage;
            if (stage == 0) {
                for (auto *strings : {&inputArguments, &inputEnvironment, &inputApple})
                    for (auto &item : *strings) std::fill(item.begin(), item.end(), '!');
            }
            std::vector<std::string> churn;
            for (unsigned i = 0; i < 512; ++i) churn.emplace_back((i * 37 + stage * 17) % 8192, 'A' + i % 26);
            check(churn.size() == 512 && !churn.back().empty(), "native preparation exercises allocator growth and release");
        };
        LSBMappedImage image = {&fakeDyld, 0, 42, reinterpret_cast<uintptr_t>(&terminalEntry)};
        bool handedOff = false;
        try {
            LSBEnterDyldSim(&image, &fakeExecutable, "/RuntimeRoot/bin/fish", "/RuntimeRoot/usr/lib/dyld_sim", "/RuntimeRoot",
                           static_cast<int>(inputArguments.size()), argv.data(), envp.data(), apple.data());
        } catch (const HandoffComplete &) { handedOff = true; }
        check(handedOff && entryCalls == 1 && !mainCalls, "terminal prepare reaches the destructive handoff boundary exactly once");
        printf("Terminal startup %s vector passed (%u checks).\n", large ? "large" : "short", checks - initialChecks);
        fflush(stdout);
        _exit(0);
    }
    int status = 0;
    check(waitpid(child, &status, 0) == child, "wait for isolated terminal startup test");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "terminal startup vector survives native preparation allocations");
}

static void slidableDyldTests() {
    check(LSBConfigureRuntimeCacheBase(0x300000001) == EINVAL && LSBConfigureRuntimeCacheBase(0x100000000) == EINVAL,
          "trusted cache base still requires a valid aligned high range");
    std::vector<uint8_t> bytes(0x84000);
    auto *header = reinterpret_cast<mach_header_64 *>(bytes.data());
    header->magic = MH_MAGIC_64; header->cputype = CPU_TYPE_ARM64; header->cpusubtype = CPU_SUBTYPE_ARM64_ALL;
    header->ncmds = 1; header->sizeofcmds = sizeof(segment_command_64) + sizeof(section_64);
    auto *segment = reinterpret_cast<segment_command_64 *>(header + 1);
    segment->cmd = LC_SEGMENT_64; segment->cmdsize = header->sizeofcmds;
    memcpy(segment->segname, "__DATA_DIRTY", sizeof("__DATA_DIRTY"));
    segment->vmaddr = 0x7c000; segment->vmsize = 0x4000; segment->fileoff = 0x5c000; segment->filesize = 0x4000;
    segment->initprot = segment->maxprot = VM_PROT_READ | VM_PROT_WRITE; segment->nsects = 1;
    auto *section = reinterpret_cast<section_64 *>(segment + 1);
    section->addr = 0x7c000; section->size = 0x154;
    LSBMappedImage image = {reinterpret_cast<const mach_header *>(header), bytes.size(), reinterpret_cast<uintptr_t>(header), 0};
    auto *configuration = reinterpret_cast<LSBDyldSlideConfiguration *>(bytes.data() + LSB_DYLD_SLIDE_CONFIG_VM_OFFSET);
    const LSBDyldSlideConfiguration initial = {LSB_DYLD_SLIDE_CONFIG_MAGIC, 1, sizeof(*configuration), LSB_DYLD_SLIDE_PREFERRED_BASE, 0, 0, {}};
    std::string error;
    for (uint64_t base : {UINT64_C(0x300000000), UINT64_C(0x7000000000)}) {
        check(!LSBConfigureRuntimeCacheBase(base), "host may select either tested address before images are mapped");
        LSBSlidableDataHeader backing = {};
        backing.preferredBase = LSB_DYLD_SLIDE_PREFERRED_BASE; backing.actualBase = base; backing.cacheSpan = LSB_SLIDABLE_CACHE_SPAN;
        backing.cacheUUID[0] = 0x42;
        *configuration = initial;
        auto original = bytes;
        check(configureSlidableDyld(image, backing, error), "native preparation accepts the pinned writable marker");
        check(configuration->actualBase == base && configuration->cacheSpan == backing.cacheSpan && configuration->cacheUUID[0] == 0x42,
              "selected base and cache identity are written to DATA");
        *configuration = initial;
        check(bytes == original, "no bytes outside the writable config are changed, including TEXT and all Mach-O commands");
        section->size = 0x300;
        check(!configureSlidableDyld(image, backing, error), "configuration may not overlap any original section");
        section->size = 0x154;
        configuration->magic[0] ^= 1;
        check(!configureSlidableDyld(image, backing, error), "missing dynamic-loader marker fails before entry");
        *configuration = initial;
        ++backing.actualBase;
        check(!configureSlidableDyld(image, backing, error), "sidecar base must equal trusted bootstrap base");
    }
    check(!LSBConfigureRuntimeCacheBase(0), "ordinary cache fallback clears the selection before mapping");
}

static void entryTest() {
    LSBMappedImage image = {&fakeDyld, 0, 42, reinterpret_cast<uintptr_t>(&fakeEntry)};
    const char *argv[] = {"/RuntimeRoot/bin/df", "arg", nullptr};
    const char *envp[] = {"DYLD_ROOT_PATH=/old", "KEEP=value", "DYLD_SHARED_CACHE_DIR=/old-cache", "DYLD_PRINT_TO_STDERR=0",
                          "DYLD_PRINT_TO_FILE=/old.log", nullptr};
    const char *apple[] = {"executable_path=/app", "executable_file=0x123,0x456", "stack_guard=0x1234", "ptr_munge=", "ptr_munge=0x1234",
                           "main_stack=", "main_stack=0x1,0x2,0x3,0x4", nullptr};
    auto enter = [&] {
        return LSBEnterDyldSim(&image, &fakeExecutable, "/RuntimeRoot/bin/df", "/RuntimeRoot/usr/lib/dyld_sim", "/RuntimeRoot", 2, argv, envp, apple);
    };
    bool handedOff = false;
    try { enter(); } catch (const HandoffComplete &) { handedOff = true; }
    check(handedOff && entryCalls == 1 && mainCalls == 0, "entry is called without returning to host cleanup or invoking main");
    check(std::string(envp[3]) == "DYLD_PRINT_TO_STDERR=0", "simulator environment copy leaves host diagnostics untouched");
    check(std::string(envp[4]) == "DYLD_PRINT_TO_FILE=/old.log", "simulator logging leaves inherited host output path untouched");
    check(std::string(apple[3]) == "ptr_munge=" && std::string(apple[4]) == "ptr_munge=0x1234", "simulator token erasure leaves host apple data untouched");
    check(std::string(apple[5]) == "main_stack=" && std::string(apple[6]) == "main_stack=0x1,0x2,0x3,0x4", "stack erasure leaves host apple untouched");
    check(enter() == 1 && entryCalls == 1, "second entry is rejected");
}

static void offMainEntryTest() {
    pthread_t worker;
    int result = 0;
    int error = pthread_create(&worker, nullptr, [](void *context) -> void * {
        LSBMappedImage image = {&fakeDyld, 0, 42, reinterpret_cast<uintptr_t>(&fakeEntry)};
        const char *argv[] = {"/RuntimeRoot/bin/df", nullptr};
        *static_cast<int *>(context) = LSBEnterDyldSim(&image, &fakeExecutable, argv[0], "/RuntimeRoot/usr/lib/dyld_sim", "/RuntimeRoot",
                                                    1, argv, nullptr, nullptr);
        return nullptr;
    }, &result);
    check(!error, "create a native worker to exercise the main-thread handoff guard");
    check(!pthread_join(worker, nullptr), "join the rejected handoff worker");
    check(result == 1 && !initializedHeader && !entryCalls && !checkedFinalThreadCount, "off-main handoff rejects before initialization or detachment");
}

static void oversizedEntryTest() {
    LSBMappedImage image = {&fakeDyld, 0, 42, reinterpret_cast<uintptr_t>(&fakeEntry)};
    std::string oversized(startupStorageCapacity, 'x');
    const char *argv[] = {oversized.c_str(), nullptr};
    check(LSBEnterDyldSim(&image, &fakeExecutable, "/bin/df", "/usr/lib/dyld_sim", "/RuntimeRoot", 1, argv, nullptr, nullptr) == 1 && errno == E2BIG,
          "oversized startup arguments fail explicitly with E2BIG");
    check(!entryCalls && !checkedFinalThreadCount, "oversized startup data fails before host preparation and cache detachment");
    randomCalls = 0;
}

int main() {
    slidableDyldTests();
    check(!LSBTraceHandoffRequested(nullptr), "successful handoff diagnostics are disabled by default");
    const char *trace[] = {"LSB_TRACE_HANDOFF_EXTRA=1", "LSB_TRACE_HANDOFF=1", "LSB_TRACE_HANDOFF=0", nullptr};
    const char *noTrace[] = {"LSB_TRACE_HANDOFF=0", "LSB_TRACE_HANDOFF=1", nullptr};
    const char *missingTrace[] = {"LSB_TRACE_HANDOFF", "LSB_TRACE_HANDOFF_EXTRA=1", nullptr};
    check(LSBTraceHandoffRequested(trace) && !LSBTraceHandoffRequested(noTrace) && !LSBTraceHandoffRequested(missingTrace),
          "handoff tracing requires the first exact environment key to equal one");
    for (const char *value : {"", "01", "true", "1=extra", "-1"}) {
        std::string entry = std::string("LSB_TRACE_HANDOFF=") + value;
        const char *environment[] = {entry.c_str(), "LSB_TRACE_HANDOFF=1", nullptr};
        check(!LSBTraceHandoffRequested(environment), "invalid explicit trace values do not enable success logging");
    }
    check(retainHostCacheRequested(nullptr), "native cache retention is enabled by default");
    const char *retain[] = {"LSB_RETAIN_HOST_CACHE_EXTRA=1", "LSB_RETAIN_HOST_CACHE=1", "LSB_RETAIN_HOST_CACHE=0", nullptr};
    const char *disabled[] = {"LSB_RETAIN_HOST_CACHE=0", "LSB_RETAIN_HOST_CACHE=1", nullptr};
    const char *malformed[] = {"LSB_RETAIN_HOST_CACHE=01", nullptr};
    const char *unrelated[] = {"LSB_RETAIN_HOST_CACHE_EXTRA=0", "LSB_RETAIN_HOST_CACHE", nullptr};
    check(retainHostCacheRequested(unrelated), "unrelated and missing-value entries do not disable the default");
    check(retainHostCacheRequested(retain) && !retainHostCacheRequested(disabled) && !retainHostCacheRequested(malformed),
          "cache retention requires the first exact environment key to equal one");
    for (const char *value : {"", "true", "1=extra", "-1"}) {
        std::string entry = std::string("LSB_RETAIN_HOST_CACHE=") + value;
        const char *environment[] = {entry.c_str(), "LSB_RETAIN_HOST_CACHE=1", nullptr};
        check(!retainHostCacheRequested(environment), "invalid explicit first values disable retention without consulting later duplicates");
    }
    parserTests();
    executableParserTests();
    auto bad = thin(); put<uint32_t>(bad, 0, 0);
    mappingFailure(bad, false);
    mappingFailure(universal(true), true);
    partialPageMapping();
    partialPageMapping(true);
    rejectSignature = true; lazySignatureError = 0;
    partialPageMapping();
    check(lazySignatureRetries == 1, "native loader retries failed registration through lazy signing before mapping segments");
    rejectSignature = false; lazySignatureError = ENOTSUP;
    mappingFailure(thin(), false, true);
    mappingFailure(universal(true, true, executable()), true, true);
    executableMapping(executable(), 0);
    executableMapping(universal(false, true, executable()), page);
    executableMapping(universal(true, true, executable()), page);
    executableMapping(dylibVariant(executable()), 0);
    executableMapping(universal(true, true, dylibVariant(executable())), page);
    check(LSBConfigureRuntimeCacheBase(0x300000000) == EBUSY, "cache base cannot change after any image mapping starts");
    mainStackBoundsTest();
    nativeStackExclusionTests();
    cacheReservationTests();
    lowImageReservationTests();
    runtimeImageMappingTests();
    guardedMainStackTests();
    preservedArgumentStackTests();
    startupStorageBoundsTest();
    offMainEntryTest();
    oversizedEntryTest();
    terminalEntryTest(false);
    terminalEntryTest(true);
    entryTest();
    printf("DyldSimLoader host tests passed (%u checks). Signature authorization and device execution are not tested.\n", checks);
    return 0;
}
