#pragma once

#include "SlidableCache.h"
#include <errno.h>

typedef enum { LSBGuestMainImage, LSBForkBootstrapImage } LSBGuestImageSlot;

#define LSB_FORK_BOOTSTRAP_EXECUTABLE "/usr/local/bin/lsb-fork-snapshot-child"

// Offsets from the start of the broker's executable window. Main and cache share the usual Mach-O slide.
#define LSB_GUEST_MAIN_IMAGE_OFFSET UINT64_C(0x100000000)
#define LSB_FORK_BOOTSTRAP_IMAGE_OFFSET UINT64_C(0x13c000000)
#define LSB_GUEST_MAIN_IMAGE_CAPACITY (LSB_FORK_BOOTSTRAP_IMAGE_OFFSET - LSB_GUEST_MAIN_IMAGE_OFFSET)
#define LSB_FORK_BOOTSTRAP_IMAGE_CAPACITY UINT64_C(0x04000000)
#if LSB_FORK_BOOTSTRAP_IMAGE_OFFSET + LSB_FORK_BOOTSTRAP_IMAGE_CAPACITY > LSB_SLIDABLE_CACHE_PREFERRED_BASE
#error The fork bootstrap image must fit below the cache
#endif

// taskLimit is exclusive. The caller must still reserve this exact range without replacing existing mappings.
static inline int LSBGuestImageAddress(uint64_t cacheBase, uint64_t cacheSize, uint64_t imagePreferred, uint64_t imageSize,
                                      uint64_t pageSize, uint64_t taskLimit, LSBGuestImageSlot slot, uint64_t *address) {
    if (!address) return EINVAL;
    *address = 0;
    if (slot != LSBGuestMainImage && slot != LSBForkBootstrapImage) return EINVAL;
    uint64_t offset = slot == LSBGuestMainImage ? LSB_GUEST_MAIN_IMAGE_OFFSET : LSB_FORK_BOOTSTRAP_IMAGE_OFFSET;
    uint64_t capacity = slot == LSBGuestMainImage ? LSB_GUEST_MAIN_IMAGE_CAPACITY : LSB_FORK_BOOTSTRAP_IMAGE_CAPACITY;
    if (!pageSize || (pageSize & (pageSize - 1)) || !cacheSize || !imageSize ||
        ((cacheBase | cacheSize | imagePreferred | imageSize | LSB_SLIDABLE_CACHE_PREFERRED_BASE | offset | capacity) & (pageSize - 1))) return EINVAL;
    if (imageSize > capacity || cacheSize > LSB_SLIDABLE_CACHE_RESERVATION_SIZE) return ENOSPC;
    if (cacheBase < LSB_SLIDABLE_CACHE_PREFERRED_BASE || cacheBase > taskLimit || cacheSize > taskLimit - cacheBase) return ERANGE;
    uint64_t window = cacheBase - LSB_SLIDABLE_CACHE_PREFERRED_BASE;
    if (window < LSB_SLIDABLE_CACHE_PREFERRED_BASE) return ERANGE; // Same lower bound as the broker's RX probe.
    uint64_t start = window + offset; // Both complete slots fit below cacheBase.
    if (start < imagePreferred) return ERANGE;
    *address = start;
    return 0;
}
