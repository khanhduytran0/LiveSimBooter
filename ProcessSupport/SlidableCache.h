#pragma once
#include <stddef.h>
#include <stdint.h>

#define LSB_SLIDABLE_CACHE_PREFERRED_BASE UINT64_C(0x180000000)
#define LSB_SLIDABLE_CACHE_SPAN UINT64_C(0x7e1b0000)
#define LSB_SLIDABLE_CACHE_FIXUPS UINT64_C(15156132)
#define LSB_SLIDABLE_CACHE_TAGGED UINT64_C(40252)
#define LSB_SLIDABLE_CACHE_RESERVATION_SIZE UINT64_C(0xc0000000)
#define LSB_SLIDABLE_DATA_NAME_CAPACITY 96

static inline int LSBSlidableCacheUUIDIsNonzero(const uint8_t uuid[16]) {
    unsigned bits = 0;
    for (unsigned i = 0; i < 16; ++i) bits |= uuid[i];
    return bits != 0;
}
static inline void LSBSlidableDataFileName(const uint8_t uuid[16], uint64_t actualBase, char output[LSB_SLIDABLE_DATA_NAME_CAPACITY]) {
    static const char prefix[] = "lsb-slide-", suffix[] = ".data", hex[] = "0123456789abcdef";
    unsigned position = 0;
    for (unsigned i = 0; i < sizeof(prefix) - 1; ++i) output[position++] = prefix[i];
    for (unsigned i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) output[position++] = '-';
        output[position++] = hex[uuid[i] >> 4];
        output[position++] = hex[uuid[i] & 15];
    }
    output[position++] = '-';
    unsigned shift = 60;
    while (shift && !((actualBase >> shift) & 15)) shift -= 4;
    for (;;) {
        output[position++] = hex[(actualBase >> shift) & 15];
        if (!shift) break;
        shift -= 4;
    }
    for (unsigned i = 0; i < sizeof(suffix); ++i) output[position++] = suffix[i];
}

// Little-endian disk ABI. The sidecar is trusted, unsigned DATA input; UUID/layout checks do not authenticate its contents.
typedef struct {
    uint32_t cacheMappingIndex, reserved;
    uint64_t cacheAddress, size, sourceFileOffset, backingFileOffset;
    uint32_t maxProtection, initialProtection;
} LSBSlidableDataMapping;
typedef struct {
    char magic[16];
    uint32_t version, headerSize;
    uint8_t cacheUUID[16];
    uint64_t preferredBase, actualBase, cacheSpan, fileSize, fixupCount, taggedCount;
    uint32_t mappingCount, mappingEntrySize;
    LSBSlidableDataMapping mappings[8];
} LSBSlidableDataHeader;
typedef struct {
    uint64_t address, size, fileOffset, slideInfoOffset, slideInfoSize, flags;
    uint32_t maxProtection, initialProtection;
} LSBSlidableCacheMapping;
// Reviewed full 23A343 layout. Different host support inputs are allowed only if they preserve this exact layout and slide profile.
static const LSBSlidableCacheMapping LSBSlidableCacheProfile[6] = {
    {0x180000000, 0x5c594000, 0,          0,          0,     0,    5, 5},
    {0x1dc594000, 0x09a84000, 0x5c594000, 0x6e504db0, 79152, 4,    3, 1},
    {0x1e8018000, 0x00004000, 0x66018000, 0x6e4fc000, 48,    0x44, 3, 1},
    {0x1e801c000, 0x046ac000, 0x6601c000, 0x6e4fc030, 36224, 0,    3, 3},
    {0x1ee6c8000, 0x03e34000, 0x6a6c8000, 0,          0,     0x20, 1, 1},
    {0x1f24fc000, 0x0bcb0000, 0x6e4fc000, 0,          0,     0,    1, 1},
};
static inline int LSBSlidableCacheMappingMatchesProfile(const LSBSlidableCacheMapping *mapping, unsigned index) {
    if (index >= 6) return 0;
    const LSBSlidableCacheMapping *expected = &LSBSlidableCacheProfile[index];
    return mapping->address == expected->address && mapping->size == expected->size && mapping->fileOffset == expected->fileOffset &&
        mapping->slideInfoOffset == expected->slideInfoOffset && mapping->slideInfoSize == expected->slideInfoSize && mapping->flags == expected->flags &&
        mapping->maxProtection == expected->maxProtection && mapping->initialProtection == expected->initialProtection;
}
typedef struct {
    uint8_t uuid[16];
    uint64_t preferredBase, span, fileSize;
    uint32_t mappingCount;
    LSBSlidableCacheMapping mappings[8];
} LSBSlidableCacheInfo;
typedef struct {
    LSBSlidableDataHeader header;
    char fileName[LSB_SLIDABLE_DATA_NAME_CAPACITY];
    int reused;
} LSBSlidableCachePreparation;

