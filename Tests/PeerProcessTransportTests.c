#include "../ProcessSupport/SpawnTransport.h"
#include "../ProcessSupport/PrivateXPC.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
extern int proc_pidinfo(int, int, uint64_t, void *, int);
extern int posix_spawnattr_set_registered_ports_np(posix_spawnattr_t *, mach_port_t *, uint32_t);
_Static_assert(sizeof(struct proc_bsdshortinfo) == LSB_PEER_SHORT_INFO_BYTES, "short BSD info wire size");

enum {
    Valid, Repeat, Missing, Denied, EncodingFailure, WrongVersion, WrongError, WrongReplyPID, WrongID,
    MissingAudit, AuditShort, AuditType, AuditPID, AuditVersion, MissingInfo, InfoShort, InfoLong, InfoFD, InfoPID,
    MissingPath, PathEmpty, PathRelative, PathLong, PathType, MissingTask, TaskNull, TaskType, TaskFD, TaskAudit,
    ExtraFD, ExtraPort, ErrorWithIdentity, ReplyCount
};
enum { BadRequestCount = 13, BadCheckInCount = 17 };
enum {
    ResolveSame, ResolveDifferent, ResolveMissing, ResolveDenied, ResolveVersion, ResolveErrorType, ResolveRequestID,
    ResolveMissingPID, ResolveZeroPID, ResolveNegativePID, ResolveUnsignedPID, ResolveOverflowPID, ResolveErrorPID,
    ResolveExtraFD, ResolveExtraPort, ResolveReplyCount
};

int LSBTestCopyBrokerPort(mach_port_t *port) {
    mach_port_array_t ports = NULL;
    mach_msg_type_number_t count = 0;
    if (mach_ports_lookup(mach_task_self(), &ports, &count)) return EIO;
    *port = count ? ports[0] : MACH_PORT_NULL;
    for (unsigned i = 1; i < count; ++i) if (MACH_PORT_VALID(ports[i])) mach_port_deallocate(mach_task_self(), ports[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)ports, count * sizeof(*ports));
    return MACH_PORT_VALID(*port) ? 0 : ENOENT;
}

static LSBPeerProcessIdentity selfIdentity(void) {
    LSBPeerProcessIdentity identity = {0};
    assert(proc_pidinfo(getpid(), PROC_PIDT_SHORTBSDINFO, 0, identity.shortInfo, sizeof(identity.shortInfo)) == sizeof(identity.shortInfo));
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&identity.audit, &count) && count == TASK_AUDIT_TOKEN_COUNT);
    assert(!task_name_for_pid(mach_task_self(), getpid(), &identity.taskName));
    strcpy(identity.executablePath, "/Applications/SpringBoard.app/SpringBoard");
    return identity;
}

static mach_port_urefs_t references(mach_port_t port) {
    mach_port_urefs_t count = 0;
    assert(!mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, &count));
    return count;
}

static unsigned descriptorCount(void) {
    unsigned count = 0;
    for (int fd = 0; fd < 256; ++fd) if (fcntl(fd, F_GETFD) >= 0) ++count;
    return count;
}

static void addFD(xpc_object_t object, const char *key) {
    xpc_object_t descriptor = xpc_fd_create(STDERR_FILENO);
    assert(descriptor);
    xpc_dictionary_set_value(object, key, descriptor);
    xpc_release(descriptor);
}

static void setIdentity(xpc_object_t object, const LSBPeerProcessIdentity *identity) {
    xpc_dictionary_set_data(object, "peer-audit", &identity->audit, sizeof(identity->audit));
    xpc_dictionary_set_data(object, "peer-short-info", identity->shortInfo, sizeof(identity->shortInfo));
    xpc_dictionary_set_string(object, "peer-path", identity->executablePath);
    xpc_dictionary_set_mach_send(object, "peer-task-name", identity->taskName);
}

