#include "../ProcessSupport/SpawnTransport.h"
#include "../ProcessSupport/PrivateXPC.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// The client is built with the real guest transport and connection module; the server uses native libxpc and the production decoder.
// Both local executables use Mac libxpc in sim2host mode. Actual guest-library interoperability still requires the device test.
// No Mach-message interposer is linked into either executable.
extern char **environ;
extern int posix_spawnattr_set_registered_ports_np(posix_spawnattr_t *, mach_port_t *, uint32_t);
enum { ConcurrentRequests = 8, DescriptorCount = 16, MalformedRequests = 8, MalformedForkRequests = 13 };
static const audit_token_t forkPeer = {{1, 2, 3, 4, 5, 42, 7, 9}};
static LSBForkWireRange forkRange(unsigned i) {
    const uint64_t size = LSB_MAX_FORK_BYTES / LSB_MAX_FORK_RANGES;
    return (LSBForkWireRange){0x120000000ull + i * size, size};
}
#ifndef LSB_GUEST_RUNTIME
static audit_token_t selfAudit(void) {
    audit_token_t audit = {{0}};
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&audit, &count) && count == TASK_AUDIT_TOKEN_COUNT);
    return audit;
}

#endif

#ifdef LSB_GUEST_RUNTIME
#include "../Guest/libLiveSimSpawn/BrokerConnection.h"
__attribute__((constructor(100))) static void earlyBrokerCall(void) {
    xpc_object_t reply = (xpc_object_t)(uintptr_t)1;
    assert(LSBBrokerConnectionCall(MACH_PORT_NULL, NULL, &reply) == ENOTSUP && !reply);
    // Exercise production entry points too: they must reject before getenv, XPC allocation, pthread locks, or guest TLS are usable.
    mach_port_t port = (mach_port_t)1;
    assert(LSBTransportCopyBrokerPort(&port) == ENOTSUP && port == MACH_PORT_NULL);
    assert(LSBTransportKill(42, 0) == ENOTSUP);
    LSBProcessList *list = (LSBProcessList *)(uintptr_t)1;
    size_t size = 1;
    assert(LSBTransportListProcesses(&list, &size) == ENOTSUP && !list && !size);
    audit_token_t target = {{1, 2, 3, 4, 5, 42, 7, 9}};
    LSBForkWireRange range = forkRange(0);
    assert(LSBTransportOfferFork(&target, &range, 1) == ENOTSUP && LSBTransportAcceptFork(&target) == ENOTSUP);
    unsigned char *entitlements = (unsigned char *)(uintptr_t)1;
    size = 1;
    assert(LSBTransportCopyEntitlements(&target, 16, &entitlements, &size) == ENOTSUP && !entitlements && !size);
}
static mach_port_t broker;
static mach_port_t argumentPort;
static int argumentFD;
static unsigned descriptorCount(void) {
    unsigned count = 0;
    for (int fd = 0; fd < 256; ++fd) if (fcntl(fd, F_GETFD) >= 0) ++count;
    return count;
}
static mach_port_urefs_t sendReferences(mach_port_t port) {
    mach_port_urefs_t count = 0;
    assert(!mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, &count));
    return count;
}
static void validateInheritedBrokerPort(void) {
    mach_port_t copy = MACH_PORT_NULL;
    assert(LSBTransportCopyBrokerPort(NULL) == EINVAL);
    assert(!unsetenv(LSB_BROKER_PORT_ENV));
    assert(LSBTransportCopyBrokerPort(&copy) == ENOENT && copy == MACH_PORT_NULL);
    const char *invalid[] = {"", "0", "-1", "+1", " 1", "1x", "4294967295", "4294967296", "100000000000000000000000"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!setenv(LSB_BROKER_PORT_ENV, invalid[i], 1));
        assert(LSBTransportCopyBrokerPort(&copy) == EINVAL && copy == MACH_PORT_NULL);
    }
    mach_port_t receiveOnly;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &receiveOnly));
    char value[16]; snprintf(value, sizeof(value), "%u", receiveOnly);
    assert(!setenv(LSB_BROKER_PORT_ENV, value, 1));
    assert(LSBTransportCopyBrokerPort(&copy) == EACCES && copy == MACH_PORT_NULL);
    assert(!mach_port_mod_refs(mach_task_self(), receiveOnly, MACH_PORT_RIGHT_RECEIVE, -1));
    snprintf(value, sizeof(value), "%u", broker);
    assert(!setenv(LSB_BROKER_PORT_ENV, value, 1));
    mach_port_urefs_t before = sendReferences(broker);
    assert(!LSBTransportCopyBrokerPort(&copy) && copy == broker && sendReferences(broker) == before + 1);
    assert(!mach_port_deallocate(mach_task_self(), copy) && sendReferences(broker) == before);
}

