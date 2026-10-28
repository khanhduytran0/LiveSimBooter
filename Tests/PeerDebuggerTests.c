#include "../ProcessSupport/PeerDebugger.h"

#include <assert.h>
#include <errno.h>
#include <mach/notify.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef union { mach_msg_header_t header; uint64_t alignment; unsigned char bytes[8192]; } Message;
enum {
    RealReply, WrongHost, NoRequestAudit, BadRequestVersion, BadRequestReserved, BadRequestID,
    FakeSuccess, WrongNonce, WrongVersion, WrongID, WrongPID, NegativeError, HugeError, RemoteError, WrongSize, ComplexReply, DelayedReply
};
typedef struct {
    mach_port_t endpoint, fixture;
    unsigned variant;
    audit_token_t host;
    unsigned char nonce[LSB_PEER_DEBUGGER_NONCE_BYTES];
} Server;

static audit_token_t ownAudit(void) {
    audit_token_t audit;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&audit, &count));
    assert(count == TASK_AUDIT_TOKEN_COUNT && audit.val[5] == (unsigned)getpid());
    return audit;
}

static mach_port_t newPort(void) {
    mach_port_t port;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port));
    assert(!mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND));
    return port;
}

static void closePort(mach_port_t port) {
    assert(!mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(!mach_port_deallocate(mach_task_self(), port));
}

static void expectSendReferences(mach_port_t port, mach_port_urefs_t expected) {
    mach_port_urefs_t actual = 0;
    assert(!mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, &actual));
    assert(actual == expected);
}

static bool isZero(const void *bytes, size_t size) {
    const unsigned char *p = bytes;
    while (size--) if (*p++) return false;
    return true;
}

static double monotonicSeconds(void) {
    struct timespec now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

static void *serve(void *context) {
    Server *server = context;
    Message received = {0};
    mach_msg_option_t options = MACH_RCV_MSG | MACH_RCV_TIMEOUT;
    if (server->variant != NoRequestAudit)
        options |= MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) | MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT);
    assert(!mach_msg(&received.header, options, 0, sizeof(received), server->endpoint, 2000, MACH_PORT_NULL));
    assert(received.header.msgh_size == sizeof(LSBPeerDebuggerRequest));
    LSBPeerDebuggerRequest *request = (void *)&received;
    memcpy(server->nonce, request->nonce, sizeof(server->nonce));
    if (server->variant <= BadRequestID || server->variant == DelayedReply) {
        if (server->variant == WrongHost) ++server->host.val[7];
        if (server->variant == BadRequestVersion) ++request->version;
        if (server->variant == BadRequestReserved) request->reserved = 1;
        if (server->variant == BadRequestID) ++request->header.msgh_id;
        if (server->variant == DelayedReply) usleep(100000);
        errno = ERANGE;
        LSBPeerDebuggerReply(&received.header, sizeof(received), &server->host);
        assert(errno == ERANGE);
        return NULL;
    }
    Message response = {0};
    LSBPeerDebuggerResponse *reply = (void *)&response;
    *reply = (LSBPeerDebuggerResponse){
        .header = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0), .msgh_size = sizeof(*reply),
                   .msgh_remote_port = received.header.msgh_remote_port, .msgh_id = LSB_PEER_DEBUGGER_REPLY_ID},
        .version = LSB_PEER_DEBUGGER_VERSION
    };
    memcpy(reply->nonce, request->nonce, sizeof(reply->nonce));
    reply->info.kp_proc.p_pid = getpid();
    reply->info.kp_proc.p_flag = P_TRACED;
    if (server->variant == WrongNonce) ++reply->nonce[0];
    if (server->variant == WrongVersion) ++reply->version;
    if (server->variant == WrongID) ++reply->header.msgh_id;
    if (server->variant == WrongPID) ++reply->info.kp_proc.p_pid;
    if (server->variant == NegativeError) reply->error = -1;
    if (server->variant == HugeError) reply->error = INT32_MAX;
    if (server->variant == RemoteError) reply->error = EPERM;
    if (server->variant == WrongSize) reply->header.msgh_size += sizeof(uint32_t);
    if (server->variant == ComplexReply) {
        struct Complex { mach_msg_header_t header; mach_msg_body_t body; mach_msg_port_descriptor_t descriptor; } *complex = (void *)&response;
        complex->header.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
        complex->header.msgh_size = sizeof(*complex);
        complex->body.msgh_descriptor_count = 1;
        complex->descriptor = (mach_msg_port_descriptor_t){.name = server->fixture,
            .disposition = MACH_MSG_TYPE_MOVE_SEND, .type = MACH_MSG_PORT_DESCRIPTOR};
    }
    received.header.msgh_remote_port = MACH_PORT_NULL;
    mach_msg_destroy(&received.header);
    assert(!mach_msg(&response.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, response.header.msgh_size, 0, MACH_PORT_NULL, 100, MACH_PORT_NULL));
    return NULL;
}

