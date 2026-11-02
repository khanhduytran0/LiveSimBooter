#include "../LiveSimBooter/NativeSupportBindings.cpp"
#include <cassert>

namespace {
uintptr_t overrideTest(const char *, const char *name, uintptr_t) { return !strcmp(name, "_test") ? 0x123456789abc : 0; }
void *denyMap(void *, size_t, int, int, int, off_t) { return MAP_FAILED; }
int denyProtect(void *, size_t, int) { return -1; }
const LSBNativeSupportMemory operations = {mmap, mprotect};
struct Fixture {
    uint8_t *memory; size_t page;
    Fixture() {
        state = State{}; page = size_t(getpagesize());
        memory = static_cast<uint8_t *>(mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
        assert(memory != MAP_FAILED);
        *cell() = 0x1110;
        assert(mprotect(memory + page, page, PROT_READ) == 0);
        state.pages.push_back(Page{reinterpret_cast<uintptr_t>(memory), std::vector<uint8_t>(page, 0x2a)});
        state.writable.push_back(Writable{reinterpret_cast<uintptr_t>(memory + page), page, PROT_READ});
        state.pointers.push_back(Pointer{reinterpret_cast<uintptr_t>(cell()), 0x1110, 0x1234, 0x1234, 0, 0});
        state.prepared = true;
    }
    uint64_t *cell() const { return reinterpret_cast<uint64_t *>(memory + page); }
    ~Fixture() { state = State{}; munmap(memory, page * 2); }
};
struct InterposerImage {
    mach_header_64 header;
    segment_command_64 text;
    segment_command_64 data;
    section_64 section;
    uintptr_t tuples[2];
    InterposerImage(uintptr_t replacement, uintptr_t replacee) : header{}, text{}, data{}, section{}, tuples{replacement, replacee} {
        header.magic = MH_MAGIC_64; header.ncmds = 2; header.sizeofcmds = sizeof(text) + sizeof(data) + sizeof(section);
        text.cmd = LC_SEGMENT_64; text.cmdsize = sizeof(text); strcpy(text.segname, "__TEXT"); text.vmaddr = reinterpret_cast<uintptr_t>(this);
        text.vmsize = sizeof(*this);
        data.cmd = LC_SEGMENT_64; data.cmdsize = sizeof(data) + sizeof(section); strcpy(data.segname, "__DATA");
        data.vmaddr = reinterpret_cast<uintptr_t>(tuples); data.vmsize = sizeof(tuples); data.nsects = 1;
        section.addr = data.vmaddr; section.size = sizeof(tuples); strcpy(section.sectname, "__interpose");
        // Match clang's actual __DATA,__interpose section in libLiveSimSpawn: S_REGULAR, not S_INTERPOSING.
        section.flags = S_REGULAR;
    }
    const mach_header *mh() const { return reinterpret_cast<const mach_header *>(&header); }
};
}

int main() {
    // Existing compressed edges survive rebuilding. A missing native symbol becomes an absent terminal, not an absolute zero success.
    std::vector<uint8_t> input = {0, 1, '_', 't', 'e', 's', 't', 0, 9, 3, 0, 0x80, 1, 0};
    Trie trie{View{input.data(), input.size()}, "test", RTLD_DEFAULT, overrideTest, {}, {}, {}};
    assert(trie.parse(0, "", 0) == 0);
    auto output = trie.encode(128);
    View view{output.data(), output.size()}; size_t cursor = 8; uint64_t child = uleb(view, cursor); cursor = size_t(child);
    uint64_t length = uleb(view, cursor); assert(length > 0); assert(uleb(view, cursor) == 2); assert(uleb(view, cursor) == 0x123456789abc);
    assert(trie.exports.at("_test").originalOffset == 128);
    Trie missing{View{input.data(), input.size()}, "test", RTLD_DEFAULT, nullptr, {}, {}, {}};
    missing.parse(0, "", 0); auto missingOutput = missing.encode(128); view = {missingOutput.data(), missingOutput.size()};
    cursor = 8; child = uleb(view, cursor); cursor = size_t(child); assert(uleb(view, cursor) == 0);
    bool rejected = false;
    try { trie.encode(4); } catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    {
        Fixture fixture;
        assert(LSBApplyNativeSupportBindings(&operations) == 0);
        assert(*fixture.cell() == 0x1234 && fixture.memory[0] == 0x2a);
        assert(LSBApplyNativeSupportBindings(&operations) == EINVAL);
        InterposerImage first(0x5678, 0x1234), second(0x9999, 0x1234), chained(0xaaaa, 0x5678);
        assert(LSBApplyNativeSupportInterposing(first.mh(), &operations) == 0 && *fixture.cell() == 0x5678);
        assert(LSBApplyNativeSupportInterposing(second.mh(), &operations) == 0 && *fixture.cell() == 0x5678);
        assert(LSBApplyNativeSupportInterposing(chained.mh(), &operations) == 0 && *fixture.cell() == 0xaaaa);
        first.section.size = 15;
        assert(LSBApplyNativeSupportInterposing(first.mh(), &operations) == EINVAL);
    }
    for (const char *segment : {"__DATA", "__DATA_CONST", "__AUTH", "__AUTH_CONST"}) {
        Fixture fixture;
        assert(LSBApplyNativeSupportBindings(&operations) == 0);
        InterposerImage image(0x5678, 0x1234);
        strcpy(image.data.segname, segment);
        assert(LSBApplyNativeSupportInterposing(image.mh(), &operations) == 0 && *fixture.cell() == 0x5678);
    }
    {
        Fixture fixture;
        assert(LSBApplyNativeSupportBindings(&operations) == 0);
        InterposerImage image(0x5678, 0x1234);
        strcpy(image.section.sectname, "__interposer");
        assert(LSBApplyNativeSupportInterposing(image.mh(), &operations) == 0 && *fixture.cell() == 0x1234);
        strcpy(image.section.sectname, "__interpose"); strcpy(image.data.segname, "__CUSTOM");
        assert(LSBApplyNativeSupportInterposing(image.mh(), &operations) == 0 && *fixture.cell() == 0x1234);
        image.section.flags = S_INTERPOSING;
        assert(LSBApplyNativeSupportInterposing(image.mh(), &operations) == 0 && *fixture.cell() == 0x5678);
    }
    {
        Fixture fixture; state.pointers[0].before = 99;
        assert(LSBApplyNativeSupportBindings(&operations) == EINVAL && *fixture.cell() == 0x1110);
    }
    {
        Fixture fixture; LSBNativeSupportMemory failing{denyMap, mprotect};
        assert(LSBApplyNativeSupportBindings(&failing) == EIO && *fixture.cell() == 0x1110);
    }
    {
        Fixture fixture; LSBNativeSupportMemory failing{mmap, denyProtect};
        assert(LSBApplyNativeSupportBindings(&failing) == EIO && *fixture.cell() == 0x1110);
    }
    assert(!LSBNativeSupportBindingsPrepared());
    puts("NativeSupportBindings: absolute exports, missing exports, bounded trie, private remap, pointer validation and interpose replay passed");
}
