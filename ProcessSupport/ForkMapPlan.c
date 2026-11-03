#include "ForkMapPlan.h"
#include <errno.h>

static int validateRanges(const LSBForkMapRange *ranges, size_t count, uint64_t pageSize, bool source) {
    uint64_t previousEnd = 0;
    const unsigned protections = LSB_FORK_PROT_READ | LSB_FORK_PROT_WRITE | LSB_FORK_PROT_EXECUTE;
    for (size_t i = 0; i < count; ++i) {
        const LSBForkMapRange *range = &ranges[i];
        if (!range->size || ((range->start | range->size) & (pageSize - 1)) || range->start < previousEnd ||
            range->size > UINT64_MAX - range->start || (range->maxProtection & ~protections) || (range->currentProtection & ~range->maxProtection))
            return EINVAL;
        if (range->immutableImage && range->size > UINT64_MAX - range->imageOffset) return EINVAL;
        if (source && range->inheritance > LSB_FORK_INHERIT_NONE) return ENOTSUP;
        previousEnd = range->start + range->size;
    }
    return 0;
}

static bool reusableAt(const LSBForkMapRange *source, const LSBForkMapRange *child, uint64_t address) {
    return source->immutableImage && source->immutableImage == child->immutableImage && !source->dirty && !child->dirty &&
           source->currentProtection != 0 && !(source->maxProtection & LSB_FORK_PROT_WRITE) &&
           source->currentProtection == child->currentProtection && source->maxProtection == child->maxProtection &&
           source->inheritance == child->inheritance && source->imageOffset + (address - source->start) == child->imageOffset + (address - child->start);
}

static int walk(const LSBForkMapRange *source, size_t sourceCount, const LSBForkMapRange *child, size_t childCount,
                uint64_t minimum, uint64_t limit, LSBForkMapStep *steps, size_t *count) {
    size_t used = 0, nextChild = 0;
    for (size_t i = 0; i < sourceCount; ++i) {
        const LSBForkMapRange *range = &source[i];
        uint64_t end = range->start + range->size;
        LSBForkMapAction action = LSB_FORK_MAP_SKIP;
        if (range->inheritance != LSB_FORK_INHERIT_NONE) {
            if (range->start < minimum || end > limit) return ERANGE;
            if (!range->maxProtection) action = LSB_FORK_MAP_GUARD;
            else action = range->inheritance == LSB_FORK_INHERIT_SHARE ? LSB_FORK_MAP_SHARE : LSB_FORK_MAP_COPY;
        }
        uint64_t address = range->start;
        while (address < end) {
            uint64_t next = end;
            LSBForkMapAction segmentAction = action;
            if (action != LSB_FORK_MAP_SKIP) {
                while (nextChild < childCount && child[nextChild].start + child[nextChild].size <= address) ++nextChild;
                if (nextChild < childCount && child[nextChild].start < end) {
                    const LSBForkMapRange *occupied = &child[nextChild];
                    if (occupied->start > address) {
                        next = occupied->start;
                    } else {
                        if (!reusableAt(range, occupied, address)) return EADDRINUSE;
                        uint64_t childEnd = occupied->start + occupied->size;
                        if (childEnd < next) next = childEnd;
                        segmentAction = LSB_FORK_MAP_REUSE;
                    }
                }
            }
            if (used == SIZE_MAX) return EOVERFLOW;
            if (steps) steps[used] = (LSBForkMapStep){address, next - address, i, segmentAction};
            ++used;
            address = next;
        }
    }
    *count = used;
    return 0;
}

int LSBPlanForkMappings(const LSBForkMapRange *source, size_t sourceCount, const LSBForkMapRange *child, size_t childCount,
                        uint64_t childMinimum, uint64_t childLimit, uint64_t pageSize, LSBForkMapStep *steps, size_t capacity, size_t *count) {
    if (!count) return EINVAL;
    *count = 0;
    if ((sourceCount && !source) || (childCount && !child) || (!steps && capacity) || !pageSize || (pageSize & (pageSize - 1)) ||
        childMinimum >= childLimit || ((childMinimum | childLimit) & (pageSize - 1))) return EINVAL;
    int error = validateRanges(source, sourceCount, pageSize, true);
    if (!error) error = validateRanges(child, childCount, pageSize, false);
    if (!error) error = walk(source, sourceCount, child, childCount, childMinimum, childLimit, NULL, count);
    if (error || !steps) return error;
    if (capacity < *count) return ENOSPC;
    return walk(source, sourceCount, child, childCount, childMinimum, childLimit, steps, count);
}
