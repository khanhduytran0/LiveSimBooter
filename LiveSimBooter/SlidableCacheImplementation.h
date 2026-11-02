// Runtime cache adapter, included inside SimSyscallHelpers.cpp's anonymous namespace.
// A separately prepared, immutable data file holds the exact v2-rebased non-executable segments. Mapping it MAP_PRIVATE shares
// their initial pages between guests; only subsequent guest writes require CoW. Signed TEXT and read-only metadata use the DSC FD.
// Sidecar data is trusted runtime input; the DSC code signature does not authenticate this separate file.
struct SlidableCacheHeader {
    char magic[16];
    uint32_t mappingOffset, mappingCount;
    uint8_t metadataBeforeUUID[64];
    uint8_t uuid[16];
    uint8_t metadataBeforeRegion[120];
    uint64_t sharedRegionStart, sharedRegionSize, maxSlide;
    uint8_t imageTrieMetadata[64];
    uint32_t mappingWithSlideOffset, mappingWithSlideCount;
};
using SlidableCacheMapping = LSBSlidableCacheMapping;
static_assert(offsetof(SlidableCacheHeader, uuid) == 88 && offsetof(SlidableCacheHeader, sharedRegionStart) == 224);
static_assert(offsetof(SlidableCacheHeader, mappingWithSlideOffset) == 312 && sizeof(SlidableCacheHeader) == 320);
static_assert(sizeof(SlidableCacheMapping) == 56);
struct SlidableCacheState {
    SlidableCacheMapping mappings[8];
    LSBSlidableDataHeader backing;
    uint32_t count, seen;
    int cacheFD = -1, dataFD = -1;
    bool enabled, trace, ready, mapped;
    LSBSlidableCacheProfileKind profile = LSBSlidableCacheOriginal;
};
static SlidableCacheState sSlidableCache;

static uint64_t slidableCacheDelta() { return sSlidableCache.backing.actualBase - sSlidableCache.backing.preferredBase; }

