#include "../LiveSimBooter/CompactImageInfo.cpp"

#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace lsb { pid_t getpid() { return 1234; } }
static unsigned checks;
static void check(bool condition, const char *message) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct alignas(16384) TestImage { mach_header_64 header; uuid_command uuid; segment_command_64 pageZero, text; };
static TestImage fixtures[3];
struct alignas(16384) CacheImageFixture {
    uint8_t padding[4096];
    mach_header_64 header;
    uuid_command uuid;
    segment_command_64 text, data;
};
static CacheImageFixture cachedFixture;
static_assert(offsetof(CacheImageFixture, header) == 4096, "Exercise a cached image not aligned to the VM page size");
static uint64_t little(const uint8_t *bytes, unsigned size) {
    uint64_t result = 0;
    for (unsigned index = 0; index < size; ++index) result |= uint64_t(bytes[index]) << (index * 8);
    return result;
}
static uint32_t referenceCRC(const uint8_t *bytes, size_t size) {
    uint32_t table[256];
    for (unsigned value = 0; value < 256; ++value) {
        uint32_t entry = value;
        for (unsigned bit = 0; bit < 8; ++bit) entry = (entry & 1) ? ((entry >> 1) ^ 0x82f63b78) : (entry >> 1);
        table[value] = entry;
    }
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < size; ++index) crc = table[(crc ^ bytes[index]) & 255] ^ (crc >> 8);
    return ~crc;
}
static uint32_t referenceChecksum(const uint8_t *bytes, size_t size) {
    uint32_t table[256];
    for (unsigned value = 0; value < 256; ++value) {
        uint32_t entry = value << 24;
        for (unsigned bit = 0; bit < 8; ++bit) entry = (entry & 0x80000000) ? ((entry << 1) ^ 0x04c11db7) : (entry << 1);
        table[value] = entry;
    }
    uint32_t crc = 0;
    for (size_t index = 0; index < size; ++index) crc = table[(crc >> 24) ^ bytes[index]] ^ (crc << 8);
    for (size_t remaining = size; remaining; remaining >>= 8) crc = table[(crc >> 24) ^ (remaining & 255)] ^ (crc << 8);
    return ~crc;
}
static void appendLittle(std::vector<uint8_t> &bytes, uint64_t value, unsigned width) {
    for (unsigned index = 0; index < width; ++index) bytes.push_back(static_cast<uint8_t>(value >> (8 * index)));
}
static void append(std::vector<uint8_t> &bytes, const char *text, size_t size) { bytes.insert(bytes.end(), text, text + size); }
static std::vector<uint8_t> nativeArchive(bool modern, const char *path = "process.plist", unsigned width = 2) {
    // Native 26 emits CKSF then DATA (often preceded by process.cinfo); native 27 emits DATA then XATA.
    const uint8_t payload[] = {'b', 'p', 'l', 'i', 's', 't', '0', '0'};
    size_t pathLength = std::strlen(path);
    std::vector<uint8_t> bytes;
    append(bytes, "AA01", 4); appendLittle(bytes, (modern ? 27 : 29) + pathLength + width, 2);
    append(bytes, "TYP1FPATP", 9); appendLittle(bytes, pathLength, 2); append(bytes, path, pathLength);
    if (!modern) { append(bytes, "CKSF", 4); appendLittle(bytes, referenceChecksum(payload, sizeof(payload)), 4); }
    append(bytes, width == 2 ? "DATA" : width == 4 ? "DATB" : "DATC", 4); appendLittle(bytes, sizeof(payload), width);
    if (modern) { append(bytes, "XATA", 4); appendLittle(bytes, 34, 2); }
    bytes.insert(bytes.end(), payload, payload + sizeof(payload));
    if (modern) {
        appendLittle(bytes, 34, 4); append(bytes, "com.apple.dyld.crc32c", 22); appendLittle(bytes, bytes.size() + 8, 4);
        appendLittle(bytes, referenceCRC(bytes.data(), bytes.size()), 4);
    }
    return bytes;
}
static void testEncodingSelection() {
    check(posixChecksum(reinterpret_cast<const uint8_t *>("123456789"), 9) == 930766865, "POSIX cksum standard check value");
    check(posixChecksum(nullptr, 0) == UINT32_MAX, "POSIX cksum empty input");
    check(lsb::configureCompactImageInfo(nullptr, 0) == EINVAL, "missing native snapshot rejected");
    for (bool modern : {false, true}) {
        for (unsigned width : {2u, 4u, 8u}) {
            auto bytes = nativeArchive(modern, "process.plist", width);
            check(lsb::configureCompactImageInfo(bytes.data(), bytes.size()) == 0, "native DATA, DATB and DATC widths are accepted");
            check(sEncoding == (modern ? ArchiveEncoding::crc32cAttribute : ArchiveEncoding::posixChecksum), "native encoding is retained as a local enum");
            for (size_t truncated = 1; truncated < bytes.size(); ++truncated) {
                std::vector<uint8_t> prefix(bytes.begin(), bytes.begin() + truncated);
                check(lsb::configureCompactImageInfo(prefix.data(), prefix.size()) != 0, "every truncated archive is rejected");
            }
            auto corrupt = bytes; corrupt.back() ^= 1;
            check(lsb::configureCompactImageInfo(corrupt.data(), corrupt.size()) == EBADMSG, "native checksum corruption rejected");
        }
    }
    auto legacy = nativeArchive(false, "process.cinfo"), plist = nativeArchive(false);
    legacy.insert(legacy.end(), plist.begin(), plist.end());
    check(lsb::configureCompactImageInfo(legacy.data(), legacy.size()) == 0 && sEncoding == ArchiveEncoding::posixChecksum,
          "native 26 process.cinfo followed by process.plist selects legacy encoding");
    auto unknown = plist; unknown[0] = 'B';
    check(lsb::configureCompactImageInfo(unknown.data(), unknown.size()) == ENOTSUP, "unknown archive magic rejected");
    unknown = plist; unknown[30] = 'Z';
    check(lsb::configureCompactImageInfo(unknown.data(), unknown.size()) == ENOTSUP, "unknown structural field rejected");
    unknown = plist; unknown[41] = 'D';
    check(lsb::configureCompactImageInfo(unknown.data(), unknown.size()) == ENOTSUP, "unsupported blob width rejected");
    unknown = plist; unknown[42] = 0xff; unknown[43] = 0xff;
    check(lsb::configureCompactImageInfo(unknown.data(), unknown.size()) == EBADMSG, "oversized blob length rejected");
    unknown = plist; unknown[4] = 5;
    check(lsb::configureCompactImageInfo(unknown.data(), unknown.size()) == EBADMSG, "undersized record header rejected");
    unknown = plist; std::memcpy(unknown.data() + 38, "CKSF", 4);
    check(lsb::configureCompactImageInfo(unknown.data(), unknown.size()) == EBADMSG, "duplicate checksum field rejected");
    auto disguised = nativeArchive(false, "process.plistCKSF");
    check(lsb::configureCompactImageInfo(disguised.data(), disguised.size()) == ENOTSUP, "path substring does not identify the process plist");
    auto duplicated = plist; duplicated.insert(duplicated.end(), plist.begin(), plist.end());
    check(lsb::configureCompactImageInfo(duplicated.data(), duplicated.size()) == EBADMSG, "duplicate process plist rejected");
    auto modern = nativeArchive(true);
    auto attribute = modern; attribute[attribute.size() - 30] ^= 1;
    check(lsb::configureCompactImageInfo(attribute.data(), attribute.size()) == ENOTSUP, "unknown xattr name cannot select modern encoding");
    auto mixed = legacy; mixed.insert(mixed.end(), modern.begin(), modern.end());
    check(lsb::configureCompactImageInfo(mixed.data(), mixed.size()) != 0, "mixed legacy and modern archive rejected");
    check(lsb::configureCompactImageInfo(plist.data(), kMaxNativeArchiveSize + 1) == E2BIG, "native archive size is bounded before reading");
    sLocked = true;
    check(lsb::configureCompactImageInfo(plist.data(), plist.size()) == EBUSY, "selection uses the publication lock");
    sLocked = false;
    check(sEncoding == ArchiveEncoding::posixChecksum, "failed selection preserves the prior encoding");
}
static std::vector<uint8_t> currentBytes(const dyld_all_image_infos &info) {
    const auto *start = reinterpret_cast<const uint8_t *>(info.compact_dyld_image_info_addr);
    return {start, start + info.compact_dyld_image_info_size};
}
static CFTypeRef value(CFDictionaryRef dictionary, CFStringRef key) {
    CFTypeRef result = CFDictionaryGetValue(dictionary, key);
    check(result != nullptr, "required property-list key is present");
    return result;
}
static uint64_t number(CFDictionaryRef dictionary, CFStringRef key) {
    auto item = static_cast<CFNumberRef>(value(dictionary, key));
    check(CFGetTypeID(item) == CFNumberGetTypeID(), "integer field has the correct property-list type");
    int64_t result = 0;
    check(CFNumberGetValue(item, kCFNumberSInt64Type, &result), "property-list integer decodes");
    return static_cast<uint64_t>(result);
}
static void checkSnapshot(const dyld_all_image_infos &info, uint64_t state, bool secondHasUUID = true, const char *secondPath = "/RuntimeRoot/bin/df") {
    auto data = currentBytes(info);
    bool modern = sEncoding == ArchiveEncoding::crc32cAttribute;
    size_t headerSize = modern ? 44 : 46;
    check(data.size() > 78 && std::memcmp(data.data(), "AA01", 4) == 0 && little(data.data() + 4, 2) == headerSize, "native AA01 archive header");
    check(std::memcmp(data.data() + 6, "TYP1FPATP\x0d\x00process.plist", 24) == 0, "single process.plist regular-file record");
    size_t plistSize = static_cast<size_t>(little(data.data() + (modern ? 34 : 42), 4));
    if (modern) {
        check(std::memcmp(data.data() + 30, "DATB", 4) == 0, "modern data length field");
        check(std::memcmp(data.data() + 38, "XATA\x22\x00", 6) == 0 && plistSize + 44 + 34 == data.size(), "archive data and xattr lengths agree");
        const auto *attribute = data.data() + 44 + plistSize;
        check(little(attribute, 4) == 34 && std::memcmp(attribute + 4, "com.apple.dyld.crc32c", 22) == 0, "native dyld CRC attribute");
        check(little(attribute + 26, 4) == data.size(), "CRC attribute records the complete archive length");
        check(referenceCRC(data.data(), data.size() - 4) == little(attribute + 30, 4), "CRC validates archive bytes excluding final checksum");
        check(referenceCRC(data.data(), data.size()) == 0x48674bc7, "complete archive has the native CRC32C residue");
    } else {
        check(std::memcmp(data.data() + 30, "CKSF", 4) == 0 && std::memcmp(data.data() + 38, "DATB", 4) == 0, "legacy checksum and data fields");
        check(headerSize + plistSize == data.size(), "legacy archive has no CRC xattr suffix");
        check(referenceChecksum(data.data() + headerSize, plistSize) == little(data.data() + 34, 4), "independent POSIX checksum validates legacy payload");
    }
    CFDataRef encoded = CFDataCreate(kCFAllocatorDefault, data.data() + headerSize, static_cast<CFIndex>(plistSize));
    CFErrorRef error = nullptr;
    CFPropertyListRef decoded = CFPropertyListCreateWithData(kCFAllocatorDefault, encoded, kCFPropertyListImmutable, nullptr, &error);
    check(decoded && !error && CFGetTypeID(decoded) == CFDictionaryGetTypeID(), "independent CoreFoundation binary-plist decoder accepts snapshot");
    auto root = static_cast<CFDictionaryRef>(decoded);
    check(number(root, CFSTR("proc")) == 1234 && number(root, CFSTR("plat")) == PLATFORM_IOSSIMULATOR, "real process ID and simulator platform");
    check(number(root, CFSTR("time")) == info.infoArrayChangeTimestamp && info.infoArrayChangeTimestamp != 0, "blob and legacy timestamps agree and are nonzero");
    check(number(root, CFSTR("stat")) == state && number(root, CFSTR("init")) == info.initialImageCount, "process state and initial count");
    check(number(root, CFSTR("flag")) == 4 && number(root, CFSTR("flags")) == 4, "private-region flags support both atlas spellings");
    auto images = static_cast<CFArrayRef>(value(root, CFSTR("imgs")));
    check(CFGetTypeID(images) == CFArrayGetTypeID() && CFArrayGetCount(images) == 3, "image list includes dyld once");
    const char *paths[] = {"/app/LiveSimBooter", secondPath, "/RuntimeRoot/usr/lib/dyld_sim"};
    for (unsigned index = 0; index < 3; ++index) {
        auto image = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(images, index));
        check(number(image, CFSTR("addr")) == reinterpret_cast<uintptr_t>(&fixtures[index]), "images are sorted by load address");
        auto path = static_cast<CFStringRef>(value(image, CFSTR("file")));
        char pathBuffer[PATH_MAX];
        check(CFStringGetCString(path, pathBuffer, sizeof(pathBuffer), kCFStringEncodingUTF8) && std::strcmp(pathBuffer, paths[index]) == 0,
              "path survives the system binary-plist decoder");
        if (index != 1 || secondHasUUID) {
            auto uuid = static_cast<CFDataRef>(value(image, CFSTR("uuid")));
            check(CFGetTypeID(uuid) == CFDataGetTypeID() && CFDataGetLength(uuid) == 16 &&
                  std::memcmp(CFDataGetBytePtr(uuid), fixtures[index].uuid.uuid, 16) == 0, "UUID bytes round-trip");
        } else check(!CFDictionaryGetValue(image, CFSTR("uuid")), "absent UUID is omitted");
        auto segments = static_cast<CFArrayRef>(value(image, CFSTR("segs")));
        check(CFArrayGetCount(segments) == 1, "PAGEZERO is omitted while mapped segments are retained");
        auto segment = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(segments, 0));
        check(CFEqual(value(segment, CFSTR("name")), CFSTR("__TEXT")), "segment name");
        check(number(image, CFSTR("padr")) == fixtures[index].text.vmaddr && number(segment, CFSTR("padr")) == fixtures[index].text.vmaddr,
              "preferred image and segment addresses");
        check(number(segment, CFSTR("size")) == 32768 && number(segment, CFSTR("fsze")) == 16384 &&
              number(segment, CFSTR("foff")) == 0 && number(segment, CFSTR("perm")) == 5, "segment mapping metadata");
    }
    CFRelease(decoded); CFRelease(encoded);
}
static void expectFailure(dyld_all_image_infos &info, int error, uint64_t state = 0) {
    uintptr_t oldAddress = info.compact_dyld_image_info_addr;
    size_t oldSize = info.compact_dyld_image_info_size;
    uint64_t oldTimestamp = info.infoArrayChangeTimestamp;
    auto oldBytes = currentBytes(info);
    check(lsb::publishCompactImageInfo(&info, state) == error, "invalid input returns expected errno");
    check(info.compact_dyld_image_info_addr == oldAddress && info.compact_dyld_image_info_size == oldSize && info.infoArrayChangeTimestamp == oldTimestamp,
          "failed publication preserves descriptor and timestamp");
    check(currentBytes(info) == oldBytes, "failed publication preserves active bytes");
}
static void checkCachedSnapshot(const dyld_all_image_infos &info) {
    auto bytes = currentBytes(info);
    bool modern = sEncoding == ArchiveEncoding::crc32cAttribute;
    CFDataRef encoded = CFDataCreate(kCFAllocatorDefault, bytes.data() + (modern ? 44 : 46),
                                   static_cast<CFIndex>(little(bytes.data() + (modern ? 34 : 42), 4)));
    CFErrorRef error = nullptr;
    CFPropertyListRef decoded = CFPropertyListCreateWithData(kCFAllocatorDefault, encoded, kCFPropertyListImmutable, nullptr, &error);
    check(decoded && !error && CFGetTypeID(decoded) == CFDictionaryGetTypeID(), "cache snapshot decodes independently");
    auto root = static_cast<CFDictionaryRef>(decoded);
    check(!CFDictionaryGetValue(root, CFSTR("dsc1")), "flattened images do not also appear through a cache bitmap");
    auto images = static_cast<CFArrayRef>(value(root, CFSTR("imgs")));
    check(CFArrayGetCount(images) == 4, "all ordinary and cached images are present exactly once");
    unsigned found = 0;
    for (CFIndex index = 0; index < CFArrayGetCount(images); ++index) {
        auto image = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(images, index));
        if (number(image, CFSTR("addr")) != reinterpret_cast<uintptr_t>(&cachedFixture.header)) continue;
        ++found;
        check(number(image, CFSTR("padr")) == 0x180022000, "cache image preferred address comes from TEXT despite nonzero file offset");
        check(CFEqual(value(image, CFSTR("file")), CFSTR("/usr/lib/libcache-test.dylib")), "cache image install path is retained");
        auto segments = static_cast<CFArrayRef>(value(image, CFSTR("segs")));
        check(CFArrayGetCount(segments) == 2, "cache text and data mappings are both retained");
        auto text = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(segments, 0));
        auto data = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(segments, 1));
        check(number(text, CFSTR("padr")) == 0x180022000 && number(text, CFSTR("foff")) == 0x22000, "packed cache text coordinates");
        check(number(data, CFSTR("padr")) == 0x1800a0128 && number(data, CFSTR("foff")) == 0xa0128 &&
              number(data, CFSTR("size")) == 0x178 && number(data, CFSTR("perm")) == 3, "unaligned cache data coordinates");
        uint64_t slide = number(image, CFSTR("addr")) - number(image, CFSTR("padr"));
        check(number(data, CFSTR("padr")) + slide == reinterpret_cast<uintptr_t>(&cachedFixture.header) + 0x7e128,
              "Atlas address-minus-preferred-address calculation locates the real cached segment");
    }
    check(found == 1, "cached header appears exactly once");
    CFRelease(decoded); CFRelease(encoded);
}
static void testSnapshots(bool modern, const char *exportPath) {
    sEncoding = ArchiveEncoding::unconfigured; sTimestamp = sInitializationState = 0; sActiveBuffer = 0;
    auto native = nativeArchive(modern);
    for (unsigned index = 0; index < 3; ++index) {
        auto &image = fixtures[index];
        image.header.magic = MH_MAGIC_64; image.header.ncmds = 3;
        image.header.sizeofcmds = sizeof(uuid_command) + 2 * sizeof(segment_command_64);
        image.uuid.cmd = LC_UUID; image.uuid.cmdsize = sizeof(uuid_command);
        for (unsigned byte = 0; byte < 16; ++byte) image.uuid.uuid[byte] = static_cast<uint8_t>(index * 16 + byte);
        image.pageZero.cmd = image.text.cmd = LC_SEGMENT_64;
        image.pageZero.cmdsize = image.text.cmdsize = sizeof(segment_command_64);
        std::strcpy(image.pageZero.segname, "__PAGEZERO"); std::strcpy(image.text.segname, "__TEXT");
        image.text.vmaddr = index == 1 ? 0x100000000 : 0;
        image.text.vmsize = 32768; image.text.filesize = 16384; image.text.initprot = 5;
    }
    const auto *app = reinterpret_cast<const mach_header *>(&fixtures[0]);
    const auto *df = reinterpret_cast<const mach_header *>(&fixtures[1]);
    const auto *dyld = reinterpret_cast<const mach_header *>(&fixtures[2]);
    dyld_image_info images[] = {{dyld, "/RuntimeRoot/usr/lib/dyld_sim", 0}, {app, "/app/LiveSimBooter", 0}, {df, "/RuntimeRoot/bin/df", 0}, {}};
    alignas(16) dyld_all_image_infos info = {};
    info.infoArray = images; info.infoArrayCount = 3; info.initialImageCount = 2; info.platform = PLATFORM_IOSSIMULATOR;
    info.dyldImageLoadAddress = dyld; info.dyldPath = images[0].imageFilePath; info.processDetachedFromSharedRegion = true;
    check(lsb::publishCompactImageInfo(&info) == ENXIO, "publication requires native-reader configuration");
    check(lsb::configureCompactImageInfo(native.data(), native.size()) == 0, "native-reader configuration succeeds before first publication");
    std::memset(native.data(), 0, native.size());
    check(lsb::publishCompactImageInfo(&info) == 0, "initial snapshot publishes");
    check(lsb::configureCompactImageInfo(native.data(), native.size()) == EBUSY, "encoding cannot change after first publication");
    checkSnapshot(info, 0x10);
    auto firstSnapshot = currentBytes(info); uintptr_t firstAddress = info.compact_dyld_image_info_addr;
    info.libSystemInitialized = true;
    check(lsb::publishCompactImageInfo(&info) == 0, "second snapshot publishes");
    check(info.compact_dyld_image_info_addr != firstAddress &&
          std::memcmp(reinterpret_cast<const void *>(firstAddress), firstSnapshot.data(), firstSnapshot.size()) == 0, "previous snapshot retained");
    checkSnapshot(info, 0x30);
    fixtures[1].uuid.cmd = LC_SOURCE_VERSION;
    check(lsb::publishCompactImageInfo(&info) == 0, "missing UUID supported"); checkSnapshot(info, 0x30, false);
    fixtures[1].uuid.cmd = LC_UUID;
    images[2].imageFilePath = "/RuntimeRoot/bin/đĩa-😀";
    check(lsb::publishCompactImageInfo(&info) == 0, "UTF-8 paths encode as UTF-16 plist strings");
    checkSnapshot(info, 0x30, true, images[2].imageFilePath);
    images[2].imageFilePath = "/RuntimeRoot/bin/df";
    check(lsb::publishCompactImageInfo(nullptr) == EINVAL, "null metadata rejected");
    info.infoArrayCount = kMaxImages + 1; expectFailure(info, E2BIG); info.infoArrayCount = 3;
    info.infoArray = nullptr; expectFailure(info, EINVAL); info.infoArray = images;
    cachedFixture.header.magic = MH_MAGIC_64; cachedFixture.header.ncmds = 3; cachedFixture.header.flags = MH_DYLIB_IN_CACHE;
    cachedFixture.header.sizeofcmds = sizeof(uuid_command) + 2 * sizeof(segment_command_64);
    cachedFixture.uuid.cmd = LC_UUID; cachedFixture.uuid.cmdsize = sizeof(uuid_command);
    cachedFixture.text.cmd = cachedFixture.data.cmd = LC_SEGMENT_64;
    cachedFixture.text.cmdsize = cachedFixture.data.cmdsize = sizeof(segment_command_64);
    std::strcpy(cachedFixture.text.segname, "__TEXT"); std::strcpy(cachedFixture.data.segname, "__DATA");
    cachedFixture.text.vmaddr = 0x180022000; cachedFixture.text.fileoff = 0x22000;
    cachedFixture.text.vmsize = cachedFixture.text.filesize = 0x2380; cachedFixture.text.initprot = 5;
    cachedFixture.data.vmaddr = 0x1800a0128; cachedFixture.data.fileoff = 0xa0128;
    cachedFixture.data.vmsize = cachedFixture.data.filesize = 0x178; cachedFixture.data.initprot = 3;
    images[3] = {reinterpret_cast<const mach_header *>(&cachedFixture.header), "/usr/lib/libcache-test.dylib", 0};
    info.infoArrayCount = 4; info.sharedCacheBaseAddress = 0x180010000; info.sharedCacheSlide = 0x10000; info.sharedCacheUUID[0] = 42;
    check(lsb::publishCompactImageInfo(&info) == 0, "shared cache and packed Mach-O headers are supported");
    checkCachedSnapshot(info);
    check(info.sharedCacheBaseAddress == 0x180010000 && info.sharedCacheSlide == 0x10000 && info.sharedCacheUUID[0] == 42,
          "snapshot leaves authoritative legacy cache metadata unchanged");
    cachedFixture.header.flags = 0; expectFailure(info, EINVAL); cachedFixture.header.flags = MH_DYLIB_IN_CACHE;
    info.infoArrayCount = 3; info.sharedCacheBaseAddress = info.sharedCacheSlide = 0; info.sharedCacheUUID[0] = 0;
    fixtures[1].header.magic = 0; expectFailure(info, ENOEXEC); fixtures[1].header.magic = MH_MAGIC_64;
    fixtures[1].uuid.cmdsize = 7; expectFailure(info, ENOEXEC); fixtures[1].uuid.cmdsize = sizeof(uuid_command);
    fixtures[1].header.ncmds = 4; expectFailure(info, ENOEXEC); fixtures[1].header.ncmds = 3;
    fixtures[1].header.sizeofcmds = 1024 * 1024 + 1; expectFailure(info, ENOEXEC);
    fixtures[1].header.sizeofcmds = sizeof(uuid_command) + 2 * sizeof(segment_command_64);
    fixtures[1].text.nsects = 1; expectFailure(info, ENOEXEC); fixtures[1].text.nsects = 0;
    images[2].imageLoadAddress = reinterpret_cast<const mach_header *>(reinterpret_cast<uintptr_t>(df) + 4);
    expectFailure(info, EINVAL); images[2].imageLoadAddress = df;
    std::string longPath(PATH_MAX, 'x'); images[2].imageFilePath = longPath.c_str(); expectFailure(info, ENAMETOOLONG);
    images[2].imageFilePath = ""; expectFailure(info, EINVAL);
    images[2].imageFilePath = "\xc0\xaf"; expectFailure(info, EILSEQ); images[2].imageFilePath = "/RuntimeRoot/bin/df";
    sLocked = true; expectFailure(info, EBUSY); sLocked = false;
    check(lsb::publishCompactImageInfo(&info, 0x40) == 0, "before-initializers state advances"); checkSnapshot(info, 0x40);
    fixtures[1].header.magic = 0; expectFailure(info, ENOEXEC, 0x50); fixtures[1].header.magic = MH_MAGIC_64;
    check(lsb::publishCompactImageInfo(&info) == 0, "failed state update does not persist"); checkSnapshot(info, 0x40);
    check(lsb::publishCompactImageInfo(&info, 0x50) == 0, "main-called state advances"); checkSnapshot(info, 0x50);
    check(lsb::publishCompactImageInfo(&info, 0x10) == 0, "initialization state cannot regress"); checkSnapshot(info, 0x50);
    info.errorMessage = "test"; info.terminationFlags = 1;
    check(lsb::publishCompactImageInfo(&info) == 0, "pre-main loader error overrides state"); checkSnapshot(info, 0x20);
    info.terminationFlags = 0;
    check(lsb::publishCompactImageInfo(&info) == 0, "post-main loader error overrides state"); checkSnapshot(info, 0x60);
    info.errorMessage = nullptr;
    check(lsb::publishCompactImageInfo(&info) == 0, "error state does not replace saved progress"); checkSnapshot(info, 0x50);
    expectFailure(info, EINVAL, 0x21);
    if (exportPath) {
        auto bytes = currentBytes(info); FILE *output = std::fopen(exportPath, "wb");
        check(output && std::fwrite(bytes.data(), 1, bytes.size(), output) == bytes.size(), "export snapshot for independent native-SPI validation");
        std::fclose(output);
    }
}

