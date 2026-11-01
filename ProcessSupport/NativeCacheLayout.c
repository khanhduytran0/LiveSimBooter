#include "NativeCacheLayout.h"
#include "SlidableCache.h"
#include "PrivateSharedRegion.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Same stable cache-header prefix validated by DyldSimLoader.cpp; no runtime cache is mapped by this preflight.
typedef struct {
    char magic[16];
    uint32_t mappingOffset, mappingCount;
    uint8_t metadataBeforeSharedRegion[200];
    uint64_t sharedRegionStart, sharedRegionSize;
} NativeCacheReservationHeader;
_Static_assert(sizeof(NativeCacheReservationHeader) == 240 && offsetof(NativeCacheReservationHeader, sharedRegionStart) == 224,
               "dyld cache reservation header layout");

static LSBNativeCacheLayout compareCache(const NativeCacheReservationHeader *header, uint64_t fileSize, uint64_t nativeHeader) {
    if (fileSize < sizeof(*header) || memcmp(header->magic, "dyld_v1   arm64", sizeof(header->magic)) ||
        header->mappingOffset < sizeof(*header) || !header->mappingCount || header->mappingOffset > fileSize ||
        (uint64_t)header->mappingCount * 32 > fileSize - header->mappingOffset || !header->sharedRegionStart || !header->sharedRegionSize ||
        (header->sharedRegionStart & 4095) || (header->sharedRegionSize & 4095) ||
        header->sharedRegionSize > UINT64_MAX - header->sharedRegionStart || !nativeHeader || (nativeHeader & 4095))
        return LSB_NATIVE_CACHE_LAYOUT_UNKNOWN;
    // This preflight only prevents automatic detachment. The host and loader reject unsupported slide tables or cache payloads.
    if (header->sharedRegionStart == LSB_SLIDABLE_CACHE_PREFERRED_BASE && header->sharedRegionSize == LSB_SLIDABLE_CACHE_SPAN &&
        header->mappingCount == 6) return LSB_NATIVE_CACHE_LAYOUT_SLIDABLE;
    uint64_t guestEnd = header->sharedRegionStart + header->sharedRegionSize;
    // Match SharedCacheHandoff.cpp's conservative retention bound: the entire guest reservation must precede the native header.
    if (guestEnd <= nativeHeader) return LSB_NATIVE_CACHE_LAYOUT_DISJOINT;
    // Only a native header actually inside the guest reservation establishes a collision. No guessed native range size is used.
    return nativeHeader >= header->sharedRegionStart ? LSB_NATIVE_CACHE_LAYOUT_OVERLAP : LSB_NATIVE_CACHE_LAYOUT_UNKNOWN;
}

LSBNativeCacheLayout LSBReadNativeCacheLayout(const char *runtimeRoot) {
    int saved = errno;
    LSBNativeCacheLayout layout = LSB_NATIVE_CACHE_LAYOUT_UNKNOWN;
    static const char suffix[] = "/System/Library/Caches/com.apple.dyld/dyld_sim_shared_cache_arm64";
    char path[PATH_MAX];
    size_t length = runtimeRoot ? strnlen(runtimeRoot, sizeof(path)) : 0;
    if (!length || runtimeRoot[0] != '/' || length > sizeof(path) - sizeof(suffix)) return layout;
    memcpy(path, runtimeRoot, length);
    memcpy(path + length, suffix, sizeof(suffix));
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) { errno = saved; return layout; }
    struct stat info;
    NativeCacheReservationHeader header;
    if (!fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_size >= (off_t)sizeof(header)) {
        size_t offset = 0;
        while (offset < sizeof(header)) {
            ssize_t count = pread(fd, (char *)&header + offset, sizeof(header) - offset, (off_t)offset);
            if (count <= 0) break; // Interrupted/unreadable preflight is unknown; do not delay startup with retries.
            offset += (size_t)count;
        }
        uint64_t nativeHeader = 0;
        if (offset == sizeof(header) && !__shared_region_check_np(&nativeHeader)) layout = compareCache(&header, (uint64_t)info.st_size, nativeHeader);
    }
    close(fd);
    errno = saved;
    return layout;
}