static void replyVariant(LSBReceivedMessage *message, const LSBPeerProcessIdentity *identity, mach_port_t otherTask, unsigned variant) {
    if (variant <= EncodingFailure) {
        int error = variant == Missing ? ESRCH : variant == Denied ? EACCES : 0;
        const LSBPeerProcessIdentity *value = variant == EncodingFailure || error ? NULL : identity;
        assert(LSBTransportReplyProcessIdentity(message, -1, NULL) == EINVAL && message->object);
        int result = LSBTransportReplyProcessIdentity(message, error, value);
        assert(result == (variant == EncodingFailure ? EINVAL : 0) && !message->object);
        return;
    }
    xpc_object_t reply = xpc_dictionary_create_reply(message->object);
    assert(reply);
    xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(reply, "request-id", 0);
    xpc_dictionary_set_int64(reply, "error", 0);
    xpc_dictionary_set_int64(reply, "pid", identity->audit.val[5]);
    setIdentity(reply, identity);
    audit_token_t audit = identity->audit;
    unsigned char info[LSB_PEER_SHORT_INFO_BYTES + 1] = {0};
    memcpy(info, identity->shortInfo, sizeof(identity->shortInfo));
    switch (variant) {
        case WrongVersion: xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION + 1); break;
        case WrongError: xpc_dictionary_set_string(reply, "error", "0"); break;
        case WrongReplyPID: xpc_dictionary_set_int64(reply, "pid", 1); break;
        case WrongID: xpc_dictionary_set_uint64(reply, "request-id", 1); break;
        case MissingAudit: xpc_dictionary_set_value(reply, "peer-audit", NULL); break;
        case AuditShort: xpc_dictionary_set_data(reply, "peer-audit", &audit, sizeof(audit) - 1); break;
        case AuditType: xpc_dictionary_set_string(reply, "peer-audit", "invalid"); break;
        case AuditPID: audit.val[5] = 1; xpc_dictionary_set_data(reply, "peer-audit", &audit, sizeof(audit)); break;
        case AuditVersion: ++audit.val[7]; xpc_dictionary_set_data(reply, "peer-audit", &audit, sizeof(audit)); break;
        case MissingInfo: xpc_dictionary_set_value(reply, "peer-short-info", NULL); break;
        case InfoShort: xpc_dictionary_set_data(reply, "peer-short-info", info, sizeof(info) - 2); break;
        case InfoLong: xpc_dictionary_set_data(reply, "peer-short-info", info, sizeof(info)); break;
        case InfoFD: addFD(reply, "peer-short-info"); break;
        case InfoPID: memset(info, 0, 4); xpc_dictionary_set_data(reply, "peer-short-info", info, sizeof(info) - 1); break;
        case MissingPath: xpc_dictionary_set_value(reply, "peer-path", NULL); break;
        case PathEmpty: xpc_dictionary_set_string(reply, "peer-path", ""); break;
        case PathRelative: xpc_dictionary_set_string(reply, "peer-path", "Applications/SpringBoard"); break;
        case PathLong: {
            char path[LSB_PATH_BYTES + 1];
            memset(path, 'x', sizeof(path)); path[0] = '/'; path[sizeof(path) - 1] = 0;
            xpc_dictionary_set_string(reply, "peer-path", path); break;
        }
        case PathType: xpc_dictionary_set_uint64(reply, "peer-path", 3); break;
        case MissingTask: xpc_dictionary_set_value(reply, "peer-task-name", NULL); break;
        case TaskNull: xpc_dictionary_set_mach_send(reply, "peer-task-name", MACH_PORT_NULL); break;
        case TaskType: xpc_dictionary_set_uint64(reply, "peer-task-name", identity->taskName); break;
        case TaskFD: addFD(reply, "peer-task-name"); break;
        case TaskAudit: xpc_dictionary_set_mach_send(reply, "peer-task-name", otherTask); break;
        case ExtraFD: addFD(reply, "unexpected"); break;
        case ExtraPort: xpc_dictionary_set_mach_send(reply, "unexpected", identity->taskName); break;
        case ErrorWithIdentity: xpc_dictionary_set_int64(reply, "error", ESRCH); xpc_dictionary_set_int64(reply, "pid", 0); break;
        default: abort();
    }
    assert(!xpc_pipe_routine_reply(reply));
    xpc_release(reply);
}

