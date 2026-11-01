#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h>

static uint64_t nativeHeader = 0x190000000;
static int queryError;
static unsigned queries;
static int testSharedRegion(uint64_t *address) {
    assert(address); ++queries; *address = nativeHeader; errno = queryError;
    return queryError ? -1 : 0;
}
#define __shared_region_check_np testSharedRegion
#include "../ProcessSupport/NativeCacheLayout.c"
#undef __shared_region_check_np

static NativeCacheReservationHeader fixture(void) {
    NativeCacheReservationHeader header = {.mappingOffset = 240, .mappingCount = 1,
        .sharedRegionStart = 0x110000000, .sharedRegionSize = 0x7e194000};
    memcpy(header.magic, "dyld_v1   arm64", sizeof(header.magic));
    return header;
}

int main(void) {
    NativeCacheReservationHeader header = fixture(), malformed;
    uint64_t end = header.sharedRegionStart + header.sharedRegionSize;
    assert(compareCache(&header, 512, end) == LSB_NATIVE_CACHE_LAYOUT_DISJOINT);
    assert(compareCache(&header, 512, end + 0x4000) == LSB_NATIVE_CACHE_LAYOUT_DISJOINT);
    assert(compareCache(&header, 512, end - 0x4000) == LSB_NATIVE_CACHE_LAYOUT_OVERLAP);
    assert(compareCache(&header, 512, header.sharedRegionStart) == LSB_NATIVE_CACHE_LAYOUT_OVERLAP);
    assert(compareCache(&header, 512, header.sharedRegionStart - 0x4000) == LSB_NATIVE_CACHE_LAYOUT_UNKNOWN);
    assert(compareCache(&header, 512, 0) == LSB_NATIVE_CACHE_LAYOUT_UNKNOWN);
    assert(compareCache(&header, 512, end + 1) == LSB_NATIVE_CACHE_LAYOUT_UNKNOWN);
    assert(compareCache(&header, 239, end) == LSB_NATIVE_CACHE_LAYOUT_UNKNOWN);
    assert(compareCache(&header, 260, end) == LSB_NATIVE_CACHE_LAYOUT_UNKNOWN);
    NativeCacheReservationHeader slidable = header;
    slidable.mappingCount = 6;
    slidable.sharedRegionStart = LSB_SLIDABLE_CACHE_PREFERRED_BASE;
    slidable.sharedRegionSize = LSB_SLIDABLE_CACHE_SPAN;
    assert(compareCache(&slidable, 512, 0x190000000) == LSB_NATIVE_CACHE_LAYOUT_SLIDABLE);
    assert(compareCache(&slidable, 512, 0x280000000) == LSB_NATIVE_CACHE_LAYOUT_SLIDABLE);
    assert(compareCache(&slidable, 400, 0x190000000) == LSB_NATIVE_CACHE_LAYOUT_UNKNOWN);
    slidable.mappingCount = 5;
    assert(compareCache(&slidable, 512, 0x190000000) == LSB_NATIVE_CACHE_LAYOUT_OVERLAP);
    slidable.mappingCount = 6; slidable.sharedRegionSize -= 0x4000;
    assert(compareCache(&slidable, 512, 0x190000000) == LSB_NATIVE_CACHE_LAYOUT_OVERLAP);
    slidable.sharedRegionSize = LSB_SLIDABLE_CACHE_SPAN;
    for (unsigned fault = 0; fault < 8; ++fault) {
        malformed = header;
        switch (fault) {
            case 0: malformed.magic[0] = 0; break;
            case 1: malformed.mappingOffset = 239; break;
            case 2: malformed.mappingCount = 0; break;
            case 3: malformed.mappingCount = UINT32_MAX; break;
            case 4: malformed.sharedRegionStart = 0; break;
            case 5: malformed.sharedRegionSize = 0; break;
            case 6: malformed.sharedRegionSize = 1; break;
            case 7: malformed.sharedRegionStart = UINT64_MAX - 4095; break;
        }
        assert(compareCache(&malformed, 512, end) == LSB_NATIVE_CACHE_LAYOUT_UNKNOWN);
    }
    char root[] = "/tmp/LiveSimBooter-cache-layout.XXXXXX", path[PATH_MAX];
    assert(mkdtemp(root));
    const char *directories[] = {"/System", "/System/Library", "/System/Library/Caches", "/System/Library/Caches/com.apple.dyld"};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(*directories); ++i) {
        snprintf(path, sizeof(path), "%s%s", root, directories[i]);
        assert(!mkdir(path, 0700));
    }
    snprintf(path, sizeof(path), "%s/System/Library/Caches/com.apple.dyld/dyld_sim_shared_cache_arm64", root);
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && write(fd, &header, sizeof(header)) == sizeof(header) && !ftruncate(fd, 512));
    errno = EBUSY;
    assert(LSBReadNativeCacheLayout(root) == LSB_NATIVE_CACHE_LAYOUT_DISJOINT && errno == EBUSY && queries == 1);
    nativeHeader = 0x180000000;
    assert(LSBReadNativeCacheLayout(root) == LSB_NATIVE_CACHE_LAYOUT_OVERLAP && errno == EBUSY && queries == 2);
    queryError = ENOMEM;
    assert(LSBReadNativeCacheLayout(root) == LSB_NATIVE_CACHE_LAYOUT_UNKNOWN && errno == EBUSY && queries == 3);
    assert(LSBReadNativeCacheLayout(NULL) == LSB_NATIVE_CACHE_LAYOUT_UNKNOWN && errno == EBUSY);
    assert(LSBReadNativeCacheLayout("relative") == LSB_NATIVE_CACHE_LAYOUT_UNKNOWN && errno == EBUSY);
    assert(!ftruncate(fd, 32)); errno = EBUSY;
    assert(LSBReadNativeCacheLayout(root) == LSB_NATIVE_CACHE_LAYOUT_UNKNOWN && errno == EBUSY && queries == 3);
    assert(!close(fd) && !unlink(path)); errno = EBUSY;
    assert(LSBReadNativeCacheLayout(root) == LSB_NATIVE_CACHE_LAYOUT_UNKNOWN && errno == EBUSY);
    for (unsigned i = sizeof(directories) / sizeof(*directories); i; --i) {
        snprintf(path, sizeof(path), "%s%s", root, directories[i - 1]); assert(!rmdir(path));
    }
    assert(!rmdir(root));
    puts("Native cache layout: real header reads, conservative collision boundaries, malformed/query failure and errno checks passed");
}