static void *spawnWorker(void *context) {
    unsigned index = (unsigned)(uintptr_t)context;
    LSBSpawnPlan plan = {.version = LSB_PLAN_VERSION, .argc = 2, .fdCount = DescriptorCount};
    assert(!LSBPlanAddString(&plan, "/bin/df", &plan.path));
    assert(!LSBPlanAddString(&plan, "/", &plan.cwd));
    char name[32]; snprintf(name, sizeof(name), "connection-test-%u", index);
    assert(!LSBPlanAddString(&plan, name, &plan.argv[0]));
    char *large = malloc(40001); assert(large); memset(large, 'A' + (int)index, 40000); large[40000] = 0;
    assert(!LSBPlanAddString(&plan, large, &plan.argv[1])); free(large);
    int fds[LSB_MAX_FDS];
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) fds[i] = i < DescriptorCount ? argumentFD : -1;
    for (unsigned i = 0; i < DescriptorCount; ++i) plan.fds[i].number = 100 + (int)i;
    LSBSpawnPorts ports = {.registeredPresent = true, .count = 3, .registered = {argumentPort, MACH_PORT_NULL, argumentPort},
                           .bootstrapPresent = true, .bootstrapPort = argumentPort};
    pid_t pid = 0;
    assert(!LSBTransportSpawnWithPorts(&plan, fds, &ports, &pid) && pid == 1000 + (int)index);
    return NULL;
}
static void malformedRequests(void) {
    for (unsigned variant = 0; variant < MalformedRequests; ++variant) {
        xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0), reply = NULL;
        assert(request);
        xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION);
        xpc_dictionary_set_uint64(request, "kind", LSB_REQUEST_REBOOT_USERSPACE);
        xpc_dictionary_set_uint64(request, "request-id", 0);
        xpc_dictionary_set_int64(request, "error", 0);
        switch (variant) {
            case 0: xpc_dictionary_set_uint64(request, "kind", LSB_REQUEST_CHECKIN); break;
            case 1:
                xpc_dictionary_set_uint64(request, "kind", LSB_REQUEST_READY);
                xpc_dictionary_set_uint64(request, "request-id", 1);
                break;
            case 2: xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION - 1); break;
            case 3: xpc_dictionary_set_string(request, "kind", "kill"); break;
            case 4: xpc_dictionary_set_mach_send(request, "guest-broker-port", argumentPort); break;
            case 5: {
                xpc_object_t descriptor = xpc_fd_create(argumentFD); assert(descriptor);
                xpc_dictionary_set_value(request, "unexpected-fd", descriptor); xpc_release(descriptor);
                break;
            }
            case 6: xpc_dictionary_set_int64(request, "error", -1); break;
            case 7: xpc_dictionary_set_data(request, "unexpected-audit", &(audit_token_t){{0}}, sizeof(audit_token_t)); break;
        }
        assert(!LSBBrokerConnectionCall(broker, request, &reply) && reply);
        assert(xpc_dictionary_get_int64(reply, "error") == EPROTO);
        audit_token_t audit = {{0}}; xpc_dictionary_get_audit_token(reply, &audit);
        assert(audit.val[5] == (unsigned)getppid() && audit.val[7]);
        xpc_release(reply); xpc_release(request);
    }
}

