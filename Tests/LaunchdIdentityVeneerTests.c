#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Shared offsets are checked here; the test runner generates the native fixture assembly around the patcher's actual veneers.
struct Fixture {
    uint64_t before[31], paddingBefore, after[31], paddingAfter;
    uint64_t vectorsBefore[32][2], vectorsAfter[32][2];
    uint64_t spBefore, spAfter, nzcvBefore, nzcvAfter, fpcrBefore, fpcrAfter, fpsrBefore, fpsrAfter;
    uint64_t auditAddress, resolverAudit, dlsymHandle, dlsymName, dlsymCalls, resolverCalls, result, available, resumeIndex;
    uint32_t audit[8], auditAfter[8];
} LSBTestFixture;
_Static_assert(offsetof(struct Fixture, after) == 0x100 && offsetof(struct Fixture, vectorsBefore) == 0x200, "GPR fixture layout");
_Static_assert(offsetof(struct Fixture, vectorsAfter) == 0x400 && offsetof(struct Fixture, spBefore) == 0x600, "SIMD fixture layout");
_Static_assert(offsetof(struct Fixture, auditAddress) == 0x640 && offsetof(struct Fixture, resumeIndex) == 0x680, "Call fixture layout");
_Static_assert(offsetof(struct Fixture, audit) == 0x688 && offsetof(struct Fixture, auditAfter) == 0x6a8, "Audit fixture layout");

extern void LSBTestRunVeneer(unsigned index);
extern void *LSBTestFakeDlsym(void *handle, const char *symbol);
extern const char LSBTestSymbolName[];
void *(*LSBTestDlsymPointer)(void *, const char *) = LSBTestFakeDlsym;
static const unsigned destinations[] = {1, 1, 8, 0, 9, 0, 0, 0, 8};

static void verify(unsigned index, int available, int32_t result) {
    memset(&LSBTestFixture, 0, sizeof(LSBTestFixture));
    struct Fixture *f = &LSBTestFixture;
    for (unsigned reg = 0; reg < 31; ++reg) f->before[reg] = UINT64_C(0x123456789abc0000) + reg * 0x101;
    for (unsigned reg = 0; reg < 32; ++reg) {
        f->vectorsBefore[reg][0] = UINT64_C(0xaabbccddeeff0000) + reg * 0x101;
        f->vectorsBefore[reg][1] = UINT64_C(0x001122334455ffff) - reg * 0x101;
    }
    f->nzcvBefore = 0xa0000000;
    f->fpcrBefore = 0;
    f->fpsrBefore = 0x08000000;
    f->result = (uint32_t)result;
    f->available = available;
    f->resumeIndex = UINT64_MAX;
    const uint32_t audit[] = {17, 501, 20, 501, 20, 712, 19, 28};
    memcpy(f->audit, audit, sizeof(audit));
    LSBTestRunVeneer(index);
    assert(f->resumeIndex == index && f->spAfter == f->spBefore);
    assert(f->nzcvAfter == f->nzcvBefore && f->fpcrAfter == f->fpcrBefore && f->fpsrAfter == f->fpsrBefore);
    for (unsigned reg = 0; reg < 31; ++reg) {
        uint64_t expected = reg == destinations[index] ? (uint32_t)(available ? result : -1) : f->before[reg];
        if (f->after[reg] != expected) {
            fprintf(stderr, "veneer %u x%u: expected 0x%llx, received 0x%llx\n", index, reg,
                    (unsigned long long)expected, (unsigned long long)f->after[reg]);
            assert(f->after[reg] == expected);
        }
    }
    assert(!memcmp(f->vectorsBefore, f->vectorsAfter, sizeof(f->vectorsBefore)));
    assert(f->dlsymCalls == 1 && f->dlsymHandle == (uint64_t)-2);
    assert(f->dlsymName == (uintptr_t)LSBTestSymbolName && !strcmp(LSBTestSymbolName, "LSBLaunchdResolveAuditPID"));
    assert(f->resolverCalls == (uint64_t)available && (!available || f->resolverAudit == f->auditAddress));
    assert(!memcmp(f->audit, audit, sizeof(audit)) && !memcmp(f->auditAfter, audit, sizeof(audit)));
}

int main(void) {
    for (unsigned index = 0; index < sizeof(destinations) / sizeof(destinations[0]); ++index) {
        verify(index, 1, 0x12345678);
        verify(index, 1, -1);
        verify(index, 1, 0);
        verify(index, 0, 0x12345678);
    }
    puts("LaunchdIdentityVeneerTests passed (nine sites, found/missing helper, all GPR/SIMD/flags/SP/LR)");
    return 0;
}