static void testImageCapacity(bool modern) {
    sEncoding = ArchiveEncoding::unconfigured; sTimestamp = sInitializationState = 0; sActiveBuffer = 0;
    auto native = nativeArchive(modern);
    check(lsb::configureCompactImageInfo(native.data(), native.size()) == 0, "configure large-image snapshot encoding");
    constexpr size_t stride = 4096;
    static_assert(sizeof(mach_header_64) + sizeof(uuid_command) + kMaxSegments * sizeof(segment_command_64) <= stride);
    void *allocation = nullptr;
    check(posix_memalign(&allocation, stride, stride * (kMaxImages + 1)) == 0, "allocate distinct packed-cache image headers");
    std::memset(allocation, 0, stride * (kMaxImages + 1));
    std::vector<dyld_image_info> images(kMaxImages + 1);
    std::string path(PATH_MAX - 3, 'a');
    path += "\xc3\xa9"; // One non-ASCII scalar makes the long ASCII prefix use the larger UTF-16 plist representation too.
    for (size_t index = 0; index < images.size(); ++index) {
        auto header = reinterpret_cast<mach_header_64 *>(static_cast<uint8_t *>(allocation) + index * stride);
        header->magic = MH_MAGIC_64; header->flags = MH_DYLIB_IN_CACHE;
        header->ncmds = 2; header->sizeofcmds = sizeof(uuid_command) + sizeof(segment_command_64);
        auto uuid = reinterpret_cast<uuid_command *>(header + 1);
        uuid->cmd = LC_UUID; uuid->cmdsize = sizeof(*uuid);
        uuid->uuid[0] = index & 255; uuid->uuid[1] = (index >> 8) & 255;
        auto segments = reinterpret_cast<segment_command_64 *>(uuid + 1);
        for (size_t segmentIndex = 0; segmentIndex < kMaxSegments; ++segmentIndex) {
            auto &segment = segments[segmentIndex];
            segment.cmd = LC_SEGMENT_64; segment.cmdsize = sizeof(segment);
            std::memset(segment.segname, 's', sizeof(segment.segname));
            segment.segname[14] = '\xc3'; segment.segname[15] = '\xa9';
            segment.vmaddr = 0x180000000 + segmentIndex * 0x10000;
            segment.vmsize = segment.filesize = stride; segment.fileoff = segmentIndex * stride; segment.initprot = 5;
        }
        images[index] = {reinterpret_cast<const mach_header *>(header), "/usr/lib/capacity-test.dylib", 0};
    }
    alignas(16) dyld_all_image_infos info = {};
    info.infoArray = images.data(); info.platform = PLATFORM_IOSSIMULATOR; info.processDetachedFromSharedRegion = true;
    for (size_t count : {size_t(1025), size_t(1353), kMaxImages}) {
        size_t segmentsPerImage = count == kMaxImages ? kMaxSegments : 1;
        if (count == kMaxImages) {
            for (auto &image : images) {
                auto header = const_cast<mach_header *>(image.imageLoadAddress);
                header->ncmds = 1 + kMaxSegments;
                header->sizeofcmds = sizeof(uuid_command) + kMaxSegments * sizeof(segment_command_64);
                image.imageFilePath = path.c_str();
            }
        }
        info.infoArrayCount = count - 1; info.initialImageCount = count;
        info.dyldImageLoadAddress = images[count - 1].imageLoadAddress; info.dyldPath = images[count - 1].imageFilePath;
        check(lsb::publishCompactImageInfo(&info, 0x40) == 0, "publish more than 1024 images through exact capacity including separate dyld");
        auto bytes = currentBytes(info);
        size_t headerSize = modern ? kModernHeaderSize : kLegacyHeaderSize;
        size_t plistSize = little(bytes.data() + (modern ? 34 : 42), 4);
        check(bytes.size() < kBufferSize && bytes.size() < kMaxNativeArchiveSize, "maximum supported image metadata fits bounded archive storage");
        if (modern) check(referenceCRC(bytes.data(), bytes.size() - 4) == little(bytes.data() + bytes.size() - 4, 4), "large archive CRC32C validates");
        else check(referenceChecksum(bytes.data() + headerSize, plistSize) == little(bytes.data() + 34, 4), "large archive POSIX checksum validates");
        CFDataRef encoded = CFDataCreate(kCFAllocatorDefault, bytes.data() + headerSize, plistSize);
        CFErrorRef error = nullptr;
        CFPropertyListRef decoded = CFPropertyListCreateWithData(kCFAllocatorDefault, encoded, kCFPropertyListImmutable, nullptr, &error);
        check(decoded && !error && CFGetTypeID(decoded) == CFDictionaryGetTypeID(), "CoreFoundation independently decodes large-image archive");
        auto decodedImages = static_cast<CFArrayRef>(value(static_cast<CFDictionaryRef>(decoded), CFSTR("imgs")));
        check(CFArrayGetCount(decodedImages) == static_cast<CFIndex>(count), "all images including dyld survive large snapshot");
        for (size_t index = 0; index < count; ++index) {
            auto image = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(decodedImages, index));
            check(number(image, CFSTR("addr")) == reinterpret_cast<uintptr_t>(images[index].imageLoadAddress), "large snapshot preserves each distinct image");
            auto segments = static_cast<CFArrayRef>(value(image, CFSTR("segs")));
            check(CFArrayGetCount(segments) == static_cast<CFIndex>(segmentsPerImage), "maximum segment count survives serialization");
            char actualPath[PATH_MAX];
            check(CFStringGetCString(static_cast<CFStringRef>(value(image, CFSTR("file"))), actualPath, sizeof(actualPath), kCFStringEncodingUTF8) &&
                  std::strcmp(actualPath, images[index].imageFilePath) == 0, "maximum UTF-16 path survives the independent decoder");
        }
        CFRelease(decoded); CFRelease(encoded);
        if (count == kMaxImages) std::printf("Capacity archive (%s): %zu images, %zu segments each, %zu bytes.\n",
                                            modern ? "CRC32C" : "POSIX", count, segmentsPerImage, bytes.size());
    }
    info.infoArrayCount = kMaxImages;
    check(lsb::publishCompactImageInfo(&info) == 0, "dyld already in an exact-capacity array is not counted twice");
    info.dyldImageLoadAddress = images[kMaxImages].imageLoadAddress;
    expectFailure(info, E2BIG, 0x50);
    check(sInitializationState == 0x40, "overflow does not advance initialization state");
    info.dyldImageLoadAddress = images[kMaxImages - 1].imageLoadAddress;
    info.infoArrayCount = kMaxImages + 1;
    expectFailure(info, E2BIG);
    info.infoArrayCount = kMaxImages;
    check(lsb::publishCompactImageInfo(&info) == 0, "valid publication recovers after image-capacity failures");
    std::free(allocation);
}

