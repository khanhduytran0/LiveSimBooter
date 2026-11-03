#include <assert.h>
#include <dirent.h>
#include <sys/mman.h>
#include <time.h>
#define mach_vm_map testMachVMMap
#define mach_vm_deallocate testMachVMDeallocate
#include "../ProcessSupport/SlidableCache.c"
#undef mach_vm_map
#undef mach_vm_deallocate

static unsigned reservationCalls, releaseCalls;
static kern_return_t reservationError, releaseError;
static mach_vm_address_t selectedAddress = UINT64_C(0x7100000000);
static mach_vm_size_t expectedProbeSize = LSB_SLIDABLE_CACHE_RESERVATION_SIZE;
kern_return_t testMachVMMap(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t size, mach_vm_offset_t mask, int flags,
                            mem_entry_name_port_t object, memory_object_offset_t offset, boolean_t copy,
                            vm_prot_t current, vm_prot_t maximum, vm_inherit_t inheritance) {
    assert(task == mach_task_self() && *address == LSB_SLIDABLE_CACHE_PREFERRED_BASE && size == expectedProbeSize);
    assert(!mask && flags == VM_FLAGS_ANYWHERE && !object && !offset && !copy && inheritance == VM_INHERIT_NONE);
    assert(current == (VM_PROT_READ | VM_PROT_EXECUTE) && maximum == current);
    ++reservationCalls; *address = selectedAddress;
    return reservationError;
}
kern_return_t testMachVMDeallocate(vm_map_t task, mach_vm_address_t address, mach_vm_size_t size) {
    assert(task == mach_task_self() && address == selectedAddress && size == expectedProbeSize);
    ++releaseCalls;
    return releaseError;
}

static const uint8_t testUUID[16] = {0xf7, 0x97, 0x08, 0xe3, 0x14, 0xb8, 0x59, 0x68, 0xa6, 0x55, 0x9e, 0x5e, 0x57, 0x91, 0xef, 0xdd};
static const uint64_t testBase = UINT64_C(0x7000000000);

static uint64_t get64(const uint8_t *bytes) { uint64_t value; memcpy(&value, bytes, 8); return value; }
static void put64(uint8_t *bytes, uint64_t value) { memcpy(bytes, &value, 8); }
static void pageTests(void) {
    uint8_t page[4096] = {0};
    SlideCounts counts = {0};
    const uint64_t preferred = LSB_SLIDABLE_CACHE_PREFERRED_BASE, span = LSB_SLIDABLE_CACHE_SPAN;
    // Unaligned-to-eight but four-byte-aligned pointer, a tagged pointer, and a final null chain value.
    put64(page + 4, preferred + 0x120 | UINT64_C(12) << 38);
    put64(page + 16, UINT64_C(0xab00000000000000) | preferred + 0x456 | UINT64_C(8) << 38);
    assert(!rebasePage(page, 1, preferred, span, testBase, &counts));
    assert(get64(page + 4) == testBase + 0x120 && get64(page + 16) == (UINT64_C(0xab00000000000000) | testBase + 0x456));
    assert(get64(page + 24) == 0 && counts.fixups == 3 && counts.tagged == 1);
    assert(!rebasePage(page, 0x4000, preferred, span, testBase, &counts) && counts.fixups == 3);
    assert(rebasePage(page, 0x8000, preferred, span, testBase, &counts) == ENOTSUP);
    assert(rebasePage(page, 0xffff, preferred, span, testBase, &counts) == ENOTSUP);
    assert(rebasePage(page, 1023, preferred, span, testBase, &counts) == EINVAL);
    put64(page, preferred - 1); assert(rebasePage(page, 0, preferred, span, testBase, &counts) == EINVAL);
    put64(page, preferred + span); assert(rebasePage(page, 0, preferred, span, testBase, &counts) == EINVAL);
    put64(page, preferred | UINT64_C(4) << 38); assert(rebasePage(page, 0, preferred, span, testBase, &counts) == EINVAL);
    put64(page, preferred | UINT64_C(4096) << 38); assert(rebasePage(page, 0, preferred, span, testBase, &counts) == EINVAL);
    put64(page, preferred); assert(rebasePage(page, 0, preferred, span, UINT64_MAX, &counts) == EOVERFLOW);
    SlideInfoV2 slide = {2, 4096, 40, 4, 0, 0, UINT64_C(0x00ffff0000000000), 0};
    LSBSlidableCacheMapping mapping = {.size = 16384, .slideInfoSize = 48};
    assert(!validateSlideInfo(&slide, &mapping));
    slide.extrasCount = 1; assert(validateSlideInfo(&slide, &mapping) == ENOTSUP); slide.extrasCount = 0;
    slide.deltaMask = 0; assert(validateSlideInfo(&slide, &mapping) == ENOTSUP); slide.deltaMask = UINT64_C(0x00ffff0000000000);
    slide.startsOffset = UINT32_MAX; assert(validateSlideInfo(&slide, &mapping) == EINVAL); slide.startsOffset = 40;
    slide.startsCount = UINT32_MAX; assert(validateSlideInfo(&slide, &mapping) == EINVAL);
}