static void realSelfAndAuthentication(void) {
    unsigned char previousNonce[LSB_PEER_DEBUGGER_NONCE_BYTES] = {0};
    for (unsigned variant = RealReply; variant <= DelayedReply; ++variant) {
        Server server = {.endpoint = newPort(), .variant = variant, .host = ownAudit()};
        if (variant == ComplexReply) server.fixture = newPort();
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, serve, &server));
        audit_token_t peer = ownAudit();
        struct kinfo_proc info;
        memset(&info, 0xa5, sizeof(info));
        errno = EDOM;
        int error = LSBPeerDebuggerQuery(server.endpoint, &peer, variant == DelayedReply ? 20 : 500, &info);
        assert(errno == EDOM);
        int expected = variant == RealReply || variant == FakeSuccess ? 0 : variant == RemoteError ? EPERM :
                       variant == DelayedReply ? ETIMEDOUT : EPROTO;
        if (error != expected) fprintf(stderr, "variant=%u error=%d expected=%d\n", variant, error, expected);
        assert(error == expected);
        assert(!pthread_join(thread, NULL));
        if (error) assert(isZero(&info, sizeof(info)));
        else {
            assert(info.kp_proc.p_pid == getpid());
            if (variant == RealReply) {
                struct kinfo_proc native = {0}; size_t size = sizeof(native);
                int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
                assert(!sysctl(mib, 4, &native, &size, NULL, 0));
                assert(size == sizeof(native) && (native.kp_proc.p_flag & P_TRACED) == (info.kp_proc.p_flag & P_TRACED));
            }
        }
        assert(!isZero(server.nonce, sizeof(server.nonce)) && memcmp(previousNonce, server.nonce, sizeof(previousNonce)));
        memcpy(previousNonce, server.nonce, sizeof(previousNonce));
        expectSendReferences(server.endpoint, 1);
        if (server.fixture) {
            expectSendReferences(server.fixture, 0); // Rejected complex reply relinquishes the transferred send reference.
            assert(!mach_port_mod_refs(mach_task_self(), server.fixture, MACH_PORT_RIGHT_RECEIVE, -1));
        }
        closePort(server.endpoint);
    }
    for (unsigned field = 0; field < 8; ++field) {
        Server server = {.endpoint = newPort(), .variant = FakeSuccess, .host = ownAudit()};
        audit_token_t peer = ownAudit();
        peer.val[field] ^= 1;
        pthread_t thread; assert(!pthread_create(&thread, NULL, serve, &server));
        struct kinfo_proc info;
        assert(LSBPeerDebuggerQuery(server.endpoint, &peer, 500, &info) == EACCES && isZero(&info, sizeof(info)));
        assert(!pthread_join(thread, NULL));
        expectSendReferences(server.endpoint, 1);
        closePort(server.endpoint);
    }
}

