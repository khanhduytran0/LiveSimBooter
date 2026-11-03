#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// These values match the Darwin VM API. The planner itself makes no Mach calls.
enum { LSB_FORK_PROT_READ = 1, LSB_FORK_PROT_WRITE = 2, LSB_FORK_PROT_EXECUTE = 4 };
enum { LSB_FORK_INHERIT_SHARE = 0, LSB_FORK_INHERIT_COPY = 1, LSB_FORK_INHERIT_NONE = 2 };

typedef struct {
    uint64_t start, size;
    uint8_t currentProtection, maxProtection, inheritance;
    bool dirty;
    // Trusted, snapshot-local identity for verified immutable signed bytes, shared by both inventories.
    // A path, UUID, matching bytes, or VM object ID alone is insufficient. Use zero when uncertain.
    uint64_t immutableImage, imageOffset;
} LSBForkMapRange;

typedef enum {
    LSB_FORK_MAP_COPY, LSB_FORK_MAP_SHARE, LSB_FORK_MAP_GUARD, LSB_FORK_MAP_REUSE, LSB_FORK_MAP_SKIP
} LSBForkMapAction;

typedef struct {
    uint64_t start, size;
    size_t sourceIndex;
    LSBForkMapAction action;
} LSBForkMapStep;

// Ranges must be sorted, disjoint and page-aligned within each inventory. Child bounds are [minimum, limit).
// All existing child ranges are protected. Only matching verified immutable mappings may be reused.
// Passing NULL steps with zero capacity counts the required steps. A short buffer returns ENOSPC and the
// required count. Any other error sets count to zero. No steps are written on error.
int LSBPlanForkMappings(const LSBForkMapRange *source, size_t sourceCount, const LSBForkMapRange *child, size_t childCount,
                        uint64_t childMinimum, uint64_t childLimit, uint64_t pageSize, LSBForkMapStep *steps, size_t capacity, size_t *count);

#ifdef __cplusplus
}
#endif
