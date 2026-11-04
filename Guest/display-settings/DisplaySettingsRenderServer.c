#include "PrivateDisplaySettings.h"
#include <errno.h>
#include <mach/ndr.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// Original QuartzCore __TEXT constants at 0x2bf958..0x2bf970 and server handlers +0x2ad70c..0x2ad89c.
// These four operations alone are supported; the host never constructs an arbitrary render-server request.
enum { GetFlags = 40259, SetFlags = 40260, GetValue = 40261, SetValue = 40262, ReplyDelta = 100, TimeoutMS = 1000 };
typedef struct { mach_msg_header_t header; NDR_record_t ndr; uint32_t first, second; } RenderRequest;
typedef struct { mach_msg_header_t header; NDR_record_t ndr; kern_return_t error; uint32_t value; } RenderReply;
_Static_assert(sizeof(RenderRequest) == 40 && offsetof(RenderRequest, first) == 32, "CoreAnimation request layout");
_Static_assert(sizeof(RenderReply) == 40 && offsetof(RenderReply, error) == 32, "CoreAnimation reply layout");

static int transportError(kern_return_t error) {
    if (error == MACH_SEND_TIMED_OUT || error == MACH_RCV_TIMED_OUT) return ETIMEDOUT;
    if (error == MACH_SEND_INVALID_DEST || error == MACH_RCV_PORT_DIED) return ENOTCONN;
    if (error == KERN_INVALID_ARGUMENT) return EINVAL;
    return EIO;
}

static int transaction(mach_port_t server, int operation, uint32_t first, uint32_t second, uint32_t *value) {
    if (!MACH_PORT_VALID(server)) return ENOTCONN;
    mach_port_t replyPort = MACH_PORT_NULL;
    kern_return_t result = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &replyPort);
    if (result) return transportError(result);
    union { RenderRequest request; struct { RenderReply reply; mach_msg_max_trailer_t trailer; } received; } message = {0};
    bool getter = operation == GetFlags || operation == GetValue;
    mach_msg_size_t size = operation == GetFlags ? 24 : operation == GetValue ? 36 : 40;
    message.request = (RenderRequest){.header = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE),
        .msgh_size = size, .msgh_remote_port = server, .msgh_local_port = replyPort, .msgh_id = operation},
        .ndr = NDR_record, .first = first, .second = second};
    result = mach_msg(&message.request.header, MACH_SEND_MSG | MACH_RCV_MSG | MACH_SEND_TIMEOUT | MACH_RCV_TIMEOUT,
        size, sizeof(message), replyPort, TimeoutMS, MACH_PORT_NULL);
    int error = 0;
    if (result) error = transportError(result);
    else {
        const RenderReply *reply = &message.received.reply;
        bool complex = (reply->header.msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0;
        if (complex || reply->header.msgh_id != operation + ReplyDelta || reply->header.msgh_remote_port != MACH_PORT_NULL ||
            reply->header.msgh_local_port != replyPort || (reply->header.msgh_size != 36 && reply->header.msgh_size != 40) ||
            memcmp(&reply->ndr, &NDR_record, sizeof(NDR_record))) error = EPROTO;
        else if (reply->error) error = transportError(reply->error);
        else if (reply->header.msgh_size != (getter ? 40u : 36u)) error = EPROTO;
        else if (value) *value = reply->value;
        // Even malformed replies may carry rights; dispose them without leaking them into this persistent worker.
        if (complex || reply->header.msgh_remote_port != MACH_PORT_NULL) mach_msg_destroy(&message.received.reply.header);
    }
    mach_port_destroy(mach_task_self(), replyPort);
    return error;
}

int LSBRenderServerGetFlags(mach_port_t server, uint32_t *flags) {
    if (!flags) return EINVAL;
    return transaction(server, GetFlags, 0, 0, flags);
}
int LSBRenderServerSetFlags(mach_port_t server, uint32_t mask, uint32_t flags) {
    return transaction(server, SetFlags, mask, flags, NULL);
}
int LSBRenderServerGetHUDValue(mach_port_t server, int32_t *value) {
    if (!value) return EINVAL;
    uint32_t bits = 0;
    int error = transaction(server, GetValue, 1, 0, &bits);
    if (error) return error;
    float observed;
    memcpy(&observed, &bits, sizeof(observed));
    if (!isfinite(observed) || observed < 0 || observed > 8 || floorf(observed) != observed) return ERANGE;
    *value = (int32_t)observed;
    return 0;
}
int LSBRenderServerSetHUDValue(mach_port_t server, int32_t value) {
    if (value < 0 || value > 8) return EINVAL;
    float converted = (float)value;
    uint32_t bits;
    memcpy(&bits, &converted, sizeof(bits));
    return transaction(server, SetValue, 1, bits, NULL);
}
