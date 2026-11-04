// The inherited stock zones are part of the fork snapshot, separate from the observer's own registration table.
#pragma once
#include <stdint.h>

enum { ForkAllocatorMaxZones = 8 };
typedef struct {
    uint64_t image;
    uint64_t zones[ForkAllocatorMaxZones];
    uint32_t count, regionCookie; // Only the 16-bit libmalloc region marker used by this process.
} ForkAllocatorState;
_Static_assert(sizeof(ForkAllocatorState) == 80, "allocator checkpoint wire layout");