static void malformedRequestCleanup(void) {
    mach_port_t endpoint = newPort(), reply = newPort(), fixture = newPort();
    struct { mach_msg_header_t header; mach_msg_body_t body; mach_msg_port_descriptor_t port; } request = {
        .header = {.msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE),
                   .msgh_size = sizeof(request), .msgh_remote_port = endpoint, .msgh_local_port = reply,
                   .msgh_id = LSB_PEER_DEBUGGER_REQUEST_ID},
        .body = {.msgh_descriptor_count = 1},
        .port = {.name = fixture, .disposition = MACH_MSG_TYPE_MOVE_SEND, .type = MACH_MSG_PORT_DESCRIPTOR}
    };
    assert(!mach_msg(&request.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(request), 0, MACH_PORT_NULL, 100, MACH_PORT_NULL));
    Message message = {0};
    assert(!mach_msg(&message.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
                    MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT), 0, sizeof(message), endpoint, 100, MACH_PORT_NULL));
    audit_token_t host = ownAudit();
    LSBPeerDebuggerReply(&message.header, sizeof(message), &host);
    expectSendReferences(fixture, 0);
    assert(!mach_msg(&message.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(message), reply, 100, MACH_PORT_NULL));
    assert(message.header.msgh_id == MACH_NOTIFY_SEND_ONCE); // The rejected request's send-once right was destroyed.
    mach_msg_destroy(&message.header);
    expectSendReferences(endpoint, 1); expectSendReferences(reply, 1);
    closePort(endpoint); closePort(reply);
    assert(!mach_port_mod_refs(mach_task_self(), fixture, MACH_PORT_RIGHT_RECEIVE, -1));
}

static void boundedFailureAndBorrowedRights(void) {
    audit_token_t peer = ownAudit();
    mach_port_t endpoint = newPort();
    struct kinfo_proc info;
    assert(LSBPeerDebuggerQuery(endpoint, &peer, 0, &info) == EINVAL && isZero(&info, sizeof(info)));
    assert(LSBPeerDebuggerQuery(endpoint, &peer, 501, &info) == EINVAL);
    assert(LSBPeerDebuggerQuery(endpoint, NULL, 50, &info) == EINVAL);
    assert(LSBPeerDebuggerQuery(endpoint, &peer, 50, NULL) == EINVAL);
    assert(LSBPeerDebuggerQuery(MACH_PORT_NULL, &peer, 50, &info) == EINVAL);
    double start = monotonicSeconds();
    assert(LSBPeerDebuggerQuery(endpoint, &peer, 20, &info) == ETIMEDOUT && isZero(&info, sizeof(info)));
    assert(monotonicSeconds() - start < 0.5);
    expectSendReferences(endpoint, 1);
    closePort(endpoint); // Destroys the queued request after the caller's private reply receive right has gone.

    endpoint = newPort();
    mach_port_limits_t limits = {.mpl_qlimit = 1};
    assert(!mach_port_set_attributes(mach_task_self(), endpoint, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT));
    mach_msg_header_t filler = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0), .msgh_size = sizeof(filler), .msgh_remote_port = endpoint};
    assert(!mach_msg(&filler, MACH_SEND_MSG, sizeof(filler), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL));
    start = monotonicSeconds();
    assert(LSBPeerDebuggerQuery(endpoint, &peer, 20, &info) == ETIMEDOUT && isZero(&info, sizeof(info)));
    assert(monotonicSeconds() - start < 0.5);
    expectSendReferences(endpoint, 1); // Failed COPY_SEND copyin must not consume the owner's send reference.
    closePort(endpoint);

    endpoint = newPort();
    assert(!mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(LSBPeerDebuggerQuery(endpoint, &peer, 20, &info) == EPIPE && isZero(&info, sizeof(info)));
    mach_port_urefs_t refs = 0;
    assert(!mach_port_get_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_DEAD_NAME, &refs) && refs == 1);
    assert(!mach_port_deallocate(mach_task_self(), endpoint));
}

int main(void) {
    alarm(20);
    realSelfAndAuthentication();
    malformedRequestCleanup();
    boundedFailureAndBorrowedRights();
    puts("PeerDebuggerTests: fresh native self info, full audit+nonce validation, bounded timeout and Mach-right cleanup passed");
    return 0;
}
