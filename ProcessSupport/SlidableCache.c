#include "SlidableCache.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include "PrivateMachVM.h"
#endif

typedef struct {
    char magic[16];
    uint32_t mappingOffset, mappingCount;
    uint8_t metadataBeforeSignature[16];
    uint64_t signatureOffset, signatureSize;
    uint8_t metadataBeforeUUID[32], uuid[16], metadataBeforePlatform[112];
    uint32_t platform, formatFlags;
    uint64_t preferredBase, span, maxSlide;
    uint8_t trieMetadata[64];
    uint32_t mappingWithSlideOffset, mappingWithSlideCount;
    uint8_t metadataBeforeSubCaches[72];
    uint32_t subCacheArrayOffset, subCacheArrayCount;
    uint8_t metadataBeforeImages[48];
    uint32_t imagesOffset, imagesCount;
} CacheHeader;
typedef struct {
    uint32_t version, pageSize, startsOffset, startsCount, extrasOffset, extrasCount;
    uint64_t deltaMask, valueAdd;
} SlideInfoV2;
typedef struct { uint64_t fixups, tagged; } SlideCounts;
_Static_assert(sizeof(CacheHeader) == 456 && offsetof(CacheHeader, uuid) == 88 && offsetof(CacheHeader, preferredBase) == 224,
               "dyld cache header prefix");
_Static_assert(offsetof(CacheHeader, mappingWithSlideOffset) == 312 && offsetof(CacheHeader, imagesOffset) == 448, "dyld cache header tables");
_Static_assert(sizeof(SlideInfoV2) == 40 && sizeof(LSBSlidableCacheMapping) == 56, "dyld v2 slide ABI");

