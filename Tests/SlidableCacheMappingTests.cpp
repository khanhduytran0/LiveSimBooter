// Sparse files exercise header validation and FD routing. mmap/VM queries are mocked; no cache code or anonymous code executes.
#include "../ProcessSupport/SlidableCache.h"
#include "../LiveSimBooter/NativeSupportBindings.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

struct MappingCall { void *address; size_t size; int protection, flags, fd; off_t offset; };
struct RejectedMapping { int status; };
static std::vector<MappingCall> calls;
static unsigned vmQueries;
static mach_msg_type_number_t vmReplyCount = VM_REGION_SUBMAP_INFO_COUNT_64;
static bool stripExecute;
static int lastOpenedFD = -1;
static unsigned nativeSupportApplications;
static bool nativeSupportReady;
static uint64_t textMappingSize = LSBSlidableCacheProfile[0].size;
int LSBNativeSupportBindingsPrepared() { return nativeSupportReady; }
int LSBApplyNativeSupportBindings(const LSBNativeSupportMemory *) { ++nativeSupportApplications; return 0; }
namespace lsb {
int *errnoAddress() { return &errno; }
[[noreturn]] void exitProcess(int status) { throw RejectedMapping{status}; }
int open(const char *path, int flags) { return lastOpenedFD = ::open(path, flags); }
int close(int fd) { return ::close(fd); }
int mprotect(void *, size_t, int) { return 0; }
int fstat(int fd, struct stat *info) { return ::fstat(fd, info); }
ssize_t pread(int fd, void *bytes, size_t size, off_t offset) { return ::pread(fd, bytes, size, offset); }
size_t copyString(char *output, const char *input, size_t size) { return strlcpy(output, input, size); }
mach_port_t task_self_trap() { return mach_task_self(); }
void *mmap(void *address, size_t size, int protection, int flags, int fd, off_t offset) {
    calls.push_back({address, size, protection, flags, fd, offset});
    return address;
}
kern_return_t vm_region_recurse_64(vm_map_read_t, vm_address_t *, vm_size_t *size, natural_t *,
                                 vm_region_recurse_info_t info, mach_msg_type_number_t *count) {
    ++vmQueries;
    auto *region = reinterpret_cast<vm_region_submap_info_data_64_t *>(info);
    *region = {};
    region->protection = stripExecute ? VM_PROT_READ : VM_PROT_READ | VM_PROT_EXECUTE;
    region->max_protection = stripExecute ? VM_PROT_READ | VM_PROT_WRITE : VM_PROT_ALL;
    *size = textMappingSize;
    *count = vmReplyCount;
    return KERN_SUCCESS;
}
}
static void debuggerLog(const char *, ...) {}
#include "../LiveSimBooter/SlidableCacheImplementation.h"

static void reset() {
    if (sSlidableCache.dataFD >= 0) close(sSlidableCache.dataFD);
    sSlidableCache = {};
    calls.clear(); vmQueries = 0; stripExecute = false; vmReplyCount = VM_REGION_SUBMAP_INFO_COUNT_64; nativeSupportApplications = 0;
}

static void expectRejected(void *address, size_t size, int protection, int flags, int fd, off_t offset) {
    try { slidableCacheMmap(address, size, protection, flags, fd, offset); assert(false); }
    catch (const RejectedMapping &failure) { assert(failure.status == 126); }
}

