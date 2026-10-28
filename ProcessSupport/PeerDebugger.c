#include "PeerDebugger.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool validAudit(const audit_token_t *audit) {
    return audit && audit->val[5] && audit->val[5] <= INT_MAX;
}

static bool copyAudit(const mach_msg_header_t *message, size_t capacity, audit_token_t *audit) {
    if (message->msgh_size > capacity) return false;
    size_t offset = ((size_t)message->msgh_size + 3) & ~(size_t)3;
    if (offset > capacity || sizeof(mach_msg_audit_trailer_t) > capacity - offset) return false;
    mach_msg_audit_trailer_t trailer;
    memcpy(&trailer, (const unsigned char *)message + offset, sizeof(trailer));
    if (trailer.msgh_trailer_type != MACH_MSG_TRAILER_FORMAT_0 || trailer.msgh_trailer_size < sizeof(trailer) ||
        trailer.msgh_trailer_size > capacity - offset) return false;
    *audit = trailer.msgh_audit;
    return true;
}

static int64_t monotonicNanoseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static mach_msg_timeout_t remainingMilliseconds(int64_t deadline) {
    int64_t now = monotonicNanoseconds();
    return now < 0 || now >= deadline ? 0 : (mach_msg_timeout_t)((deadline - now + 999999) / 1000000);
}

static int query(mach_port_t endpoint, const audit_token_t *expected, uint32_t timeoutMS, struct kinfo_proc *info) {
    if (!info) return EINVAL;
    memset(info, 0, sizeof(*info));
    if (!MACH_PORT_VALID(endpoint) || !validAudit(expected) || !timeoutMS || timeoutMS > LSB_PEER_DEBUGGER_MAX_TIMEOUT_MS) return EINVAL;
    int64_t now = monotonicNanoseconds();
    if (now < 0) return errno;
    int64_t deadline = now + (int64_t)timeoutMS * 1000000;
    mach_port_t replyPort = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &replyPort)) return ENOMEM;
    LSBPeerDebuggerRequest request = {
        .header = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE),
                   .msgh_size = sizeof(request), .msgh_remote_port = endpoint, .msgh_local_port = replyPort,
                   .msgh_id = LSB_PEER_DEBUGGER_REQUEST_ID},
        .version = LSB_PEER_DEBUGGER_VERSION
    };
    arc4random_buf(request.nonce, sizeof(request.nonce));
    int error = ETIMEDOUT;
    mach_msg_timeout_t timeout = remainingMilliseconds(deadline);
    if (!timeout) goto cleanup;
    mach_msg_return_t result = mach_msg(&request.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT | MACH_SEND_INTERRUPT,
                                      sizeof(request), 0, MACH_PORT_NULL, timeout, MACH_PORT_NULL);
    if (result) {
        // Copyin failure may return copies as received rights. Never destroy the caller's original borrowed COPY_SEND reference.
        if (MACH_MSGH_BITS_REMOTE(request.header.msgh_bits) == MACH_MSG_TYPE_PORT_SEND) mach_msg_destroy(&request.header);
        error = result == MACH_SEND_TIMED_OUT ? ETIMEDOUT : result == MACH_SEND_INVALID_DEST ? EPIPE :
                result == MACH_SEND_INTERRUPTED ? EINTR : EIO;
        goto cleanup;
    }
    union { mach_msg_header_t header; uint64_t alignment; unsigned char bytes[sizeof(LSBPeerDebuggerResponse) + MAX_TRAILER_SIZE]; } reply = {0};
    timeout = remainingMilliseconds(deadline);
    if (!timeout) goto cleanup;
    result = mach_msg(&reply.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_INTERRUPT |
                     MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) | MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT),
                     0, sizeof(reply), replyPort, timeout, MACH_PORT_NULL);
    if (result) {
        if ((result & ~MACH_MSG_MASK) == MACH_RCV_BODY_ERROR) mach_msg_destroy(&reply.header);
        error = result == MACH_RCV_TIMED_OUT ? ETIMEDOUT : result == MACH_RCV_TOO_LARGE ? EMSGSIZE :
                result == MACH_RCV_INTERRUPTED ? EINTR : EIO;
        goto cleanup;
    }
    // Timeout arguments have millisecond precision. A delayed/suspended caller must not accept a reply after the shared deadline.
    now = monotonicNanoseconds();
    if (now < 0 || now >= deadline) {
        error = now < 0 ? EIO : ETIMEDOUT;
        mach_msg_destroy(&reply.header);
        goto cleanup;
    }
    error = EPROTO;
    audit_token_t audit;
    if (reply.header.msgh_size == sizeof(LSBPeerDebuggerResponse) && reply.header.msgh_id == LSB_PEER_DEBUGGER_REPLY_ID &&
        reply.header.msgh_bits == MACH_MSGH_BITS(0, MACH_MSG_TYPE_PORT_SEND_ONCE) && reply.header.msgh_local_port == replyPort &&
        reply.header.msgh_remote_port == MACH_PORT_NULL && reply.header.msgh_voucher_port == MACH_PORT_NULL &&
        copyAudit(&reply.header, sizeof(reply), &audit)) {
        LSBPeerDebuggerResponse response;
        memcpy(&response, reply.bytes, sizeof(response));
        if (response.version == LSB_PEER_DEBUGGER_VERSION && !memcmp(response.nonce, request.nonce, sizeof(request.nonce)) &&
            response.error >= 0 && response.error <= ELAST) {
            error = EACCES;
            if (!memcmp(&audit, expected, sizeof(audit))) {
                error = response.error;
                if (!error && response.info.kp_proc.p_pid != (pid_t)expected->val[5]) error = EPROTO;
                if (!error) *info = response.info;
            }
        }
    }
    mach_msg_destroy(&reply.header);