static LSBSlidableCacheInfo fixtureInfoForProfile(LSBSlidableCacheProfileKind kind) {
    const LSBSlidableCacheLayout *layout = LSBSlidableCacheLayoutForProfile(kind);
    assert(layout);
    LSBSlidableCacheInfo info = {.preferredBase = LSB_SLIDABLE_CACHE_PREFERRED_BASE, .span = layout->span, .mappingCount = 6};
    memcpy(info.uuid, testUUID, 16); memcpy(info.mappings, layout->mappings, 6 * sizeof(*layout->mappings));
    info.fileSize = info.mappings[5].fileOffset + info.mappings[5].size + 0x4000;
    return info;
}
static LSBSlidableCacheInfo fixtureInfo(void) { return fixtureInfoForProfile(LSBSlidableCacheOriginal); }
static void headerTests(void) {
    LSBSlidableCacheInfo info = fixtureInfo();
    LSBSlidableDataHeader good, bad;
    fillHeader(&info, testBase, &good, LSBSlidableCacheOriginal);
    assert(LSBValidateSlidableDataHeader(&good, good.fileSize, testUUID, info.preferredBase, info.span));
    char name[LSB_SLIDABLE_DATA_NAME_CAPACITY];
    LSBSlidableDataFileName(testUUID, testBase, name);
    assert(!strcmp(name, "lsb-slide-f79708e3-14b8-5968-a655-9e5e5791efdd-7000000000.data"));
    for (unsigned failure = 0; failure < 16; ++failure) {
        bad = good;
        switch (failure) {
            case 0: bad.magic[0] = 0; break;
            case 1: ++bad.version; break;
            case 2: ++bad.headerSize; break;
            case 3: bad.cacheUUID[0] ^= 1; break;
            case 4: bad.actualBase = UINT64_MAX; break;
            case 5: bad.actualBase = 0; break;
            case 6: ++bad.cacheSpan; break;
            case 7: --bad.fixupCount; break;
            case 8: ++bad.taggedCount; break;
            case 9: ++bad.mappingCount; break;
            case 10: ++bad.mappings[0].backingFileOffset; break;
            case 11: bad.mappings[1].maxProtection = 7; break;
            case 12: bad.mappings[2].size = UINT64_MAX; break;
            case 13: ++bad.mappings[3].reserved; break;
            case 14: ++bad.mappings[0].sourceFileOffset; break;
            case 15: ++bad.fileSize; break;
        }
        assert(!LSBValidateSlidableDataHeader(&bad, good.fileSize, testUUID, info.preferredBase, info.span));
    }
}

