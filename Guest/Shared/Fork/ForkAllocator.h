// Stock arm64 23A343 libsystem_malloc. No application symbols or allocation sizes are inferred from pointers.
#pragma once
#include "ForkAllocatorState.h"
#include "../../../ProcessSupport/SpawnTransport.h"
#include <dlfcn.h>
#include <errno.h>
#include <mach-o/loader.h>
#include <malloc/malloc.h>
#include <stdbool.h>
#include <string.h>

extern void _malloc_fork_prepare(void);
extern void _malloc_fork_parent(void);

typedef int (*ForkAllocatorVisit)(void *, uint64_t, uint64_t); // Capture emits page spans; recheck/adopt ask for coverage of raw byte ranges.

// Layout from the pinned _szone_ptr_in_use_enumerator and the tiny/small/medium walkers. _rack_init stores
// magazines immediately AFTER the depot magazine. _create_scalable_szone puts three identical racks 0x300 bytes apart.
typedef struct {
    size_t capacity, shift;
    uintptr_t *slots;
    void *next;
} ForkRegionGeneration;
typedef struct {
    uint32_t lock, type;
    size_t regions, deallocated;
    ForkRegionGeneration *current, generations[2];
    uintptr_t initial[64];
    int32_t magazinesCount;
    uint32_t mask, maskShift, flags;
    uintptr_t magazines;
    uint8_t rest[0x300 - 0x278];
} ForkMallocRack;
typedef struct { uintptr_t address; size_t size; uint64_t reclaim; } ForkLargeEntry;
typedef struct {
    uint8_t prefix[0x4080];
    ForkMallocRack racks[3];
    uint32_t largeLock, largeObjects, largeCapacity, padding;
    ForkLargeEntry *large;
    size_t largeBytes;
    uint32_t oldest, newest;
    ForkLargeEntry cache[64];
    int cacheDepth;
    uint8_t tail[0x4fd8 - 0x4fac];
    uint8_t mediumEnabled;
} ForkScalableZone;
_Static_assert(sizeof(ForkMallocRack) == 0x300 && offsetof(ForkMallocRack, current) == 0x18 &&
               offsetof(ForkMallocRack, magazinesCount) == 0x260 && offsetof(ForkMallocRack, magazines) == 0x270, "23A343 malloc rack");
_Static_assert(offsetof(ForkScalableZone, large) == 0x4990 && offsetof(ForkScalableZone, cacheDepth) == 0x4fa8 &&
               offsetof(ForkScalableZone, mediumEnabled) == 0x4fd8 && sizeof(ForkLargeEntry) == 24, "23A343 malloc zone");

typedef struct { LSBForkWireRange ranges[LSB_MAX_FORK_RANGES]; unsigned count; } ForkMallocPages;
enum { ForkMallocPage = 16384, ForkMagazineBytes = 2560, ForkScalableZoneBytes = 0x5000 };

static inline int forkMallocAddPages(void *context, uint64_t address, uint64_t bytes) {
    ForkMallocPages *pages = context;
    if (!address || !bytes || bytes > LSB_MAX_FORK_BYTES || address > UINT64_MAX - bytes - (ForkMallocPage - 1)) return E2BIG;
    uint64_t lo = address & ~(uint64_t)(ForkMallocPage - 1), hi = (address + bytes + ForkMallocPage - 1) & ~(uint64_t)(ForkMallocPage - 1);
    unsigned first = 0;
    while (first < pages->count && pages->ranges[first].start + pages->ranges[first].size < lo) ++first;
    unsigned end = first;
    while (end < pages->count && pages->ranges[end].start <= hi) {
        LSBForkWireRange range = pages->ranges[end++];
        if (range.start < lo) lo = range.start;
        if (range.start + range.size > hi) hi = range.start + range.size;
    }
    uint64_t total = hi - lo;
    for (unsigned i = 0; i < pages->count; ++i) if (i < first || i >= end) total += pages->ranges[i].size;
    if (total > LSB_MAX_FORK_BYTES || (end == first && pages->count == LSB_MAX_FORK_RANGES)) return E2BIG;
    memmove(&pages->ranges[first + 1], &pages->ranges[end], (pages->count - end) * sizeof(pages->ranges[0]));
    pages->count = pages->count - (end - first) + 1;
    pages->ranges[first] = (LSBForkWireRange){lo, hi - lo};
    return 0;
}

