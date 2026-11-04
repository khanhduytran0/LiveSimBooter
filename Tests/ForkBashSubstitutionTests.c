#include "ForkBashFixture.h"
#include "../Guest/libLiveSimSpawn/ForkBashSubstitution.h"

int main(void) {
    size_t page = vm_page_size;
    uintptr_t image = (uintptr_t)bashFixture();
    void *stack = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    void *heap = mmap(NULL, BashEarlyHeapBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(stack != MAP_FAILED && heap != MAP_FAILED);
    bashHeapInit(heap);
    bashEarlyHeap = heap;
    TrialOffer offer = {.mainHeader = image, .resume = TrialShell, .caller.sp = (uintptr_t)stack};
    BashMakeChildFrame *frame = stack;
    frame->lr = image + BashSubstitutionReturn;
    frame->fp = (uintptr_t)(frame + 1) + 0xf0;
    frame->x19 = (uintptr_t)bashEarlyStrdup("df -P .");
    BashCapturePipe *ends = (void *)((char *)(frame + 1) + 0x10);
    int capture[2];
    assert(!pipe(capture) && capture[1] < LSB_MAX_FDS);
    *ends = (BashCapturePipe){capture[0], capture[1]};
    TrialPlan plan = {};
    assert(!addSpan(&plan, "fixture-stack", (uintptr_t)stack, (uintptr_t)stack + page));
    assert(addBashSubstitution(&plan, &offer, frame) == ENOTSUP); // Source is outside the offer.
    assert(!addSpan(&plan, "fixture-heap", (uintptr_t)heap, (uintptr_t)heap + BashEarlyHeapBytes));
    assert(!fcntl(capture[1], F_SETFD, FD_CLOEXEC));
    assert(!addBashSubstitution(&plan, &offer, frame));
    assert(offer.closeOnExec[capture[1] / 64] == (UINT64_C(1) << (capture[1] % 64)));
    *ends = (BashCapturePipe){capture[1], capture[0]};
    assert(addBashSubstitution(&plan, &offer, frame) == ENOTSUP); // Reversed pipe.
    *ends = (BashCapturePipe){capture[0], capture[1]};
    frame->x19++;
    assert(addBashSubstitution(&plan, &offer, frame) == ENOTSUP); // Bash must be able to free the source after parsing.
    frame->x19 = (uintptr_t)bashEarlyStrdup("printf '%s' $(date)");
    assert(!addBashSubstitution(&plan, &offer, frame));
    frame->x19 = (uintptr_t)bashEarlyStrdup("cat <(date)");
    assert(addBashSubstitution(&plan, &offer, frame) == ENOTSUP);
    close(capture[0]);
    close(capture[1]);
    assert(!munmap(heap, BashEarlyHeapBytes) && !munmap(stack, page));
    free((void *)image);
    puts("Bash command substitution source ownership and pipe inheritance passed");
    return 0;
}
