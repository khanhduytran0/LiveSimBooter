#include "ForkAllocator.h"
#include "../ProcessSupport/PrivateMachVM.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

extern void _malloc_fork_prepare(void);
extern void _malloc_fork_parent(void);

static int covered(void *ctx, uint64_t address, uint64_t size) {
    const ForkMallocPages *pages = ctx;
    for (unsigned i = 0; i < pages->count; ++i) {
        LSBForkWireRange r = pages->ranges[i];
        if (address >= r.start && size <= r.size && address - r.start <= r.size - size) return 0;
    }
    return EFAULT;
}

static int stockCapture(void) {
    char *tiny = malloc(32), *large = malloc(262144);
    if (!tiny || !large) return 1;
    memcpy(tiny, "tiny-canary", 12);
    memcpy(large, "large-canary", 13);
    ForkAllocatorState state;
    ForkMallocPages pages = {};
    int error = forkAllocatorCapture(&state, forkMallocAddPages, &pages);
    if (!error) {
        _malloc_fork_prepare();
        error = forkAllocatorRecheck(&state, covered, &pages);
        _malloc_fork_parent();
    }
    size_t bytes = 0;
    for (unsigned i = 0; i < pages.count; ++i) bytes += pages.ranges[i].size;
    printf("allocator capture errno=%d zones=%u ranges=%u bytes=%zu tiny/large covered=%d/%d sizes=%zu/%zu\n",
           error, state.count, pages.count, bytes, !covered(&pages, (uintptr_t)tiny, 32), !covered(&pages, (uintptr_t)large, 262144),
           malloc_size(tiny), malloc_size(large));
    bool passed = !error && !covered(&pages, (uintptr_t)tiny, 32) && !covered(&pages, (uintptr_t)large, 262144) &&
        !strcmp(tiny, "tiny-canary") && !strcmp(large, "large-canary");
    free(tiny);
    free(large);
    return !passed;
}

// Isolated guest control for the registry transition. A local zone with real scalable allocator metadata stands in for a copied zone.
static int promoteControl(void) {
    ForkAllocatorState state = {.count = 1};
    int error = forkMallocImage(&state.image);
    if (!error) error = forkMallocProcessCookie(state.image, &state.regionCookie);
    if (error) return error;
    char *original = malloc(64);
    malloc_zone_t *other = malloc_create_zone(0, 0);
    if (!original || !forkMallocScalable(state.image, other)) return 1;
    char *inherited = malloc_zone_malloc(other, 128);
    if (!inherited) return 2;
    memcpy(inherited, "before-fork", sizeof("before-fork"));
    state.zones[0] = (uintptr_t)other;
    malloc_zone_unregister(other); // No other caller has had a reference to this test-created secondary zone.
    other->introspect->force_lock(other);
    ForkMallocPages pages = {};
    error = forkMallocWalk((uintptr_t)other, forkMallocAddPages, &pages, NULL);
    // Leave the test zone locked as it would be in a parent's prepared snapshot.
    if (!error) error = forkAllocatorPromote(&state, covered, &pages);
    printf("promotion errno=%d", error);
    if (error) { puts(""); return 3; }
    malloc_zone_t *primary = malloc_default_zone(), *oldOwner = malloc_zone_from_ptr(original);
    char *fresh = malloc(256);
    bool inheritedDefault = malloc_zone_from_ptr(inherited) == primary, freshDefault = fresh && malloc_zone_from_ptr(fresh) == primary;
    bool oldSecondary = oldOwner && oldOwner != primary && oldOwner != other && malloc_size(original) >= 64;
    printf(" inherited-default=%d fresh-default=%d previous-secondary=%d\n", inheritedDefault, freshDefault, oldSecondary);
    if (!inheritedDefault || !freshDefault || !oldSecondary || malloc_size(inherited) < 128) return 4;
    inherited = malloc_zone_realloc(primary, inherited, 4096);
    if (!inherited || strcmp(inherited, "before-fork")) return 5;
    malloc_zone_free(primary, inherited);
    free(original);
    free(fresh);
    return 0;
}