static inline int forkMallocImage(uint64_t *base) {
    Dl_info info = {};
    if (!dladdr((const void *)&malloc_get_all_zones, &info) || !info.dli_fbase) return ENOTSUP;
    const struct mach_header_64 *mh = info.dli_fbase;
    if (mh->magic != MH_MAGIC_64 || vm_page_size != ForkMallocPage) return ENOTSUP;
    const uint8_t uuid[] = {0xf5,0x62,0x07,0xa1,0xac,0x9e,0x3e,0xa1,0xb7,0xcf,0x12,0xb9,0x76,0xe0,0x14,0xc5};
    const uint8_t *cursor = (const void *)(mh + 1);
    size_t remaining = mh->sizeofcmds;
    for (unsigned i = 0; i < mh->ncmds; ++i) {
        const struct load_command *lc = (const void *)cursor;
        if (remaining < sizeof(*lc) || lc->cmdsize < sizeof(*lc) || lc->cmdsize > remaining) return ENOEXEC;
        if (lc->cmd == LC_UUID) {
            if (lc->cmdsize != sizeof(struct uuid_command) || memcmp(((const struct uuid_command *)lc)->uuid, uuid, sizeof(uuid))) return ENOTSUP;
            *base = (uintptr_t)mh;
            return 0;
        }
        remaining -= lc->cmdsize;
        cursor += lc->cmdsize;
    }
    return ENOTSUP;
}

// free_small reads the process's region marker at malloc_entropy+1. The cache builder relocates __DATA independently
// of __TEXT, so recover the referenced address from the pinned arm64 ADRP/ADD/LDURH/CMP, not an on-disk data offset.
static inline int forkMallocProcessCookie(uint64_t image, uint32_t *cookie) {
    const uint32_t *code = (const void *)(uintptr_t)(image + 0xb768);
    if ((code[0] & 0x9f00001fu) != 0x9000000au || (code[1] & 0xffc003ffu) != 0x9100014au ||
        code[2] != 0x7840114au || code[3] != 0x6b0a011fu) return ENOTSUP;
    int64_t pages = (int64_t)(((code[0] >> 5) & 0x7ffffu) << 2 | ((code[0] >> 29) & 3));
    if (pages & 0x100000) pages -= 0x200000;
    uintptr_t address = ((uintptr_t)code & ~(uintptr_t)4095) + pages * 4096 + ((code[1] >> 10) & 0xfff);
    Dl_info owner = {};
    if (!dladdr((void *)address, &owner) || (uintptr_t)owner.dli_fbase != image) return ENOTSUP;
    uint16_t marker;
    memcpy(&marker, (const void *)(address + 1), sizeof(marker));
    *cookie = marker;
    return 0;
}

typedef struct { uint32_t from, to; } ForkMallocCookies;

static inline bool forkMallocScalable(uint64_t image, const malloc_zone_t *zone) {
    return zone && zone->version == 16 && (uintptr_t)zone->malloc == image + 0x7900 && (uintptr_t)zone->size == image + 0x7850 &&
        (uintptr_t)zone->free == image + 0x729c && (uintptr_t)zone->realloc == image + 0x7b94 && zone->introspect &&
        (uintptr_t)zone->introspect->enumerator == image + 0x868c && (uintptr_t)zone->introspect->reinit_lock == image + 0x8e54;
}