static void selectionTests(void) {
    LSBSlidableCacheInfo info = fixtureInfo();
    uint64_t base = 0;
    assert(!LSBSelectSlidableCacheBase(&info, &base) && base == selectedAddress);
    assert(reservationCalls == 1 && releaseCalls == 1); // The synthetic unmapped address was never accessed.
    reservationError = KERN_NO_SPACE;
    assert(LSBSelectSlidableCacheBase(&info, &base) == ENOMEM && reservationCalls == 2 && releaseCalls == 1);
    reservationError = KERN_PROTECTION_FAILURE;
    assert(LSBSelectSlidableCacheBase(&info, &base) == EACCES && releaseCalls == 1);
    reservationError = KERN_SUCCESS; selectedAddress += 1;
    assert(LSBSelectSlidableCacheBase(&info, &base) == ERANGE && releaseCalls == 2);
    selectedAddress -= 1; releaseError = KERN_FAILURE;
    assert(LSBSelectSlidableCacheBase(&info, &base) == EIO && releaseCalls == 3);
    releaseError = KERN_SUCCESS;
    assert(LSBSelectSlidableCacheBase(NULL, &base) == EINVAL);
}

static void makeCacheForProfile(int fd, LSBSlidableCacheProfileKind kind) {
    LSBSlidableCacheInfo info = fixtureInfoForProfile(kind);
    CacheHeader header = {.mappingOffset = 0x200, .mappingCount = 6, .signatureOffset = info.fileSize - 0x4000, .signatureSize = 0x4000,
        .platform = 7, .formatFlags = 0x200, .preferredBase = info.preferredBase, .span = info.span, .maxSlide = 0x20000000,
        .mappingWithSlideOffset = 0x400, .mappingWithSlideCount = 6, .imagesOffset = 0x1000, .imagesCount = 1353};
    memcpy(header.magic, "dyld_v1   arm64", 16); memcpy(header.uuid, testUUID, 16);
    assert(!ftruncate(fd, (off_t)info.fileSize) && !writeAt(fd, &header, sizeof(header), 0));
    assert(!writeAt(fd, info.mappings, 6 * sizeof(*info.mappings), header.mappingWithSlideOffset));
    for (unsigned i = 0; i < 6; ++i) {
        const LSBSlidableCacheMapping *m = &info.mappings[i];
        struct { uint64_t address, size, offset; uint32_t maxprot, initprot; } legacy = {m->address, m->size, m->fileOffset, m->maxProtection, m->initialProtection};
        assert(!writeAt(fd, &legacy, sizeof(legacy), header.mappingOffset + i * sizeof(legacy)));
        if (!m->slideInfoSize) continue;
        SlideInfoV2 slide = {2, 4096, 40, (uint32_t)(m->size / 4096), 0, 0, UINT64_C(0x00ffff0000000000), 0};
        assert(!writeAt(fd, &slide, sizeof(slide), m->slideInfoOffset));
        uint16_t *starts = malloc(slide.startsCount * 2); assert(starts);
        for (uint32_t j = 0; j < slide.startsCount; ++j) starts[j] = 0x4000;
        assert(!writeAt(fd, starts, slide.startsCount * 2, m->slideInfoOffset + 40)); free(starts);
    }
    uint64_t mask = UINT64_C(0x007ffffffffffff8); assert(!writeAt(fd, &mask, sizeof(mask), 0x61bb8));
}
static void fileTests(void) {
    char directory[] = "/tmp/LSB-slidable-test.XXXXXX";
    assert(mkdtemp(directory));
    int dir = open(directory, O_RDONLY | O_DIRECTORY); assert(dir >= 0);
    int fd = openat(dir, "cache", O_CREAT | O_EXCL | O_RDWR, 0600); assert(fd >= 0);
    makeCacheForProfile(fd, LSBSlidableCacheOriginal);
    LSBSlidableCacheInfo info;
    assert(LSBInspectSlidableCache(fd, &info) == 1);
    uint32_t platform = 1; assert(!writeAt(fd, &platform, 4, 216));
    assert(LSBInspectSlidableCache(fd, &info) == -1 && errno == ENOTSUP);
    platform = 7; assert(!writeAt(fd, &platform, 4, 216));
    assert(LSBInspectSlidableCache(fd, &info) == 1);
    const struct { uint64_t offset, replacement; unsigned bytes; int error; } malformedHeaders[] = {
        {offsetof(CacheHeader, formatFlags), 0, 4, ENOTSUP},
        {offsetof(CacheHeader, subCacheArrayCount), 1, 4, ENOTSUP},
        {offsetof(CacheHeader, maxSlide), 0, 8, ENOTSUP},
        {offsetof(CacheHeader, imagesCount), 1352, 4, ENOTSUP},
        {offsetof(CacheHeader, mappingWithSlideOffset), UINT32_MAX, 4, EINVAL},
        {offsetof(CacheHeader, signatureSize), UINT64_MAX, 8, EINVAL},
        {0x200, UINT64_MAX, 8, EINVAL}, // Legacy and slide mappings must agree.
        {0x61bb8, UINT64_C(0x0000000ffffffff8), 8, ENOTSUP},
    };
    for (unsigned i = 0; i < sizeof(malformedHeaders) / sizeof(*malformedHeaders); ++i) {
        uint64_t saved = 0;
        assert(!readAt(fd, &saved, malformedHeaders[i].bytes, malformedHeaders[i].offset));
        assert(!writeAt(fd, &malformedHeaders[i].replacement, malformedHeaders[i].bytes, malformedHeaders[i].offset));
        assert(LSBInspectSlidableCache(fd, &info) == -1 && errno == malformedHeaders[i].error);
        assert(!writeAt(fd, &saved, malformedHeaders[i].bytes, malformedHeaders[i].offset));
    }
    assert(LSBInspectSlidableCache(fd, &info) == 1);
    LSBSlidableCachePreparation prepared;
    // A malformed first chain must fail before publication; the O_EXCL temporary must be removed.
    uint16_t malformed = 1023; assert(!writeAt(fd, &malformed, 2, info.mappings[1].slideInfoOffset + 40));
    assert(LSBPrepareSlidableCache(fd, dir, testBase, &prepared) == EINVAL);
    assert(faccessat(dir, prepared.fileName, F_OK, 0) && errno == ENOENT);
    DIR *scan = opendir(directory); assert(scan); unsigned entries = 0;
    for (struct dirent *entry; (entry = readdir(scan));) if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) ++entries;
    assert(entries == 1); closedir(scan);
    // Existing immutable data may be reused without regenerating or rewriting its inode.
    fillHeader(&info, testBase, &prepared.header, LSBSlidableCacheOriginal); LSBSlidableDataFileName(info.uuid, testBase, prepared.fileName);
    int data = openat(dir, prepared.fileName, O_CREAT | O_EXCL | O_RDWR, 0600); assert(data >= 0);
    assert(!ftruncate(data, (off_t)prepared.header.fileSize) && !writeAt(data, &prepared.header, sizeof(prepared.header), 0));
    assert(LSBPrepareSlidableCache(fd, dir, testBase, &prepared) == EINVAL); // A writable file is not immutable prepared data.
    assert(!fchmod(data, 0444));
    struct stat first, second; assert(!fstat(data, &first)); close(data);
    assert(!LSBPrepareSlidableCache(fd, dir, testBase, &prepared) && prepared.reused);
    assert(!fstatat(dir, prepared.fileName, &second, 0) && first.st_ino == second.st_ino);
    assert(!unlinkat(dir, prepared.fileName, 0));
    assert(!symlinkat("cache", dir, prepared.fileName));
    assert(LSBPrepareSlidableCache(fd, dir, testBase, &prepared) == ELOOP);
    assert(!unlinkat(dir, prepared.fileName, 0));
    // Ordinary caches with no slide records keep their existing loader path.
    for (unsigned i = 0; i < 6; ++i) info.mappings[i].slideInfoSize = 0;
    assert(!writeAt(fd, info.mappings, 6 * sizeof(*info.mappings), 0x400));
    assert(LSBInspectSlidableCache(fd, &info) == 0);
    close(fd); assert(!unlinkat(dir, "cache", 0)); close(dir); assert(!rmdir(directory));
}

