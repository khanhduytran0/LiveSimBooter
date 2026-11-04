#include <assert.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/ndr.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static unsigned allocations, releases, destroys, calls;
static int expectedID, variant;
static uint32_t expectedFirst, expectedSecond, returnedValue;
static kern_return_t transportFailure, serverFailure, allocationFailure;
static kern_return_t mockAllocate(ipc_space_t task, mach_port_right_t right, mach_port_name_t *name) {
    (void)task;
    assert(right == MACH_PORT_RIGHT_RECEIVE);
    allocations++;
    if (allocationFailure) return allocationFailure;
    *name = 47;
    return KERN_SUCCESS;
}
static kern_return_t mockDestroy(ipc_space_t task, mach_port_name_t name) {
    (void)task; assert(name == 47); releases++; return KERN_SUCCESS;
}
static void mockMessageDestroy(mach_msg_header_t *header) { (void)header; destroys++; }
static mach_msg_return_t mockMessage(mach_msg_header_t *header, mach_msg_option_t options, mach_msg_size_t sent,
                                    mach_msg_size_t limit, mach_port_name_t receive, mach_msg_timeout_t timeout, mach_port_name_t notify) {
    calls++;
    assert(options == (MACH_SEND_MSG | MACH_RCV_MSG | MACH_SEND_TIMEOUT | MACH_RCV_TIMEOUT));
    assert(receive == 47 && timeout == 1000 && notify == MACH_PORT_NULL && limit >= 40);
    assert(header->msgh_remote_port == 91 && header->msgh_local_port == 47 && header->msgh_id == expectedID);
    assert(header->msgh_bits == MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE));
    assert(sent == (expectedID == 40259 ? 24u : expectedID == 40261 ? 36u : 40u));
    assert(header->msgh_size == sent);
    if (sent > 24) {
        assert(!memcmp((char *)header + 24, &NDR_record, 8));
        assert(*(uint32_t *)((char *)header + 32) == expectedFirst);
        if (sent == 40) assert(*(uint32_t *)((char *)header + 36) == expectedSecond);
    }
    if (transportFailure) return transportFailure;
    bool getter = expectedID == 40259 || expectedID == 40261;
    *header = (mach_msg_header_t){.msgh_size = getter && !serverFailure ? 40 : 36,
        .msgh_local_port = 47, .msgh_id = expectedID + 100};
    memcpy((char *)header + 24, &NDR_record, 8);
    *(kern_return_t *)((char *)header + 32) = serverFailure;
    *(uint32_t *)((char *)header + 36) = returnedValue;
    if (variant == 1) header->msgh_id++;
    if (variant == 2) header->msgh_size = 35;
    if (variant == 3) header->msgh_bits |= MACH_MSGH_BITS_COMPLEX;
    if (variant == 4) header->msgh_remote_port = 123;
    if (variant == 5) header->msgh_local_port++;
    if (variant == 6) ((char *)header)[24] ^= 1;
    if (variant == 7) header->msgh_size = getter ? 36 : 40;
    return MACH_MSG_SUCCESS;
}
#define mach_port_allocate mockAllocate
#define mach_port_destroy mockDestroy
#define mach_msg mockMessage
#define mach_msg_destroy mockMessageDestroy
#include "../Guest/display-settings/DisplaySettingsRenderServer.c"
#undef mach_port_allocate
#undef mach_port_destroy
#undef mach_msg
#undef mach_msg_destroy

static uint32_t bits(float value) { uint32_t result; memcpy(&result, &value, 4); return result; }
int main(void) {
    uint32_t flags = 0;
    expectedID = GetFlags;
    returnedValue = 0x10400001;
    assert(!LSBRenderServerGetFlags(91, &flags) && flags == returnedValue);
    expectedID = SetFlags; expectedFirst = 0x10000000; expectedSecond = 0;
    assert(!LSBRenderServerSetFlags(91, expectedFirst, expectedSecond));
    expectedID = GetValue; expectedFirst = 1;
    int32_t value = -1;
    for (int i = 0; i <= 8; i++) { returnedValue = bits((float)i); assert(!LSBRenderServerGetHUDValue(91, &value) && value == i); }
    const float invalid[] = {-1, 9, 0.5, NAN, INFINITY};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        returnedValue = bits(invalid[i]); value = -1;
        assert(LSBRenderServerGetHUDValue(91, &value) == ERANGE && value == -1);
    }
    expectedID = SetValue; expectedFirst = 1;
    for (int i = 0; i <= 8; i++) { expectedSecond = bits((float)i); assert(!LSBRenderServerSetHUDValue(91, i)); }
    unsigned previous = calls;
    assert(LSBRenderServerSetHUDValue(91, -1) == EINVAL && LSBRenderServerSetHUDValue(91, 9) == EINVAL);
    assert(LSBRenderServerGetHUDValue(91, NULL) == EINVAL && LSBRenderServerGetFlags(91, NULL) == EINVAL && calls == previous);
    assert(LSBRenderServerGetFlags(MACH_PORT_NULL, &flags) == ENOTCONN && calls == previous);
    expectedID = GetFlags;
    for (variant = 1; variant <= 7; variant++) {
        flags = 0xface;
        assert(LSBRenderServerGetFlags(91, &flags) == EPROTO && flags == 0xface);
    }
    variant = 0;
    assert(destroys == 2);
    serverFailure = KERN_FAILURE;
    assert(LSBRenderServerGetFlags(91, &flags) == EIO);
    serverFailure = KERN_INVALID_ARGUMENT;
    assert(LSBRenderServerGetFlags(91, &flags) == EINVAL);
    serverFailure = 0;
    transportFailure = MACH_RCV_TIMED_OUT;
    assert(LSBRenderServerGetFlags(91, &flags) == ETIMEDOUT);
    transportFailure = MACH_SEND_INVALID_DEST;
    assert(LSBRenderServerGetFlags(91, &flags) == ENOTCONN);
    transportFailure = 0;
    assert(allocations == releases && calls == releases);
    allocationFailure = KERN_RESOURCE_SHORTAGE;
    assert(LSBRenderServerGetFlags(91, &flags) == EIO && allocations == releases + 1 && calls == releases);
    puts("Display settings render-server tests passed");
}