// visit must authorize each span before it is dereferenced. Re-run the same walk on the copied zone before calling its methods.
static inline int forkMallocWalk(uintptr_t address, ForkAllocatorVisit visit, void *ctx, const ForkMallocCookies *cookies) {
    if (!address || (address & (ForkMallocPage - 1))) return EINVAL;
    int error = visit(ctx, address, ForkScalableZoneBytes);
    if (error) return error;
    const ForkScalableZone *s = (const void *)address;
    if (s->cacheDepth || s->mediumEnabled > 1) return ENOTSUP; // Cached freed large mappings have separate reclamation state.
    const size_t regionSizes[] = {1u << 20, 8u << 20, 128u << 20};
    // Last word before the heap payload in struct tiny_region / small_region / medium_region, verified in free_*.
    // list pointer checks use rack->cookie, a different value that must travel with its own free lists.
    const size_t markerOffsets[] = {0x407c, 0x81fc, 0x7ffc};
    for (unsigned i = 0; i < 2u + s->mediumEnabled; ++i) {
        const ForkMallocRack *r = &s->racks[i];
        if ((r->flags & 3u) || r->type != i + 1 || r->magazinesCount < 1 || r->magazinesCount > 64 || r->magazines < ForkMagazineBytes + ForkMallocPage ||
            ((r->magazines - ForkMagazineBytes) & (ForkMallocPage - 1)) ||
            (r->current != &r->generations[0] && r->current != &r->generations[1])) return ENOTSUP;
        // rack_init maps the entire magazine array (including depot [-1]) between two guard pages; rack_destroy deallocates all three.
        size_t magazineBytes = (1u + (unsigned)r->magazinesCount) * ForkMagazineBytes;
        magazineBytes = (magazineBytes + ForkMallocPage - 1) & ~(size_t)(ForkMallocPage - 1);
        error = visit(ctx, r->magazines - ForkMagazineBytes - ForkMallocPage, magazineBytes + 2 * ForkMallocPage);
        if (error) return error;
        const ForkRegionGeneration *gen = r->current;
        if (gen->capacity < 64 || gen->capacity > 65536 || !gen->slots || (gen->capacity & (gen->capacity - 1)) ||
            gen->next != (gen == &r->generations[0] ? &r->generations[1] : &r->generations[0])) return ENOTSUP;
        error = visit(ctx, (uintptr_t)gen->slots, gen->capacity * sizeof(*gen->slots));
        if (error) return error;
        for (size_t n = 0; n < gen->capacity; ++n) {
            uintptr_t region = gen->slots[n];
            if (!region || region == UINTPTR_MAX) continue;
            if (region & (regionSizes[i] - 1)) return EINVAL;
            error = visit(ctx, region, regionSizes[i]);
            if (error) return error;
            if (cookies) {
                uint32_t *marker = (uint32_t *)(region + markerOffsets[i]);
                if (*marker != cookies->from) return EINVAL;
                if (cookies->to != cookies->from) *marker = cookies->to;
            }
        }
    }
    if (!s->largeCapacity) return s->largeObjects || s->large ? EINVAL : 0;
    if (!s->large || s->largeCapacity > LSB_MAX_FORK_BYTES / sizeof(*s->large)) return E2BIG;
    error = visit(ctx, (uintptr_t)s->large, (size_t)s->largeCapacity * sizeof(*s->large));
    if (error) return error;
    size_t used = 0;
    for (unsigned i = 0; i < s->largeCapacity; ++i) {
        ForkLargeEntry e = s->large[i];
        if (!e.address) continue;
        if (!e.size || ((e.address | e.size) & (ForkMallocPage - 1))) return EINVAL;
        error = visit(ctx, e.address, e.size);
        if (error) return error;
        ++used;
    }
    return used == s->largeObjects ? 0 : EINVAL;
}

static inline int forkMallocRegistry(vm_address_t **zones, unsigned *count) {
    return malloc_get_all_zones(mach_task_self(), NULL, zones, count) == KERN_SUCCESS ? 0 : EIO;
}

static inline int forkAllocatorCapture(ForkAllocatorState *state, ForkAllocatorVisit visit, void *ctx) {
    *state = (ForkAllocatorState){};
    int error = forkMallocImage(&state->image);
    if (!error) error = forkMallocProcessCookie(state->image, &state->regionCookie);
    ForkMallocCookies cookies = {state->regionCookie, state->regionCookie};
    vm_address_t *zones = NULL;
    unsigned count = 0;
    if (error) return error;
    ForkMallocPages pages = {};
    // The registry can change or a custom zone can unregister itself on another thread. Use libmalloc's own ordering.
    _malloc_fork_prepare();
    error = forkMallocRegistry(&zones, &count);
    if (!error && count != 1) error = ENOTSUP;
    if (!error) {
        malloc_zone_t *zone = (void *)zones[0];
        if (!forkMallocScalable(state->image, zone)) error = ENOTSUP;
        else error = forkMallocWalk((uintptr_t)zone, forkMallocAddPages, &pages, &cookies);
        if (!error) state->zones[0] = (uintptr_t)zone;
    }
    _malloc_fork_parent();
    if (error) return error;
    state->count = count;
    for (unsigned i = 0; i < pages.count && !error; ++i) error = visit(ctx, pages.ranges[i].start, pages.ranges[i].size);
    return error;
}

// Called only while the parent's actual malloc fork-prepare locks are held. Callback must be bounded, nonallocating, and lock-free.
static inline int forkAllocatorRecheck(const ForkAllocatorState *state, ForkAllocatorVisit covered, void *ctx) {
    vm_address_t *zones = NULL;
    unsigned count = 0;
    int error = forkMallocRegistry(&zones, &count);
    if (error) return error;
    if (count != 1 || state->count != 1 || zones[0] != state->zones[0] || !forkMallocScalable(state->image, (void *)zones[0])) return EAGAIN;
    ForkMallocCookies cookies = {state->regionCookie, state->regionCookie};
    return forkMallocWalk(zones[0], covered, ctx, &cookies);
}

