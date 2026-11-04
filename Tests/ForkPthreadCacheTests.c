#include "../Guest/Shared/Fork/ForkPthreadRegistry.h"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>

int main(void) {
    uint8_t *page = mmap(NULL, vm_page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(page != MAP_FAILED);
    memset(page, 0x3a, vm_page_size);
    // Sections share a hardware page with unrelated bytes. A trailing const mismatch must reject the entire packet.
    uintptr_t a = (uintptr_t)page;
    ForkCacheCheckpointPlan plan = {.count = 2, .used = 26, .spans = {{a, a + 17, 11, ForkCacheCopy}, {a, a + 256, 15, ForkCacheCompare}}};
    ForkCacheObserver *observer = calloc(1, sizeof(*observer));
    ForkCachePacket *packet = calloc(1, sizeof(*packet));
    assert(observer && packet && !forkCacheCapturePrepared(&plan, packet));
    observer->original = *packet;
    memset(packet->bytes, 0xcd, 11);
    page[257] = 0x12;
    assert(forkCacheApplyPrepared(observer, packet) == EBUSY && page[17] == 0x3a);
    page[257] = 0x3a;
    packet->plan.spans[0].address++;
    assert(forkCacheApplyPrepared(observer, packet) == EPROTO && page[17] == 0x3a);
    packet->plan = plan;
    assert(!forkCacheApplyPrepared(observer, packet));
    for (unsigned i = 0; i < vm_page_size; ++i) assert(page[i] == (i >= 17 && i < 28 ? 0xcd : 0x3a));
    FILE *file = tmpfile();
    assert(file && fwrite(observer->original.bytes, 1, 11, file) == 11 && !fflush(file));
    LSBSlidableDataHeader header = {.preferredBase = a, .actualBase = a, .mappingCount = 1, .mappings = {{.cacheAddress = a + 17, .size = 11}}};
    uint8_t current[11];
    memcpy(current, observer->original.bytes, sizeof(current));
    assert(!forkCachePristine(&header, fileno(file), &plan.spans[0], current, packet->bytes));
    current[7] ^= 1;
    assert(forkCachePristine(&header, fileno(file), &plan.spans[0], current, packet->bytes) == EBUSY);
    memcpy(current, packet->bytes, 8);
    assert(!forkCachePristine(&header, fileno(file), &plan.spans[0], current, packet->bytes));
    fclose(file);
    free(observer); free(packet);
    assert(!munmap(page, vm_page_size));
    puts("Cached atfork state checks section bounds, pristine local ownership, and all destinations before a private write");
}
