#include "NativeSupportBindings.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <vector>

// Export kind2 is dyld's bindAbsolute path (Loader::hasExportedSymbol). Only separately reserved LINKEDIT pages are replaced;
// signed TEXT, native code, and native export tables are untouched. Cache use sites come from Apple's v4 patch table ABI.
namespace {
constexpr uint64_t preferredBase = 0x180000000, expectedSpan = 0x7e14c000;
constexpr uint64_t reserveOffset = 0x7e130000;
constexpr size_t reserveSizes[] = {0x10000, 0x4000, 0x4000};
constexpr const char *supportNames[] = {"/usr/lib/system/libsystem_kernel.dylib", "/usr/lib/system/libsystem_platform.dylib",
                                      "/usr/lib/system/libsystem_pthread.dylib"};
struct CacheHeader {
    char magic[16]; uint32_t mappingsOffset, mappingsCount;
    uint8_t beforeUUID[64]; uint8_t uuid[16]; uint64_t cacheType;
    uint8_t beforePatches[40]; uint64_t patchesAddress, patchesSize;
    uint8_t beforePlatform[48]; uint32_t platform, flags;
    uint64_t base, span, maxSlide; uint8_t beforeImages[200]; uint32_t imagesOffset, imagesCount;
};
static_assert(offsetof(CacheHeader, patchesAddress) == 152 && offsetof(CacheHeader, base) == 224 && offsetof(CacheHeader, imagesOffset) == 448);
struct Mapping { uint64_t address, size, fileOffset; uint32_t maximum, initial; };
struct CacheImage { uint64_t address, time, inode; uint32_t pathOffset, padding; };
struct PatchSpan { uint64_t address, count; };
struct PatchHeader {
    uint32_t version, locationVersion;
    PatchSpan images, exports, clients, clientExports, locations, names, gotClients, gotExports, gotLocations;
};
struct PatchImage { uint32_t clientsStart, clientsCount, exportsStart, exportsCount; };
struct PatchExport { uint32_t implementationOffset, nameAndKind; };
struct PatchClient { uint32_t image, exportsStart, exportsCount; };
struct PatchClientExport { uint32_t exportIndex, locationsStart, locationsCount; };
struct PatchLocation { uint32_t offset, metadata; };
static_assert(sizeof(Mapping) == 32 && sizeof(CacheImage) == 32 && sizeof(PatchHeader) == 152 && sizeof(PatchImage) == 16);
static_assert(sizeof(PatchExport) == 8 && sizeof(PatchClient) == 12 && sizeof(PatchClientExport) == 12 && sizeof(PatchLocation) == 8);
struct Image { uint64_t address; std::string name; uint32_t exportsOffset = 0, exportsSize = 0; };
struct Page { uintptr_t address; std::vector<uint8_t> contents; };
struct Pointer { uintptr_t address; uint64_t before, after, target, addend, tag; };
struct Writable { uintptr_t address; size_t size; int initial; };
struct State { std::vector<Page> pages; std::vector<Pointer> pointers; std::vector<Writable> writable; bool prepared = false, applied = false; };
State state;

void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool contains(uint64_t size, uint64_t offset, uint64_t length) { return offset <= size && length <= size - offset; }
bool matchesName(const char name[16], const char *match, bool prefix = false) {
    for (size_t i = 0; i < 16; ++i) {
        if (!match[i]) return prefix || !name[i];
        if (name[i] != match[i]) return false;
    }
    return false;
}
struct View {
    const uint8_t *bytes; size_t size;
    template<class T> T get(uint64_t offset) const {
        require(contains(size, offset, sizeof(T)), "Truncated native-support cache field");
        T result; memcpy(&result, bytes + offset, sizeof(result)); return result;
    }
    std::string string(uint64_t offset, size_t maximum = 4096) const {
        require(offset < size, "Native-support string outside cache");
        maximum = std::min(maximum, size - size_t(offset));
        const char *text = reinterpret_cast<const char *>(bytes + offset);
        const char *end = static_cast<const char *>(memchr(text, 0, maximum));
        require(end, "Unterminated native-support string"); return std::string(text, end);
    }
};
struct File {
    int fd = -1; void *bytes = MAP_FAILED; size_t size = 0;
    ~File() { if (bytes != MAP_FAILED) munmap(bytes, size); if (fd >= 0) close(fd); }
};
struct Dylib { void *handle = nullptr; ~Dylib() { if (handle) dlclose(handle); } };
uint64_t locate(const std::vector<Mapping> &maps, uint64_t address, size_t length) {
    for (const auto &map : maps)
        if (address >= map.address && contains(map.size, address - map.address, length)) return map.fileOffset + address - map.address;
    throw std::runtime_error("Native-support address outside cache mappings");
}
uint64_t uleb(View data, size_t &cursor) {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 70; shift += 7) {
        uint8_t byte = data.get<uint8_t>(cursor++);
        require(shift < 63 || byte <= 1, "Overflowing export ULEB"); value |= uint64_t(byte & 127) << shift;
        if (!(byte & 128)) return value;
    }
    throw std::runtime_error("Unterminated export ULEB");
}
void appendUleb(std::vector<uint8_t> &bytes, uint64_t value) {
    do { uint8_t byte = value & 127; value >>= 7; bytes.push_back(byte | (value ? 128 : 0)); } while (value);
}
struct Edge { std::string name; size_t child; };
struct Node { std::vector<uint8_t> terminal; std::vector<Edge> edges; size_t offset = 0; };
struct Export { uint64_t originalOffset, target; };
using ExportMap = std::unordered_map<std::string, Export>;
struct Trie {
    View input; const char *library; void *handle; LSBNativeSupportOverrideCallback override;
    std::vector<Node> nodes; std::unordered_set<size_t> visited; ExportMap exports;
    size_t parse(size_t offset, const std::string &prefix, unsigned depth) {
        require(depth < 512 && prefix.size() < 4096 && nodes.size() < 16384 && visited.insert(offset).second, "Malformed support export trie");
        size_t index = nodes.size(); nodes.emplace_back();
        size_t cursor = offset; uint64_t terminalLength = uleb(input, cursor);
        require(contains(input.size, cursor, terminalLength + 1), "Invalid support export terminal extent");
        size_t terminalEnd = cursor + terminalLength;
        if (terminalLength) {
            require(!prefix.empty() && prefix[0] == '_', "Unsupported support export name");
            uint64_t flags = uleb(input, cursor), value = uleb(input, cursor);
            require(flags == 0 && cursor == terminalEnd && exports.find(prefix) == exports.end(), "Expected distinct regular placeholder export");
            uintptr_t native = reinterpret_cast<uintptr_t>(dlsym(handle, prefix.c_str() + 1));
            uintptr_t target = override ? override(library, prefix.c_str(), native) : native;
            exports.emplace(prefix, Export{value, target});
            if (target) { appendUleb(nodes[index].terminal, 2); appendUleb(nodes[index].terminal, target); }
        }
        cursor = terminalEnd; unsigned count = input.get<uint8_t>(cursor++);
        std::unordered_set<unsigned char> firstBytes;
        for (unsigned i = 0; i < count; ++i) {
            std::string edge = input.string(cursor); cursor += edge.size() + 1;
            require(!edge.empty() && firstBytes.insert(edge[0]).second, "Ambiguous support trie edge");
            uint64_t child = uleb(input, cursor); require(child < input.size, "Support trie child outside reserve");
            size_t childIndex = parse(child, prefix + edge, depth + 1);
            nodes[index].edges.push_back(Edge{std::move(edge), childIndex});
        }
        return index;
    }
    std::vector<uint8_t> encodeNode(const Node &node) const {
        std::vector<uint8_t> bytes; appendUleb(bytes, node.terminal.size());
        bytes.insert(bytes.end(), node.terminal.begin(), node.terminal.end()); bytes.push_back(uint8_t(node.edges.size()));
        for (const auto &edge : node.edges) {
            bytes.insert(bytes.end(), edge.name.begin(), edge.name.end()); bytes.push_back(0); appendUleb(bytes, nodes[edge.child].offset);
        }
        return bytes;
    }
    std::vector<uint8_t> encode(size_t capacity) {
        for (unsigned iteration = 0; iteration < 32; ++iteration) {
            bool changed = false; size_t cursor = 0;
            for (auto &node : nodes) {
                changed |= node.offset != cursor; node.offset = cursor; cursor += encodeNode(node).size();
            }
            require(cursor <= capacity, "Native export trie exceeds its reserved LINKEDIT capacity");
            if (!changed) {
                std::vector<uint8_t> result; result.reserve(capacity);
                for (const auto &node : nodes) { auto bytes = encodeNode(node); result.insert(result.end(), bytes.begin(), bytes.end()); }
                result.resize(capacity, 0); return result;
            }
        }
        throw std::runtime_error("Native export trie offsets did not converge");
    }
};
template<class T> T table(View file, const std::vector<Mapping> &maps, PatchSpan span, uint64_t index) {
    require(index < span.count && span.count <= SIZE_MAX / sizeof(T), "Native-support patch table index outside bounds");
    uint64_t offset = locate(maps, span.address, span.count * sizeof(T)); return file.get<T>(offset + index * sizeof(T));
}
void range(PatchSpan span, uint64_t first, uint64_t count) { require(contains(span.count, first, count), "Native-support patch table range outside bounds"); }
}