#if defined(__cplusplus)
static_assert(sizeof(LSBSlidableDataMapping) == 48 && sizeof(LSBSlidableDataHeader) == 480, "slidable data disk ABI");
static_assert(offsetof(LSBSlidableDataHeader, cacheUUID) == 24 && offsetof(LSBSlidableDataHeader, mappings) == 96, "slidable data offsets");
extern "C" {
#else
_Static_assert(sizeof(LSBSlidableDataMapping) == 48 && sizeof(LSBSlidableDataHeader) == 480, "slidable data disk ABI");
_Static_assert(offsetof(LSBSlidableDataHeader, cacheUUID) == 24 && offsetof(LSBSlidableDataHeader, mappings) == 96, "slidable data offsets");
#endif

// Pure validation, safe before guest TLS: no allocation, errno access or library calls. Returns 1 for a supported header.
static inline int LSBValidateSlidableDataHeader(const LSBSlidableDataHeader *header, uint64_t fileSize,
                                               const uint8_t cacheUUID[16], uint64_t preferredBase, uint64_t cacheSpan) {
    static const char magic[16] = "LSBSlideDataV1";
    if (!header || !cacheUUID) return 0;
    for (unsigned i = 0; i < 16; ++i) if (header->magic[i] != magic[i] || header->cacheUUID[i] != cacheUUID[i]) return 0;
    if (!LSBSlidableCacheUUIDIsNonzero(cacheUUID) || preferredBase != LSB_SLIDABLE_CACHE_PREFERRED_BASE || cacheSpan != LSB_SLIDABLE_CACHE_SPAN ||
        header->version != 1 || header->headerSize != sizeof(*header) || header->preferredBase != preferredBase || header->cacheSpan != cacheSpan ||
        header->actualBase < preferredBase || (header->actualBase & 0x3fff) || header->actualBase > UINT64_C(0x0080000000000000) - cacheSpan ||
        header->fileSize != fileSize || header->fixupCount != LSB_SLIDABLE_CACHE_FIXUPS || header->taggedCount != LSB_SLIDABLE_CACHE_TAGGED ||
        header->mappingCount != 3 || header->mappingEntrySize != sizeof(LSBSlidableDataMapping)) return 0;
    uint64_t nextOffset = 0x4000, previousEnd = preferredBase;
    for (unsigned i = 0; i < header->mappingCount; ++i) {
        const LSBSlidableDataMapping *mapping = &header->mappings[i];
        const LSBSlidableCacheMapping *expected = &LSBSlidableCacheProfile[i + 1];
        if (mapping->cacheMappingIndex != i + 1 || mapping->reserved || mapping->maxProtection != 3 ||
            mapping->cacheAddress != expected->address || mapping->size != expected->size || mapping->sourceFileOffset != expected->fileOffset ||
            mapping->initialProtection != expected->initialProtection ||
            (mapping->initialProtection != 1 && mapping->initialProtection != 3) || !mapping->size ||
            ((mapping->cacheAddress | mapping->size | mapping->sourceFileOffset) & 0x3fff) || mapping->cacheAddress < previousEnd ||
            mapping->cacheAddress < preferredBase || mapping->cacheAddress - preferredBase > cacheSpan ||
            mapping->size > cacheSpan - (mapping->cacheAddress - preferredBase) || mapping->backingFileOffset != nextOffset ||
            nextOffset > fileSize || mapping->size > fileSize - nextOffset) return 0;
        previousEnd = mapping->cacheAddress + mapping->size;
        nextOffset += mapping->size;
    }
    if (nextOffset != fileSize) return 0;
    for (unsigned i = header->mappingCount; i < 8; ++i) {
        const LSBSlidableDataMapping *mapping = &header->mappings[i];
        if (mapping->cacheMappingIndex || mapping->reserved || mapping->cacheAddress || mapping->size || mapping->sourceFileOffset ||
            mapping->backingFileOffset || mapping->maxProtection || mapping->initialProtection) return 0;
    }
    return 1;
}

// Native host only. Inspection returns 1 for the pinned slidable cache, 0 for ordinary caches, or -1 with errno on failure.
int LSBInspectSlidableCache(int cacheFD, LSBSlidableCacheInfo *info);
// Probe a 3 GiB RX reservation with a preferred-base hint, without faulting any pages; releases it before returning. Returns errno or zero.
int LSBSelectSlidableCacheBase(const LSBSlidableCacheInfo *info, uint64_t *actualBase);
// Caller serializes session selection. Publishes/reuses an immutable UUID+base file in directoryFD; never modifies the cache. Returns errno or zero.
int LSBPrepareSlidableCache(int cacheFD, int directoryFD, uint64_t actualBase, LSBSlidableCachePreparation *result);

#if defined(__cplusplus)
}
#endif