static void resolveReplyVariant(LSBReceivedMessage *message, mach_port_t task, unsigned variant) {
    if (variant <= ResolveDenied) {
        int error = variant == ResolveMissing ? ESRCH : variant == ResolveDenied ? EACCES : 0;
        pid_t pid = error ? 0 : getpid() + (variant == ResolveDifferent ? 100000 : 0);
        assert(!LSBTransportReply(message, error, pid, 0, NULL, NULL, NULL));
        return;
    }
    xpc_object_t reply = xpc_dictionary_create_reply(message->object);
    assert(reply);
    xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(reply, "request-id", 0);
    xpc_dictionary_set_int64(reply, "error", 0);
    xpc_dictionary_set_int64(reply, "pid", getpid() + 100000);
    switch (variant) {
        case ResolveVersion: xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION + 1); break;
        case ResolveErrorType: xpc_dictionary_set_string(reply, "error", "0"); break;
        case ResolveRequestID: xpc_dictionary_set_uint64(reply, "request-id", 1); break;
        case ResolveMissingPID: xpc_dictionary_set_value(reply, "pid", NULL); break;
        case ResolveZeroPID: xpc_dictionary_set_int64(reply, "pid", 0); break;
        case ResolveNegativePID: xpc_dictionary_set_int64(reply, "pid", -1); break;
        case ResolveUnsignedPID: xpc_dictionary_set_uint64(reply, "pid", getpid()); break;
        case ResolveOverflowPID: xpc_dictionary_set_int64(reply, "pid", (int64_t)INT_MAX + 1); break;
        case ResolveErrorPID: xpc_dictionary_set_int64(reply, "error", ESRCH); break;
        case ResolveExtraFD: addFD(reply, "unexpected"); break;
        case ResolveExtraPort: xpc_dictionary_set_mach_send(reply, "unexpected", task); break;
        default: abort();
    }
    assert(!xpc_pipe_routine_reply(reply));
    xpc_release(reply);
}