static void forkTransport(void) {
    LSBForkWireRange ranges[LSB_MAX_FORK_RANGES + 1];
    for (unsigned i = 0; i <= LSB_MAX_FORK_RANGES; ++i) ranges[i] = forkRange(i);
    assert(LSBTransportOfferFork(&forkPeer, NULL, 1) == EINVAL && LSBTransportOfferFork(&forkPeer, ranges, 0) == EINVAL);
    assert(LSBTransportOfferFork(&forkPeer, ranges, LSB_MAX_FORK_RANGES + 1) == EINVAL && LSBTransportAcceptFork(NULL) == EINVAL);
    assert(!LSBTransportOfferFork(&forkPeer, ranges, LSB_MAX_FORK_RANGES));
    assert(LSBTransportAcceptFork(&forkPeer) == ESTALE);
    for (unsigned i = 0; i < MalformedForkRequests; ++i) {
        xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0), reply = NULL;
        audit_token_t audit = forkPeer;
        LSBForkWireRange pair[] = {forkRange(0), forkRange(1)};
        size_t length = sizeof(pair);
        uint64_t kind = LSB_REQUEST_FORK_OFFER;
        xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION);
        xpc_dictionary_set_uint64(request, "request-id", 0);
        xpc_dictionary_set_int64(request, "error", 0);
        switch (i) {
            case 0: audit.val[7] = 0; break;
            case 1: audit.val[5] = 0; break;
            case 2: length--; break;
            case 3: length = 0; break;
            case 4: pair[1].start = pair[0].start; break;
            case 5: pair[1].size = LSB_MAX_FORK_BYTES; break;
            case 6: pair[1].start = UINT64_MAX - 0x3fff; break;
            case 7: pair[0].size = 0; break;
            case 8: ++pair[0].start; break;
            case 9: kind = LSB_REQUEST_FORK_ACCEPT; break; // ranges on accept
            case 10: xpc_dictionary_set_mach_send(request, "extra-right", argumentPort); break;
            case 11: break; // oversized array
            case 12: break; // wrong type for a required field
        }
        xpc_dictionary_set_uint64(request, "kind", kind);
        xpc_dictionary_set_data(request, "target-audit", &audit, sizeof(audit));
        if (i == 11) xpc_dictionary_set_data(request, "fork-ranges", ranges, sizeof(ranges));
        else if (i == 12) xpc_dictionary_set_string(request, "fork-ranges", "address");
        else xpc_dictionary_set_data(request, "fork-ranges", pair, length);
        assert(!LSBBrokerConnectionCall(broker, request, &reply) && reply);
        assert(xpc_dictionary_get_count(reply) == 4 && xpc_dictionary_get_int64(reply, "error") == EPROTO);
        xpc_release(reply); xpc_release(request);
    }
}
int main(void) {
    alarm(30);
    assert(!LSBRequireSingleThread()); // Native pre-handoff IPC is covered separately; guest construction has not started workers yet.
    mach_port_array_t ports = NULL; mach_msg_type_number_t count = 0;
    assert(!mach_ports_lookup(mach_task_self(), &ports, &count) && count >= 1 && MACH_PORT_VALID(ports[0]));
    broker = ports[0];
    for (unsigned i = 1; i < count; ++i) if (MACH_PORT_VALID(ports[i])) assert(!mach_port_deallocate(mach_task_self(), ports[i]));
    assert(!vm_deallocate(mach_task_self(), (vm_address_t)ports, count * sizeof(*ports)));
    validateInheritedBrokerPort();
    mach_port_urefs_t inheritedReferences = sendReferences(broker);
    assert(!LSBTransportKill(42, 0)); // Lazy initialization through the actual production callBroker path.
    // The endpoint constructor consumes its input reference. A separate copy must keep our inherited caller reference alive.
    assert(sendReferences(broker) == inheritedReferences + 1); // Caller plus the persistent connection's endpoint.
    assert(!LSBTransportKill(42, 0) && sendReferences(broker) == inheritedReferences + 1);
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &argumentPort));
    assert(!mach_port_insert_right(mach_task_self(), argumentPort, argumentPort, MACH_MSG_TYPE_MAKE_SEND));
    char path[] = "/tmp/LSBBrokerConnection.XXXXXX";
    argumentFD = mkstemp(path); assert(argumentFD >= 0 && !unlink(path) && write(argumentFD, "payload", 7) == 7);
    unsigned descriptors = descriptorCount();
    pthread_t threads[ConcurrentRequests];
    for (unsigned i = 0; i < ConcurrentRequests; ++i) assert(!pthread_create(&threads[i], NULL, spawnWorker, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < ConcurrentRequests; ++i) assert(!pthread_join(threads[i], NULL));
    assert(descriptorCount() == descriptors && sendReferences(argumentPort) == 1);
    audit_token_t brokerAudit = {{0}};
    assert(!LSBTransportRegisterDebuggerPort(argumentPort, &brokerAudit));
    assert(brokerAudit.val[5] == (unsigned)getppid() && brokerAudit.val[7]);
    assert(sendReferences(argumentPort) == 1);
    mach_port_t task = MACH_PORT_NULL;
    assert(!LSBTransportCopyProcessTask(42, &task) && MACH_PORT_VALID(task));
    audit_token_t returned = {{0}}; count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(task, TASK_AUDIT_TOKEN, (task_info_t)&returned, &count));
    assert(!memcmp(&returned, &brokerAudit, sizeof(returned)) && !mach_port_deallocate(mach_task_self(), task));
    malformedRequests();
    forkTransport();
    bool signalCaller = true;
    assert(LSBTransportCheckTerminalIO(0, &signalCaller) == EINVAL && !signalCaller);
    assert(!LSBTransportCheckTerminalIO(LSB_TERMINAL_READ, &signalCaller) && signalCaller);
    assert(!LSBTransportCheckTerminalIO(LSB_TERMINAL_WRITE, &signalCaller) && !signalCaller);
    assert(LSBTransportCheckTerminalIO(LSB_TERMINAL_CONTROL, &signalCaller) == EIO && !signalCaller);
    assert(descriptorCount() == descriptors && sendReferences(argumentPort) == 1);
    // The server disconnects this request without a reply. Never reconnect/replay a possibly mutating request.
    assert(LSBTransportRebootUserspace() == EPIPE);
    for (unsigned i = 0; i < 3; ++i) assert(LSBTransportKill(42, 0) == EPIPE);
    assert(descriptorCount() == descriptors && sendReferences(argumentPort) == 1);
    close(argumentFD); mach_port_deallocate(mach_task_self(), argumentPort);
    mach_port_mod_refs(mach_task_self(), argumentPort, MACH_PORT_RIGHT_RECEIVE, -1);
    assert(!mach_port_deallocate(mach_task_self(), broker));
    puts("BrokerConnection client: production RPC, concurrent large graphs, FD/right ownership, audit replies and terminal disconnect passed");
    return 0;
}
#else
static _Atomic unsigned accepted, spawns, completed, malformed, outstanding;
static _Atomic pid_t clientPID;
static audit_token_t clientAudit;
static mach_port_t taskName;
static LSBReceivedMessage *delayedRequests[ConcurrentRequests];
static void receiveRequest(xpc_object_t event, xpc_connection_t peer, dispatch_queue_t queue) {
    if (xpc_get_type(event) != XPC_TYPE_DICTIONARY) return;
    LSBReceivedMessage *message = calloc(1, sizeof(*message)); assert(message);
    int error = LSBTransportReceiveConnection(event, peer, message);
    if (error) {
        assert(error == EPROTO && !message->object && !message->replyConnection);
        atomic_fetch_add(&malformed, 1); free(message); return;
    }
    assert(message->object && message->replyConnection);
    assert(message->audit.val[5] == (unsigned)atomic_load(&clientPID) && message->audit.val[7]);
    if (!clientAudit.val[5]) clientAudit = message->audit;
    assert(!memcmp(&message->audit, &clientAudit, sizeof(clientAudit)));
    if (message->kind == LSB_REQUEST_SPAWN) {
        unsigned index = UINT_MAX;
        assert(sscanf(message->plan.strings + message->plan.argv[0], "connection-test-%u", &index) == 1 && index < ConcurrentRequests);
        assert(message->plan.fdCount == DescriptorCount && message->plan.argc == 2);
        const char *large = message->plan.strings + message->plan.argv[1];
        assert(strlen(large) == 40000);
        for (unsigned i = 0; i < 40000; ++i) assert(large[i] == 'A' + (int)index);
        for (unsigned i = 0; i < DescriptorCount; ++i) {
            char bytes[7]; assert(pread(message->fds[i], bytes, sizeof(bytes), 0) == 7 && !memcmp(bytes, "payload", 7));
            assert(fcntl(message->fds[i], F_GETFD) & FD_CLOEXEC);
        }
        assert(message->ports.count == 3 && message->ports.registeredPresent && message->ports.bootstrapPresent);
        assert(message->ports.registered[0] && !message->ports.registered[1] &&
               message->ports.registered[0] == message->ports.registered[2] && message->ports.bootstrapPort == message->ports.registered[0]);
        assert(!delayedRequests[index]); delayedRequests[index] = message;
        atomic_fetch_add(&spawns, 1); atomic_fetch_add(&outstanding, 1);
        if (atomic_load(&spawns) == ConcurrentRequests) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC), queue, ^{
            // No caller receives a reply before all concurrent requests arrive. A client lock held across RPC would deadlock this test.
            for (unsigned i = 0; i < ConcurrentRequests; ++i) {
                LSBReceivedMessage *pending = delayedRequests[i]; assert(pending);
                assert(!LSBTransportReply(pending, 0, 1000 + (int)i, i + 1, NULL, NULL, NULL));
                assert(!pending->object && !pending->replyConnection && fcntl(pending->fds[0], F_GETFD) >= 0);
                LSBTransportDisposeMessage(pending); free(pending); delayedRequests[i] = NULL;
                atomic_fetch_sub(&outstanding, 1); atomic_fetch_add(&completed, 1);
            }
        });
        return;
    }
    if (message->kind == LSB_REQUEST_KILL) {
        assert(message->targetPID == 42 && !message->signalNumber);
        assert(!LSBTransportReply(message, 0, 42, 0, NULL, NULL, NULL));
    } else if (message->kind == LSB_REQUEST_CHECK_TERMINAL_IO) {
        int error = message->terminalAccess == LSB_TERMINAL_CONTROL ? EIO : 0;
        pid_t local = message->terminalAccess == LSB_TERMINAL_READ ? 42 : 0;
        assert(!LSBTransportReply(message, error, local, 0, NULL, NULL, NULL));
    } else if (message->kind == LSB_REQUEST_REGISTER_DEBUGGER_PORT) {
        assert(MACH_PORT_VALID(message->processDebuggerPort));
        assert(!LSBTransportReply(message, 0, 0, 0, NULL, NULL, NULL));
    } else if (message->kind == LSB_REQUEST_COPY_PROCESS_TASK) {
        audit_token_t audit = selfAudit();
        assert(!LSBTransportReplyProcessTask(message, 0, taskName, &audit));
    } else if (message->kind == LSB_REQUEST_FORK_OFFER || message->kind == LSB_REQUEST_FORK_ACCEPT) {
        bool offer = message->kind == LSB_REQUEST_FORK_OFFER;
        assert(!memcmp(&message->targetAudit, &forkPeer, sizeof(forkPeer)));
        assert(message->forkRangeCount == (offer ? LSB_MAX_FORK_RANGES : 0));
        for (unsigned i = 0; i < message->forkRangeCount; ++i) {
            LSBForkWireRange expected = forkRange(i);
            assert(!memcmp(&expected, &message->forkRanges[i], sizeof(expected)));
        }
        assert(!LSBTransportReply(message, offer ? 0 : ESTALE, 0, 0, NULL, NULL, NULL));
    } else {
        assert(message->kind == LSB_REQUEST_REBOOT_USERSPACE && atomic_load(&completed) == ConcurrentRequests);
        xpc_connection_cancel(peer);
    }
    LSBTransportDisposeMessage(message); free(message);
}
int main(int argc, char **argv) {
    assert(argc == 2); alarm(40);
    assert(!task_name_for_pid(mach_task_self(), getpid(), &taskName));
    dispatch_queue_t queue = dispatch_queue_create("test.broker.connection", DISPATCH_QUEUE_SERIAL);
    xpc_connection_t listener = xpc_connection_create(NULL, queue); assert(listener);
    xpc_connection_enable_sim2host_4sim(listener);
    xpc_connection_set_event_handler(listener, ^(xpc_object_t event) {
        if (xpc_get_type(event) != XPC_TYPE_CONNECTION) return;
        atomic_fetch_add(&accepted, 1);
        xpc_connection_t peer = (xpc_connection_t)event;
        xpc_connection_set_target_queue(peer, queue);
        xpc_connection_enable_sim2host_4sim(peer);
        xpc_connection_set_event_handler(peer, ^(xpc_object_t request) { receiveRequest(request, peer, queue); });
        xpc_connection_activate(peer);
    });
    xpc_connection_activate(listener);
    xpc_endpoint_t endpoint = xpc_endpoint_create(listener); assert(endpoint);
    mach_port_t port = xpc_endpoint_copy_listener_port_4sim(endpoint); assert(MACH_PORT_VALID(port));
    posix_spawnattr_t attributes; assert(!posix_spawnattr_init(&attributes));
    assert(!posix_spawnattr_set_registered_ports_np(&attributes, &port, 1));
    char *arguments[] = {argv[1], NULL}; pid_t child = 0;
    // Serialize launch bookkeeping before the listener starts accepting so the full kernel PID comparison is deterministic.
    dispatch_suspend(queue);
    assert(!posix_spawn(&child, argv[1], NULL, &attributes, arguments, environ));
    atomic_store(&clientPID, child); dispatch_resume(queue);
    posix_spawnattr_destroy(&attributes);
    int status = 0; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    dispatch_sync(queue, ^{ assert(atomic_load(&accepted) == 1 && atomic_load(&spawns) == ConcurrentRequests && !atomic_load(&outstanding)); });
    assert(atomic_load(&malformed) == MalformedRequests + MalformedForkRequests);
    xpc_connection_cancel(listener); xpc_release(endpoint); xpc_release(listener);
    mach_port_deallocate(mach_task_self(), port); mach_port_deallocate(mach_task_self(), taskName); dispatch_release(queue);
    puts("BrokerConnection server: sim2host endpoint, authenticated decoding, asynchronous ownership and forbidden startup requests passed");
    return 0;
}
#endif
