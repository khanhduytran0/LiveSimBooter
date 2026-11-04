#include "../Guest/Shared/Fork/ForkPthreadRegistry.h"
#include <assert.h>
#include <mach-o/ldsyms.h>
#include <stdio.h>

static int sampleData;
static void sample(void) { ++sampleData; }

static int allow(void *context, uint64_t address, uint64_t bytes) {
    return address == *(uintptr_t *)context || !bytes ? EACCES : 0;
}

int main(void) {
    assert(forkPthreadOnceComplete(UINT64_MAX) && forkPthreadOnceComplete(1) && forkPthreadOnceComplete(0x2e0a5));
    assert(!forkPthreadOnceComplete(0) && !forkPthreadOnceComplete(0x12302) && !forkPthreadOnceComplete(0x12303));
    uintptr_t pc = (uintptr_t)sample, image = (uintptr_t)&_mh_execute_header, excluded = 0;
    uint8_t uuid[16], again[16];
    assert(!forkPthreadImage(pc, image, uuid, allow, &excluded));
    assert(!forkPthreadImage(pc, image, again, NULL, NULL) && !memcmp(uuid, again, sizeof(uuid)));
    assert(forkPthreadImage(pc + 1, image, again, NULL, NULL) == ENOTSUP);
    assert(forkPthreadImage((uintptr_t)&sampleData, image, again, NULL, NULL) == ENOEXEC);
    assert(forkPthreadImage(pc, image | 1, again, NULL, NULL) == ENOTSUP);
    excluded = pc;
    assert(forkPthreadImage(pc, image, again, allow, &excluded) == ENOTSUP);
    excluded = image;
    assert(forkPthreadImage(pc, image, again, allow, &excluded) == ENOTSUP);
    puts("Private atfork target requires a covered executable section, matching image kind, and RX memory");
    return 0;
}