static int contains(uint64_t total, uint64_t offset, uint64_t size) { return offset <= total && size <= total - offset; }
static int readAt(int fd, void *bytes, size_t size, uint64_t offset) {
    if (offset > INT64_MAX || size > (uint64_t)INT64_MAX - offset) return EOVERFLOW;
    while (size) {
        ssize_t count = pread(fd, bytes, size, (off_t)offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count ? errno : EIO;
        bytes = (uint8_t *)bytes + count; size -= (size_t)count; offset += (uint64_t)count;
    }
    return 0;
}
static int writeAt(int fd, const void *bytes, size_t size, uint64_t offset) {
    if (offset > INT64_MAX || size > (uint64_t)INT64_MAX - offset) return EOVERFLOW;
    while (size) {
        ssize_t count = pwrite(fd, bytes, size, (off_t)offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count ? errno : EIO;
        bytes = (const uint8_t *)bytes + count; size -= (size_t)count; offset += (uint64_t)count;
    }
    return 0;
}
static int validateSlideInfo(const SlideInfoV2 *slide, const LSBSlidableCacheMapping *mapping) {
    // Apple's pinned dyld fd8d0c4d arm64 v2 format: bits40..55 hold a four-byte-unit link; the top byte remains a pointer tag.
    if (slide->version != 2 || slide->pageSize != 4096 || slide->deltaMask != UINT64_C(0x00ffff0000000000) ||
        slide->valueAdd || slide->extrasOffset || slide->extrasCount) return ENOTSUP;
    if (slide->startsOffset < sizeof(*slide) || (slide->startsOffset & 1) ||
        (uint64_t)slide->startsCount * slide->pageSize != mapping->size ||
        !contains(mapping->slideInfoSize, slide->startsOffset, (uint64_t)slide->startsCount * 2)) return EINVAL;
    return 0;
}

int LSBInspectSlidableCacheWithProfile(int cacheFD, LSBSlidableCacheInfo *info, LSBSlidableCacheProfileKind kind) {
    const LSBSlidableCacheLayout *layout = LSBSlidableCacheLayoutForProfile(kind);
    if (!info || !layout) { errno = EINVAL; return -1; }
    memset(info, 0, sizeof(*info));
    CacheHeader header;
    struct stat status;
    int error = 0;
    if (fstat(cacheFD, &status)) return -1;
    if (!S_ISREG(status.st_mode) || status.st_size < (off_t)sizeof(header)) { errno = EINVAL; return -1; }
    if ((error = readAt(cacheFD, &header, sizeof(header), 0))) { errno = error; return -1; }
    if (memcmp(header.magic, "dyld_v1   arm64", 16)) return 0;
    if (!header.mappingWithSlideCount) return 0;
    if (header.mappingWithSlideCount > 8 || header.mappingWithSlideOffset < sizeof(header) ||
        !contains((uint64_t)status.st_size, header.mappingWithSlideOffset, (uint64_t)header.mappingWithSlideCount * 56)) {
        errno = EINVAL; return -1;
    }
    if ((error = readAt(cacheFD, info->mappings, header.mappingWithSlideCount * 56, header.mappingWithSlideOffset))) {
        errno = error; return -1;
    }
    int hasSlide = 0;
    for (unsigned i = 0; i < header.mappingWithSlideCount; ++i) hasSlide |= info->mappings[i].slideInfoSize != 0;
    if (!hasSlide) { memset(info, 0, sizeof(*info)); return 0; }
    if (header.preferredBase != LSB_SLIDABLE_CACHE_PREFERRED_BASE || header.span != layout->span ||
        header.platform != 7 || !(header.formatFlags & 0x200) || header.subCacheArrayCount || header.maxSlide != 0x20000000 ||
        header.mappingCount != 6 || header.mappingWithSlideCount != 6 || header.imagesCount != 1353 || !LSBSlidableCacheUUIDIsNonzero(header.uuid)) {
        errno = ENOTSUP; return -1;
    }
    if (header.mappingOffset < sizeof(header) || !contains((uint64_t)status.st_size, header.mappingOffset, 6 * 32) ||
        header.imagesOffset < sizeof(header) || !contains((uint64_t)status.st_size, header.imagesOffset, (uint64_t)header.imagesCount * 32) ||
        !header.signatureSize || (header.signatureOffset & 0x3fff) ||
        header.signatureOffset < layout->mappings[5].fileOffset + layout->mappings[5].size ||
        !contains((uint64_t)status.st_size, header.signatureOffset, header.signatureSize) ||
        header.signatureOffset + header.signatureSize != (uint64_t)status.st_size) { errno = EINVAL; return -1; }
    for (unsigned i = 0; i < 6; ++i) {
        const LSBSlidableCacheMapping *mapping = &info->mappings[i];
        if (!LSBSlidableCacheMappingMatchesProfileKind(mapping, i, kind)) { errno = ENOTSUP; return -1; }
        if (!contains(header.signatureOffset, mapping->fileOffset, mapping->size)) { errno = EINVAL; return -1; }
        struct { uint64_t address, size, fileOffset; uint32_t maxProtection, initialProtection; } legacy;
        if ((error = readAt(cacheFD, &legacy, sizeof(legacy), header.mappingOffset + i * 32))) { errno = error; return -1; }
        if (legacy.address != mapping->address || legacy.size != mapping->size || legacy.fileOffset != mapping->fileOffset ||
            legacy.maxProtection != mapping->maxProtection || legacy.initialProtection != mapping->initialProtection) { errno = EINVAL; return -1; }
        if (!mapping->slideInfoSize) continue;
        SlideInfoV2 slide;
        if (!contains(header.signatureOffset, mapping->slideInfoOffset, mapping->slideInfoSize)) { errno = EINVAL; return -1; }
        if ((error = readAt(cacheFD, &slide, sizeof(slide), mapping->slideInfoOffset)) || (error = validateSlideInfo(&slide, mapping))) {
            errno = error; return -1;
        }
    }
    // Full-cache libobjc's exported debug scalar, independently resolved by the setup tool's export-trie validator.
    uint64_t isaMask;
    if ((error = readAt(cacheFD, &isaMask, sizeof(isaMask), 0x61bb8))) { errno = error; return -1; }
    if (isaMask != UINT64_C(0x007ffffffffffff8)) { errno = ENOTSUP; return -1; }
    memcpy(info->uuid, header.uuid, 16);
    info->preferredBase = header.preferredBase; info->span = header.span; info->fileSize = (uint64_t)status.st_size; info->mappingCount = 6;
    return 1;
}

static int validBase(uint64_t actualBase, uint64_t span) {
    return actualBase >= LSB_SLIDABLE_CACHE_PREFERRED_BASE && !(actualBase & 0x3fff) && span &&
        span <= LSB_SLIDABLE_CACHE_RESERVATION_SIZE && actualBase <= UINT64_C(0x0080000000000000) - span;
}
int LSBSelectSlidableCacheBaseWithProfile(const LSBSlidableCacheInfo *info, uint64_t *actualBase, LSBSlidableCacheProfileKind kind) {
    const LSBSlidableCacheLayout *layout = LSBSlidableCacheLayoutForProfile(kind);
    if (!layout || !info || !actualBase || info->preferredBase != LSB_SLIDABLE_CACHE_PREFERRED_BASE || info->span != layout->span) return EINVAL;
#if defined(__APPLE__)
    // For native support, use the start of the executable window as the guest slide. This keeps the main image
    // below the DSC, in the same order as their preferred Mach-O addresses. Probe through the last cache page.
    uint64_t cacheOffset = kind == LSBSlidableCacheNativeSupport ? info->preferredBase : 0;
    mach_vm_size_t probeSize = cacheOffset ? cacheOffset + info->span : LSB_SLIDABLE_CACHE_RESERVATION_SIZE;
    mach_vm_address_t address = info->preferredBase;
    kern_return_t result = mach_vm_map(mach_task_self(), &address, probeSize, 0, VM_FLAGS_ANYWHERE,
                                      MACH_PORT_NULL, 0, FALSE, VM_PROT_READ | VM_PROT_EXECUTE, VM_PROT_READ | VM_PROT_EXECUTE, VM_INHERIT_NONE);
    if (result != KERN_SUCCESS) return result == KERN_NO_SPACE ? ENOMEM : result == KERN_PROTECTION_FAILURE ? EACCES : EIO;
    // No loads, stores or instruction fetches occur in this anonymous reservation.
    kern_return_t release = mach_vm_deallocate(mach_task_self(), address, probeSize);
    if (release != KERN_SUCCESS) return EIO;
    if (address < info->preferredBase || address > UINT64_MAX - cacheOffset || !validBase(address + cacheOffset, info->span)) return ERANGE;
    *actualBase = address + cacheOffset;
    return 0;
#else
    return ENOTSUP;
#endif
}

static int rebasePage(uint8_t page[4096], uint16_t start, uint64_t preferredBase, uint64_t span, uint64_t actualBase, SlideCounts *counts) {
    if (span > UINT64_C(0x0080000000000000) || actualBase > UINT64_C(0x0080000000000000) - span) return EOVERFLOW;
    if (start == 0x4000) return 0; // DYLD_CACHE_SLIDE_PAGE_ATTR_NO_REBASE, not 0xffff.
    if (start & 0xc000) return ENOTSUP; // This pinned cache has no extras/multiple-chain pages.
    uint32_t offset = (uint32_t)start * 4;
    for (;;) {
        if (offset > 4096 - 8) return EINVAL; // The reviewed cache has no cross-page pointers.
        uint64_t encoded;
        memcpy(&encoded, page + offset, sizeof(encoded));
        uint64_t delta = (encoded & UINT64_C(0x00ffff0000000000)) >> 38;
        uint64_t value = encoded & UINT64_C(0xff0000ffffffffff);
        if (value) {
            uint64_t address = value & UINT64_C(0x00ffffffffffffff);
            if (address < preferredBase || address - preferredBase >= span) return EINVAL;
            uint64_t relocated = actualBase + address - preferredBase;
            if (relocated >= UINT64_C(0x0080000000000000)) return EOVERFLOW;
            counts->tagged += (value >> 56) != 0;
            value = (value & UINT64_C(0xff00000000000000)) | relocated;
        }
        memcpy(page + offset, &value, sizeof(value));
        ++counts->fixups;
        if (!delta) return 0;
        if (delta < 8 || delta > 4096 - 8 - offset) return EINVAL;
        offset += (uint32_t)delta;
    }
}

static void fillHeader(const LSBSlidableCacheInfo *info, uint64_t actualBase, LSBSlidableDataHeader *header, LSBSlidableCacheProfileKind kind) {
    const LSBSlidableCacheLayout *layout = LSBSlidableCacheLayoutForProfile(kind);
    memset(header, 0, sizeof(*header));
    memcpy(header->magic, "LSBSlideDataV1", 14);
    header->version = 1; header->headerSize = sizeof(*header); memcpy(header->cacheUUID, info->uuid, 16);
    header->preferredBase = info->preferredBase; header->actualBase = actualBase; header->cacheSpan = info->span;
    header->fileSize = 0x4000; header->mappingEntrySize = sizeof(LSBSlidableDataMapping);
    for (unsigned i = 0; i < info->mappingCount; ++i) {
        const LSBSlidableCacheMapping *mapping = &info->mappings[i];
        if (!(mapping->maxProtection & 2)) continue;
        LSBSlidableDataMapping *backing = &header->mappings[header->mappingCount++];
        *backing = (LSBSlidableDataMapping){i, 0, mapping->address, mapping->size, mapping->fileOffset, header->fileSize,
                                          mapping->maxProtection, mapping->initialProtection};
        header->fileSize += mapping->size;
    }
    header->fixupCount = layout->fixups; header->taggedCount = layout->tagged;
}
static int reuseSidecar(int directoryFD, const char *fileName, const LSBSlidableCacheInfo *info, uint64_t actualBase,
                        LSBSlidableDataHeader *header, LSBSlidableCacheProfileKind kind) {
    int fd = openat(directoryFD, fileName, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno;
    struct stat status;
    int error = 0;
    if (fstat(fd, &status)) error = errno;
    else if (!S_ISREG(status.st_mode) || (status.st_mode & 0222) || status.st_size < (off_t)sizeof(*header)) error = EINVAL;
    else error = readAt(fd, header, sizeof(*header), 0);
    if (!error && (!LSBValidateSlidableDataHeaderWithProfile(header, (uint64_t)status.st_size, info->uuid, info->preferredBase, info->span, kind) ||
                   header->actualBase != actualBase)) error = EINVAL;
    close(fd);
    return error;
}
static int generateData(int cacheFD, int outputFD, const LSBSlidableCacheInfo *info, const LSBSlidableDataHeader *header) {
    uint8_t *buffer = malloc(65536);
    if (!buffer) return ENOMEM;
    int error = 0;
    SlideCounts counts = {0};
    for (unsigned index = 0; !error && index < header->mappingCount; ++index) {
        const LSBSlidableDataMapping *backing = &header->mappings[index];
        const LSBSlidableCacheMapping *mapping = &info->mappings[backing->cacheMappingIndex];
        SlideInfoV2 slide;
        error = readAt(cacheFD, &slide, sizeof(slide), mapping->slideInfoOffset);
        if (error || (error = validateSlideInfo(&slide, mapping))) break;
        uint16_t *starts = malloc((size_t)slide.startsCount * sizeof(*starts));
        if (!starts) { error = ENOMEM; break; }
        error = readAt(cacheFD, starts, (size_t)slide.startsCount * sizeof(*starts), mapping->slideInfoOffset + slide.startsOffset);
        for (uint64_t offset = 0; !error && offset < mapping->size; offset += 65536) {
            size_t size = mapping->size - offset < 65536 ? (size_t)(mapping->size - offset) : 65536;
            error = readAt(cacheFD, buffer, size, mapping->fileOffset + offset);
            for (size_t page = 0; !error && page < size; page += 4096)
                error = rebasePage(buffer + page, starts[(offset + page) / 4096], info->preferredBase, info->span, header->actualBase, &counts);
            if (!error) error = writeAt(outputFD, buffer, size, backing->backingFileOffset + offset);
        }
        free(starts);
    }
    free(buffer);
    if (!error && (counts.fixups != header->fixupCount || counts.tagged != header->taggedCount)) error = EINVAL;
    return error;
}

int LSBPrepareSlidableCacheWithProfile(int cacheFD, int directoryFD, uint64_t actualBase, LSBSlidableCachePreparation *result,
                                      LSBSlidableCacheProfileKind kind) {
    if (!result) return EINVAL;
    memset(result, 0, sizeof(*result));
    struct stat before, directory;
    if (fstat(cacheFD, &before) || fstat(directoryFD, &directory)) return errno;
    if (!S_ISDIR(directory.st_mode)) return ENOTDIR;
    LSBSlidableCacheInfo info;
    int supported = LSBInspectSlidableCacheWithProfile(cacheFD, &info, kind);
    if (supported <= 0) return supported < 0 ? errno : ENOTSUP;
    if (!validBase(actualBase, info.span)) return EINVAL;
    LSBSlidableDataFileName(info.uuid, actualBase, result->fileName);
    int error = reuseSidecar(directoryFD, result->fileName, &info, actualBase, &result->header, kind);
    if (!error) { result->reused = 1; return 0; }
    if (error != ENOENT) return error;
    fillHeader(&info, actualBase, &result->header, kind);
    char temporary[128];
    int outputFD = -1;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        snprintf(temporary, sizeof(temporary), ".%s.%08x.tmp", result->fileName, arc4random());
        outputFD = openat(directoryFD, temporary, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (outputFD >= 0) break;
        if (errno != EEXIST) return errno;
    }
    if (outputFD < 0) return EEXIST;
    if (ftruncate(outputFD, (off_t)result->header.fileSize)) error = errno;
    else error = generateData(cacheFD, outputFD, &info, &result->header);
    struct stat after;
    if (!error && fstat(cacheFD, &after)) error = errno;
    if (!error && (before.st_dev != after.st_dev || before.st_ino != after.st_ino || before.st_size != after.st_size ||
                   before.st_mtime != after.st_mtime || before.st_ctime != after.st_ctime)) error = ESTALE;
#if defined(__APPLE__)
    if (!error && (before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec || before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec)) error = ESTALE;
#endif
    if (!error) error = writeAt(outputFD, &result->header, sizeof(result->header), 0);
    if (!error && fchmod(outputFD, 0444)) error = errno;
    if (!error && fsync(outputFD)) error = errno;
    if (close(outputFD) && !error) error = errno;
    if (!error) {
#if defined(__APPLE__)
        if (renameatx_np(directoryFD, temporary, directoryFD, result->fileName, RENAME_EXCL)) error = errno;
#else
        if (linkat(directoryFD, temporary, directoryFD, result->fileName, 0)) error = errno;
        else unlinkat(directoryFD, temporary, 0);
#endif
        if (error == EEXIST) {
            error = reuseSidecar(directoryFD, result->fileName, &info, actualBase, &result->header, kind);
            if (!error) result->reused = 1;
        }
    }
    // On error only our O_EXCL temporary is removed; an existing immutable sidecar is never overwritten or unlinked.
    unlinkat(directoryFD, temporary, 0);
    return error;
}

int LSBInspectSlidableCache(int cacheFD, LSBSlidableCacheInfo *info) {
    return LSBInspectSlidableCacheWithProfile(cacheFD, info, LSBSlidableCacheOriginal);
}
int LSBInspectSlidableCacheProfile(int cacheFD, LSBSlidableCacheInfo *info, LSBSlidableCacheProfileKind *kind) {
    if (kind) *kind = LSBSlidableCacheOriginal;
    if (info) memset(info, 0, sizeof(*info));
    if (!info || !kind) { errno = EINVAL; return -1; }
    int supported = LSBInspectSlidableCacheWithProfile(cacheFD, info, LSBSlidableCacheOriginal);
    if (supported < 0 && errno == ENOTSUP) {
        // Retry only an unsupported profile, never a malformed/unreadable input. The second validator checks the entire native layout.
        supported = LSBInspectSlidableCacheWithProfile(cacheFD, info, LSBSlidableCacheNativeSupport);
        if (supported > 0) *kind = LSBSlidableCacheNativeSupport;
        else if (!supported) { errno = ENOTSUP; supported = -1; } // A changing file must not fall back to the ordinary-cache path.
    }
    if (supported <= 0) memset(info, 0, sizeof(*info));
    return supported;
}
int LSBSelectSlidableCacheBase(const LSBSlidableCacheInfo *info, uint64_t *actualBase) {
    return LSBSelectSlidableCacheBaseWithProfile(info, actualBase, LSBSlidableCacheOriginal);
}
int LSBPrepareSlidableCache(int cacheFD, int directoryFD, uint64_t actualBase, LSBSlidableCachePreparation *result) {
    return LSBPrepareSlidableCacheWithProfile(cacheFD, directoryFD, actualBase, result, LSBSlidableCacheOriginal);
}
