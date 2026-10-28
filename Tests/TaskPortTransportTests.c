#include "../ProcessSupport/SpawnTransport.c"
#include <assert.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
extern char **environ;
extern int posix_spawnattr_set_registered_ports_np(posix_spawnattr_t *, mach_port_t *, uint32_t);

enum { Valid, Denied, EncodingFailure, ErrorCapability, BadVersion, BadRequestID, BadPID, MissingAudit, MissingTask,
       ShortAudit, WrongAuditType, WrongTaskType, NullTask, NonTask, WrongAuditWord, LastAuditWord = WrongAuditWord + 7,
       ZeroGeneration, ExtraFD, ErrorData, Variants };
enum { CapturePresent, CaptureAbsent, CaptureMissingPort, CaptureMissingNonce, CaptureShortNonce, CaptureWrongNonce,
       CaptureWrongPort, CaptureNullPort, CaptureNativeRole, CaptureLaterFailure, CaptureVariants };
enum { MalformedRequests = 9 };
static const unsigned char captureNonce[LSB_TASK_CAPTURE_NONCE_BYTES] = {1, 2, 3, 4, 5, 6, 7, 8};

int LSBTestCopyBrokerPort(mach_port_t *port) {
    mach_port_array_t ports = NULL;
    mach_msg_type_number_t count = 0;
    if (mach_ports_lookup(mach_task_self(), &ports, &count)) return EIO;
    *port = count ? ports[0] : MACH_PORT_NULL;
    for (unsigned i = 1; i < count; ++i) if (MACH_PORT_VALID(ports[i])) mach_port_deallocate(mach_task_self(), ports[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)ports, count * sizeof(*ports));
    return MACH_PORT_VALID(*port) ? 0 : ENOENT;
}
static unsigned descriptorCount(void) {
    unsigned count = 0;
    for (int fd = 0; fd < 256; ++fd) if (fcntl(fd, F_GETFD) >= 0) ++count;
    return count;
}
static mach_port_urefs_t references(mach_port_t port) {
    mach_port_urefs_t count = 0;
    assert(!mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, &count));
    return count;
}
static void addFD(xpc_object_t object) {
    xpc_object_t fd = xpc_fd_create(STDERR_FILENO);
    assert(fd); xpc_dictionary_set_value(object, "unexpected-fd", fd); xpc_release(fd);
}
static void finishReply(LSBReceivedMessage *message, xpc_object_t reply) {
    assert(!xpc_pipe_routine_reply(reply));
    xpc_release(reply); xpc_release(message->object); message->object = NULL;
}
static void replyVariant(LSBReceivedMessage *message, unsigned variant, mach_port_t task, const audit_token_t *audit, mach_port_t nonTask) {
    if (variant <= ErrorCapability) {
        int error = variant == Denied || variant == ErrorCapability ? EPERM : 0;
        mach_port_t value = variant == EncodingFailure ? nonTask : variant == Denied ? MACH_PORT_NULL : task;
        assert(LSBTransportReplyProcessTask(message, -1, value, audit) == EINVAL && message->object);
        int result = LSBTransportReplyProcessTask(message, error, value, variant == Denied ? NULL : audit);
        assert(result == (variant == EncodingFailure || variant == ErrorCapability ? EINVAL : 0) && !message->object);
        return;
    }
    xpc_object_t reply = xpc_dictionary_create_reply(message->object);
    assert(reply);
    xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION + (variant == BadVersion));
    xpc_dictionary_set_uint64(reply, "request-id", variant == BadRequestID);
    xpc_dictionary_set_int64(reply, "error", variant == ErrorData ? EPERM : 0);
    xpc_dictionary_set_int64(reply, "pid", variant == ErrorData ? 0 : 42 + (variant == BadPID));
    audit_token_t target = *audit;
    if (variant >= WrongAuditWord && variant <= LastAuditWord) ++target.val[variant - WrongAuditWord];
    if (variant == ZeroGeneration) target.val[7] = 0;
    if (variant != MissingAudit) xpc_dictionary_set_data(reply, "target-audit", &target, sizeof(target) - (variant == ShortAudit));
    if (variant == WrongAuditType) xpc_dictionary_set_string(reply, "target-audit", "not an audit token");
    if (variant != MissingTask) xpc_dictionary_set_mach_send(reply, "process-task", variant == NullTask ? MACH_PORT_NULL : variant == NonTask ? nonTask : task);
    if (variant == WrongTaskType) xpc_dictionary_set_int64(reply, "process-task", task);
    if (variant == ExtraFD) addFD(reply);
    finishReply(message, reply);
}
static void replyCapture(LSBReceivedMessage *message, unsigned variant, mach_port_t broker, int statusFD, int wakeFD) {
    LSBSpawnPlan plan = {.version = LSB_PLAN_VERSION, .argc = 1};
    assert(!LSBPlanAddString(&plan, "/bin/df", &plan.path));
    assert(!LSBPlanAddString(&plan, "df", &plan.argv[0]));
    assert(!LSBPlanAddString(&plan, "/", &plan.cwd));
    int pid = (int)message->audit.val[5];
    if (variant <= CaptureAbsent) {
        LSBBootstrapMetadata metadata = {.guestPID = pid, .parentPID = getpid(), .nativeProductionStatus = LSB_NATIVE_DEVELOPMENT};
        if (variant == CapturePresent) {
            metadata.taskCapture.port = broker;
            memcpy(metadata.taskCapture.nonce, captureNonce, sizeof(captureNonce));
        }
        assert(!LSBTransportReplyBootstrapWithMetadata(message, 0, pid, 123, "/RuntimeRoot", &plan, NULL, statusFD, wakeFD, &metadata));
        return;
    }
    xpc_object_t reply = xpc_dictionary_create_reply(message->object);
    assert(reply && !encodePlan(reply, &plan, NULL));
    xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(reply, "request-id", 123);
    xpc_dictionary_set_int64(reply, "error", 0);
    xpc_dictionary_set_int64(reply, "pid", pid);
    xpc_dictionary_set_int64(reply, "guest-pid", pid);
    xpc_dictionary_set_int64(reply, "parent-pid", getpid());
    xpc_dictionary_set_uint64(reply, "bootstrap-version", LSB_BOOTSTRAP_VERSION);
    xpc_dictionary_set_uint64(reply, "execution-role", variant == CaptureNativeRole ? LSB_EXECUTION_NATIVE_METAL_SERVICE : LSB_EXECUTION_SIMULATOR);
    xpc_dictionary_set_uint64(reply, "native-production-status", LSB_NATIVE_DEVELOPMENT);
    if (variant != CaptureLaterFailure) xpc_dictionary_set_string(reply, "runtime-root", "/RuntimeRoot");
    xpc_object_t status = xpc_fd_create(statusFD), wake = xpc_fd_create(wakeFD);
    assert(status && wake);
    xpc_dictionary_set_value(reply, "child-status-fd", status); xpc_dictionary_set_value(reply, "child-wake-fd", wake);
    xpc_release(status); xpc_release(wake);
    if (variant != CaptureMissingPort) xpc_dictionary_set_mach_send(reply, "task-capture-port", variant == CaptureNullPort ? MACH_PORT_NULL : broker);
    if (variant != CaptureMissingNonce) xpc_dictionary_set_data(reply, "task-capture-nonce", captureNonce, sizeof(captureNonce) - (variant == CaptureShortNonce));
    if (variant == CaptureWrongPort) xpc_dictionary_set_uint64(reply, "task-capture-port", broker);
    if (variant == CaptureWrongNonce) xpc_dictionary_set_string(reply, "task-capture-nonce", "bad nonce");
    finishReply(message, reply);
}
static void malformedRequest(unsigned variant) {
    mach_port_t port = MACH_PORT_NULL;
    assert(!LSBTestCopyBrokerPort(&port));
    xpc_object_t pipe = xpc_pipe_create_from_port(port, 0), request = createRequest(LSB_REQUEST_COPY_PROCESS_TASK, 0, 0), reply = NULL;
    assert(pipe && request);
    xpc_dictionary_set_int64(request, "target-pid", 42);
    switch (variant) {
        case 0: xpc_dictionary_set_int64(request, "target-pid", 0); break;
        case 1: xpc_dictionary_set_uint64(request, "target-pid", 42); break;
        case 2: xpc_dictionary_set_int64(request, "target-pid", (int64_t)INT_MAX + 1); break;
        case 3: addFD(request); break;
        case 4: xpc_dictionary_set_mach_send(request, "process-task", port); break;
        case 5: xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION - 1); break;
        case 6: xpc_dictionary_set_uint64(request, "request-id", 1); break;
        case 7: xpc_dictionary_set_mach_send(request, "task-capture-port", port); break;
        case 8: xpc_dictionary_set_data(request, "task-capture-nonce", captureNonce, sizeof(captureNonce)); break;
    }
    if (variant >= 7) { // Bootstrap offers are forbidden on otherwise valid READY messages too, not only on the strict task-query schema.
        xpc_dictionary_set_uint64(request, "kind", LSB_REQUEST_READY);
        xpc_dictionary_set_uint64(request, "request-id", 1);
        xpc_dictionary_set_value(request, "target-pid", NULL);
    }
    assert(!xpc_pipe_routine(pipe, request, &reply) && reply && xpc_dictionary_get_int64(reply, "error") == EPROTO);
    xpc_release(reply); xpc_release(request); xpc_release(pipe); mach_port_deallocate(mach_task_self(), port);
}
static int client(void) {
    mach_port_t task = 123, held = MACH_PORT_NULL;
    assert(LSBTransportCopyProcessTask(42, NULL) == EINVAL);
    assert(LSBTransportCopyProcessTask(0, &task) == EINVAL && !task);
    assert(!LSBTransportCopyProcessTask(42, &held)); // Keep one reference so later failed imports have a measurable baseline.
    for (unsigned variant = 0; variant < Variants; ++variant) {
        unsigned descriptors = descriptorCount(); mach_port_urefs_t refs = references(held);
        task = 123;
        int result = LSBTransportCopyProcessTask(42, &task);
        int expected = variant == Valid ? 0 : variant == Denied ? EPERM : variant == EncodingFailure || variant == ErrorCapability ? EINVAL : EPROTO;
        if (result != expected) fprintf(stderr, "Task reply variant %u: got %d expected %d\n", variant, result, expected);
        assert(result == expected);
        if (!result) {
            assert(task == held && references(held) == refs + 1);
            assert(!mach_port_deallocate(mach_task_self(), task));
        } else assert(!task);
        assert(references(held) == refs && descriptorCount() == descriptors);
    }
    assert(!mach_port_deallocate(mach_task_self(), held));
    mach_port_t broker = MACH_PORT_NULL;
    assert(!LSBTestCopyBrokerPort(&broker));
    for (unsigned variant = 0; variant < CaptureVariants; ++variant) {
        unsigned descriptors = descriptorCount(); mach_port_urefs_t refs = references(broker);
        LSBBootstrapReply reply; int fds[LSB_MAX_FDS];
        int error = LSBTransportCheckIn(&reply, fds);
        assert(error == (variant <= CaptureAbsent ? 0 : EPROTO));
        if (!error) {
            assert(reply.taskCapture.port == (variant == CapturePresent ? broker : MACH_PORT_NULL));
            if (reply.taskCapture.port) {
                assert(!memcmp(reply.taskCapture.nonce, captureNonce, sizeof(captureNonce)));
                assert(references(broker) == refs + 1);
                assert(!mach_port_deallocate(mach_task_self(), reply.taskCapture.port));
            }
            close(reply.statusFD); close(reply.wakeFD);
        } else {
            assert(!reply.taskCapture.port && reply.statusFD == -1 && reply.wakeFD == -1);
            for (unsigned i = 0; i < sizeof(reply.taskCapture.nonce); ++i) assert(!reply.taskCapture.nonce[i]);
        }
        assert(references(broker) == refs && descriptorCount() == descriptors);
    }
    assert(!mach_port_deallocate(mach_task_self(), broker));
    for (unsigned variant = 0; variant < MalformedRequests; ++variant) malformedRequest(variant);
    assert(!LSBRequireSingleThread());
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--client")) return client();
    alarm(30);
    mach_port_t broker = MACH_PORT_NULL, task = MACH_PORT_NULL, nonTask = mach_host_self();
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &broker));
    assert(!mach_port_insert_right(mach_task_self(), broker, broker, MACH_MSG_TYPE_MAKE_SEND));
    // A movable name right tests transport identity/ownership. Kernel-exception capture separately establishes control-right provenance.
    assert(!task_name_for_pid(mach_task_self(), getpid(), &task));
    audit_token_t audit; mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(task, TASK_AUDIT_TOKEN, (task_info_t)&audit, &count));
    LSBTaskPortCaptureOffer offer = {0};
    assert(!validateTaskCapture(NULL, LSB_EXECUTION_SIMULATOR) && !validateTaskCapture(&offer, LSB_EXECUTION_NATIVE_METAL_SERVICE));
    offer.nonce[0] = 1; assert(validateTaskCapture(&offer, LSB_EXECUTION_SIMULATOR) == EINVAL);
    offer.port = broker; assert(!validateTaskCapture(&offer, LSB_EXECUTION_SIMULATOR));
    assert(validateTaskCapture(&offer, LSB_EXECUTION_NATIVE_METAL_SERVICE) == EINVAL);
    offer.port = MACH_PORT_DEAD; assert(validateTaskCapture(&offer, LSB_EXECUTION_SIMULATOR) == EINVAL);
    char path[] = "/tmp/LSBTaskCapture.XXXXXX";
    int statusFD = mkstemp(path), wake[2];
    assert(statusFD >= 0 && !unlink(path) && !ftruncate(statusFD, LSB_CHILD_STATUS_MAP_SIZE));
    assert(!pipe(wake) && !fcntl(wake[0], F_SETFL, O_NONBLOCK));
    posix_spawnattr_t attributes;
    assert(!posix_spawnattr_init(&attributes) && !posix_spawnattr_set_registered_ports_np(&attributes, &broker, 1));
    char *arguments[] = {argv[0], "--client", NULL}; pid_t child = 0;
    assert(!posix_spawn(&child, argv[0], NULL, &attributes, arguments, environ));
    posix_spawnattr_destroy(&attributes);
    for (unsigned i = 0; i <= Variants; ++i) {
        LSBReceivedMessage message;
        assert(!LSBTransportReceive(broker, &message) && message.kind == LSB_REQUEST_COPY_PROCESS_TASK && message.targetPID == 42);
        assert(message.audit.val[5] == (unsigned)child && !message.requestID && !message.peerIdentity.taskName);
        replyVariant(&message, i ? i - 1 : Valid, task, &audit, nonTask);
        LSBTransportDisposeMessage(&message);
    }
    for (unsigned variant = 0; variant < CaptureVariants; ++variant) {
        LSBReceivedMessage message;
        assert(!LSBTransportReceive(broker, &message) && message.kind == LSB_REQUEST_CHECKIN);
        replyCapture(&message, variant, broker, statusFD, wake[0]);
        LSBTransportDisposeMessage(&message);
    }
    for (unsigned variant = 0; variant < MalformedRequests; ++variant) {
        unsigned descriptors = descriptorCount(); mach_port_urefs_t refs = references(broker);
        LSBReceivedMessage message;
        assert(LSBTransportReceive(broker, &message) == EPROTO && !message.object);
        assert(descriptorCount() == descriptors && references(broker) == refs);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    close(statusFD); close(wake[0]); close(wake[1]);
    assert(!mach_port_deallocate(mach_task_self(), task));
    assert(!mach_port_deallocate(mach_task_self(), nonTask));
    assert(!mach_port_deallocate(mach_task_self(), broker));
    assert(!mach_port_mod_refs(mach_task_self(), broker, MACH_PORT_RIGHT_RECEIVE, -1));
    puts("TaskPortTransport: strict task/audit replies, send-right ownership, paired simulator-only bootstrap offer and malformed requests passed");
    return 0;
}