static void testChecksumPageBoundary() {
    const size_t pageSize = static_cast<size_t>(getpagesize());
    auto *pages = static_cast<uint8_t *>(mmap(nullptr, 2 * pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    check(pages != MAP_FAILED && !mprotect(pages + pageSize, pageSize, PROT_NONE), "allocate guarded checksum input");
    for (size_t i = 0; i < pageSize; ++i) pages[i] = static_cast<uint8_t>(i * 17 + (i >> 3));
    for (size_t length = 0; length < 270; ++length) {
        const uint8_t *input = pages + pageSize - length;
        check(crc32c(input, length) == referenceCRC(input, length), "CRC32C unaligned input ending at a guard page");
        check(posixChecksum(input, length) == referenceChecksum(input, length), "POSIX cksum unaligned input ending at a guard page");
    }
    check(!munmap(pages, 2 * pageSize), "release guarded checksum input");
}

int main(int argc, char **argv) {
    testChecksumPageBoundary();
    check(crc32c(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0xe3069283, "standard CRC32C check value");
    check(crc32c(nullptr, 0) == 0, "empty CRC32C check value");
    testEncodingSelection();
    testSnapshots(false, argc == 3 ? argv[2] : nullptr);
    testSnapshots(true, argc >= 2 ? argv[1] : nullptr);
    testImageCapacity(false);
    testImageCapacity(true);
    std::printf("Compact image static storage: buffers=%zu metadata=%zu total=%zu bytes.\n", sizeof(sBuffers),
                sizeof(sImages) + sizeof(sObjectOffsets) + sizeof(sImageObjects),
                sizeof(sBuffers) + sizeof(sImages) + sizeof(sObjectOffsets) + sizeof(sImageObjects));
    std::printf("Compact image metadata: %u checks passed\n", checks);
}