static void checkRequest(const LSBPeerProcessIdentity *identity, unsigned variant, unsigned kind) {
    bool checkin = kind == LSB_REQUEST_CHECKIN;
    mach_port_t broker = MACH_PORT_NULL;
    assert(!LSBTestCopyBrokerPort(&broker));
    xpc_object_t pipe = xpc_pipe_create_from_port(broker, 0), request = xpc_dictionary_create(NULL, NULL, 0), reply = NULL;
    assert(pipe && request);
    xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(request, "kind", kind);
    xpc_dictionary_set_uint64(request, "request-id", 0);
    xpc_dictionary_set_int64(request, "error", 0);
    if (checkin) {
        xpc_dictionary_set_data(request, "self-short-info", identity->shortInfo, sizeof(identity->shortInfo));
        xpc_dictionary_set_mach_send(request, "self-task-name", identity->taskName);
        unsigned char info[LSB_PEER_SHORT_INFO_BYTES + 1] = {0};
        memcpy(info, identity->shortInfo, sizeof(identity->shortInfo));
        LSBWorkerStackGeometry stack = { .bottom = 8 * vm_page_size, .top = 16 * vm_page_size };
        switch (variant) {
            case 0: xpc_dictionary_set_value(request, "self-short-info", NULL); break;
            case 1: xpc_dictionary_set_data(request, "self-short-info", info, sizeof(info) - 2); break;
            case 2: xpc_dictionary_set_data(request, "self-short-info", info, sizeof(info)); break;
            case 3: addFD(request, "self-short-info"); break;
            case 4: memset(info, 0, 4); xpc_dictionary_set_data(request, "self-short-info", info, sizeof(info) - 1); break;
            case 5: xpc_dictionary_set_value(request, "self-task-name", NULL); break;
            case 6: xpc_dictionary_set_mach_send(request, "self-task-name", MACH_PORT_NULL); break;
            case 7: xpc_dictionary_set_uint64(request, "self-task-name", identity->taskName); break;
            case 8: addFD(request, "self-task-name"); break;
            case 9: addFD(request, "extra"); break;
            case 10: xpc_dictionary_set_data(request, "peer-audit", &identity->audit, sizeof(identity->audit)); break;
            case 11: xpc_dictionary_set_int64(request, "error", EPERM); break;
            case 12: addFD(request, "self-worker-stack"); break;
            case 13: xpc_dictionary_set_data(request, "self-worker-stack", &stack, sizeof(stack) - 1); break;
            case 14: ++stack.top; xpc_dictionary_set_data(request, "self-worker-stack", &stack, sizeof(stack)); break;
            case 15: stack.top = stack.bottom; xpc_dictionary_set_data(request, "self-worker-stack", &stack, sizeof(stack)); break;
            case 16: stack.vmFloor = stack.top; xpc_dictionary_set_data(request, "self-worker-stack", &stack, sizeof(stack)); break;
            case BadCheckInCount: break; // Geometry unavailable; normal authenticated check-in is still possible.
            default: abort();
        }
    } else {
        xpc_dictionary_set_int64(request, "target-pid", getppid());
        switch (variant) {
            case 0: xpc_dictionary_set_value(request, "target-pid", NULL); break;
            case 1: xpc_dictionary_set_uint64(request, "target-pid", getppid()); break;
            case 2: xpc_dictionary_set_int64(request, "target-pid", -1); break;
            case 3: xpc_dictionary_set_int64(request, "target-pid", 0); break;
            case 4: xpc_dictionary_set_int64(request, "target-pid", (int64_t)INT_MAX + 1); break;
            case 5: xpc_dictionary_set_uint64(request, "request-id", 1); break;
            case 6: xpc_dictionary_set_int64(request, "error", EPERM); break;
            case 7: addFD(request, "extra"); break;
            case 8: xpc_dictionary_set_mach_send(request, "extra", identity->taskName); break;
            case 9: xpc_dictionary_set_data(request, "target-audit", &identity->audit, sizeof(identity->audit)); break;
            case 10: xpc_dictionary_set_uint64(request, "execution-role", 0); break;
            case 11: xpc_dictionary_set_uint64(request, "operation", 16); break;
            case 12: xpc_dictionary_set_uint64(request, "self-worker-stack", 0); break;
            default: abort();
        }
    }
    assert(!xpc_pipe_routine(pipe, request, &reply));
    assert(reply && xpc_dictionary_get_int64(reply, "error") == (checkin && variant == BadCheckInCount ? EAGAIN : EPROTO));
    xpc_release(reply); xpc_release(request); xpc_release(pipe);
    mach_port_deallocate(mach_task_self(), broker);
}

