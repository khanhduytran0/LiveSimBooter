#include "../Guest/Shared/Fork/ForkBashEarlyHeap.h"
#include <assert.h>
#include <sys/mman.h>
#include <sys/wait.h>

// No code from the fixture executes. The two VM leaves model the pinned image and its read-only chained import table.
static void testProcursusHeapImports(void) {
    size_t bytes = ProcursusBashGOT + vm_page_size;
    uint8_t *image = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(image != MAP_FAILED);
    struct mach_header_64 *mh = (void *)image;
    *mh = (struct mach_header_64){.magic = MH_MAGIC_64, .cputype = CPU_TYPE_ARM64, .filetype = MH_EXECUTE,
                                  .ncmds = 1, .sizeofcmds = sizeof(struct uuid_command)};
    struct uuid_command *uuid = (void *)(mh + 1);
    *uuid = (struct uuid_command){LC_UUID, sizeof(*uuid), {5,0x79,0x98,11,0x28,0x5b,0x3e,0x49,0xa3,0xb1,0xff,0x5e,0xa2,9,0x9c,0x97}};
    *(uint32_t *)(image + ProcursusBashForkReturn - 4) = 0x940181d9;
    const uint32_t stub[] = {0x900000f0, 0xf9413a10, 0xd61f0200};
    memcpy(image + ProcursusBashGetpidStub, stub, sizeof(stub));
    uintptr_t *slot = (void *)(image + 0xa03e8);
    *slot = 7;
    assert(!mprotect(image + ProcursusBashGOT, vm_page_size, PROT_READ));
    uuid->uuid[0] ^= 1;
    assert(bashEarlyBindAllocations((uintptr_t)image) == ENOEXEC && *slot == 7);
    uuid->uuid[0] ^= 1;
    assert(!bashEarlyBindAllocations((uintptr_t)image) && *slot == (uintptr_t)&bashEarlyMalloc);
    assert(!*(uintptr_t *)(image + ProcursusBashGetpidSlot)); // An adjacent non-allocator import stays intact.
    vm_region_submap_info_data_64_t info = {};
    assert(forkTrialLeaf((uintptr_t)slot, sizeof(*slot), &info) == KERN_SUCCESS && info.protection == VM_PROT_READ);
    assert(!munmap(image, bytes));
}

int main(void) {
    testProcursusHeapImports();
    BashHeap *heap = mmap(NULL, BashEarlyHeapBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(heap != MAP_FAILED);
    bashHeapInit(heap);
    bashEarlyHeap = heap;
    assert(bashHeapValid(heap) && !bashHeapOwns(NULL));

    sigset_t before, after;
    assert(!sigprocmask(0, NULL, &before));
    char *a = bashEarlyStrdup("parent allocation");
    wchar_t *b = bashEarlyWcsdup(L"owned wide string");
    char *c = NULL, *cwd = bashEarlyGetcwd(NULL, 0);
    assert(a && b && cwd && cwd[0] == '/' && bashHeapOwns(cwd));
    assert(bashEarlyAsprintf(&c, "a%cb", 0) == 3 && !memcmp(c, "a\0b", 4));
    assert(bashHeapOwns(a) && !((uintptr_t)a & 15) && bashHeapOwns(b) && !((uintptr_t)b & 15) && bashHeapValid(heap));
    assert(!bashEarlyRealloc(a, SIZE_MAX) && errno == ENOMEM && !strcmp(a, "parent allocation"));
    assert(!sigprocmask(0, NULL, &after) && after == before);

    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        // Same allocator base and free state as a resumed guest. Only its own CoW copy is changed.
        bashEarlyHeapChild = true;
        assert(bashHeapValid(heap));
        bashEarlyFree(b);
        char *grown = bashEarlyRealloc(a, 16384);
        assert(grown && !strcmp(grown, "parent allocation"));
        strcpy(grown, "child allocation");
        assert(bashHeapValid(heap));
        _exit(37);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 37);
    assert(!strcmp(a, "parent allocation") && !wcscmp(b, L"owned wide string") && bashHeapValid(heap));
    bashEarlyFree(a);
    bashEarlyFree(b);
    bashEarlyFree(c);
    bashEarlyFree(cwd);
    assert(bashHeapValid(heap));

    void *slots[37] = {};
    for (unsigned i = 0; i < 2000; ++i) {
        unsigned slot = (i * 17) % 37;
        if (i % 3 == 0) {
            bashEarlyFree(slots[slot]);
            slots[slot] = NULL;
        } else {
            slots[slot] = bashEarlyRealloc(slots[slot], 1 + (i * 137u % 4000));
            assert(slots[slot]);
        }
        assert(bashHeapValid(heap));
    }
    for (unsigned i = 0; i < 37; ++i) bashEarlyFree(slots[i]);
    size_t largest = BashEarlyHeapBytes - sizeof(*heap) - sizeof(BashHeapBlock);
    void *whole = bashEarlyMalloc(largest);
    assert(whole && !bashEarlyMalloc(1) && errno == ENOMEM && bashHeapValid(heap));
    bashEarlyFree(whole);
    assert(bashHeapValid(heap));
    heap->search = BashEarlyHeapBytes - 1;
    assert(!bashHeapValid(heap));
    assert(!munmap(heap, BashEarlyHeapBytes));
    puts("Bash-owned heap split/coalesce, bounds, signal mask, and private child state passed");
}
