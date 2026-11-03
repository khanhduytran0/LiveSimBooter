// Private allocator for the opt-in, noninteractive Bash fork trial. Only the pinned executable's imports are rebound.
// A live object and its allocation metadata have the same addresses on both sides of the CoW snapshot.
#pragma once
#include "ForkBashState.h"
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>

#define BashEarlyHeapOffset UINT64_C(0x3b800000)
enum { BashEarlyHeapBytes = 4 * 1024 * 1024, BashHeapMagic = 0x42414831, BashHeapLive = 1, BashHeapAlign = 16 };

typedef struct { uint32_t magic, capacity, search, version; } BashHeap;
typedef struct { uint32_t spanAndLive, previousSpan; uint64_t ownerOffset; } BashHeapBlock;
_Static_assert(sizeof(BashHeap) == BashHeapAlign && sizeof(BashHeapBlock) == BashHeapAlign, "Bash allocator alignment");

static BashHeap *bashEarlyHeap;
static bool bashEarlyHeapChild;

static inline uintptr_t bashEarlyHeapBase(void) { return (uintptr_t)bashEarlyHeap; }

static inline BashHeapBlock *bashHeapBlock(BashHeap *heap, uint32_t offset) { return (void *)((char *)heap + offset); }

static inline uint32_t bashHeapSpan(const BashHeapBlock *block) { return block->spanAndLive & ~BashHeapLive; }

static inline void bashHeapInit(BashHeap *heap) {
    *heap = (BashHeap){BashHeapMagic, BashEarlyHeapBytes, sizeof(*heap), 1};
    *bashHeapBlock(heap, sizeof(*heap)) = (BashHeapBlock){.spanAndLive = BashEarlyHeapBytes - sizeof(*heap)};
}

static inline bool bashHeapValid(BashHeap *heap) {
    if (heap->magic != BashHeapMagic || heap->version != 1 || heap->capacity != BashEarlyHeapBytes) return false;
    uint32_t previous = 0;
    bool foundSearch = false;
    for (uint32_t i = sizeof(*heap); i < heap->capacity;) {
        BashHeapBlock *block = bashHeapBlock(heap, i);
        uint32_t span = bashHeapSpan(block);
        if (span < 2 * BashHeapAlign || span % BashHeapAlign || span > heap->capacity - i || block->previousSpan != previous ||
            block->ownerOffset != ((block->spanAndLive & BashHeapLive) ? i : 0)) return false;
        foundSearch |= i == heap->search;
        previous = span;
        i += span;
    }
    return foundSearch;
}

static inline bool bashHeapOwns(const void *pointer) {
    uintptr_t address = (uintptr_t)pointer, start = bashEarlyHeapBase();
    return start && address >= start && address - start < BashEarlyHeapBytes;
}

static inline BashHeapBlock *bashHeapAllocation(BashHeap *heap, const void *pointer) {
    uintptr_t distance = (uintptr_t)pointer - (uintptr_t)heap;
    if (distance < sizeof(*heap) + sizeof(BashHeapBlock) || distance >= heap->capacity || distance % BashHeapAlign) return NULL;
    uint32_t offset = (uint32_t)distance - sizeof(BashHeapBlock);
    BashHeapBlock *block = bashHeapBlock(heap, offset);
    uint32_t span = bashHeapSpan(block);
    return (block->spanAndLive & BashHeapLive) && block->ownerOffset == offset && span >= 2 * BashHeapAlign &&
           !(span % BashHeapAlign) && span <= heap->capacity - offset ? block : NULL;
}

// Only Bash's single command interpreter uses this allocator. A pending shell signal must see a complete split or coalesce.
static inline void bashHeapBlockSignals(sigset_t *previous) {
    sigset_t all;
    sigfillset(&all);
    if (sigprocmask(SIG_BLOCK, &all, previous) < 0) _exit(125);
}

static inline void bashHeapRestoreSignals(const sigset_t *previous) {
    if (sigprocmask(SIG_SETMASK, previous, NULL) < 0) _exit(125);
}

