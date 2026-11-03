#include "../ProcessSupport/GuestImagePlacement.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const uint64_t window = UINT64_C(0x7000000000), base = window + LSB_SLIDABLE_CACHE_PREFERRED_BASE;
    const uint64_t span = LSB_NATIVE_SUPPORT_CACHE_SPAN, limit = UINT64_C(0x8000000000);
    const uint64_t preferred = UINT64_C(0x100000000), page = 16384, bashSize = 0x108000;
    uint64_t main, bootstrap;
    assert(!LSBGuestImageAddress(base, span, preferred, bashSize, page, limit, LSBGuestMainImage, &main));
    assert(main == UINT64_C(0x7100000000) && main - preferred == window);
    assert(!LSBGuestImageAddress(base, span, preferred, page, page, limit, LSBForkBootstrapImage, &bootstrap));
    assert(bootstrap == UINT64_C(0x713c000000) && main + LSB_GUEST_MAIN_IMAGE_CAPACITY == bootstrap);
    assert(bootstrap + LSB_FORK_BOOTSTRAP_IMAGE_CAPACITY <= base);
    uint64_t address;
    assert(!LSBGuestImageAddress(UINT64_C(0x480000000), span, preferred, bashSize, page, UINT64_C(0xfc0000000), LSBGuestMainImage, &address));
    assert(address == UINT64_C(0x400000000));

    // Fully occupied slots fit below the cache, whose end can equal the exclusive task maximum.
    assert(!LSBGuestImageAddress(base, span, preferred, LSB_GUEST_MAIN_IMAGE_CAPACITY, page, base + span, LSBGuestMainImage, &address));
    assert(address == main);
    assert(LSBGuestImageAddress(base, span, preferred, LSB_GUEST_MAIN_IMAGE_CAPACITY + page, page, limit, LSBGuestMainImage, &address) == ENOSPC && !address);
    uint64_t bootstrapSize = LSB_FORK_BOOTSTRAP_IMAGE_CAPACITY;
    assert(!LSBGuestImageAddress(base, span, preferred, bootstrapSize, page, base + span, LSBForkBootstrapImage, &address));
    assert(address == bootstrap);
    assert(LSBGuestImageAddress(base, span, preferred, bootstrapSize + page, page, limit, LSBForkBootstrapImage, &address) == ENOSPC);
    assert(LSBGuestImageAddress(base, LSB_SLIDABLE_CACHE_RESERVATION_SIZE + page, preferred, page, page, limit, LSBForkBootstrapImage, &address) == ENOSPC);
    assert(LSBGuestImageAddress(base, span, main + page, bashSize, page, limit, LSBGuestMainImage, &address) == ERANGE);
    assert(LSBGuestImageAddress(base, span, preferred, bashSize, page, base + span - 1, LSBGuestMainImage, &address) == ERANGE);
    assert(LSBGuestImageAddress(LSB_SLIDABLE_CACHE_PREFERRED_BASE - page, span, preferred, page, page, limit, LSBGuestMainImage, &address) == ERANGE);
    assert(LSBGuestImageAddress(2 * LSB_SLIDABLE_CACHE_PREFERRED_BASE - page, span, preferred, page, page, limit, LSBGuestMainImage, &address) == ERANGE);
    assert(LSBGuestImageAddress(UINT64_MAX - page + 1, span, preferred, page, page, UINT64_MAX, LSBGuestMainImage, &address) == ERANGE);

    assert(LSBGuestImageAddress(base + 1, span, preferred, page, page, limit, LSBGuestMainImage, &address) == EINVAL);
    assert(LSBGuestImageAddress(base, 0, preferred, page, page, limit, LSBGuestMainImage, &address) == EINVAL);
    assert(LSBGuestImageAddress(base, span, preferred, 0, page, limit, LSBGuestMainImage, &address) == EINVAL);
    assert(LSBGuestImageAddress(base, span, preferred, page - 1, page, limit, LSBGuestMainImage, &address) == EINVAL);
    assert(LSBGuestImageAddress(base, span, preferred, page, 0, limit, LSBGuestMainImage, &address) == EINVAL);
    assert(LSBGuestImageAddress(base, span, preferred, page, page - 1, limit, LSBGuestMainImage, &address) == EINVAL);
    assert(LSBGuestImageAddress(base, span, preferred, page, page, limit, (LSBGuestImageSlot)42, &address) == EINVAL);
    assert(LSBGuestImageAddress(base, span, preferred, page, page, limit, LSBGuestMainImage, NULL) == EINVAL);
    puts("Guest image placement: separate slots, alignment, capacity, task bounds and overflow passed");
    return 0;
}
