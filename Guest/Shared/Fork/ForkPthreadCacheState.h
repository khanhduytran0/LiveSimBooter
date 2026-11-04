#pragma once
#include <stdint.h>

enum { ForkCacheMaxBytes = 65536, ForkCacheMaxSpans = 32, ForkCacheMaxOwners = 4 };
enum { ForkCacheCompare = 1, ForkCacheCopy = 2 };
typedef struct { uint64_t image, address; uint32_t bytes, kind; } ForkCacheSpan;
typedef struct { uint32_t count, used; ForkCacheSpan spans[ForkCacheMaxSpans]; } ForkCacheCheckpointPlan;
typedef struct { ForkCacheCheckpointPlan plan; uint8_t bytes[ForkCacheMaxBytes]; } ForkCachePacket;
// Original observer bytes are local; ensure its own bootstrap prepare did not acquire a lock or mutate this owner.
typedef struct { ForkCachePacket original; } ForkCacheObserver;
_Static_assert(sizeof(ForkCacheSpan) == 24 && sizeof(ForkCachePacket) == 66312, "bounded prepared guest cache sections");
