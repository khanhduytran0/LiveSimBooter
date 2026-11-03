#include "../ProcessSupport/ForkMapPlan.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

enum { P = 0x4000, LIMIT = 32 * P, RW = LSB_FORK_PROT_READ | LSB_FORK_PROT_WRITE, RX = LSB_FORK_PROT_READ | LSB_FORK_PROT_EXECUTE };

static LSBForkMapRange region(unsigned page, unsigned pages, unsigned protection, unsigned inheritance) {
    return (LSBForkMapRange){.start = page * P, .size = pages * P, .currentProtection = protection, .maxProtection = protection, .inheritance = inheritance};
}

static void basicActions(void) {
    LSBForkMapRange source[] = {
        region(1, 1, RW, LSB_FORK_INHERIT_NONE), region(4, 2, RW, LSB_FORK_INHERIT_COPY),
        region(6, 1, RW, LSB_FORK_INHERIT_SHARE), region(7, 1, 0, LSB_FORK_INHERIT_COPY), region(8, 1, 0, LSB_FORK_INHERIT_COPY)
    };
    // A temporarily inaccessible private mapping can have dirty bytes. Only a zero maximum denotes a guard.
    source[4].maxProtection = RW;
    source[4].dirty = true;
    LSBForkMapRange child = region(1, 1, RX, LSB_FORK_INHERIT_COPY);
    LSBForkMapStep steps[5];
    size_t count;
    assert(!LSBPlanForkMappings(source, 5, &child, 1, 3 * P, LIMIT, P, steps, 5, &count) && count == 5);
    const LSBForkMapAction expected[] = {LSB_FORK_MAP_SKIP, LSB_FORK_MAP_COPY, LSB_FORK_MAP_SHARE, LSB_FORK_MAP_GUARD, LSB_FORK_MAP_COPY};
    for (size_t i = 0; i < count; ++i) {
        assert(steps[i].action == expected[i] && steps[i].sourceIndex == i);
        assert(steps[i].start == source[i].start && steps[i].size == source[i].size);
    }
    source[0].inheritance = LSB_FORK_INHERIT_COPY;
    assert(LSBPlanForkMappings(source, 5, &child, 1, 3 * P, LIMIT, P, steps, 5, &count) == ERANGE && count == 0);
}

static void mixedLayout(void) {
    LSBForkMapRange source = region(4, 6, RX, LSB_FORK_INHERIT_COPY);
    source.immutableImage = 11;
    source.imageOffset = 4 * P;
    LSBForkMapRange child[] = {region(3, 2, RX, LSB_FORK_INHERIT_COPY), region(6, 2, RX, LSB_FORK_INHERIT_COPY), region(9, 2, RX, LSB_FORK_INHERIT_COPY)};
    for (unsigned i = 0; i < 3; ++i) {
        child[i].imageOffset = child[i].start;
        child[i].immutableImage = 11;
    }
    LSBForkMapStep steps[5], original[5];
    memset(steps, 0x5a, sizeof(steps));
    memcpy(original, steps, sizeof(steps));
    size_t count = 0;
    assert(!LSBPlanForkMappings(&source, 1, child, 3, P, LIMIT, P, NULL, 0, &count) && count == 5);
    assert(LSBPlanForkMappings(&source, 1, child, 3, P, LIMIT, P, steps, 4, &count) == ENOSPC && count == 5);
    assert(memcmp(steps, original, sizeof(steps)) == 0);
    child[2].imageOffset += P;
    assert(LSBPlanForkMappings(&source, 1, child, 3, P, LIMIT, P, steps, 5, &count) == EADDRINUSE && !count);
    assert(memcmp(steps, original, sizeof(steps)) == 0);
    child[2].imageOffset -= P;
    assert(!LSBPlanForkMappings(&source, 1, child, 3, P, LIMIT, P, steps, 5, &count) && count == 5);
    const unsigned starts[] = {4, 5, 6, 8, 9};
    const unsigned sizes[] = {1, 1, 2, 1, 1};
    for (size_t i = 0; i < count; ++i) {
        assert(steps[i].action == (i % 2 ? LSB_FORK_MAP_COPY : LSB_FORK_MAP_REUSE));
        assert(steps[i].sourceIndex == 0 && steps[i].start == starts[i] * P && steps[i].size == sizes[i] * P);
    }
}