static void nativeSupportProfileTests(void) {
    const LSBSlidableCacheProfileKind kind = LSBSlidableCacheNativeSupport;
    LSBSlidableCacheInfo info = fixtureInfoForProfile(kind);
    LSBSlidableDataHeader header;
    fillHeader(&info, testBase, &header, kind);
    assert(!LSBValidateSlidableDataHeader(&header, header.fileSize, testUUID, info.preferredBase, info.span));
    assert(LSBValidateSlidableDataHeaderWithProfile(&header, header.fileSize, testUUID, info.preferredBase, info.span, kind));
    assert(!LSBValidateSlidableDataHeaderWithProfile(&header, header.fileSize, testUUID, info.preferredBase, info.span, 42));
    assert(!LSBSlidableCacheMappingMatchesProfile(&info.mappings[0], 0));
    assert(LSBSlidableCacheMappingMatchesProfileKind(&info.mappings[0], 0, kind));
    assert(!LSBSlidableCacheMappingMatchesProfileKind(&info.mappings[0], 0, 42));
    uint64_t base = 0;
    assert(LSBSelectSlidableCacheBase(&info, &base) == EINVAL);
    expectedProbeSize = UINT64_C(0x1fe14c000);
    selectedAddress = UINT64_C(0x300000000);
    assert(!LSBSelectSlidableCacheBaseWithProfile(&info, &base, kind) && base == UINT64_C(0x480000000));
    selectedAddress = UINT64_C(0x7000000000);
    assert(!LSBSelectSlidableCacheBaseWithProfile(&info, &base, kind) && base == UINT64_C(0x7180000000));
    selectedAddress = UINT64_MAX - 0x3fff;
    assert(LSBSelectSlidableCacheBaseWithProfile(&info, &base, kind) == ERANGE);
    expectedProbeSize = LSB_SLIDABLE_CACHE_RESERVATION_SIZE;
    header.fixupCount = LSB_SLIDABLE_CACHE_FIXUPS;
    assert(!LSBValidateSlidableDataHeaderWithProfile(&header, header.fileSize, testUUID, info.preferredBase, info.span, kind));

    char directory[] = "/tmp/LSB-native-support-test.XXXXXX";
    assert(mkdtemp(directory));
    int dir = open(directory, O_RDONLY | O_DIRECTORY); assert(dir >= 0);
    int fd = openat(dir, "cache", O_CREAT | O_EXCL | O_RDWR, 0600); assert(fd >= 0);
    makeCacheForProfile(fd, kind);
    assert(LSBInspectSlidableCache(fd, &info) == -1 && errno == ENOTSUP);
    assert(LSBInspectSlidableCacheWithProfile(fd, &info, kind) == 1);
    assert(info.span == LSB_NATIVE_SUPPORT_CACHE_SPAN);
    assert(LSBInspectSlidableCacheWithProfile(fd, &info, 42) == -1 && errno == EINVAL);
    assert(LSBInspectSlidableCacheWithProfile(fd, &info, kind) == 1);
    LSBSlidableCachePreparation prepared;
    assert(LSBPrepareSlidableCache(fd, dir, testBase, &prepared) == ENOTSUP);
    fillHeader(&info, testBase, &prepared.header, kind);
    LSBSlidableDataFileName(info.uuid, testBase, prepared.fileName);
    int data = openat(dir, prepared.fileName, O_CREAT | O_EXCL | O_RDWR, 0600); assert(data >= 0);
    assert(!ftruncate(data, (off_t)prepared.header.fileSize) && !writeAt(data, &prepared.header, sizeof(prepared.header), 0));
    assert(!fchmod(data, 0444)); close(data);
    assert(!LSBPrepareSlidableCacheWithProfile(fd, dir, testBase, &prepared, kind) && prepared.reused);
    assert(!unlinkat(dir, prepared.fileName, 0));
    close(fd); assert(!unlinkat(dir, "cache", 0)); close(dir); assert(!rmdir(directory));
}