cleanup:
    mach_port_mod_refs(mach_task_self(), replyPort, MACH_PORT_RIGHT_RECEIVE, -1);
    return error;
}

int LSBPeerDebuggerQuery(mach_port_t endpoint, const audit_token_t *expectedPeer, uint32_t timeoutMS, struct kinfo_proc *info) {
    int saved = errno;
    int error = query(endpoint, expectedPeer, timeoutMS, info);
    errno = saved;
    return error;
}

static int copySelfInfo(struct kinfo_proc *info) {
    audit_token_t audit = {{0}};
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    if (task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&audit, &count) != KERN_SUCCESS ||
        count != TASK_AUDIT_TOKEN_COUNT || !validAudit(&audit)) return EACCES;
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)audit.val[5]};
    size_t size = sizeof(*info);
    if (sysctl(mib, 4, info, &size, NULL, 0)) return errno ? errno : EIO;
    if (size != sizeof(*info) || info->kp_proc.p_pid != (pid_t)audit.val[5]) return ESRCH;
    return 0;
}

void LSBPeerDebuggerReply(mach_msg_header_t *received, size_t capacity, const audit_token_t *expectedHost) {
    if (!received || capacity < sizeof(*received)) return;
    int saved = errno;
    audit_token_t audit;
    bool valid = validAudit(expectedHost) && received->msgh_size == sizeof(LSBPeerDebuggerRequest) &&
        received->msgh_size <= capacity && received->msgh_id == LSB_PEER_DEBUGGER_REQUEST_ID &&
        received->msgh_bits == MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND_ONCE, MACH_MSG_TYPE_PORT_SEND) &&
        MACH_PORT_VALID(received->msgh_remote_port) && received->msgh_voucher_port == MACH_PORT_NULL &&
        copyAudit(received, capacity, &audit) && !memcmp(&audit, expectedHost, sizeof(audit));
    LSBPeerDebuggerRequest request = {0};
    if (valid) {
        memcpy(&request, received, sizeof(request));
        valid = request.version == LSB_PEER_DEBUGGER_VERSION && !request.reserved;
    }
    if (valid) {
        LSBPeerDebuggerResponse response = {
            .header = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0), .msgh_size = sizeof(response),
                       .msgh_remote_port = received->msgh_remote_port, .msgh_id = LSB_PEER_DEBUGGER_REPLY_ID},
            .version = LSB_PEER_DEBUGGER_VERSION
        };
        received->msgh_remote_port = MACH_PORT_NULL; // The response takes the imported send-once right.
        memcpy(response.nonce, request.nonce, sizeof(response.nonce));
        response.error = copySelfInfo(&response.info);
        if (response.error) memset(&response.info, 0, sizeof(response.info));
        // Never block the exception server behind a paused or vanished requester.
        mach_msg_return_t result = mach_msg(&response.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT | MACH_SEND_INTERRUPT,
                                          sizeof(response), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
        if (result) mach_msg_destroy(&response.header);
    }
    // Received messages came from a successful kernel receive, so their descriptors are safe for the standard destruction routine.
    mach_msg_destroy(received);
    errno = saved;
}