[[noreturn]] static void slidableCacheFailure(const char *message) {
    debuggerLog("[slidable-cache] FAILED: %s errno=%d\n", message, *lsb::errnoAddress());
    lsb::exitProcess(126);
}
static bool slidableContains(uint64_t total, uint64_t offset, uint64_t bytes) {
    return offset <= total && bytes <= total - offset;
}
static bool slidableBytesEqual(const void *first, const void *second, size_t size) {
    const auto *a = static_cast<const uint8_t *>(first);
    const auto *b = static_cast<const uint8_t *>(second);
    unsigned different = 0;
    for (size_t index = 0; index < size; ++index) different |= a[index] ^ b[index];
    return different == 0;
}
static int slidableRead(int fd, void *bytes, size_t count, off_t offset) {
    auto *cursor = static_cast<uint8_t *>(bytes);
    while (count) {
        ssize_t result = lsb::pread(fd, cursor, count, offset);
        if (result < 0 && *lsb::errnoAddress() == EINTR) continue;
        if (result <= 0) return result < 0 ? *lsb::errnoAddress() : EIO;
        cursor += result;
        offset += result;
        count -= static_cast<size_t>(result);
    }
    return 0;
}
static int prepareSlidableData(const char *runtimeRoot, uint64_t actualBase, const uint8_t cacheUUID[16], bool trace,
                               LSBSlidableCacheProfileKind profile = LSBSlidableCacheOriginal) {
    if (sSlidableCache.enabled || sSlidableCache.dataFD >= 0) return EALREADY;
    sSlidableCache.trace = trace;
    const auto *layout = LSBSlidableCacheLayoutForProfile(profile);
    if (!layout) return EINVAL;
    sSlidableCache.profile = profile;
    if (!actualBase) return 0;
    if (!runtimeRoot || runtimeRoot[0] != '/' || !cacheUUID || !LSBSlidableCacheUUIDIsNonzero(cacheUUID)) return EINVAL;
    char path[PATH_MAX];
    static constexpr char directory[] = "/System/Library/Caches/com.apple.dyld/";
    char name[LSB_SLIDABLE_DATA_NAME_CAPACITY];
    LSBSlidableDataFileName(cacheUUID, actualBase, name);
    size_t length = lsb::copyString(path, runtimeRoot, sizeof(path));
    if (length > sizeof(path) - sizeof(directory) - sizeof(name)) return ENAMETOOLONG;
    length += lsb::copyString(path + length, directory, sizeof(path) - length);
    lsb::copyString(path + length, name, sizeof(path) - length);
    int fd = lsb::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return *lsb::errnoAddress();
    int error = 0;
    struct stat info = {};
    LSBSlidableDataHeader &header = sSlidableCache.backing;
    if (lsb::fstat(fd, &info)) error = *lsb::errnoAddress();
    else if (!S_ISREG(info.st_mode) || (info.st_mode & 0222) || info.st_size < static_cast<off_t>(sizeof(header))) error = EINVAL;
    else error = slidableRead(fd, &header, sizeof(header), 0);
    if (!error && (!LSBValidateSlidableDataHeaderWithProfile(&header, static_cast<uint64_t>(info.st_size), cacheUUID,
                                                  LSB_SLIDABLE_CACHE_PREFERRED_BASE, layout->span, profile) || header.actualBase != actualBase))
        error = EINVAL;
    if (error) { lsb::close(fd); return error; }
    sSlidableCache.dataFD = fd;
    sSlidableCache.enabled = true;
    return 0;
}
static const LSBSlidableDataMapping *slidableBackingForMapping(unsigned index) {
    for (unsigned i = 0; i < sSlidableCache.backing.mappingCount; ++i)
        if (sSlidableCache.backing.mappings[i].cacheMappingIndex == index) return &sSlidableCache.backing.mappings[i];
    return nullptr;
}
static void prepareSlidableMappings(int fd) {
    SlidableCacheHeader header = {};
    if (slidableRead(fd, &header, sizeof(header), 0)) slidableCacheFailure("cache header read");
    static constexpr char cacheMagic[16] = "dyld_v1   arm64";
    if (!slidableBytesEqual(header.magic, cacheMagic, sizeof(cacheMagic)) ||
        !slidableBytesEqual(header.uuid, sSlidableCache.backing.cacheUUID, sizeof(header.uuid)) || header.sharedRegionStart != LSB_SLIDABLE_CACHE_PREFERRED_BASE ||
        header.sharedRegionSize != sSlidableCache.backing.cacheSpan || header.mappingCount != 6 ||
        header.mappingWithSlideCount != header.mappingCount || header.mappingWithSlideOffset < sizeof(header) ||
        !slidableContains(0x4000, header.mappingWithSlideOffset, header.mappingWithSlideCount * sizeof(SlidableCacheMapping)))
        slidableCacheFailure("cache UUID/layout mismatch");
    sSlidableCache.count = header.mappingWithSlideCount;
    size_t bytes = sSlidableCache.count * sizeof(SlidableCacheMapping);
    if (slidableRead(fd, sSlidableCache.mappings, bytes, header.mappingWithSlideOffset)) slidableCacheFailure("cache mapping table read");
    for (unsigned i = 0; i < sSlidableCache.count; ++i) {
        const SlidableCacheMapping &m = sSlidableCache.mappings[i];
        if (!LSBSlidableCacheMappingMatchesProfileKind(&m, i, sSlidableCache.profile)) slidableCacheFailure("cache mapping profile mismatch");
        if (m.address < LSB_SLIDABLE_CACHE_PREFERRED_BASE || !slidableContains(sSlidableCache.backing.cacheSpan,
            m.address - LSB_SLIDABLE_CACHE_PREFERRED_BASE, m.size) || !m.size || (m.address & 0x3fff) || (m.size & 0x3fff) ||
            (m.fileOffset & 0x3fff)) slidableCacheFailure("cache mapping bounds");
        if (i && m.address < sSlidableCache.mappings[i - 1].address + sSlidableCache.mappings[i - 1].size)
            slidableCacheFailure("cache mapping overlap");
        const LSBSlidableDataMapping *backing = slidableBackingForMapping(i);
        if (!!(m.maxProtection & PROT_WRITE) != !!backing) slidableCacheFailure("sidecar mapping coverage");
        if (backing && ((m.maxProtection & PROT_EXEC) || !m.slideInfoSize || backing->cacheAddress != m.address ||
            backing->size != m.size || backing->sourceFileOffset != m.fileOffset || backing->maxProtection != m.maxProtection ||
            backing->initialProtection != m.initialProtection)) slidableCacheFailure("sidecar/cache mapping disagreement");
    }
    sSlidableCache.cacheFD = fd;
    sSlidableCache.ready = true;
    if (sSlidableCache.trace)
        debuggerLog("[slidable-cache] actual=0x%llx slide=0x%llx shared data ready\n",
                    uint64_t(sSlidableCache.backing.actualBase), uint64_t(slidableCacheDelta()));
}
static void slidableCheckVMRegion(unsigned index) {
    // protection/max_protection/share_mode/is_submap belong to the original full-info prefix.
    // Older kernels can return V0/V1/V2 when a newer SDK requests V3; appended fields are not required here.
    constexpr mach_msg_type_number_t requiredInfoCount = VM_REGION_SUBMAP_INFO_V0_COUNT_64;
    static_assert(requiredInfoCount * sizeof(natural_t) >= offsetof(vm_region_submap_info_data_64_t, is_submap) + sizeof(boolean_t));
    const bool executable = sSlidableCache.mappings[index].initialProtection & PROT_EXEC;
    if (!sSlidableCache.trace && !executable) return;
    const SlidableCacheMapping &mapping = sSlidableCache.mappings[index];
    vm_address_t cursor = mapping.address + slidableCacheDelta();
    const vm_address_t end = cursor + mapping.size;
    natural_t depth = 0;
    for (unsigned attempt = 0; cursor < end && attempt < 64; ++attempt) {
        vm_address_t address = cursor;
        vm_size_t size = 0;
        vm_region_submap_info_data_64_t info = {};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        kern_return_t result = lsb::vm_region_recurse_64(lsb::task_self_trap(), &address, &size, &depth,
                                                       reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
        if (result != KERN_SUCCESS || count < requiredInfoCount) {
            if (sSlidableCache.trace)
                debuggerLog("[slidable-cache] VM region=%u query=0x%llx failed kr=%d count=%u\n", index, uint64_t(cursor), result, count);
            if (executable) slidableCacheFailure("cannot verify executable cache VM protection");
            return;
        }
        if (info.is_submap) { if (++depth > 16) break; continue; }
        if (sSlidableCache.trace)
            debuggerLog("[slidable-cache] VM region=%u actual=0x%llx size=0x%llx current=%d max=%d share=%u depth=%u\n",
                        index, uint64_t(address), uint64_t(size), info.protection, info.max_protection, unsigned(info.share_mode), depth);
        if (address > cursor || !size || size > UINT64_MAX - address || address + size <= cursor) break;
        if (executable && ((info.protection & (VM_PROT_READ | VM_PROT_EXECUTE)) != (VM_PROT_READ | VM_PROT_EXECUTE) ||
            !(info.max_protection & VM_PROT_EXECUTE))) slidableCacheFailure("cache TEXT was not granted executable protection");
        cursor = address + size;
    }
    if (executable && cursor < end) slidableCacheFailure("executable cache VM query did not cover the whole mapping");
}
static void *slidableCacheMmap(void *address, size_t size, int protection, int flags, int fd, off_t offset) {
    if (!sSlidableCache.enabled || sSlidableCache.mapped) return lsb::mmap(address, size, protection, flags, fd, offset);
    if ((flags & MAP_FIXED) && fd >= 0 && offset == 0) {
        SlidableCacheHeader candidate = {};
        if (slidableRead(fd, &candidate, sizeof(candidate), 0)) slidableCacheFailure("cannot identify fixed-file mapping before cache");
        if (slidableBytesEqual(candidate.uuid, sSlidableCache.backing.cacheUUID, sizeof(candidate.uuid)) &&
            reinterpret_cast<uintptr_t>(address) != sSlidableCache.backing.actualBase)
            slidableCacheFailure("refusing matching cache MAP_FIXED at an unexpected base");
    }
    if (!sSlidableCache.ready && reinterpret_cast<uintptr_t>(address) == sSlidableCache.backing.actualBase &&
        fd >= 0 && offset == 0 && (flags & MAP_FIXED)) prepareSlidableMappings(fd);
    int mappingIndex = -1, mappingFD = fd;
    off_t mappingOffset = offset;
    if (sSlidableCache.ready && fd == sSlidableCache.cacheFD && (flags & MAP_FIXED)) {
        for (unsigned i = 0; i < sSlidableCache.count; ++i) {
            const SlidableCacheMapping &m = sSlidableCache.mappings[i];
            if (m.address + slidableCacheDelta() == reinterpret_cast<uintptr_t>(address) && m.size == size &&
                m.fileOffset == uint64_t(offset)) mappingIndex = i;
        }
        if (mappingIndex < 0 || (sSlidableCache.seen & (1u << mappingIndex))) slidableCacheFailure("unexpected cache mapping request");
        if (const LSBSlidableDataMapping *backing = slidableBackingForMapping(static_cast<unsigned>(mappingIndex))) {
            if ((protection & PROT_EXEC) || !(flags & MAP_PRIVATE) || (flags & MAP_SHARED)) slidableCacheFailure("unsafe sidecar mapping flags");
            mappingFD = sSlidableCache.dataFD;
            mappingOffset = static_cast<off_t>(backing->backingFileOffset);
        }
    }
    void *result = lsb::mmap(address, size, protection, flags, mappingFD, mappingOffset);
    if (mappingIndex >= 0) {
        if (result == MAP_FAILED || result != address) slidableCacheFailure("cache mmap");
        slidableCheckVMRegion(static_cast<unsigned>(mappingIndex));
        sSlidableCache.seen |= 1u << mappingIndex;
        if (sSlidableCache.seen == (1u << sSlidableCache.count) - 1) {
            if (sSlidableCache.profile == LSBSlidableCacheNativeSupport) {
                const LSBNativeSupportMemory memory = {lsb::mmap, lsb::mprotect};
                if (!LSBNativeSupportBindingsPrepared() || LSBApplyNativeSupportBindings(&memory))
                    slidableCacheFailure("native support binding application");
            }
            sSlidableCache.mapped = true;
            lsb::close(sSlidableCache.dataFD);
            sSlidableCache.dataFD = -1;
            if (sSlidableCache.trace) debuggerLog("[slidable-cache] shared data mapped: %llu pre-rebased pointers; no per-process rebase writes\n",
                                                 uint64_t(sSlidableCache.backing.fixupCount));
        }
    }
    return result;
}