static void profileDiscoveryTests(void) {
    char path[] = "/tmp/LSB-cache-profile-test.XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    assert(!unlink(path));
    LSBSlidableCacheInfo info, empty = {0};
    LSBSlidableCacheProfileKind selected = LSBSlidableCacheNativeSupport;
    assert(LSBInspectSlidableCacheProfile(-1, &info, &selected) == -1 && errno == EBADF);
    assert(selected == LSBSlidableCacheOriginal && !memcmp(&info, &empty, sizeof(info)));
    assert(LSBInspectSlidableCacheProfile(fd, NULL, &selected) == -1 && errno == EINVAL);
    assert(LSBInspectSlidableCacheProfile(fd, &info, NULL) == -1 && errno == EINVAL);
    assert(LSBInspectSlidableCacheProfile(fd, &info, &selected) == -1 && errno == EINVAL); // Truncated header is not a profile mismatch.

    for (unsigned profile = LSBSlidableCacheOriginal; profile <= LSBSlidableCacheNativeSupport; ++profile) {
        LSBSlidableCacheProfileKind expected = (LSBSlidableCacheProfileKind)profile;
        makeCacheForProfile(fd, expected);
        selected = (LSBSlidableCacheProfileKind)42;
        assert(LSBInspectSlidableCacheProfile(fd, &info, &selected) == 1 && selected == expected);
        assert(info.span == LSBSlidableCacheLayoutForProfile(expected)->span && !memcmp(info.uuid, testUUID, sizeof(testUUID)));
        const struct { uint64_t offset, value; unsigned size; int error; } invalid[] = {
            {offsetof(CacheHeader, span), UINT64_C(0x7e150000), 8, ENOTSUP}, // Unknown, otherwise well-formed slide profile.
            {offsetof(CacheHeader, mappingWithSlideOffset), UINT32_MAX, 4, EINVAL},
            {offsetof(CacheHeader, signatureSize), 0, 8, EINVAL},
            {0x200, UINT64_MAX, 8, EINVAL}, // Known profile with inconsistent legacy mappings.
            {0x400 + offsetof(LSBSlidableCacheMapping, initialProtection), 1, 4, ENOTSUP},
            {info.mappings[1].slideInfoOffset + offsetof(SlideInfoV2, startsCount), 1, 4, EINVAL},
            {info.mappings[1].slideInfoOffset + offsetof(SlideInfoV2, version), 3, 4, ENOTSUP},
        };
        for (unsigned index = 0; index < sizeof(invalid) / sizeof(*invalid); ++index) {
            uint64_t saved = 0;
            assert(!readAt(fd, &saved, invalid[index].size, invalid[index].offset));
            assert(!writeAt(fd, &invalid[index].value, invalid[index].size, invalid[index].offset));
            selected = LSBSlidableCacheNativeSupport;
            memset(&info, 0xa5, sizeof(info));
            assert(LSBInspectSlidableCacheProfile(fd, &info, &selected) == -1 && errno == invalid[index].error);
            assert(selected == LSBSlidableCacheOriginal && !memcmp(&info, &empty, sizeof(info)));
            assert(!writeAt(fd, &saved, invalid[index].size, invalid[index].offset));
        }
        assert(LSBInspectSlidableCacheProfile(fd, &info, &selected) == 1 && selected == expected);
        // The right span alone must never select native support over a different mapping profile.
        uint64_t otherSpan = LSBSlidableCacheLayoutForProfile(expected == LSBSlidableCacheOriginal ?
                                                            LSBSlidableCacheNativeSupport : LSBSlidableCacheOriginal)->span;
        assert(!writeAt(fd, &otherSpan, sizeof(otherSpan), offsetof(CacheHeader, span)));
        int mismatchError = expected == LSBSlidableCacheOriginal ? ENOTSUP : EINVAL; // Native signature extent is too short for Original.
        assert(LSBInspectSlidableCacheProfile(fd, &info, &selected) == -1 && errno == mismatchError);
    }
    makeCacheForProfile(fd, LSBSlidableCacheOriginal);
    uint32_t noSlides = 0;
    assert(!writeAt(fd, &noSlides, sizeof(noSlides), offsetof(CacheHeader, mappingWithSlideCount)));
    selected = LSBSlidableCacheNativeSupport;
    assert(LSBInspectSlidableCacheProfile(fd, &info, &selected) == 0);
    assert(selected == LSBSlidableCacheOriginal && !memcmp(&info, &empty, sizeof(info)));
    assert(!close(fd));
}