static void testRackCoverage(void) {
    void *mem = NULL;
    assert(!posix_memalign(&mem, ForkMallocPage, ForkScalableZoneBytes));
    ForkScalableZone *s = mem;
    memset(s, 0, ForkScalableZoneBytes);
    size_t extents[] = {1u << 20, 8u << 20, 128u << 20}, offsets[] = {0x407c, 0x81fc, 0x7ffc};
    mach_vm_address_t regions[3] = {};
    for (unsigned i = 0; i != 3; ++i) {
        // Reserve aligned VA; only the page containing each marker is touched.
        assert(!mach_vm_map(mach_task_self(), &regions[i], extents[i], extents[i] - 1, VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, false,
                            VM_PROT_DEFAULT, VM_PROT_ALL, VM_INHERIT_COPY));
        *(uint32_t *)(uintptr_t)(regions[i] + offsets[i]) = 0x1234;
        ForkMallocRack *r = &s->racks[i];
        r->current = &r->generations[0];
        r->current->capacity = 64;
        r->current->next = &r->generations[1];
        r->current->slots = r->initial;
        r->type = i + 1;
        r->magazinesCount = 2;
        r->magazines = 0x2000000 + 0x40000 * i + ForkMagazineBytes;
        r->initial[17] = regions[i];
        r->initial[18] = UINTPTR_MAX; // Reclaimed hash entry.
    }
    ForkLargeEntry large[] = {{0, 0, 0}, {0x50000000, 3 * ForkMallocPage, 0}};
    s->largeCapacity = 2;
    s->largeObjects = 1;
    s->large = large;
    ForkMallocPages pages = {};
    assert(!forkMallocWalk((uintptr_t)s, forkMallocAddPages, &pages, NULL));
    assert(!covered(&pages, 0x2000000 - ForkMallocPage, 3 * ForkMallocPage)); // Depot, magazines and both guard pages.
    assert(!covered(&pages, regions[0], extents[0]) && !covered(&pages, regions[1], extents[1]));
    assert(!covered(&pages, large[1].address, large[1].size));
    assert(!forkMallocWalk((uintptr_t)s, covered, &pages, NULL));
    large[1].size += ForkMallocPage;
    assert(forkMallocWalk((uintptr_t)s, covered, &pages, NULL) == EFAULT); // A new allocation outside the offered checkpoint.
    large[1].size -= ForkMallocPage;
    s->racks[0].current = &s->racks[1].generations[0];
    assert(forkMallocWalk((uintptr_t)s, covered, &pages, NULL) == ENOTSUP);
    s->racks[0].current = &s->racks[0].generations[0];
    s->cacheDepth = 1;
    assert(forkMallocWalk((uintptr_t)s, covered, &pages, NULL) == ENOTSUP);
    s->cacheDepth = 0;
    s->mediumEnabled = 1;
    ForkMallocCookies check = {0x1234, 0x1234}, adapt = {0x1234, 0x4567};
    // Add the medium region to the test's explicit coverage inventory without applying the broker's byte limit to this synthetic check.
    ForkMallocPages small = {};
    s->racks[2].initial[17] = 0;
    assert(!forkMallocWalk((uintptr_t)s, forkMallocAddPages, &small, &check));
    s->racks[2].initial[17] = regions[2];
    small.ranges[small.count++] = (LSBForkWireRange){regions[2], extents[2]};
    assert(!forkMallocWalk((uintptr_t)s, covered, &small, &check));
    uint32_t *last = (uint32_t *)(uintptr_t)(regions[2] + offsets[2]);
    *last = 0xbeef;
    assert(forkMallocWalk((uintptr_t)s, covered, &small, &check) == EINVAL);
    assert(*(uint32_t *)(uintptr_t)(regions[0] + offsets[0]) == check.from); // Read-only verification cannot hide corruption.
    *last = check.from;
    assert(!forkMallocWalk((uintptr_t)s, covered, &small, &adapt));
    for (unsigned i = 0; i < 3; ++i) {
        assert(*(uint32_t *)(uintptr_t)(regions[i] + offsets[i]) == adapt.to);
        assert(!mach_vm_deallocate(mach_task_self(), regions[i], extents[i]));
    }
    free(s);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--capture")) return stockCapture();
    if (argc == 2 && !strcmp(argv[1], "--promote")) return promoteControl();
    ForkMallocPages p = {};
    assert(!forkMallocAddPages(&p, 0x4000, 1));
    assert(!forkMallocAddPages(&p, 0xc010, 1));
    assert(p.count == 2 && !forkMallocAddPages(&p, 0x6000, 0x3800));
    assert(p.count == 1 && p.ranges[0].start == 0x4000 && p.ranges[0].size == 0xc000);
    assert(forkMallocAddPages(&p, UINT64_MAX - 8, 128) == E2BIG);
    assert(forkMallocAddPages(&p, 0x1000000, LSB_MAX_FORK_BYTES) == E2BIG && p.count == 1);
    testRackCoverage();
    puts("Allocator snapshot page union, rack coverage and changed-zone rejection passed");
    return 0;
}