extern "C" int LSBPrepareNativeSupportBindings(const char *cachePath, uint64_t actualBase, LSBNativeSupportOverrideCallback override,
                                               char *error, size_t errorSize) {
    if (state.prepared || state.applied) return EALREADY;
    if (!cachePath || !actualBase || (actualBase & 0x3fff)) return EINVAL;
    try {
        State next; File file;
        file.fd = open(cachePath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        require(file.fd >= 0, "Cannot open native-support cache");
        struct stat status = {}; require(fstat(file.fd, &status) == 0 && S_ISREG(status.st_mode) && status.st_size > 0, "Invalid native-support cache file");
        file.size = size_t(status.st_size); file.bytes = mmap(nullptr, file.size, PROT_READ, MAP_PRIVATE, file.fd, 0);
        require(file.bytes != MAP_FAILED, "Cannot inspect native-support cache");
        View view{static_cast<const uint8_t *>(file.bytes), file.size}; CacheHeader header = view.get<CacheHeader>(0);
        require(!memcmp(header.magic, "dyld_v1   arm64", 16) && header.base == preferredBase && header.span == expectedSpan &&
                header.platform == 7 && header.cacheType == 0 && header.mappingsCount == 6 && header.imagesCount == 1353,
                "Unsupported native-support cache profile");
        require(actualBase <= UINTPTR_MAX - header.span, "Native-support cache address overflow");
        std::vector<Mapping> maps;
        for (unsigned i = 0; i < header.mappingsCount; ++i) {
            Mapping map = view.get<Mapping>(header.mappingsOffset + i * sizeof(Mapping));
            require(map.address >= header.base && contains(header.span, map.address - header.base, map.size) &&
                    contains(file.size, map.fileOffset, map.size), "Invalid native-support mapping"); maps.push_back(map);
            if (map.maximum & PROT_WRITE) {
                require(!(map.maximum & PROT_EXEC), "Writable native-support mapping is executable");
                next.writable.push_back(Writable{uintptr_t(actualBase + map.address - header.base), size_t(map.size), int(map.initial)});
            }
        }
        std::vector<Image> images;
        for (unsigned i = 0; i < header.imagesCount; ++i) {
            CacheImage cached = view.get<CacheImage>(header.imagesOffset + i * sizeof(CacheImage));
            Image image{cached.address, view.string(cached.pathOffset)};
            uint64_t offset = locate(maps, image.address, sizeof(mach_header_64)); auto mh = view.get<mach_header_64>(offset);
            require(mh.magic == MH_MAGIC_64 && mh.cputype == CPU_TYPE_ARM64 && mh.cpusubtype == 0 && mh.filetype == MH_DYLIB,
                    "Unsupported native-support client architecture");
            size_t cursor = offset + sizeof(mh), end = cursor + mh.sizeofcmds;
            require(end <= view.size, "Invalid cached load commands");
            for (unsigned j = 0; j < mh.ncmds; ++j) {
                auto command = view.get<load_command>(cursor);
                require(command.cmdsize >= 8 && contains(end, cursor, command.cmdsize), "Invalid cached load command");
                if (command.cmd == LC_DYLD_EXPORTS_TRIE) {
                    auto exports = view.get<linkedit_data_command>(cursor); image.exportsOffset = exports.dataoff; image.exportsSize = exports.datasize;
                }
                cursor += command.cmdsize;
            }
            require(cursor == end, "Cached command count mismatch"); images.push_back(std::move(image));
        }
        PatchHeader patches = view.get<PatchHeader>(locate(maps, header.patchesAddress, header.patchesSize));
        require(patches.version == 4 && patches.locationVersion == 0 && patches.images.count == images.size() && patches.gotLocations.count == 0,
                "Unsupported native-support patch table");
        std::unordered_set<uintptr_t> locations;
        uint64_t reserveAddress = header.base + reserveOffset;
        for (unsigned library = 0; library < 3; ++library) {
            auto iterator = std::find_if(images.begin(), images.end(), [library](const Image &image) { return image.name == supportNames[library]; });
            require(iterator != images.end(), "Missing native-support placeholder"); const Image &image = *iterator;
            require(image.exportsSize == reserveSizes[library] && image.exportsOffset == locate(maps, reserveAddress, image.exportsSize),
                    "Native support export trie is not in its dedicated reserve");
            Dylib native; native.handle = dlopen(supportNames[library], RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD | RTLD_FIRST);
            require(native.handle, "Native support library is not already loaded");
            Trie trie{View{view.bytes + image.exportsOffset, image.exportsSize}, supportNames[library], native.handle, override, {}, {}, {}};
            trie.parse(0, "", 0);
            next.pages.push_back(Page{uintptr_t(actualBase + reserveAddress - header.base), trie.encode(image.exportsSize)});
            auto libraryIndex = size_t(iterator - images.begin()); auto entry = table<PatchImage>(view, maps, patches.images, libraryIndex);
            range(patches.exports, entry.exportsStart, entry.exportsCount); range(patches.clients, entry.clientsStart, entry.clientsCount);
            for (unsigned ci = 0; ci < entry.clientsCount; ++ci) {
                auto client = table<PatchClient>(view, maps, patches.clients, entry.clientsStart + ci);
                require(client.image < images.size(), "Native-support patch client outside images");
                range(patches.clientExports, client.exportsStart, client.exportsCount);
                for (unsigned ei = 0; ei < client.exportsCount; ++ei) {
                    auto exported = table<PatchClientExport>(view, maps, patches.clientExports, client.exportsStart + ei);
                    require(exported.exportIndex >= entry.exportsStart && exported.exportIndex - entry.exportsStart < entry.exportsCount,
                            "Native-support client references another library export");
                    auto target = table<PatchExport>(view, maps, patches.exports, exported.exportIndex);
                    require(!(target.nameAndKind >> 28) && target.nameAndKind < patches.names.count, "Invalid support patch export name");
                    std::string name = view.string(locate(maps, patches.names.address, patches.names.count) + target.nameAndKind,
                                                   patches.names.count - target.nameAndKind);
                    auto symbol = trie.exports.find(name);
                    require(symbol != trie.exports.end() && symbol->second.originalOffset == target.implementationOffset, "Patch export disagrees with placeholder");
                    if (!symbol->second.target) throw std::runtime_error(image.name + " missing required native symbol " + name);
                    range(patches.locations, exported.locationsStart, exported.locationsCount);
                    for (unsigned li = 0; li < exported.locationsCount; ++li) {
                        auto patch = table<PatchLocation>(view, maps, patches.locations, exported.locationsStart + li);
                        require(!(patch.metadata & 1), "Authenticated native-support cache patch is unsupported");
                        uint64_t address = actualBase + images[client.image].address - header.base + patch.offset;
                        require((address & 7) == 0 && locations.insert(address).second, "Unaligned or duplicate native-support pointer");
                        require(std::any_of(next.writable.begin(), next.writable.end(), [address](const Writable &map) {
                            return address >= map.address && contains(map.size, address - map.address, 8);
                        }), "Native-support pointer is not in writable guest data");
                        uint64_t addend = patch.metadata >> 9, tag = uint64_t(((patch.metadata >> 1) & 127) << 1) << 56;
                        uint64_t original = actualBase + image.address - header.base + target.implementationOffset + addend;
                        require(symbol->second.target <= UINT64_MAX - addend, "Native-support target addend overflow");
                        next.pointers.push_back(Pointer{uintptr_t(address), original | tag, (symbol->second.target + addend) | tag,
                                                        symbol->second.target, addend, tag});
                    }
                }
            }
            reserveAddress += reserveSizes[library];
        }
        require(next.pages.size() == 3 && next.pointers.size() == 16074, "Native-support reference inventory changed");
        next.prepared = true; state = std::move(next); return 0;
    } catch (const std::exception &failure) {
        if (error && errorSize) snprintf(error, errorSize, "%s", failure.what()); return ENOTSUP;
    }
}

extern "C" int LSBNativeSupportBindingsPrepared(void) { return state.prepared; }
extern "C" int LSBApplyNativeSupportBindings(const LSBNativeSupportMemory *memory) {
    if (!state.prepared || state.applied || !memory || !memory->map || !memory->protect) return EINVAL;
    // No allocations, native libSystem calls, TLS accesses, or exception paths after the simulator changes its state.
    for (const auto &pointer : state.pointers)
        if (*reinterpret_cast<const volatile uint64_t *>(pointer.address) != pointer.before) return EINVAL;
    for (const auto &page : state.pages) {
        void *result = memory->map(reinterpret_cast<void *>(page.address), page.contents.size(), PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (result != reinterpret_cast<void *>(page.address)) return EIO;
        auto *destination = reinterpret_cast<volatile uint8_t *>(page.address);
        for (size_t i = 0; i < page.contents.size(); ++i) destination[i] = page.contents[i];
        if (memory->protect(result, page.contents.size(), PROT_READ)) return EIO;
    }
    for (const auto &map : state.writable)
        if (memory->protect(reinterpret_cast<void *>(map.address), map.size, PROT_READ | PROT_WRITE)) return EIO;
    for (const auto &pointer : state.pointers) *reinterpret_cast<volatile uint64_t *>(pointer.address) = pointer.after;
    for (const auto &map : state.writable)
        if (memory->protect(reinterpret_cast<void *>(map.address), map.size, map.initial)) return EIO;
    state.applied = true; return 0;
}

extern "C" int LSBApplyNativeSupportInterposing(const mach_header *image, const LSBNativeSupportMemory *memory) {
    if (!state.prepared) return 0;
    if (!state.applied || !image || !memory || !memory->protect || image->magic != MH_MAGIC_64) return EINVAL;
    const auto *header = reinterpret_cast<const mach_header_64 *>(image);
    const uint8_t *commands = reinterpret_cast<const uint8_t *>(header + 1);
    const uint8_t *end = commands + header->sizeofcmds;
    if (header->sizeofcmds > 1024 * 1024 || header->ncmds > 16384) return EINVAL;
    uintptr_t slide = 0;
    bool foundText = false, writable = false;
    const uint8_t *cursor = commands;
    for (unsigned i = 0; i < header->ncmds; ++i) {
        if (size_t(end - cursor) < sizeof(load_command)) return EINVAL;
        const auto *command = reinterpret_cast<const load_command *>(cursor);
        if (command->cmdsize < 8 || command->cmdsize > size_t(end - cursor)) return EINVAL;
        if (command->cmd == LC_SEGMENT_64) {
            if (command->cmdsize < sizeof(segment_command_64)) return EINVAL;
            const auto *segment = reinterpret_cast<const segment_command_64 *>(cursor);
            if (segment->segname[0] == '_' && segment->segname[1] == '_' && segment->segname[2] == 'T' && segment->segname[3] == 'E' &&
                segment->segname[4] == 'X' && segment->segname[5] == 'T' && segment->segname[6] == 0) {
                if (foundText) return EINVAL;
                slide = reinterpret_cast<uintptr_t>(image) - segment->vmaddr; foundText = true;
            }
        }
        cursor += command->cmdsize;
    }
    if (!foundText || cursor != end) return EINVAL;
    cursor = commands;
    for (unsigned i = 0; i < header->ncmds; ++i) {
        const auto *command = reinterpret_cast<const load_command *>(cursor);
        if (command->cmd == LC_SEGMENT_64) {
            const auto *segment = reinterpret_cast<const segment_command_64 *>(cursor);
            if (segment->nsects > (command->cmdsize - sizeof(*segment)) / sizeof(section_64)) return EINVAL;
            const auto *sections = reinterpret_cast<const section_64 *>(segment + 1);
            for (unsigned s = 0; s < segment->nsects; ++s) {
                const section_64 &section = sections[s];
                // dyld fd8d0c4 UnsafeHeader::forEachInterposingSection also accepts named sections: clang emits our tuples as S_REGULAR.
                bool named = matchesName(section.sectname, "__interpose") &&
                    (matchesName(segment->segname, "__DATA", true) || matchesName(segment->segname, "__AUTH", true));
                if ((section.flags & SECTION_TYPE) != S_INTERPOSING && !named) continue;
                if (section.size % 16 || section.addr < segment->vmaddr || !contains(segment->vmsize, section.addr - segment->vmaddr, section.size))
                    return EINVAL;
                const auto *tuples = reinterpret_cast<const uintptr_t *>(section.addr + slide);
                for (size_t ti = 0; ti < section.size / 16; ++ti) {
                    uintptr_t replacement = tuples[ti * 2], replacee = tuples[ti * 2 + 1];
                    if (!replacement || !replacee || replacement == replacee) continue;
                    for (auto &pointer : state.pointers) {
                        if (pointer.target != replacee) continue;
                        if (replacement > UINTPTR_MAX - pointer.addend) return EINVAL;
                        if (!writable) {
                            for (const auto &map : state.writable)
                                if (memory->protect(reinterpret_cast<void *>(map.address), map.size, PROT_READ | PROT_WRITE)) return EIO;
                            writable = true;
                        }
                        pointer.target = replacement; pointer.after = (replacement + pointer.addend) | pointer.tag;
                        *reinterpret_cast<volatile uint64_t *>(pointer.address) = pointer.after;
                    }
                }
            }
        }
        cursor += command->cmdsize;
    }
    if (writable) {
        for (const auto &map : state.writable)
            if (memory->protect(reinterpret_cast<void *>(map.address), map.size, map.initial)) return EIO;
    }
    return 0;
}