static inline int forkAllocatorValidate(const ForkAllocatorState *state, ForkAllocatorVisit covered, void *ctx, malloc_zone_t **reference) {
    uint64_t image = 0;
    int error = forkMallocImage(&image);
    if (error || state->image != image || state->count != 1 || state->regionCookie > UINT16_MAX) return error ? error : ENOTSUP;
    vm_address_t *own = NULL;
    unsigned count = 0;
    error = forkMallocRegistry(&own, &count);
    if (error) return error;
    if (count != 1 || !forkMallocScalable(image, (void *)own[0])) return ENOTSUP;
    if (own[0] == state->zones[0]) return EEXIST;
    malloc_zone_t *trusted = (void *)own[0];
    ForkMallocCookies cookies = {state->regionCookie, state->regionCookie};
    error = forkMallocWalk(state->zones[0], covered, ctx, &cookies);
    if (error) return error;
    const malloc_zone_t *zone = (const void *)(uintptr_t)state->zones[0];
    // The 23A343 scalable zone fills 0xc8 bytes of public v16 entries; the zone_name may legitimately differ.
    const size_t name = offsetof(malloc_zone_t, zone_name), rest = name + sizeof(zone->zone_name);
    if (memcmp(zone, trusted, name) || memcmp((const char *)zone + rest, (const char *)trusted + rest, 0xc8 - rest)) return ENOTSUP;
    // Only after all methods, ranges and original region markers have been verified, translate the independent CoW
    // copies for the observer's libmalloc. Existing observer allocations retain their own markers.
    ForkMallocCookies adapt = {.from = state->regionCookie};
    error = forkMallocProcessCookie(image, &adapt.to);
    if (!error && adapt.from != adapt.to) error = forkMallocWalk(state->zones[0], covered, ctx, &adapt);
    if (!error) *reference = trusted;
    return error;
}

// libsystem_malloc-792.0.2: malloc_create_legacy_default_zone and malloc_zone_register both branch to the same
// register_while_locked(zone, make_default). Validate both call sites before using this image-private entry point.
static inline bool forkMallocCalls(uintptr_t pc, uintptr_t target, unsigned makeDefault) {
    const uint32_t *instructions = (const uint32_t *)pc;
    int64_t displacement = (int64_t)target - (int64_t)pc;
    return displacement % 4 == 0 && displacement >= -(INT64_C(1) << 27) && displacement < (INT64_C(1) << 27) &&
        instructions[-1] == (0x52800001u | (makeDefault << 5)) && instructions[0] == (0x94000000u | ((uint32_t)(displacement / 4) & 0x3ffffffu));
}

// Promote a single restored default zone using malloc's normal registration primitive. The original default remains registered,
// including while other observer threads are live. Run a balanced local prepare/parent pair, as these threads survived the restore.
static inline int forkAllocatorPromote(const ForkAllocatorState *state, ForkAllocatorVisit covered, void *ctx) {
    malloc_zone_t *trusted = NULL;
    int error = forkAllocatorValidate(state, covered, ctx, &trusted);
    if (error) return error;
    uintptr_t target = state->image + 0x28d8c;
    if ((uintptr_t)&malloc_zone_register != state->image + 0x291a8 ||
        !forkMallocCalls(state->image + 0x291d4, target, 0) || !forkMallocCalls(state->image + 0x29260, target, 1)) return ENOTSUP;
    const unsigned *capacity = dlsym(RTLD_DEFAULT, "malloc_num_zones_allocated");
    Dl_info owner = {};
    if (!capacity || !dladdr(capacity, &owner) || (uintptr_t)owner.dli_fbase != state->image) return ENOTSUP;
    void (*registerWhileLocked)(malloc_zone_t *, bool) = (void (*)(malloc_zone_t *, bool))target;
    malloc_zone_t *inherited = (void *)(uintptr_t)state->zones[0];
    trusted->introspect->reinit_lock(inherited);
    _malloc_fork_prepare();
    vm_address_t *registry = NULL;
    unsigned count = 0;
    error = forkMallocRegistry(&registry, &count);
    if (!error && (count != 1 || (void *)registry[0] != trusted || *capacity < 2)) error = ENOTSUP;
    if (!error) {
        // malloc_fork_parent will unlock every registered zone. Own the inherited locks locally first.
        trusted->introspect->force_lock(inherited);
        registerWhileLocked(inherited, true);
        // We excluded registry growth above. A failed registration leaves the old table and count in place.
        if ((void *)registry[0] != inherited) {
            trusted->introspect->force_unlock(inherited);
            error = EIO;
        }
    }
    _malloc_fork_parent();
    if (error) return error;
    error = forkMallocRegistry(&registry, &count);
    return !error && count == 2 && (void *)registry[0] == inherited && (void *)registry[1] == trusted ? 0 : EIO;
}