static void collision(LSBForkMapRange source, LSBForkMapRange child) {
    size_t count = 1;
    assert(LSBPlanForkMappings(&source, 1, &child, 1, 0, LIMIT, P, NULL, 0, &count) == EADDRINUSE && !count);
}

static void reservedRanges(void) {
    LSBForkMapRange code = region(4, 1, RX, LSB_FORK_INHERIT_COPY), child = code;
    collision(code, child);
    code.immutableImage = 1;
    collision(code, child);
    child.immutableImage = 2;
    collision(code, child);
    child = code;
    child.dirty = true;
    collision(code, child);
    collision(child, code);
    child = code;
    child.inheritance = LSB_FORK_INHERIT_SHARE;
    collision(code, child);
    child = code;
    child.currentProtection = LSB_FORK_PROT_READ;
    collision(code, child);
    child = code;
    child.maxProtection |= LSB_FORK_PROT_WRITE;
    collision(child, child);
    child.currentProtection = RW;
    collision(child, child);
    child.maxProtection = child.currentProtection = 0;
    collision(code, child);
    // One destination range spans two separate compatible source ranges.
    LSBForkMapRange slices[2] = {code, code};
    slices[1].start += P;
    slices[1].imageOffset += P;
    child = code;
    child.size += P;
    LSBForkMapStep steps[2];
    size_t count = 0;
    assert(!LSBPlanForkMappings(slices, 2, &child, 1, 0, LIMIT, P, steps, 2, &count) && count == 2);
    assert(steps[0].action == LSB_FORK_MAP_REUSE && steps[1].action == LSB_FORK_MAP_REUSE && steps[1].sourceIndex == 1);
}

static void malformedRanges(void) {
    LSBForkMapRange ranges[] = {region(4, 2, RW, LSB_FORK_INHERIT_COPY), region(5, 1, RW, LSB_FORK_INHERIT_COPY)};
    size_t count;
    assert(!LSBPlanForkMappings(NULL, 0, NULL, 0, 0, LIMIT, P, NULL, 0, &count) && !count);
    assert(LSBPlanForkMappings(ranges, 2, NULL, 0, 0, LIMIT, P, NULL, 0, &count) == EINVAL);
    assert(LSBPlanForkMappings(NULL, 0, ranges, 2, 0, LIMIT, P, NULL, 0, &count) == EINVAL);
    LSBForkMapRange invalid = ranges[0];
    invalid.size = UINT64_MAX - P + 1;
    assert(LSBPlanForkMappings(&invalid, 1, NULL, 0, 0, LIMIT, P, NULL, 0, &count) == EINVAL);
    invalid = ranges[0];
    invalid.immutableImage = 1;
    invalid.imageOffset = UINT64_MAX;
    assert(LSBPlanForkMappings(&invalid, 1, NULL, 0, 0, LIMIT, P, NULL, 0, &count) == EINVAL);
    invalid = ranges[0];
    invalid.maxProtection = 0;
    assert(LSBPlanForkMappings(&invalid, 1, NULL, 0, 0, LIMIT, P, NULL, 0, &count) == EINVAL);
    invalid = ranges[0];
    invalid.inheritance = 3;
    assert(LSBPlanForkMappings(&invalid, 1, NULL, 0, 0, LIMIT, P, NULL, 0, &count) == ENOTSUP);
    assert(LSBPlanForkMappings(ranges, 1, NULL, 0, 0, 5 * P, P, NULL, 0, &count) == ERANGE);
    assert(LSBPlanForkMappings(ranges, 1, NULL, 0, 0, LIMIT, 3, NULL, 0, &count) == EINVAL);
    invalid = ranges[0];
    ++invalid.start;
    assert(LSBPlanForkMappings(&invalid, 1, NULL, 0, 0, LIMIT, P, NULL, 0, &count) == EINVAL);
}

int main(void) {
    basicActions();
    mixedLayout();
    reservedRanges();
    malformedRanges();
    puts("fork mapping planner passed");
    return 0;
}