static int client(void) {
    alarm(30);
    LSBBootstrapReply bootstrap;
    int fds[LSB_MAX_FDS];
    assert(LSBTransportCheckIn(&bootstrap, fds) == EAGAIN);
    LSBPeerProcessIdentity identity;
    memset(&identity, 0xff, sizeof(identity));
    assert(LSBTransportCopyProcessIdentity(0, &identity) == EINVAL && !identity.taskName && !identity.audit.val[5]);
    assert(LSBTransportCopyProcessIdentity(-1, &identity) == EINVAL && !identity.taskName);
    assert(LSBTransportCopyProcessIdentity(getppid(), NULL) == EINVAL);
    assert(LSBTransportCopyChildIdentity(0, &identity) == EINVAL && !identity.taskName);
    assert(LSBTransportCopyChildIdentity(getppid(), NULL) == EINVAL);
    pid_t resolved = -1;
    assert(LSBTransportResolveNativeChildPID(0, &resolved) == EINVAL && !resolved);
    resolved = -1;
    assert(LSBTransportResolveNativeChildPID(-1, &resolved) == EINVAL && !resolved);
    assert(LSBTransportResolveNativeChildPID(getppid(), NULL) == EINVAL);
    for (unsigned logical = 0; logical < 2; ++logical) for (unsigned variant = 0; variant < ReplyCount; ++variant) {
        unsigned descriptors = descriptorCount();
        int result = logical ? LSBTransportCopyChildIdentity(getppid() + 100000, &identity) : LSBTransportCopyProcessIdentity(getppid(), &identity);
        int expected = variant <= Repeat ? 0 : variant == Missing ? ESRCH : variant == Denied ? EACCES :
            variant == EncodingFailure ? EINVAL : EPROTO;
        assert(result == expected);
        if (!result) {
            assert(identity.audit.val[5] == (unsigned)getppid() && LSBPeerProcessIdentityPID(&identity) == (unsigned)getppid());
            assert(!strcmp(identity.executablePath, "/Applications/SpringBoard.app/SpringBoard"));
            assert(references(identity.taskName) == 1);
            audit_token_t actual;
            mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
            assert(!task_info(identity.taskName, TASK_AUDIT_TOKEN, (task_info_t)&actual, &count));
            assert(!memcmp(&actual, &identity.audit, sizeof(actual)));
            mach_port_t held = identity.taskName;
            LSBPeerProcessIdentityDispose(&identity);
            mach_port_type_t type;
            assert(mach_port_type(mach_task_self(), held, &type) == KERN_INVALID_NAME);
            LSBPeerProcessIdentityDispose(&identity);
        } else {
            LSBPeerProcessIdentity empty = {0};
            assert(!memcmp(&identity, &empty, sizeof(identity)));
        }
        assert(descriptorCount() == descriptors);
    }
    for (unsigned variant = 0; variant < ResolveReplyCount; ++variant) {
        unsigned descriptors = descriptorCount();
        resolved = -1;
        int result = LSBTransportResolveNativeChildPID(getppid(), &resolved);
        int expected = variant <= ResolveDifferent ? 0 : variant == ResolveMissing ? ESRCH : variant == ResolveDenied ? EACCES : EPROTO;
        assert(result == expected);
        assert(resolved == (result ? 0 : getppid() + (variant == ResolveDifferent ? 100000 : 0)));
        assert(descriptorCount() == descriptors);
    }
    LSBPeerProcessIdentity self = selfIdentity();
    mach_port_urefs_t before = references(self.taskName);
    for (unsigned variant = 0; variant < BadRequestCount; ++variant) checkRequest(&self, variant, LSB_REQUEST_COPY_PROCESS_IDENTITY);
    for (unsigned variant = 0; variant < BadRequestCount; ++variant) checkRequest(&self, variant, LSB_REQUEST_RESOLVE_NATIVE_CHILD_PID);
    for (unsigned variant = 0; variant <= BadCheckInCount; ++variant) checkRequest(&self, variant, LSB_REQUEST_CHECKIN);
    assert(references(self.taskName) == before);
    LSBPeerProcessIdentityDispose(&self);
    assert(!LSBRequireSingleThread());
    puts("PeerProcessTransport client: native capability, full-audit validation, malformed replies and ownership passed");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--client")) return client();
    alarm(30);
    mach_port_t broker = MACH_PORT_NULL;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &broker));
    assert(!mach_port_insert_right(mach_task_self(), broker, broker, MACH_MSG_TYPE_MAKE_SEND));
    posix_spawnattr_t attributes;
    assert(!posix_spawnattr_init(&attributes));
    assert(!posix_spawnattr_set_registered_ports_np(&attributes, &broker, 1));
    char *arguments[] = {argv[0], "--client", NULL};
    pid_t child = 0;
    assert(!posix_spawn(&child, argv[0], NULL, &attributes, arguments, environ));
    posix_spawnattr_destroy(&attributes);
    LSBReceivedMessage message;
    assert(!LSBTransportReceive(broker, &message) && message.kind == LSB_REQUEST_CHECKIN);
    assert(message.audit.val[5] == (unsigned)child && !memcmp(&message.audit, &message.peerIdentity.audit, sizeof(message.audit)));
    assert(message.workerStack.bottom && message.workerStack.top > message.workerStack.bottom && message.workerStack.vmFloor <= message.workerStack.bottom);
    assert(LSBPeerProcessIdentityPID(&message.peerIdentity) == (unsigned)child && !message.peerIdentity.executablePath[0]);
    audit_token_t actual;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(message.peerIdentity.taskName, TASK_AUDIT_TOKEN, (task_info_t)&actual, &count));
    assert(!memcmp(&actual, &message.audit, sizeof(actual)));
    LSBPeerProcessIdentity childIdentity = message.peerIdentity;
    memset(&message.peerIdentity, 0, sizeof(message.peerIdentity)); // Explicit ownership move into this test broker snapshot.
    assert(!LSBTransportReply(&message, EAGAIN, 0, 0, NULL, NULL, NULL));
    LSBTransportDisposeMessage(&message);
    LSBPeerProcessIdentity self = selfIdentity();
    mach_port_urefs_t ownReferences = references(self.taskName), childReferences = references(childIdentity.taskName);
    for (unsigned logical = 0; logical < 2; ++logical) for (unsigned variant = 0; variant < ReplyCount; ++variant) {
        assert(!LSBTransportReceive(broker, &message) && message.kind == LSB_REQUEST_COPY_PROCESS_IDENTITY);
        assert(message.targetPID == getpid() + (logical ? 100000 : 0) && message.audit.val[5] == (unsigned)child);
        assert(!message.peerIdentity.taskName && !message.nativeServicePort && !message.ports.count);
        replyVariant(&message, &self, childIdentity.taskName, variant);
        LSBTransportDisposeMessage(&message);
        assert(references(self.taskName) == ownReferences && references(childIdentity.taskName) == childReferences);
    }
    for (unsigned variant = 0; variant < ResolveReplyCount; ++variant) {
        assert(!LSBTransportReceive(broker, &message) && message.kind == LSB_REQUEST_RESOLVE_NATIVE_CHILD_PID);
        assert(message.targetPID == getpid() && message.audit.val[5] == (unsigned)child);
        assert(!message.peerIdentity.taskName && !message.nativeServicePort && !message.ports.count);
        resolveReplyVariant(&message, childIdentity.taskName, variant);
        LSBTransportDisposeMessage(&message);
        assert(references(self.taskName) == ownReferences && references(childIdentity.taskName) == childReferences);
    }
    for (unsigned variant = 0; variant < 2 * BadRequestCount + BadCheckInCount; ++variant) {
        unsigned descriptors = descriptorCount();
        assert(LSBTransportReceive(broker, &message) == EPROTO && !message.object && !message.peerIdentity.taskName);
        assert(references(childIdentity.taskName) == childReferences);
        assert(descriptorCount() == descriptors);
    }
    assert(!LSBTransportReceive(broker, &message) && message.kind == LSB_REQUEST_CHECKIN);
    assert(!message.workerStack.bottom && !message.workerStack.top && !message.workerStack.vmFloor && message.audit.val[5] == (unsigned)child);
    assert(!LSBTransportReply(&message, EAGAIN, 0, 0, NULL, NULL, NULL));
    LSBTransportDisposeMessage(&message);
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    LSBPeerProcessIdentityDispose(&childIdentity);
    LSBPeerProcessIdentityDispose(&self);
    assert(!mach_port_mod_refs(mach_task_self(), broker, MACH_PORT_RIGHT_SEND, -1));
    assert(!mach_port_mod_refs(mach_task_self(), broker, MACH_PORT_RIGHT_RECEIVE, -1));
    puts("PeerProcessTransport parent: native CHECKIN snapshot, authenticated name right, strict requests and cleanup passed");
    return 0;
}