static inline void *bashHeapAlloc(BashHeap *heap, size_t bytes) {
    if (bytes > heap->capacity - sizeof(*heap) - sizeof(BashHeapBlock)) return NULL;
    if (!bytes) bytes = 1;
    uint32_t span = (uint32_t)((bytes + BashHeapAlign - 1) & ~(size_t)(BashHeapAlign - 1)) + sizeof(BashHeapBlock);
    uint32_t i = heap->search;
    do {
        BashHeapBlock *block = bashHeapBlock(heap, i);
        uint32_t original = bashHeapSpan(block), next = i + original;
        if (!(block->spanAndLive & BashHeapLive) && original >= span) {
            if (original - span >= 2 * BashHeapAlign) {
                BashHeapBlock *tail = bashHeapBlock(heap, i + span);
                *tail = (BashHeapBlock){.spanAndLive = original - span, .previousSpan = span};
                if (next < heap->capacity) bashHeapBlock(heap, next)->previousSpan = original - span;
                original = span;
                next = i + span;
            }
            block->spanAndLive = original | BashHeapLive;
            block->ownerOffset = i;
            heap->search = next == heap->capacity ? sizeof(*heap) : next;
            return block + 1;
        }
        i = next == heap->capacity ? sizeof(*heap) : next;
    } while (i != heap->search);
    return NULL;
}

static inline void bashHeapRelease(BashHeap *heap, BashHeapBlock *block) {
    uint32_t offset = (uint32_t)((char *)block - (char *)heap);
    uint32_t right = offset + bashHeapSpan(block);
    block->ownerOffset = 0;
    if (right < heap->capacity) {
        BashHeapBlock *next = bashHeapBlock(heap, right);
        if (!(next->spanAndLive & BashHeapLive)) right += bashHeapSpan(next);
    }
    if (block->previousSpan) {
        BashHeapBlock *previous = bashHeapBlock(heap, offset - block->previousSpan);
        if (!(previous->spanAndLive & BashHeapLive)) {
            offset -= block->previousSpan;
            block = previous;
        }
    }
    block->spanAndLive = right - offset;
    if (right < heap->capacity) bashHeapBlock(heap, right)->previousSpan = right - offset;
    heap->search = offset;
}

static inline void *bashEarlyMalloc(size_t size) {
    sigset_t previous;
    bashHeapBlockSignals(&previous);
    void *result = bashHeapAlloc(bashEarlyHeap, size);
    bashHeapRestoreSignals(&previous);
    if (!result) errno = ENOMEM;
    return result;
}

static inline void bashEarlyFree(void *pointer) {
    if (!pointer) return;
    if (!bashHeapOwns(pointer)) {
        // A libc result in the original process is still owned by libc. Its address cannot be freed by another process.
        if (bashEarlyHeapChild) _exit(125);
        free(pointer);
        return;
    }
    sigset_t previous;
    bashHeapBlockSignals(&previous);
    BashHeapBlock *block = bashHeapAllocation(bashEarlyHeap, pointer);
    if (!block) _exit(125);
    bashHeapRelease(bashEarlyHeap, block);
    bashHeapRestoreSignals(&previous);
}

static inline void *bashEarlyRealloc(void *pointer, size_t size) {
    if (!pointer) return bashEarlyMalloc(size);
    if (!bashHeapOwns(pointer)) {
        if (bashEarlyHeapChild) _exit(125);
        return realloc(pointer, size);
    }
    sigset_t previous;
    bashHeapBlockSignals(&previous);
    BashHeapBlock *block = bashHeapAllocation(bashEarlyHeap, pointer);
    if (!block) _exit(125);
    size_t available = bashHeapSpan(block) - sizeof(*block);
    void *result = pointer;
    if (size > available || !size) {
        result = bashHeapAlloc(bashEarlyHeap, size);
        if (result) {
            if (size) memcpy(result, pointer, available);
            bashHeapRelease(bashEarlyHeap, block);
        }
    }
    bashHeapRestoreSignals(&previous);
    if (!result) errno = ENOMEM;
    return result;
}

static inline void *bashHeapDuplicate(const void *source, size_t length) {
    void *result = bashEarlyMalloc(length);
    if (result) memcpy(result, source, length);
    return result;
}