int main() {
    char root[] = "/tmp/lsb-slide-mapping-XXXXXX";
    assert(mkdtemp(root));
    std::filesystem::path directory = std::filesystem::path(root) / "System/Library/Caches/com.apple.dyld";
    std::filesystem::create_directories(directory);
    uint8_t uuid[16] = {0x21, 0x43, 0x65, 0x87};
    int cacheFD = ::open((directory / "cache").c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(cacheFD >= 0);
    for (LSBSlidableCacheProfileKind profileKind : {LSBSlidableCacheOriginal, LSBSlidableCacheNativeSupport}) {
        const auto *profile = LSBSlidableCacheLayoutForProfile(profileKind);
        nativeSupportReady = profileKind == LSBSlidableCacheNativeSupport;
        textMappingSize = profile->mappings[0].size;
        ++uuid[0];
        SlidableCacheHeader cache = {};
        memcpy(cache.magic, "dyld_v1   arm64", sizeof(cache.magic));
        memcpy(cache.uuid, uuid, sizeof(uuid));
        cache.mappingCount = cache.mappingWithSlideCount = 6; cache.mappingWithSlideOffset = sizeof(cache);
        cache.sharedRegionStart = LSB_SLIDABLE_CACHE_PREFERRED_BASE; cache.sharedRegionSize = profile->span;
        assert(pwrite(cacheFD, &cache, sizeof(cache), 0) == sizeof(cache));
        assert(pwrite(cacheFD, profile->mappings, 6 * sizeof(LSBSlidableCacheMapping), sizeof(cache)) == 6 * sizeof(LSBSlidableCacheMapping));
        for (uint64_t actualBase : {UINT64_C(0x300000000), UINT64_C(0x7000000000)}) {
            LSBSlidableDataHeader header = {};
            memcpy(header.magic, "LSBSlideDataV1", sizeof("LSBSlideDataV1"));
            header.version = 1; header.headerSize = sizeof(header); memcpy(header.cacheUUID, uuid, sizeof(uuid));
            header.preferredBase = cache.sharedRegionStart; header.actualBase = actualBase; header.cacheSpan = cache.sharedRegionSize;
            header.fixupCount = profile->fixups; header.taggedCount = profile->tagged;
            header.mappingCount = 3; header.mappingEntrySize = sizeof(LSBSlidableDataMapping); header.fileSize = 0x4000;
            for (unsigned i = 0; i < 3; ++i) {
                const auto &m = profile->mappings[i + 1];
                header.mappings[i] = {i + 1, 0, m.address, m.size, m.fileOffset, header.fileSize, m.maxProtection, m.initialProtection};
                header.fileSize += m.size;
            }
            char name[LSB_SLIDABLE_DATA_NAME_CAPACITY]; LSBSlidableDataFileName(uuid, actualBase, name);
            int dataFD = ::open((directory / name).c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
            assert(dataFD >= 0 && write(dataFD, &header, sizeof(header)) == sizeof(header) && !ftruncate(dataFD, header.fileSize));
            assert(!fchmod(dataFD, 0444));
            reset();
            assert(!prepareSlidableData(root, actualBase, uuid, false, profileKind));
            const int originalDataFD = sSlidableCache.dataFD;
            for (unsigned index = 0; index < 6; ++index) {
                const auto &m = profile->mappings[index];
                auto *address = reinterpret_cast<void *>(actualBase + m.address - header.preferredBase);
                int protection = index == 2 ? PROT_READ | PROT_WRITE : m.initialProtection;
                assert(slidableCacheMmap(address, m.size, protection, MAP_FIXED | MAP_PRIVATE, cacheFD, m.fileOffset) == address);
                assert(calls.size() == index + 1 && calls.back().protection == protection);
                if (index >= 1 && index <= 3) {
                    assert(calls.back().fd == originalDataFD && calls.back().offset == static_cast<off_t>(header.mappings[index - 1].backingFileOffset));
                } else assert(calls.back().fd == cacheFD && calls.back().offset == static_cast<off_t>(m.fileOffset));
                assert(nativeSupportApplications == (nativeSupportReady && index == 5 ? 1u : 0u));
            }
            assert(sSlidableCache.mapped && sSlidableCache.dataFD == -1 && fcntl(originalDataFD, F_GETFD) < 0 && errno == EBADF);
            assert(fcntl(cacheFD, F_GETFD) >= 0 && vmQueries == 1); // Executable VM permission is checked even without diagnostic logging.
            for (mach_msg_type_number_t prefixCount : {VM_REGION_SUBMAP_INFO_V0_COUNT_64, mach_msg_type_number_t(19)}) {
                reset(); assert(!prepareSlidableData(root, actualBase, uuid, false, profileKind)); vmReplyCount = prefixCount;
                void *text = reinterpret_cast<void *>(actualBase);
                assert(slidableCacheMmap(text, profile->mappings[0].size, PROT_READ | PROT_EXEC, MAP_FIXED | MAP_PRIVATE, cacheFD, 0) == text);
                assert(vmQueries == 1 && sSlidableCache.seen == 1); // iOS 26.1's count19 and the original full-info prefix both suffice.
            }
            reset(); assert(!prepareSlidableData(root, actualBase, uuid, false, profileKind)); vmReplyCount = VM_REGION_SUBMAP_INFO_V0_COUNT_64 - 1;
            expectRejected(reinterpret_cast<void *>(actualBase), profile->mappings[0].size, PROT_READ | PROT_EXEC,
                           MAP_FIXED | MAP_PRIVATE, cacheFD, 0);
            assert(vmQueries == 1 && sSlidableCache.seen == 0); // A truncated full-info prefix must not authorize execution.
            reset(); assert(!prepareSlidableData(root, actualBase, uuid, false, profileKind));
            expectRejected(reinterpret_cast<void *>(header.preferredBase), 0x4000, PROT_READ, MAP_FIXED | MAP_PRIVATE, cacheFD, 0);
            assert(calls.empty()); // A matching cache may never overwrite the native shared region.
            reset(); assert(!prepareSlidableData(root, actualBase, uuid, false, profileKind)); stripExecute = true; vmReplyCount = 19;
            expectRejected(reinterpret_cast<void *>(actualBase), profile->mappings[0].size, PROT_READ | PROT_EXEC,
                           MAP_FIXED | MAP_PRIVATE, cacheFD, 0);
            assert(vmQueries == 1 && !sSlidableCache.mapped && sSlidableCache.seen == 0);
            reset();
            header.cacheUUID[0] ^= 1;
            assert(pwrite(dataFD, &header, sizeof(header), 0) == sizeof(header));
            assert(prepareSlidableData(root, actualBase, uuid, false, profileKind) == EINVAL && sSlidableCache.dataFD == -1);
            assert(fcntl(lastOpenedFD, F_GETFD) < 0 && errno == EBADF);
            close(dataFD);
        }
    }
    reset();
    assert(!prepareSlidableData(nullptr, 0, nullptr, false));
    assert(slidableCacheMmap(reinterpret_cast<void *>(0x1000), 0x4000, PROT_READ, MAP_PRIVATE, cacheFD, 1234) == reinterpret_cast<void *>(0x1000));
    assert(calls.back().fd == cacheFD && calls.back().offset == 1234 && !vmQueries);
    close(cacheFD);
    std::filesystem::remove_all(root);
    puts("Slidable cache mappings: dynamic bases, immutable data FD routing, requested protections, unconditional RX checks and low fallback passed");
}