static void fullCacheTest(const char *cachePath, const char *directoryPath, const char *inventoryPath, LSBSlidableCacheProfileKind kind) {
    int cache = open(cachePath, O_RDONLY), directory = open(directoryPath, O_RDONLY | O_DIRECTORY), inventory = open(inventoryPath, O_RDONLY);
    assert(cache >= 0 && directory >= 0 && inventory >= 0);
    LSBSlidableCacheInfo info;
    LSBSlidableCacheProfileKind detected;
    assert(LSBInspectSlidableCacheProfile(cache, &info, &detected) == 1 && detected == kind);
    struct timespec begin, end; clock_gettime(CLOCK_MONOTONIC, &begin);
    LSBSlidableCachePreparation prepared;
    int error = LSBPrepareSlidableCacheWithProfile(cache, directory, testBase, &prepared, kind);
    if (error) fprintf(stderr, "Preparation failed: %d %s\n", error, strerror(error));
    assert(!error); clock_gettime(CLOCK_MONOTONIC, &end);
    double seconds = end.tv_sec - begin.tv_sec + (end.tv_nsec - begin.tv_nsec) / 1e9;
    int data = openat(directory, prepared.fileName, O_RDONLY); assert(data >= 0);
    uint8_t *mapped = mmap(NULL, (size_t)prepared.header.fileSize, PROT_READ, MAP_PRIVATE, data, 0); assert(mapped != MAP_FAILED);
    struct stat input; assert(!fstat(inventory, &input) && input.st_size == (off_t)(prepared.header.fixupCount * 16));
    const uint64_t *fixups = mmap(NULL, (size_t)input.st_size, PROT_READ, MAP_PRIVATE, inventory, 0); assert(fixups != MAP_FAILED);
    unsigned mapping = 0; uint64_t tagged = 0;
    for (uint64_t i = 0; i < prepared.header.fixupCount; ++i) {
        uint64_t offset = fixups[i * 2], original = fixups[i * 2 + 1];
        while (offset >= prepared.header.mappings[mapping].sourceFileOffset + prepared.header.mappings[mapping].size) ++mapping;
        assert(mapping < 3 && offset >= prepared.header.mappings[mapping].sourceFileOffset);
        uint64_t destination = prepared.header.mappings[mapping].backingFileOffset + offset - prepared.header.mappings[mapping].sourceFileOffset;
        assert(get64(mapped + destination) == original + testBase - prepared.header.preferredBase);
        tagged += (original >> 56) != 0;
    }
    assert(tagged == prepared.header.taggedCount);
    munmap((void *)fixups, (size_t)input.st_size); munmap(mapped, (size_t)prepared.header.fileSize);
    close(data); close(inventory); close(directory); close(cache);
    printf("Full cache: %llu pointers and %llu tags match independent Python inventory; preparation %.3fs, reused=%d\n",
           (unsigned long long)prepared.header.fixupCount, (unsigned long long)prepared.header.taggedCount, seconds, prepared.reused);
}
int main(int argc, char **argv) {
    if (argc == 4 || argc == 5) {
        assert(argc == 4 || !strcmp(argv[4], "native-support"));
        fullCacheTest(argv[1], argv[2], argv[3], argc == 5 ? LSBSlidableCacheNativeSupport : LSBSlidableCacheOriginal); return 0;
    }
    assert(argc == 1);
    pageTests(); headerTests(); selectionTests(); fileTests(); nativeSupportProfileTests(); profileDiscoveryTests();
    puts("Slidable cache: exact profile discovery, v2 bounds/tags, native-support isolation, immutable reuse and failure cleanup passed");
}