static inline char *bashEarlyStrdup(const char *source) { return bashHeapDuplicate(source, strlen(source) + 1); }
static inline wchar_t *bashEarlyWcsdup(const wchar_t *source) { return bashHeapDuplicate(source, (wcslen(source) + 1) * sizeof(*source)); }

static inline char *bashEarlyGetcwd(char *destination, size_t length) {
    if (destination) return getcwd(destination, length);
    char *native = getcwd(NULL, length);
    if (!native) return NULL;
    size_t used = strlen(native) + 1;
    char *result = bashEarlyMalloc(length > used ? length : used);
    if (result) memcpy(result, native, used);
    free(native);
    return result;
}

static inline int bashEarlyAsprintf(char **result, const char *format, ...) {
    char *native = NULL;
    va_list args;
    va_start(args, format);
    int length = vasprintf(&native, format, args);
    va_end(args);
    *result = NULL;
    if (length < 0) return -1;
    *result = bashHeapDuplicate(native, (size_t)length + 1);
    free(native);
    return *result ? length : -1;
}

static inline int bashEarlyBindAllocations(uintptr_t header) {
    // llvm-objdump --macho --indirect-symbols on this UUID. xmalloc+0x18 and xrealloc+0x20 call these imports.
    static const uint32_t slots[] = {0x9c0d8, 0x9c200, 0x9c230, 0x9c308, 0x9c3f0, 0x9c4e0, 0x9c5e0};
    const uintptr_t targets[] = {(uintptr_t)&bashEarlyAsprintf, (uintptr_t)&bashEarlyFree, (uintptr_t)&bashEarlyGetcwd, (uintptr_t)&bashEarlyMalloc,
                                 (uintptr_t)&bashEarlyRealloc, (uintptr_t)&bashEarlyStrdup, (uintptr_t)&bashEarlyWcsdup};
    _Static_assert(sizeof(slots) / sizeof(*slots) == sizeof(targets) / sizeof(*targets), "Bash allocator slots");
    vm_region_submap_info_data_64_t info = {};
    if (!forkTrialBashImage(header) || forkTrialLeaf(header + slots[0], slots[6] - slots[0] + sizeof(uintptr_t), &info) != KERN_SUCCESS ||
        info.inheritance != VM_INHERIT_COPY || info.protection != (VM_PROT_READ | VM_PROT_WRITE)) return ENOEXEC;
    // Before main in the parent or before resumption in a single-threaded observer. All slots share this already-writable leaf.
    for (size_t i = 0; i < sizeof(slots) / sizeof(*slots); ++i) *(uintptr_t *)(header + slots[i]) = targets[i];
    return 0;
}

static inline int bashEarlyInstallHeap(uintptr_t header) {
    if (bashEarlyHeap || !forkTrialBashImage(header) || header > UINTPTR_MAX - BashEarlyHeapOffset - BashEarlyHeapBytes) return ENOTSUP;
    mach_vm_address_t address = header + BashEarlyHeapOffset;
    if (mach_vm_allocate(mach_task_self(), &address, BashEarlyHeapBytes, VM_FLAGS_FIXED) != KERN_SUCCESS) return EADDRINUSE;
    bashEarlyHeap = (void *)(uintptr_t)address;
    bashHeapInit(bashEarlyHeap);
    int error = bashEarlyBindAllocations(header);
    if (error) {
        bashEarlyHeap = NULL;
        mach_vm_deallocate(mach_task_self(), address, BashEarlyHeapBytes);
    }
    return error;
}

static inline int bashEarlyAdoptHeap(uintptr_t header, uintptr_t base) {
    vm_region_submap_info_data_64_t info = {};
    if (!forkTrialBashImage(header) || header > UINTPTR_MAX - BashEarlyHeapBytes - BashEarlyHeapOffset || base != header + BashEarlyHeapOffset ||
        forkTrialLeaf(base, BashEarlyHeapBytes, &info) != KERN_SUCCESS || info.protection != (VM_PROT_READ | VM_PROT_WRITE) ||
        info.inheritance != VM_INHERIT_COPY || !bashHeapValid((void *)base)) return ENOTSUP;
    int error = bashEarlyBindAllocations(header);
    if (!error) {
        bashEarlyHeap = (void *)base;
        bashEarlyHeapChild = true;
    }
    return error;
}
