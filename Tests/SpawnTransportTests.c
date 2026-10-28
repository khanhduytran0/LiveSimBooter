#include "../ProcessSupport/SpawnTransport.h"
#include "../ProcessSupport/PrivateXPC.h"
#include "../ProcessSupport/ChildStatus.h"
#include "../ProcessSupport/GuestEntitlements.h"
#if defined(LSB_XPC_WIRE_TESTING)
#include "../Guest/XPCWireCompatibility.h"
#endif

#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
extern int posix_spawnattr_set_registered_ports_np(posix_spawnattr_t *, mach_port_t *, uint32_t);
static unsigned openDescriptorCount(void);
enum { MalformedPortsCount = 10, MalformedIdentityCount = 12, MalformedRoleCount = 18, MalformedRoleRequestCount = 24,
       MalformedBootstrapCount = 14 + MalformedPortsCount + MalformedIdentityCount + MalformedRoleCount,
       MalformedKillCount = 14, MalformedAuditCount = 16, MalformedEntitlementCount = 25, PortRoundTripCount = 7,
       MalformedRebootCount = 30, RebootReplyCount = 23 };
static volatile sig_atomic_t childSignals;
static volatile sig_atomic_t signalAcknowledgementFD = -1;

static void childSignal(int number) {
    int savedErrno = errno;
    if (number == SIGCHLD) {
        ++childSignals;
        // An async-safe acknowledgement proves the handler ran while the broker still withheld the KILL reply.
        (void)pwrite(signalAcknowledgementFD, "s", 1, 6);
    }
    errno = savedErrno;
}

int LSBTestCopyBrokerPort(mach_port_t *port) {
    mach_port_array_t ports = NULL;
    mach_msg_type_number_t count = 0;
    if (mach_ports_lookup(mach_task_self(), &ports, &count)) return EIO;
    *port = count ? ports[0] : MACH_PORT_NULL;
    for (unsigned i = 1; i < count; ++i) if (MACH_PORT_VALID(ports[i])) mach_port_deallocate(mach_task_self(), ports[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)ports, count * sizeof(*ports));
    return MACH_PORT_VALID(*port) ? 0 : ENOENT;
}

static void readyUsingHeldPort(uint64_t requestID) {
    mach_port_t broker = MACH_PORT_NULL, bootstrap = MACH_PORT_NULL, receiveOnly = MACH_PORT_NULL;
    mach_port_array_t registered = NULL;
    mach_msg_type_number_t registeredCount = 0;
    assert(!LSBTestCopyBrokerPort(&broker));
    assert(!mach_ports_lookup(mach_task_self(), &registered, &registeredCount));
    assert(!task_get_bootstrap_port(mach_task_self(), &bootstrap));
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &receiveOnly));
    assert(LSBTransportReadyOnPort(MACH_PORT_NULL, requestID, 0) == EINVAL);
    assert(LSBTransportReadyOnPort(MACH_PORT_DEAD, requestID, 0) == EINVAL);
    assert(LSBTransportReadyOnPort(receiveOnly, requestID, 0) == EINVAL);
    assert(LSBTransportReadyOnPort(broker, 0, 0) == EINVAL);
    assert(LSBTransportReadyOnPort(broker, requestID, -1) == EINVAL);
    assert(!mach_port_mod_refs(mach_task_self(), receiveOnly, MACH_PORT_RIGHT_RECEIVE, -1));
    const char *environment = getenv(LSB_BROKER_PORT_ENV);
    char *savedEnvironment = environment ? strdup(environment) : NULL;
    assert(!environment || savedEnvironment);
    assert(!unsetenv(LSB_BROKER_PORT_ENV));
    assert(!mach_ports_register(mach_task_self(), NULL, 0));
    assert(!task_set_bootstrap_port(mach_task_self(), MACH_PORT_NULL));
    mach_port_t unavailable = MACH_PORT_NULL;
    assert(LSBTransportCopyBrokerPort(&unavailable) == ENOENT && unavailable == MACH_PORT_NULL);
    assert(LSBTransportRebootUserspace() == ENOENT);
    mach_port_urefs_t before, after;
    assert(!mach_port_get_refs(mach_task_self(), broker, MACH_PORT_RIGHT_SEND, &before));
    assert(!LSBTransportReadyOnPort(broker, requestID, 0));
    assert(!mach_port_get_refs(mach_task_self(), broker, MACH_PORT_RIGHT_SEND, &after) && after == before);
    assert(!task_set_bootstrap_port(mach_task_self(), bootstrap));
    assert(!mach_ports_register(mach_task_self(), registered, registeredCount));
    if (savedEnvironment) { assert(!setenv(LSB_BROKER_PORT_ENV, savedEnvironment, 1)); free(savedEnvironment); }
    for (unsigned i = 0; i < registeredCount; ++i) if (MACH_PORT_VALID(registered[i]))
        assert(!mach_port_deallocate(mach_task_self(), registered[i]));
    assert(!vm_deallocate(mach_task_self(), (vm_address_t)registered, registeredCount * sizeof(*registered)));
    if (MACH_PORT_VALID(bootstrap)) assert(!mach_port_deallocate(mach_task_self(), bootstrap));
    assert(!mach_port_deallocate(mach_task_self(), broker));
}

static LSBSpawnPlan makePlan(void) {
    LSBSpawnPlan plan = {.version = LSB_PLAN_VERSION, .argc = 2, .envc = 1, .fdCount = 1, .creationMask = 022};
    assert(!LSBPlanAddString(&plan, "/bin/df", &plan.path));
    plan.argv[0] = plan.path;
    assert(!LSBPlanAddString(&plan, "-h", &plan.argv[1]));
    assert(!LSBPlanAddString(&plan, "/", &plan.cwd));
    assert(!LSBPlanAddString(&plan, "PATH=/bin:/usr/bin", &plan.envp[0]));
    plan.fds[0].number = 7;
    plan.fds[0].flags = FD_CLOEXEC;
    assert(!LSBValidateSpawnPlan(&plan));
    return plan;
}

static void setWirePorts(xpc_object_t dictionary, mach_port_t port, int malformedVariant) {
    xpc_dictionary_set_mach_send(dictionary, "bootstrap-port", port);
    if (malformedVariant == 0) { xpc_dictionary_set_string(dictionary, "registered-ports", "not an array"); return; }
    if (malformedVariant == 6) { xpc_dictionary_set_uint64(dictionary, "registered-ports", port); return; }
    xpc_object_t array = xpc_array_create(NULL, 0);
    assert(array);
    xpc_array_set_mach_send(array, XPC_ARRAY_APPEND, port);
    if (malformedVariant == 1) {
        for (unsigned i = 0; i < LSB_MAX_REGISTERED_PORTS; ++i) xpc_array_set_mach_send(array, XPC_ARRAY_APPEND, port);
    } else if (malformedVariant == 2) xpc_array_set_string(array, XPC_ARRAY_APPEND, "wrong type after a valid right");
    else if (malformedVariant == 3) {
        xpc_object_t null = xpc_null_create();
        assert(null);
        xpc_array_append_value(array, null);
        xpc_release(null);
    } else if (malformedVariant == 4) {
        xpc_object_t descriptor = xpc_fd_create(STDERR_FILENO);
        assert(descriptor);
        xpc_array_append_value(array, descriptor);
        xpc_release(descriptor);
    } else if (malformedVariant == 5) xpc_array_set_uint64(array, XPC_ARRAY_APPEND, port);
    else {
        xpc_array_set_mach_send(array, XPC_ARRAY_APPEND, MACH_PORT_NULL);
        xpc_array_set_mach_send(array, XPC_ARRAY_APPEND, port);
    }
    xpc_dictionary_set_value(dictionary, "registered-ports", array);
    xpc_release(array);
    if (malformedVariant == 7) xpc_dictionary_set_string(dictionary, "bootstrap-port", "not a Mach send right");
    else if (malformedVariant == 8) xpc_dictionary_set_uint64(dictionary, "bootstrap-port", port);
    else if (malformedVariant == 9) {
        xpc_object_t descriptor = xpc_fd_create(STDERR_FILENO);
        assert(descriptor);
        xpc_dictionary_set_value(dictionary, "bootstrap-port", descriptor);
        xpc_release(descriptor);
    }
}

static void testMalformedPorts(const LSBSpawnPlan *plan, int fd, mach_port_t sentPort, unsigned variant) {
    mach_port_t port;
    assert(!LSBTestCopyBrokerPort(&port));
    xpc_object_t pipe = xpc_pipe_create_from_port(port, 0), request = xpc_dictionary_create(NULL, NULL, 0);
    assert(pipe && request);
    xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(request, "kind", variant % 2 ? LSB_REQUEST_EXEC : LSB_REQUEST_SPAWN);
    xpc_dictionary_set_uint64(request, "request-id", 0);
    xpc_dictionary_set_int64(request, "error", 0);
    xpc_dictionary_set_data(request, "plan", plan, sizeof(*plan));
    xpc_object_t fds = xpc_array_create(NULL, 0), descriptor = xpc_fd_create(fd);
    assert(fds && descriptor);
    xpc_array_append_value(fds, descriptor);
    xpc_dictionary_set_value(request, "fds", fds);
    xpc_release(descriptor);
    xpc_release(fds);
    setWirePorts(request, sentPort, variant);
    xpc_object_t reply = NULL;
    assert(!xpc_pipe_routine(pipe, request, &reply));
    assert(reply && xpc_dictionary_get_int64(reply, "error") == EPROTO);
    xpc_release(reply);
    xpc_release(request);
    xpc_release(pipe);
    assert(!mach_port_deallocate(mach_task_self(), port));
    mach_port_urefs_t references;
    assert(!mach_port_get_refs(mach_task_self(), sentPort, MACH_PORT_RIGHT_SEND, &references) && references == 1);
}

static void testRoleRequestInjection(const LSBSpawnPlan *plan, int fd, unsigned variant) {
    static const uint32_t kinds[] = { LSB_REQUEST_SPAWN, LSB_REQUEST_EXEC, LSB_REQUEST_CHECKIN,
        LSB_REQUEST_READY, LSB_REQUEST_KILL, LSB_REQUEST_RESOLVE_AUDIT_PID, LSB_REQUEST_COPY_ENTITLEMENTS, LSB_REQUEST_COPY_PROCESS_IDENTITY };
    assert(variant < MalformedRoleRequestCount);
    uint32_t kind = kinds[variant % (sizeof(kinds) / sizeof(kinds[0]))];
    mach_port_t port;
    assert(!LSBTestCopyBrokerPort(&port));
    xpc_object_t pipe = xpc_pipe_create_from_port(port, 0), request = xpc_dictionary_create(NULL, NULL, 0);
    assert(pipe && request);
    xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(request, "kind", kind);
    xpc_dictionary_set_uint64(request, "request-id", kind == LSB_REQUEST_READY ? 77 : 0);
    xpc_dictionary_set_int64(request, "error", 0);
    if (kind == LSB_REQUEST_SPAWN || kind == LSB_REQUEST_EXEC) {
        xpc_dictionary_set_data(request, "plan", plan, sizeof(*plan));
        xpc_object_t array = xpc_array_create(NULL, 0), descriptor = xpc_fd_create(fd);
        assert(array && descriptor);
        xpc_array_append_value(array, descriptor);
        xpc_dictionary_set_value(request, "fds", array);
        xpc_release(descriptor);
        xpc_release(array);
    }
    if (kind == LSB_REQUEST_KILL) {
        xpc_dictionary_set_int64(request, "target-pid", 4242);
        xpc_dictionary_set_int64(request, "signal", 0);
    }
    if (kind == LSB_REQUEST_RESOLVE_AUDIT_PID || kind == LSB_REQUEST_COPY_ENTITLEMENTS) {
        audit_token_t audit = {{0, 0, 0, 0, 0, 4242, 0, 1}};
        xpc_dictionary_set_data(request, "target-audit", &audit, sizeof(audit));
    }
    if (kind == LSB_REQUEST_COPY_ENTITLEMENTS) xpc_dictionary_set_uint64(request, "operation", 16);
    if (kind == LSB_REQUEST_COPY_PROCESS_IDENTITY) xpc_dictionary_set_int64(request, "target-pid", 4242);
    if (variant / 8 == 0) xpc_dictionary_set_uint64(request, "bootstrap-version", LSB_BOOTSTRAP_VERSION);
    else if (variant / 8 == 1) xpc_dictionary_set_uint64(request, "execution-role", LSB_EXECUTION_NATIVE_METAL_PROBE);
    else xpc_dictionary_set_uint64(request, "native-production-status", LSB_NATIVE_DEVELOPMENT);
    xpc_object_t reply = NULL;
    assert(!xpc_pipe_routine(pipe, request, &reply));
    assert(reply && xpc_dictionary_get_int64(reply, "error") == EPROTO);
    xpc_release(reply);
    xpc_release(request);
    xpc_release(pipe);
    assert(!mach_port_deallocate(mach_task_self(), port));
}

static LSBSpawnPorts orderedPorts(mach_port_t first, mach_port_t second, unsigned variant) {
    LSBSpawnPorts ports = {.registeredPresent = variant < 4, .count = variant < 3 ? 3 : 0,
                           .bootstrapPresent = variant < 3 || variant == 4};
    if (variant == 0) { ports.registered[0] = first; ports.registered[2] = second; }
    else if (variant == 1) { ports.registered[0] = second; ports.registered[1] = ports.registered[2] = first; }
    if (variant == 0) ports.bootstrapPort = first;
    else if (variant == 1 || variant == 4) ports.bootstrapPort = second;
    return ports;
}

static void checkKernelRegistration(const LSBSpawnPorts *ports) {
    mach_port_array_t saved = NULL, imported = NULL;
    mach_msg_type_number_t savedCount = 0, count = 0;
    if (ports->registeredPresent) {
        assert(!mach_ports_lookup(mach_task_self(), &saved, &savedCount));
        assert(!mach_ports_register(mach_task_self(), (mach_port_t *)ports->registered, ports->count));
        assert(!mach_ports_lookup(mach_task_self(), &imported, &count) && count == LSB_MAX_REGISTERED_PORTS);
        for (unsigned i = 0; i < count; ++i) assert(imported[i] == ports->registered[i]);
        assert(!mach_ports_register(mach_task_self(), saved, savedCount));
        for (unsigned i = 0; i < savedCount; ++i) if (MACH_PORT_VALID(saved[i])) assert(!mach_port_deallocate(mach_task_self(), saved[i]));
        for (unsigned i = 0; i < count; ++i) if (MACH_PORT_VALID(imported[i])) assert(!mach_port_deallocate(mach_task_self(), imported[i]));
        assert(!vm_deallocate(mach_task_self(), (vm_address_t)saved, savedCount * sizeof(*saved)));
        assert(!vm_deallocate(mach_task_self(), (vm_address_t)imported, count * sizeof(*imported)));
    }
    if (ports->bootstrapPresent) {
        mach_port_t savedBootstrap = MACH_PORT_NULL, importedBootstrap = MACH_PORT_NULL;
        assert(!task_get_bootstrap_port(mach_task_self(), &savedBootstrap));
        assert(!task_set_bootstrap_port(mach_task_self(), ports->bootstrapPort));
        assert(!task_get_bootstrap_port(mach_task_self(), &importedBootstrap) && importedBootstrap == ports->bootstrapPort);
        assert(!task_set_bootstrap_port(mach_task_self(), savedBootstrap));
        if (MACH_PORT_VALID(savedBootstrap)) assert(!mach_port_deallocate(mach_task_self(), savedBootstrap));
        if (MACH_PORT_VALID(importedBootstrap)) assert(!mach_port_deallocate(mach_task_self(), importedBootstrap));
    }
}

static void registeredPortsClient(const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS]) {
    mach_port_t first, second, receiveOnly;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &first));
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &second));
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &receiveOnly));
    assert(!mach_port_insert_right(mach_task_self(), first, first, MACH_MSG_TYPE_MAKE_SEND));
    assert(!mach_port_insert_right(mach_task_self(), second, second, MACH_MSG_TYPE_MAKE_SEND));
    LSBSpawnPorts invalid = {.registeredPresent = true, .count = LSB_MAX_REGISTERED_PORTS + 1};
    pid_t pid = 1;
    assert(LSBTransportSpawnWithPorts(plan, fds, &invalid, &pid) == EINVAL && pid == 0);
    invalid.count = 0;
    invalid.registered[2] = first;
    assert(LSBTransportSpawnWithPorts(plan, fds, &invalid, &pid) == EINVAL && pid == 0);
    invalid = (LSBSpawnPorts){.registeredPresent = true, .count = 1, .registered = {MACH_PORT_DEAD}};
    assert(LSBTransportSpawnWithPorts(plan, fds, &invalid, &pid) == EBADF && pid == 0);
    invalid.registered[0] = receiveOnly;
    assert(LSBTransportSpawnWithPorts(plan, fds, &invalid, &pid) == EBADF && pid == 0);
    invalid = (LSBSpawnPorts){.bootstrapPresent = true, .bootstrapPort = receiveOnly};
    assert(LSBTransportExecWithPorts(plan, fds, &invalid) == EBADF);
    invalid.bootstrapPort = MACH_PORT_DEAD;
    assert(LSBTransportExecWithPorts(plan, fds, &invalid) == EBADF);
    invalid = (LSBSpawnPorts){.count = 1, .registered = {first}};
    assert(LSBTransportExecWithPorts(plan, fds, &invalid) == EINVAL);
    invalid = (LSBSpawnPorts){.bootstrapPort = first};
    assert(LSBTransportExecWithPorts(plan, fds, &invalid) == EINVAL);
    assert(!mach_port_mod_refs(mach_task_self(), receiveOnly, MACH_PORT_RIGHT_RECEIVE, -1));
    for (unsigned variant = 0; variant < PortRoundTripCount; ++variant) {
        LSBSpawnPorts ports = orderedPorts(first, second, variant);
        if (variant % 2) assert(!LSBTransportExecWithPorts(plan, fds, &ports));
        else assert(!LSBTransportSpawnWithPorts(plan, fds, &ports, &pid) && pid == 4242);
        mach_port_urefs_t references;
        assert(!mach_port_get_refs(mach_task_self(), first, MACH_PORT_RIGHT_SEND, &references) && references == 1);
        assert(!mach_port_get_refs(mach_task_self(), second, MACH_PORT_RIGHT_SEND, &references) && references == 1);
        LSBBootstrapReply reply;
        int received[LSB_MAX_FDS];
        assert(!LSBTransportCheckIn(&reply, received) && reply.requestID == 80 + variant);
        assert(reply.ports.registeredPresent == ports.registeredPresent && reply.ports.count == ports.count);
        assert(!memcmp(reply.ports.registered, ports.registered, sizeof(ports.registered)));
        assert(reply.ports.bootstrapPresent == ports.bootstrapPresent && reply.ports.bootstrapPort == ports.bootstrapPort);
        assert(reply.pid == 4242);
        assert(reply.guestPID == (variant % 2 ? variant == 5 ? INT_MAX : 1234 + (int)variant : 4242));
        assert(reply.parentPID == (variant == 6 || variant % 2 ? variant == 1 ? 0 : 4567 : getppid()));
        assert(reply.executionRole == (variant == 6 ? LSB_EXECUTION_NATIVE_METAL_PROBE : LSB_EXECUTION_SIMULATOR));
        assert(reply.nativeProductionStatus == (variant == 1 ? LSB_NATIVE_DEVELOPMENT : variant == 3 ? LSB_NATIVE_PRODUCTION : LSB_NATIVE_PRODUCTION_UNKNOWN));
        assert(reply.launchdTankPort == (variant == 1 ? second : MACH_PORT_NULL));
        unsigned firstCopies = ports.bootstrapPort == first, secondCopies = ports.bootstrapPort == second;
        firstCopies += reply.launchdTankPort == first;
        secondCopies += reply.launchdTankPort == second;
        for (unsigned i = 0; i < ports.count; ++i) {
            firstCopies += ports.registered[i] == first;
            secondCopies += ports.registered[i] == second;
        }
        assert(!mach_port_get_refs(mach_task_self(), first, MACH_PORT_RIGHT_SEND, &references) && references == 1 + firstCopies);
        assert(!mach_port_get_refs(mach_task_self(), second, MACH_PORT_RIGHT_SEND, &references) && references == 1 + secondCopies);
        checkKernelRegistration(&reply.ports);
        if (MACH_PORT_VALID(reply.launchdTankPort)) assert(!mach_port_deallocate(mach_task_self(), reply.launchdTankPort));
        LSBSpawnPortsDispose(&reply.ports);
        LSBSpawnPortsDispose(&reply.ports);
        assert(!reply.ports.count && !reply.ports.registeredPresent && !reply.ports.bootstrapPresent && !reply.ports.bootstrapPort);
        for (unsigned i = 0; i < LSB_MAX_REGISTERED_PORTS; ++i) assert(reply.ports.registered[i] == MACH_PORT_NULL);
        assert(!mach_port_get_refs(mach_task_self(), first, MACH_PORT_RIGHT_SEND, &references) && references == 1);
        assert(!mach_port_get_refs(mach_task_self(), second, MACH_PORT_RIGHT_SEND, &references) && references == 1);
        close(received[0]);
        close(reply.statusFD);
        close(reply.wakeFD);
        for (unsigned target = 0; target < 2; ++target) {
            mach_port_t port = target ? second : first;
            for (unsigned i = 0; i < ports.count; ++i) if (ports.registered[i] == port) {
                struct { mach_msg_header_t header; mach_msg_max_trailer_t trailer; } ping;
                assert(!mach_msg(&ping.header, MACH_RCV_MSG, 0, sizeof(ping), port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL));
                assert(ping.header.msgh_id == (int)(900 + 10 * variant + i));
            }
        }
        assert(!LSBTransportReady(80 + variant, 0));
    }
    assert(LSBTransportExecWithPorts(plan, fds, NULL) == ENOTSUP);
    assert(LSBTransportExecWithPorts(plan, fds, NULL) == EPROTO);
    assert(!mach_port_deallocate(mach_task_self(), first) && !mach_port_deallocate(mach_task_self(), second));
    assert(!mach_port_mod_refs(mach_task_self(), first, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(!mach_port_mod_refs(mach_task_self(), second, MACH_PORT_RIGHT_RECEIVE, -1));
}

static void testMalformedRequest(const LSBSpawnPlan *plan, unsigned variant) {
    mach_port_t port;
    assert(!LSBTestCopyBrokerPort(&port));
    xpc_object_t pipe = xpc_pipe_create_from_port(port, 0);
    assert(pipe);
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(request, "kind", LSB_REQUEST_SPAWN);
    xpc_dictionary_set_uint64(request, "request-id", 0);
    xpc_dictionary_set_int64(request, "error", 0);
    LSBSpawnPlan malformed = *plan;
    if (variant == 4) { malformed.fdCount = 2; malformed.fds[1].number = 8; }
    xpc_dictionary_set_data(request, "plan", &malformed, variant == 1 ? sizeof(*plan) - 1 : sizeof(*plan));
    xpc_object_t fds = xpc_array_create(NULL, 0);
    if (variant == 4) {
        xpc_object_t descriptor = xpc_fd_create(STDERR_FILENO);
        assert(descriptor);
        xpc_array_append_value(fds, descriptor);
        xpc_release(descriptor);
    }
    if (variant != 3) xpc_array_set_string(fds, XPC_ARRAY_APPEND, "not a descriptor");
    xpc_dictionary_set_value(request, "fds", fds);
    xpc_release(fds);
    if (variant == 0) xpc_dictionary_set_string(request, "kind", "not an integer");
    xpc_object_t reply = NULL;
    assert(!xpc_pipe_routine(pipe, request, &reply));
    assert(reply && xpc_dictionary_get_int64(reply, "error") == EPROTO);
    xpc_release(reply);
    xpc_release(request);
    xpc_release(pipe);
    mach_port_deallocate(mach_task_self(), port);
    assert(!LSBRequireSingleThread());
}

static void testMalformedKill(unsigned variant) {
    mach_port_t port;
    assert(!LSBTestCopyBrokerPort(&port));
    xpc_object_t pipe = xpc_pipe_create_from_port(port, 0);
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0);
    assert(pipe && request);
    xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(request, "kind", LSB_REQUEST_KILL);
    xpc_dictionary_set_uint64(request, "request-id", variant == 10 ? 77 : 0);
    xpc_dictionary_set_int64(request, "error", variant == 11 ? EACCES : 0);
    if (variant != 0) xpc_dictionary_set_int64(request, "target-pid", 4242);
    if (variant == 1) xpc_dictionary_set_uint64(request, "target-pid", 4242);
    if (variant == 2) xpc_dictionary_set_int64(request, "target-pid", (int64_t)INT_MAX + 1);
    if (variant == 3) xpc_dictionary_set_int64(request, "target-pid", (int64_t)INT_MIN - 1);
    if (variant != 4) xpc_dictionary_set_int64(request, "signal", SIGTERM);
    if (variant == 5) xpc_dictionary_set_uint64(request, "signal", SIGTERM);
    if (variant == 6) xpc_dictionary_set_int64(request, "signal", (int64_t)INT_MAX + 1);
    if (variant == 7) xpc_dictionary_set_int64(request, "signal", (int64_t)INT_MIN - 1);
    if (variant == 8) xpc_dictionary_set_data(request, "plan", "unexpected", 10);
    if (variant == 9) {
        xpc_object_t fds = xpc_array_create(NULL, 0), descriptor = xpc_fd_create(STDERR_FILENO);
        assert(fds && descriptor);
        xpc_array_append_value(fds, descriptor);
        xpc_dictionary_set_value(request, "fds", fds);
        xpc_release(descriptor);
        xpc_release(fds);
    }
    if (variant == 12) {
        xpc_object_t array = xpc_array_create(NULL, 0);
        xpc_array_set_mach_send(array, XPC_ARRAY_APPEND, port);
        xpc_dictionary_set_value(request, "registered-ports", array);
        xpc_release(array);
    }
    if (variant == 13) xpc_dictionary_set_mach_send(request, "bootstrap-port", port);
    xpc_object_t reply = NULL;
    assert(!xpc_pipe_routine(pipe, request, &reply));
    assert(reply && xpc_dictionary_get_int64(reply, "error") == EPROTO);
    xpc_release(reply);
    xpc_release(request);
    xpc_release(pipe);
    mach_port_deallocate(mach_task_self(), port);
    assert(!LSBRequireSingleThread());
}

static void setUnexpectedFD(xpc_object_t dictionary, const char *key, int fd) {
    xpc_object_t descriptor = xpc_fd_create(fd);
    assert(descriptor);
    xpc_dictionary_set_value(dictionary, key, descriptor);
    xpc_release(descriptor);
}

static void testMalformedReboot(unsigned variant, int fd, mach_port_t sentPort) {
    mach_port_t port;
    assert(!LSBTestCopyBrokerPort(&port));
    xpc_object_t pipe = xpc_pipe_create_from_port(port, 0), request = xpc_dictionary_create(NULL, NULL, 0);
    assert(pipe && request);
    xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(request, "kind", LSB_REQUEST_REBOOT_USERSPACE);
    xpc_dictionary_set_uint64(request, "request-id", 0);
    xpc_dictionary_set_int64(request, "error", 0);
    switch (variant) {
        case 0: xpc_dictionary_set_uint64(request, "version", 1); break; // Old producers cannot silently use the new operation.
        case 1: xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION + 1); break;
        case 2: xpc_dictionary_set_int64(request, "version", LSB_TRANSPORT_VERSION); break;
        case 3: xpc_dictionary_set_string(request, "version", "2"); break;
        case 4: xpc_dictionary_set_value(request, "version", NULL); break;
        case 5: xpc_dictionary_set_string(request, "kind", "10"); break;
        case 6: xpc_dictionary_set_uint64(request, "kind", LSB_REQUEST_COPY_DEBUGGER_INFO + 1); break;
        case 7: xpc_dictionary_set_uint64(request, "request-id", 1); break;
        case 8: xpc_dictionary_set_int64(request, "request-id", 0); break;
        case 9: xpc_dictionary_set_int64(request, "error", EACCES); break;
        case 10: xpc_dictionary_set_uint64(request, "error", 0); break;
        case 11: xpc_dictionary_set_data(request, "plan", "unexpected", 10); break;
        case 12: setUnexpectedFD(request, "fds", fd); break;
        case 13: setWirePorts(request, sentPort, -1); break;
        case 14: xpc_dictionary_set_mach_send(request, "bootstrap-port", sentPort); break;
        case 15: xpc_dictionary_set_mach_send(request, "native-service-port", sentPort); break;
        case 16: xpc_dictionary_set_data(request, "target-audit", "unexpected", 10); break;
        case 17: xpc_dictionary_set_uint64(request, "operation", 16); break;
        case 18: xpc_dictionary_set_data(request, "self-short-info", "unexpected", 10); break;
        case 19: xpc_dictionary_set_mach_send(request, "self-task-name", sentPort); break;
        case 20: xpc_dictionary_set_int64(request, "target-pid", 4242); break;
        case 21: xpc_dictionary_set_int64(request, "signal", 0); break;
        case 22: xpc_dictionary_set_uint64(request, "execution-role", LSB_EXECUTION_SIMULATOR); break;
        case 23: xpc_dictionary_set_uint64(request, "bootstrap-version", LSB_BOOTSTRAP_VERSION); break;
        case 24: xpc_dictionary_set_string(request, "runtime-root", "/tmp/RuntimeRoot"); break;
        case 25: setUnexpectedFD(request, "child-status-fd", fd); break;
        case 26: xpc_dictionary_set_string(request, "peer-path", "/sbin/launchd_sim"); break;
        case 27: xpc_dictionary_set_mach_send(request, "unknown-port", sentPort); break;
        case 28: setUnexpectedFD(request, "unknown-fd", fd); break;
        case 29: xpc_dictionary_set_string(request, "unknown-metadata", "unexpected"); break;
        default: assert(false);
    }
    xpc_object_t reply = NULL;
    assert(!xpc_pipe_routine(pipe, request, &reply));
    assert(reply && xpc_dictionary_get_int64(reply, "error") == EPROTO && xpc_dictionary_get_int64(reply, "pid") == 0);
    assert(xpc_dictionary_get_uint64(reply, "version") == LSB_TRANSPORT_VERSION);
    xpc_release(reply);
    xpc_release(request);
    xpc_release(pipe);
    assert(!mach_port_deallocate(mach_task_self(), port));
    assert(!LSBRequireSingleThread());
}

static void rebootClient(mach_port_t sentPort) {
    for (unsigned variant = 0; variant < RebootReplyCount; ++variant) {
        unsigned before = openDescriptorCount();
        mach_port_urefs_t beforeRights, afterRights;
        assert(!mach_port_get_refs(mach_task_self(), sentPort, MACH_PORT_RIGHT_SEND, &beforeRights));
        int expected = variant == 0 ? 0 : variant == 1 ? EACCES : EPROTO;
        assert(LSBTransportRebootUserspace() == expected);
        assert(openDescriptorCount() == before);
        assert(!mach_port_get_refs(mach_task_self(), sentPort, MACH_PORT_RIGHT_SEND, &afterRights) && beforeRights == afterRights);
    }
    assert(!LSBRequireSingleThread());
}

static void rebootHost(mach_port_t port, pid_t child, int fd, mach_port_t sentPort) {
    for (unsigned variant = 0; variant < RebootReplyCount; ++variant) {
        LSBReceivedMessage message;
        assert(!LSBTransportReceive(port, &message) && message.kind == LSB_REQUEST_REBOOT_USERSPACE);
        assert(message.audit.val[5] == (unsigned)child && !message.requestID && !message.error && !message.targetPID && !message.signalNumber);
        assert(!message.plan.version && !message.nativeServicePort && !message.peerIdentity.taskName);
        assert(!message.ports.registeredPresent && !message.ports.bootstrapPresent && !message.ports.count);
        for (unsigned i = 0; i < LSB_MAX_FDS; ++i) assert(message.fds[i] == -1);
        if (variant < 2) {
            assert(!LSBTransportReply(&message, variant ? EACCES : 0, 0, 0, NULL, NULL, NULL));
            LSBTransportDisposeMessage(&message);
            continue;
        }
        xpc_object_t reply = xpc_dictionary_create_reply((xpc_object_t)message.object);
        assert(reply);
        xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION);
        xpc_dictionary_set_uint64(reply, "request-id", 0);
        xpc_dictionary_set_int64(reply, "error", 0);
        xpc_dictionary_set_int64(reply, "pid", 0);
        switch (variant) {
            case 2: xpc_dictionary_set_uint64(reply, "version", 1); break;
            case 3: xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION + 1); break;
            case 4: xpc_dictionary_set_string(reply, "version", "2"); break;
            case 5: xpc_dictionary_set_uint64(reply, "request-id", 1); break;
            case 6: xpc_dictionary_set_int64(reply, "request-id", 0); break;
            case 7: xpc_dictionary_set_int64(reply, "pid", child); break;
            case 8: xpc_dictionary_set_int64(reply, "pid", -1); break;
            case 9: xpc_dictionary_set_uint64(reply, "pid", 0); break;
            case 10: xpc_dictionary_set_int64(reply, "error", -1); break;
            case 11: xpc_dictionary_set_int64(reply, "error", (int64_t)INT_MAX + 1); break;
            case 12: xpc_dictionary_set_uint64(reply, "error", EACCES); break;
            case 13: xpc_dictionary_set_value(reply, "error", NULL); break;
            case 14: xpc_dictionary_set_value(reply, "pid", NULL); break;
            case 15: xpc_dictionary_set_data(reply, "plan", "unexpected", 10); break;
            case 16: setUnexpectedFD(reply, "unknown-fd", fd); break;
            case 17: xpc_dictionary_set_mach_send(reply, "unknown-port", sentPort); break;
            case 18: xpc_dictionary_set_uint64(reply, "bootstrap-version", LSB_BOOTSTRAP_VERSION); break;
            case 19: xpc_dictionary_set_uint64(reply, "kind", LSB_REQUEST_REBOOT_USERSPACE); break;
            case 20:
                xpc_dictionary_set_int64(reply, "error", EACCES);
                xpc_dictionary_set_int64(reply, "pid", child);
                break;
            case 21:
                xpc_dictionary_set_int64(reply, "error", EACCES);
                xpc_dictionary_set_uint64(reply, "request-id", 1);
                break;
            case 22:
                xpc_dictionary_set_int64(reply, "error", EACCES);
                setUnexpectedFD(reply, "child-status-fd", fd);
                break;
            default: assert(false);
        }
        assert(!xpc_pipe_routine_reply(reply));
        xpc_release(reply);
        LSBTransportDisposeMessage(&message);
    }
}

static audit_token_t ownerAudit(void) { return (audit_token_t){{1, 2, 3, 4, 5, 4242, 7, 8}}; }

static void auditResolutionClient(void) {
    audit_token_t target = ownerAudit(), original = target;
    pid_t resolved = -1;
    assert(LSBTransportResolveAuditPID(NULL, &resolved) == EINVAL && !resolved);
    assert(LSBTransportResolveAuditPID(&target, NULL) == EINVAL);
    target.val[5] = 0;
    assert(LSBTransportResolveAuditPID(&target, &resolved) == EINVAL && !resolved);
    target.val[5] = UINT32_MAX;
    assert(LSBTransportResolveAuditPID(&target, &resolved) == EINVAL && !resolved);
    target = original;
    const int errors[] = {0, 0, ESRCH, EACCES, EPROTO, EPROTO, EPROTO};
    for (unsigned variant = 0; variant < sizeof(errors) / sizeof(errors[0]); ++variant) {
        resolved = -1;
        assert(LSBTransportResolveAuditPID(&target, &resolved) == errors[variant]);
        assert(resolved == (variant == 0 ? 4242 : variant == 1 ? 1212 : 0));
        assert(!memcmp(&target, &original, sizeof(target)));
        assert(!LSBRequireSingleThread());
    }
}

static void auditResolutionHost(mach_port_t port, pid_t child) {
    audit_token_t target = ownerAudit();
    for (unsigned variant = 0; variant < 7; ++variant) {
        LSBReceivedMessage message;
        assert(!LSBTransportReceive(port, &message) && message.kind == LSB_REQUEST_RESOLVE_AUDIT_PID);
        assert(!message.requestID && !message.error && message.audit.val[5] == (unsigned)child);
        assert(!memcmp(&message.targetAudit, &target, sizeof(target)));
        assert(memcmp(&message.audit, &target, sizeof(target))); // Target payload never replaces the kernel-audited caller.
        for (unsigned index = 0; index < LSB_MAX_FDS; ++index) assert(message.fds[index] == -1);
        if (variant == 6) {
            xpc_object_t reply = xpc_dictionary_create_reply(message.object);
            assert(reply);
            xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION);
            xpc_dictionary_set_uint64(reply, "request-id", 0);
            xpc_dictionary_set_int64(reply, "error", 0);
            xpc_dictionary_set_uint64(reply, "pid", 4242);
            assert(!xpc_pipe_routine_reply(reply));
            xpc_release(reply);
        } else {
            int error = variant == 2 ? ESRCH : variant == 3 ? EACCES : 0;
            pid_t pid = variant == 1 ? 1212 : variant == 4 ? 0 : 4242;
            assert(!LSBTransportReply(&message, error, pid, variant == 5 ? 77 : 0, NULL, NULL, NULL));
        }
        LSBTransportDisposeMessage(&message);
    }
}

enum {
    EntitlementSmall, EntitlementLarge, EntitlementMaximum, EntitlementMissing, EntitlementDenied, EntitlementUnknown,
    EntitlementBadEncoding, EntitlementErrorPayload, EntitlementMissingData, EntitlementWrongType, EntitlementShortHeader,
    EntitlementTooLarge, EntitlementWrongMagic, EntitlementWrongFormat, EntitlementTruncated, EntitlementExtraBytes, EntitlementNoPayload,
    EntitlementWrongVersion, EntitlementWrongError, EntitlementNegativeError, EntitlementWrongPID, EntitlementWrongID,
    EntitlementFD, EntitlementPort, EntitlementUnknownFD, EntitlementUnknownPort, EntitlementDataFD, EntitlementDataPort,
    EntitlementReplyCount
};

static unsigned char *entitlementBlob(size_t size, unsigned operation) {
    unsigned char *bytes = malloc(size);
    assert(bytes && size >= 8);
    memset(bytes, 0x5a, size);
    uint32_t header[] = {htonl(operation == 7 ? LSB_GUEST_ENTITLEMENTS_XML_MAGIC : LSB_GUEST_ENTITLEMENTS_DER_MAGIC), htonl((uint32_t)size)};
    memcpy(bytes, header, sizeof(header));
    return bytes; // Synthetic opaque payload; the host reader separately validates DER syntax.
}

static void entitlementClient(unsigned operation) {
    size_t maximum = operation == 7 ? LSB_GUEST_ENTITLEMENTS_MAX_XML_BLOB_SIZE : LSB_GUEST_ENTITLEMENTS_MAX_BLOB_SIZE;
    audit_token_t target = ownerAudit(), original = target;
    unsigned char *bytes = (unsigned char *)1;
    size_t size = 1;
    assert(LSBTransportCopyEntitlements(NULL, operation, &bytes, &size) == EINVAL && !bytes && !size);
    assert(LSBTransportCopyEntitlements(&target, operation, NULL, &size) == EINVAL && !size);
    assert(LSBTransportCopyEntitlements(&target, operation, &bytes, NULL) == EINVAL && !bytes);
    assert(LSBTransportCopyEntitlements(&target, 0, &bytes, &size) == EINVAL && !bytes && !size);
    assert(LSBTransportCopyEntitlements(&target, 5, &bytes, &size) == EINVAL && !bytes && !size);
    assert(LSBTransportCopyEntitlements(&target, UINT_MAX, &bytes, &size) == EINVAL && !bytes && !size);
    target.val[5] = 0;
    assert(LSBTransportCopyEntitlements(&target, operation, &bytes, &size) == EINVAL && !bytes && !size);
    target.val[5] = UINT32_MAX;
    assert(LSBTransportCopyEntitlements(&target, operation, &bytes, &size) == EINVAL && !bytes && !size);
    target = original;
    unsigned char *saved = NULL;
    mach_port_t broker;
    assert(!LSBTransportCopyBrokerPort(&broker));
    for (unsigned variant = 0; variant < EntitlementReplyCount; ++variant) {
        unsigned before = openDescriptorCount();
        mach_port_urefs_t portReferences, remainingReferences;
        assert(!mach_port_get_refs(mach_task_self(), broker, MACH_PORT_RIGHT_SEND, &portReferences));
        int expected = variant <= EntitlementMaximum ? 0 : variant == EntitlementMissing ? ENOENT :
            variant == EntitlementDenied ? EACCES : variant == EntitlementUnknown ? ESRCH : variant == EntitlementBadEncoding ? EINVAL : EPROTO;
        bytes = (unsigned char *)1;
        size = 1;
        int result = LSBTransportCopyEntitlements(&target, operation, &bytes, &size);
        if (result != expected) fprintf(stderr, "Entitlement reply variant %u: got %d, expected %d\n", variant, result, expected);
        assert(result == expected);
        assert(!memcmp(&target, &original, sizeof(target)));
        if (!expected) {
            size_t length = variant == EntitlementSmall ? 16 : variant == EntitlementLarge ? 33 * 1024 : maximum;
            assert(bytes && size == length && LSBGuestEntitlementsHasValidBlobHeaderForOperation(bytes, size, operation));
            for (size_t index = 8; index < size; ++index) assert(bytes[index] == 0x5a);
            if (!variant) { saved = bytes; saved[8] = 0xa5; }
            else free(bytes);
        } else assert(!bytes && !size);
        assert(saved && saved[8] == 0xa5); // A returned copy survives subsequent replies and their XPC object destruction.
        assert(openDescriptorCount() == before);
        assert(!mach_port_get_refs(mach_task_self(), broker, MACH_PORT_RIGHT_SEND, &remainingReferences) && remainingReferences == portReferences);
        assert(!LSBRequireSingleThread());
    }
    assert(!mach_port_deallocate(mach_task_self(), broker));
    free(saved);
}

static void entitlementHost(mach_port_t port, pid_t child, unsigned operation) {
    size_t maximum = operation == 7 ? LSB_GUEST_ENTITLEMENTS_MAX_XML_BLOB_SIZE : LSB_GUEST_ENTITLEMENTS_MAX_BLOB_SIZE;
    audit_token_t target = ownerAudit();
    for (unsigned variant = 0; variant < EntitlementReplyCount; ++variant) {
        unsigned before = openDescriptorCount();
        LSBReceivedMessage message;
        assert(!LSBTransportReceive(port, &message) && message.kind == LSB_REQUEST_COPY_ENTITLEMENTS);
        assert(message.entitlementOperation == operation);
        assert(!message.requestID && !message.error && message.audit.val[5] == (unsigned)child);
        assert(!memcmp(&message.targetAudit, &target, sizeof(target)) && memcmp(&message.audit, &target, sizeof(target)));
        for (unsigned index = 0; index < LSB_MAX_FDS; ++index) assert(message.fds[index] == -1);
        assert(!message.nativeServicePort && !message.ports.registeredPresent && !message.ports.bootstrapPresent);
        size_t size = variant == EntitlementLarge ? 33 * 1024 : variant == EntitlementMaximum ? maximum :
            variant == EntitlementTooLarge ? maximum + 1 : 16;
        unsigned char *bytes = entitlementBlob(size, operation);
        if (variant <= EntitlementMaximum) {
            void *object = message.object;
            assert(LSBTransportReplyEntitlements(&message, -1, bytes, size) == EINVAL && message.object == object);
            assert(!LSBTransportReplyEntitlements(&message, 0, bytes, size) && !message.object);
            memset(bytes, 0xcc, size); // Reply storage must not borrow the host's mutable source after the call.
        } else if (variant <= EntitlementBadEncoding) {
            int error = variant == EntitlementMissing ? ENOENT : variant == EntitlementDenied ? EACCES : ESRCH;
            if (variant == EntitlementBadEncoding) {
                bytes[0] = 0;
                assert(LSBTransportReplyEntitlements(&message, 0, bytes, size) == EINVAL && !message.object);
            } else assert(!LSBTransportReplyEntitlements(&message, error, NULL, 0) && !message.object);
        } else {
            xpc_object_t reply = xpc_dictionary_create_reply(message.object);
            assert(reply);
            xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION);
            xpc_dictionary_set_uint64(reply, "request-id", 0);
            xpc_dictionary_set_int64(reply, "error", 0);
            xpc_dictionary_set_int64(reply, "pid", 0);
            if (variant == EntitlementWrongMagic) bytes[0] = 0;
            if (variant == EntitlementWrongFormat) {
                uint32_t magic = htonl(operation == 7 ? LSB_GUEST_ENTITLEMENTS_DER_MAGIC : LSB_GUEST_ENTITLEMENTS_XML_MAGIC);
                memcpy(bytes, &magic, 4);
            }
            if (variant == EntitlementTruncated) --size;
            if (variant == EntitlementExtraBytes) { uint32_t length = htonl((uint32_t)size - 1); memcpy(bytes + 4, &length, 4); }
            if (variant == EntitlementNoPayload) { size = 8; uint32_t length = htonl(8); memcpy(bytes + 4, &length, 4); }
            xpc_dictionary_set_data(reply, "entitlements", bytes, variant == EntitlementShortHeader ? 7 : size);
            switch (variant) {
                case EntitlementErrorPayload: xpc_dictionary_set_int64(reply, "error", ESRCH); break;
                case EntitlementMissingData: xpc_dictionary_set_value(reply, "entitlements", NULL); break;
                case EntitlementWrongType: xpc_dictionary_set_string(reply, "entitlements", "not data"); break;
                case EntitlementWrongVersion: xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION + 1); break;
                case EntitlementWrongError: xpc_dictionary_set_uint64(reply, "error", EACCES); break;
                case EntitlementNegativeError: xpc_dictionary_set_int64(reply, "error", -1); break;
                case EntitlementWrongPID: xpc_dictionary_set_int64(reply, "pid", 4242); break;
                case EntitlementWrongID: xpc_dictionary_set_uint64(reply, "request-id", 1); break;
                case EntitlementFD: case EntitlementUnknownFD: case EntitlementDataFD: {
                    const char *key = variant == EntitlementFD ? "fds" : variant == EntitlementUnknownFD ? "unexpected-fd" : "entitlements";
                    xpc_object_t descriptor = xpc_fd_create(STDERR_FILENO);
                    assert(descriptor);
                    xpc_dictionary_set_value(reply, key, descriptor);
                    xpc_release(descriptor);
                    break;
                }
                case EntitlementPort: case EntitlementUnknownPort: case EntitlementDataPort:
                    xpc_dictionary_set_mach_send(reply, variant == EntitlementPort ? "bootstrap-port" :
                        variant == EntitlementUnknownPort ? "unexpected-right" : "entitlements", port);
                    break;
                default: break;
            }
            assert(!xpc_pipe_routine_reply(reply));
            xpc_release(reply);
        }
        free(bytes);
        LSBTransportDisposeMessage(&message);
        assert(openDescriptorCount() == before);
    }
}

enum {
    CodeHashFirst, CodeHashSecond, CodeHashMissing, CodeHashDenied, CodeHashUnknown, CodeHashInvalidReplySize,
    CodeHashShort, CodeHashLong, CodeHashMissingData, CodeHashWrongType, CodeHashErrorPayload,
    CodeHashFD, CodeHashPort, CodeHashUnknownFD, CodeHashUnknownPort, CodeHashDataFD, CodeHashDataPort,
    CodeHashWrongPID, CodeHashWrongID, CodeHashReplyCount
};

static void codeHashClient(void) {
    audit_token_t target = ownerAudit();
    unsigned char storage[LSB_GUEST_CDHASH_BYTES + 2], saved[LSB_GUEST_CDHASH_BYTES];
    unsigned char *hash = storage + 1;
    memset(storage, 0xa5, sizeof(storage));
    assert(LSBTransportCopyCodeDirectoryHash(NULL, hash) == EINVAL);
    for (unsigned i = 0; i < LSB_GUEST_CDHASH_BYTES; ++i) assert(!hash[i]);
    assert(LSBTransportCopyCodeDirectoryHash(&target, NULL) == EINVAL);
    target.val[5] = 0;
    assert(LSBTransportCopyCodeDirectoryHash(&target, hash) == EINVAL);
    target.val[5] = UINT32_MAX;
    assert(LSBTransportCopyCodeDirectoryHash(&target, hash) == EINVAL);
    mach_port_t broker;
    assert(!LSBTransportCopyBrokerPort(&broker));
    for (unsigned variant = 0; variant < CodeHashReplyCount; ++variant) {
        target = ownerAudit();
        if (variant == CodeHashSecond) { target.val[5] = 4243; target.val[7] = 99; }
        audit_token_t original = target;
        memset(hash, 0xa5, LSB_GUEST_CDHASH_BYTES);
        unsigned before = openDescriptorCount();
        mach_port_urefs_t references, remaining;
        assert(!mach_port_get_refs(mach_task_self(), broker, MACH_PORT_RIGHT_SEND, &references));
        int expected = variant <= CodeHashSecond ? 0 : variant == CodeHashMissing ? ENOENT :
            variant == CodeHashDenied ? EACCES : variant == CodeHashUnknown ? ESRCH : variant == CodeHashInvalidReplySize ? EINVAL : EPROTO;
        int error = LSBTransportCopyCodeDirectoryHash(&target, hash);
        if (error != expected) fprintf(stderr, "Code hash reply variant %u: got %d, expected %d\n", variant, error, expected);
        assert(error == expected && !memcmp(&target, &original, sizeof(target)));
        assert(storage[0] == 0xa5 && storage[sizeof(storage) - 1] == 0xa5);
        for (unsigned i = 0; i < LSB_GUEST_CDHASH_BYTES; ++i)
            assert(hash[i] == (error ? 0 : (variant == CodeHashFirst ? 0x30 : 0x80) + i));
        if (!variant) memcpy(saved, hash, sizeof(saved));
        for (unsigned i = 0; i < sizeof(saved); ++i) assert(saved[i] == 0x30 + i);
        assert(openDescriptorCount() == before);
        assert(!mach_port_get_refs(mach_task_self(), broker, MACH_PORT_RIGHT_SEND, &remaining) && remaining == references);
        assert(!LSBRequireSingleThread());
    }
    assert(!mach_port_deallocate(mach_task_self(), broker));
}

static void codeHashHost(mach_port_t port, pid_t child) {
    for (unsigned variant = 0; variant < CodeHashReplyCount; ++variant) {
        unsigned before = openDescriptorCount();
        LSBReceivedMessage message;
        assert(!LSBTransportReceive(port, &message) && message.kind == LSB_REQUEST_COPY_ENTITLEMENTS);
        assert(message.entitlementOperation == 5 && !message.requestID && !message.error);
        audit_token_t target = ownerAudit();
        if (variant == CodeHashSecond) { target.val[5] = 4243; target.val[7] = 99; }
        assert(message.audit.val[5] == (unsigned)child && !memcmp(&message.targetAudit, &target, sizeof(target)));
        assert(memcmp(&message.audit, &target, sizeof(target)));
        for (unsigned i = 0; i < LSB_MAX_FDS; ++i) assert(message.fds[i] == -1);
        assert(!message.nativeServicePort && !message.ports.registeredPresent && !message.ports.bootstrapPresent);
        unsigned char bytes[LSB_GUEST_CDHASH_BYTES + 1];
        for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (variant == CodeHashFirst ? 0x30 : 0x80) + i;
        if (variant <= CodeHashSecond) {
            void *object = message.object;
            assert(LSBTransportReplyEntitlements(&message, -1, bytes, LSB_GUEST_CDHASH_BYTES) == EINVAL && message.object == object);
            assert(!LSBTransportReplyEntitlements(&message, 0, bytes, LSB_GUEST_CDHASH_BYTES) && !message.object);
            memset(bytes, 0xcc, sizeof(bytes)); // The sent reply must own an independent byte copy.
        } else if (variant <= CodeHashInvalidReplySize) {
            if (variant == CodeHashInvalidReplySize)
                assert(LSBTransportReplyEntitlements(&message, 0, bytes, LSB_GUEST_CDHASH_BYTES - 1) == EINVAL && !message.object);
            else {
                int error = variant == CodeHashMissing ? ENOENT : variant == CodeHashDenied ? EACCES : ESRCH;
                assert(!LSBTransportReplyEntitlements(&message, error, NULL, 0) && !message.object);
            }
        } else {
            xpc_object_t reply = xpc_dictionary_create_reply(message.object);
            assert(reply);
            xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION);
            xpc_dictionary_set_uint64(reply, "request-id", variant == CodeHashWrongID ? 77 : 0);
            xpc_dictionary_set_int64(reply, "error", variant == CodeHashErrorPayload ? EACCES : 0);
            xpc_dictionary_set_int64(reply, "pid", variant == CodeHashWrongPID ? 4242 : 0);
            size_t size = variant == CodeHashShort ? LSB_GUEST_CDHASH_BYTES - 1 :
                variant == CodeHashLong ? LSB_GUEST_CDHASH_BYTES + 1 : LSB_GUEST_CDHASH_BYTES;
            xpc_dictionary_set_data(reply, "entitlements", bytes, size);
            switch (variant) {
                case CodeHashMissingData: xpc_dictionary_set_value(reply, "entitlements", NULL); break;
                case CodeHashWrongType: xpc_dictionary_set_string(reply, "entitlements", "not data"); break;
                case CodeHashFD: case CodeHashUnknownFD: case CodeHashDataFD: {
                    const char *key = variant == CodeHashFD ? "fds" : variant == CodeHashUnknownFD ? "unexpected-fd" : "entitlements";
                    xpc_object_t descriptor = xpc_fd_create(STDERR_FILENO);
                    assert(descriptor);
                    xpc_dictionary_set_value(reply, key, descriptor);
                    xpc_release(descriptor);
                    break;
                }
                case CodeHashPort: case CodeHashUnknownPort: case CodeHashDataPort:
                    xpc_dictionary_set_mach_send(reply, variant == CodeHashPort ? "bootstrap-port" :
                        variant == CodeHashUnknownPort ? "unexpected-right" : "entitlements", port);
                    break;
                default: break;
            }
            assert(!xpc_pipe_routine_reply(reply));
            xpc_release(reply);
        }
        LSBTransportDisposeMessage(&message);
        assert(openDescriptorCount() == before);
    }
}

static void testMalformedAudit(unsigned variant, uint32_t kind) {
    mach_port_t port;
    assert(!LSBTestCopyBrokerPort(&port));
    xpc_object_t pipe = xpc_pipe_create_from_port(port, 0), request = xpc_dictionary_create(NULL, NULL, 0);
    assert(pipe && request);
    xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(request, "kind", variant == 15 ? LSB_REQUEST_CHECKIN : kind);
    xpc_dictionary_set_uint64(request, "request-id", variant == 6 ? 77 : 0);
    xpc_dictionary_set_int64(request, "error", variant == 7 ? EACCES : 0);
    if (kind == LSB_REQUEST_COPY_ENTITLEMENTS) xpc_dictionary_set_uint64(request, "operation", 16);
    audit_token_t target = ownerAudit();
    if (variant == 4) target.val[5] = 0;
    if (variant == 5) target.val[5] = (uint32_t)INT_MAX + 1;
    unsigned char payload[sizeof(target) + 1] = {0};
    memcpy(payload, &target, sizeof(target));
    if (variant != 0) xpc_dictionary_set_data(request, "target-audit", payload, sizeof(target) + (variant == 2 ? -1 : variant == 3 ? 1 : 0));
    if (variant == 1) xpc_dictionary_set_uint64(request, "target-audit", 4242);
    if (variant == 8) xpc_dictionary_set_data(request, "plan", "unexpected", 10);
    if (variant == 9) {
        xpc_object_t array = xpc_array_create(NULL, 0), descriptor = xpc_fd_create(STDERR_FILENO);
        assert(array && descriptor);
        xpc_array_append_value(array, descriptor);
        xpc_dictionary_set_value(request, "fds", array);
        xpc_release(descriptor);
        xpc_release(array);
    }
    if (variant == 10) {
        xpc_object_t array = xpc_array_create(NULL, 0);
        xpc_array_set_mach_send(array, XPC_ARRAY_APPEND, port);
        xpc_dictionary_set_value(request, "registered-ports", array);
        xpc_release(array);
    }
    if (variant == 11) xpc_dictionary_set_mach_send(request, "bootstrap-port", port);
    if (variant == 12) xpc_dictionary_set_int64(request, "target-pid", 4242);
    if (variant == 13) xpc_dictionary_set_int64(request, "signal", 0);
    if (variant == 14) xpc_dictionary_set_uint64(request, "kind", LSB_REQUEST_COPY_PROCESS_IDENTITY + 1);
    if (variant == 16) xpc_dictionary_set_mach_send(request, "native-service-port", port);
    if (variant == 17) xpc_dictionary_set_mach_send(request, "unexpected-right", port);
    if (variant == 18) {
        xpc_object_t descriptor = xpc_fd_create(STDERR_FILENO);
        assert(descriptor);
        xpc_dictionary_set_value(request, "unexpected-fd", descriptor);
        xpc_release(descriptor);
    }
    if (variant == 19) xpc_dictionary_set_data(request, "entitlements", "unexpected", 10);
    if (variant == 20) xpc_dictionary_set_value(request, "operation", NULL);
    if (variant == 21) xpc_dictionary_set_int64(request, "operation", 16);
    if (variant == 22) xpc_dictionary_set_uint64(request, "operation", 0);
    if (variant == 23) xpc_dictionary_set_uint64(request, "operation", 8);
    if (variant == 24) xpc_dictionary_set_uint64(request, "operation", UINT64_MAX);
    xpc_object_t reply = NULL;
    assert(!xpc_pipe_routine(pipe, request, &reply));
    assert(reply && xpc_dictionary_get_int64(reply, "error") == EPROTO);
    xpc_release(reply);
    xpc_release(request);
    xpc_release(pipe);
    assert(!mach_port_deallocate(mach_task_self(), port));
    assert(!LSBRequireSingleThread());
}

static void *blockedWorker(void *argument) {
    char byte;
    assert(read(*(int *)argument, &byte, 1) == 1);
    return NULL;
}

#ifdef LSB_GUEST_RUNTIME
static mach_port_t testGuestPort(void) {
    mach_port_t base = MACH_PORT_NULL, copy = MACH_PORT_NULL;
    assert(!LSBTestCopyBrokerPort(&base));
    assert(LSBTransportCopyBrokerPort(NULL) == EINVAL);
    unsetenv(LSB_BROKER_PORT_ENV);
    assert(LSBTransportCopyBrokerPort(&copy) == ENOENT && copy == MACH_PORT_NULL);
    const char *invalid[] = {"", "0", "-1", "+1", " 1", "1x", "4294967295", "4294967296", "100000000000000000000000"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!setenv(LSB_BROKER_PORT_ENV, invalid[i], 1));
        assert(LSBTransportCopyBrokerPort(&copy) == EINVAL && copy == MACH_PORT_NULL);
    }
    mach_port_t receiveOnly;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &receiveOnly));
    char value[16];
    snprintf(value, sizeof(value), "%u", receiveOnly);
    assert(!setenv(LSB_BROKER_PORT_ENV, value, 1));
    assert(LSBTransportCopyBrokerPort(&copy) == EACCES && copy == MACH_PORT_NULL);
    assert(!mach_port_mod_refs(mach_task_self(), receiveOnly, MACH_PORT_RIGHT_RECEIVE, -1));
    snprintf(value, sizeof(value), "%u", base);
    assert(!setenv(LSB_BROKER_PORT_ENV, value, 1));
    mach_port_urefs_t before = 0, after = 0;
    assert(!mach_port_get_refs(mach_task_self(), base, MACH_PORT_RIGHT_SEND, &before));
    assert(!LSBTransportCopyBrokerPort(&copy) && copy == base);
    assert(!mach_port_get_refs(mach_task_self(), base, MACH_PORT_RIGHT_SEND, &after) && after == before + 1);
    assert(!mach_port_deallocate(mach_task_self(), copy));
    assert(!mach_port_get_refs(mach_task_self(), base, MACH_PORT_RIGHT_SEND, &after) && after == before);
    return base;
}
#endif

static int client(void) {
    assert(!LSBRequireSingleThread());
#ifdef LSB_GUEST_RUNTIME
    mach_port_t brokerPort = testGuestPort();
#endif
    LSBSpawnPlan plan = makePlan();
    char path[] = "/tmp/LiveSimBooter-transport.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    unlink(path);
    assert(write(fd, "abcdef", 6) == 6 && lseek(fd, 0, SEEK_SET) == 0);
    int fds[LSB_MAX_FDS];
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) fds[i] = -1;
    fds[0] = fd;
    pid_t pid = 0;
    assert(!LSBTransportSpawn(&plan, fds, &pid) && pid == 4242);
    assert(lseek(fd, 0, SEEK_CUR) == 1 && fcntl(fd, F_GETFD) >= 0);
    assert(!LSBRequireSingleThread());

    LSBBootstrapReply reply;
    int received[LSB_MAX_FDS];
    assert(!LSBTransportCheckIn(&reply, received));
    assert(reply.requestID == 77 && !reply.error && !strcmp(reply.runtimeRoot, "/tmp/RuntimeRoot"));
    assert(reply.guestPID == getpid() && reply.parentPID == getppid());
    assert(reply.launchdTankPort == MACH_PORT_NULL && !reply.ports.count && !reply.ports.registeredPresent && !reply.ports.bootstrapPresent);
    assert(!memcmp(&reply.plan, &plan, sizeof(plan)) && received[0] >= 0);
    assert(reply.statusFD >= 0 && reply.wakeFD >= 0 && reply.statusFD != reply.wakeFD && reply.statusFD != received[0]);
    assert((fcntl(reply.statusFD, F_GETFD) & FD_CLOEXEC) && (fcntl(reply.wakeFD, F_GETFD) & FD_CLOEXEC));
    assert(fcntl(reply.wakeFD, F_GETFL) & O_NONBLOCK);
    void *ledger = mmap(NULL, LSB_CHILD_STATUS_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, reply.statusFD, 0);
    assert(ledger != MAP_FAILED && !memcmp(ledger, "ledger", 6));
    memcpy(ledger, "shared", 6);
    assert(!munmap(ledger, LSB_CHILD_STATUS_MAP_SIZE));
    char wake = 0;
    assert(read(reply.wakeFD, &wake, 1) == 1 && wake == 'w');
    assert(read(reply.wakeFD, &wake, 1) == -1 && errno == EAGAIN);
    close(reply.statusFD);
    close(reply.wakeFD);
    for (unsigned i = 1; i < LSB_MAX_FDS; ++i) assert(received[i] == -1);
    char byte = 0;
    assert(read(received[0], &byte, 1) == 1 && byte == 'b' && lseek(fd, 0, SEEK_CUR) == 2);
    close(received[0]);
    readyUsingHeldPort(reply.requestID);
    // The optional tank is an independently owned send right, surviving release of the XPC reply and imported FD cleanup.
    mach_port_t tankAnchor = MACH_PORT_NULL;
    for (unsigned index = 0; index < 2; ++index) {
        assert(!LSBTransportCheckIn(&reply, received) && reply.requestID == 78 + index);
        assert(MACH_PORT_VALID(reply.launchdTankPort));
        mach_port_type_t type;
        mach_port_urefs_t references;
        assert(!mach_port_type(mach_task_self(), reply.launchdTankPort, &type));
        assert((type & MACH_PORT_TYPE_SEND) && !(type & MACH_PORT_TYPE_RECEIVE));
        if (!index) tankAnchor = reply.launchdTankPort;
        else assert(reply.launchdTankPort == tankAnchor);
        assert(!mach_port_get_refs(mach_task_self(), tankAnchor, MACH_PORT_RIGHT_SEND, &references) && references == index + 1);
        close(reply.statusFD);
        close(reply.wakeFD);
        close(received[0]);
    }
    assert(!mach_port_deallocate(mach_task_self(), reply.launchdTankPort));
    mach_port_urefs_t tankReferences;
    assert(!mach_port_get_refs(mach_task_self(), tankAnchor, MACH_PORT_RIGHT_SEND, &tankReferences) && tankReferences == 1);
    mach_msg_header_t ping = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0), .msgh_size = sizeof(ping),
                              .msgh_remote_port = tankAnchor, .msgh_id = 885};
    assert(!mach_msg(&ping, MACH_SEND_MSG, sizeof(ping), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL));
    registeredPortsClient(&plan, fds);
    for (unsigned variant = 0; variant < MalformedPortsCount; ++variant) testMalformedPorts(&plan, fd, tankAnchor, variant);
    for (unsigned variant = 0; variant < MalformedRoleRequestCount; ++variant) testRoleRequestInjection(&plan, fd, variant);
    signalAcknowledgementFD = fd;
    struct sigaction action = {.sa_handler = childSignal}, previousAction;
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGCHLD, &action, &previousAction));
    assert(LSBTransportKill(4242, -1) == EINVAL && LSBTransportKill(4242, NSIG) == EINVAL);
    assert(LSBTransportKill(0, 0) == ENOTSUP && LSBTransportKill(-1, SIGTERM) == ENOTSUP && LSBTransportKill(-20, SIGTERM) == ENOTSUP);
    assert(!LSBTransportKill(4242, 0));
    assert(!LSBTransportKill(4242, SIGTERM));
    assert(childSignals == 1);
    assert(LSBTransportKill(4242, SIGKILL) == ESRCH);
    assert(LSBTransportKill(4242, SIGTERM) == EPROTO);
    assert(LSBTransportKill(4242, SIGTERM) == EPROTO);
    assert(!LSBRequireSingleThread());
    assert(!sigaction(SIGCHLD, &previousAction, NULL));
    signalAcknowledgementFD = -1;
    auditResolutionClient();
    entitlementClient(16);
    entitlementClient(7);
    codeHashClient();
    rebootClient(tankAnchor);
    for (unsigned variant = 0; variant < MalformedRebootCount; ++variant) testMalformedReboot(variant, fd, tankAnchor);
    for (unsigned variant = 0; variant < MalformedBootstrapCount; ++variant) {
        unsigned before = openDescriptorCount();
        assert(LSBTransportCheckIn(&reply, received) == EPROTO);
        assert(reply.nativeProductionStatus == LSB_NATIVE_PRODUCTION_UNKNOWN);
        assert(reply.statusFD == -1 && reply.wakeFD == -1 && reply.launchdTankPort == MACH_PORT_NULL);
        assert(!reply.ports.count && !reply.ports.registeredPresent && !reply.ports.bootstrapPresent && !reply.ports.bootstrapPort);
        for (unsigned i = 0; i < LSB_MAX_REGISTERED_PORTS; ++i) assert(reply.ports.registered[i] == MACH_PORT_NULL);
        for (unsigned i = 0; i < LSB_MAX_FDS; ++i) assert(received[i] == -1);
        assert(openDescriptorCount() == before);
        assert(!mach_port_get_refs(mach_task_self(), tankAnchor, MACH_PORT_RIGHT_SEND, &tankReferences) && tankReferences == 1);
    }
    assert(!mach_port_deallocate(mach_task_self(), tankAnchor));
    mach_port_type_t tankType;
    assert(mach_port_type(mach_task_self(), tankAnchor, &tankType) == KERN_INVALID_NAME);
    assert(!LSBRequireSingleThread());
    assert(LSBTransportReady(0, 0) == EINVAL);
    plan.version = 0;
    assert(LSBTransportSpawn(&plan, fds, &pid) == EINVAL && pid == 0 && fcntl(fd, F_GETFD) >= 0);
    plan.version = LSB_PLAN_VERSION;
    assert(LSBTransportSpawn(&plan, fds, &pid) == EACCES && pid == 0);
    assert(LSBTransportSpawn(&plan, fds, &pid) == EPROTO && pid == 0);
    for (unsigned variant = 0; variant < 5; ++variant) testMalformedRequest(&plan, variant);
    for (unsigned variant = 0; variant < MalformedKillCount; ++variant) testMalformedKill(variant);
    for (unsigned variant = 0; variant < MalformedAuditCount; ++variant) testMalformedAudit(variant, LSB_REQUEST_RESOLVE_AUDIT_PID);
    for (unsigned variant = 0; variant < MalformedEntitlementCount; ++variant) testMalformedAudit(variant, LSB_REQUEST_COPY_ENTITLEMENTS);
    assert(LSBTransportReady(77, ENOEXEC) == ENOEXEC);
    assert(!LSBRequireSingleThread());
    close(fd);
    int synchronization[2];
    assert(!pipe(synchronization));
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, blockedWorker, &synchronization[0]));
    assert(LSBRequireSingleThread() == EBUSY);
    assert(write(synchronization[1], "x", 1) == 1 && !pthread_join(worker, NULL));
    close(synchronization[0]);
    close(synchronization[1]);
    // pthread_join can finish just before the kernel removes the exiting thread from task_threads.
    int guard = EBUSY;
    for (unsigned i = 0; i < 100 && guard == EBUSY; ++i) { usleep(1000); guard = LSBRequireSingleThread(); }
    assert(!guard);
#ifdef LSB_GUEST_RUNTIME
    assert(!mach_port_deallocate(mach_task_self(), brokerPort));
#endif
#if defined(LSB_XPC_WIRE_TESTING)
    LSBXPCWireStatistics statistics = LSBXPCWireTestStatistics();
    fprintf(stderr, "XPC wire adapter: sent inline=%u OOL=%u, received inline=%u OOL=%u, receive retries=%u\n",
            statistics.sentInline, statistics.sentOOL, statistics.receivedInline, statistics.receivedOOL, statistics.receiveRetries);
    assert(statistics.sentInline && statistics.receivedInline);
#endif
    puts("SpawnTransport child: spawn/exec, stable identity, FD/Mach ownership, kill/SIGCHLD, reboot and malformed-message checks passed");
    return 0;
}

static void *replyWorker(void *argument) {
    LSBReceivedMessage *message = argument;
    assert(!LSBTransportReply(message, 0, 4242, 77, NULL, NULL, NULL));
    return NULL;
}

static unsigned openDescriptorCount(void) {
    unsigned count = 0;
    for (int fd = 0; fd < 256; ++fd) if (fcntl(fd, F_GETFD) >= 0) ++count;
    return count;
}

typedef struct { int status, readOnlyStatus, shortStatus, wake[2], blockingWake[2]; } BootstrapFDs;

static BootstrapFDs makeBootstrapFDs(void) {
    BootstrapFDs result;
    char path[] = "/tmp/LiveSimBooter-status.XXXXXX";
    result.status = mkstemp(path);
    assert(result.status >= 0 && !ftruncate(result.status, LSB_CHILD_STATUS_MAP_SIZE));
    result.readOnlyStatus = open(path, O_RDONLY | O_CLOEXEC);
    assert(result.readOnlyStatus >= 0 && !unlink(path));
    assert(pwrite(result.status, "ledger", 6, 0) == 6);
    char shortPath[] = "/tmp/LiveSimBooter-short-status.XXXXXX";
    result.shortStatus = mkstemp(shortPath);
    assert(result.shortStatus >= 0 && !unlink(shortPath));
    assert(!pipe(result.wake) && !pipe(result.blockingWake));
    assert(!fcntl(result.wake[0], F_SETFL, O_NONBLOCK));
    assert(write(result.wake[1], "w", 1) == 1);
    return result;
}

static void closeBootstrapFDs(BootstrapFDs *fds) {
    close(fds->status);
    close(fds->readOnlyStatus);
    close(fds->shortStatus);
    close(fds->wake[0]);
    close(fds->wake[1]);
    close(fds->blockingWake[0]);
    close(fds->blockingWake[1]);
}

static void replyMalformedBootstrap(LSBReceivedMessage *message, const LSBSpawnPlan *plan, int planFD,
                                    const BootstrapFDs *fds, mach_port_t tank, unsigned variant) {
    xpc_object_t reply = xpc_dictionary_create_reply((xpc_object_t)message->object);
    assert(reply);
    xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(reply, "bootstrap-version", LSB_BOOTSTRAP_VERSION);
    xpc_dictionary_set_uint64(reply, "execution-role", LSB_EXECUTION_SIMULATOR);
    xpc_dictionary_set_uint64(reply, "native-production-status", LSB_NATIVE_PRODUCTION);
    xpc_dictionary_set_uint64(reply, "request-id", 77);
    xpc_dictionary_set_int64(reply, "error", 0);
    xpc_dictionary_set_int64(reply, "pid", 4242);
    xpc_dictionary_set_int64(reply, "guest-pid", 4242);
    xpc_dictionary_set_int64(reply, "parent-pid", 1);
    xpc_dictionary_set_string(reply, "runtime-root", "/tmp/RuntimeRoot");
    xpc_dictionary_set_data(reply, "plan", plan, sizeof(*plan));
    xpc_object_t array = xpc_array_create(NULL, 0), descriptor = xpc_fd_create(planFD);
    assert(array && descriptor);
    xpc_array_append_value(array, descriptor);
    xpc_dictionary_set_value(reply, "fds", array);
    xpc_release(descriptor);
    xpc_release(array);
    setWirePorts(reply, tank, variant >= 14 && variant < 14 + MalformedPortsCount ? (int)variant - 14 : -1);
    if (variant >= 14 + MalformedPortsCount && variant < 14 + MalformedPortsCount + MalformedIdentityCount) {
        switch (variant - 14 - MalformedPortsCount) {
            case 0: xpc_dictionary_set_value(reply, "guest-pid", NULL); break;
            case 1: xpc_dictionary_set_string(reply, "guest-pid", "4242"); break;
            case 2: xpc_dictionary_set_uint64(reply, "guest-pid", 4242); break;
            case 3: xpc_dictionary_set_int64(reply, "guest-pid", 0); break;
            case 4: xpc_dictionary_set_int64(reply, "guest-pid", -1); break;
            case 5: xpc_dictionary_set_int64(reply, "guest-pid", (int64_t)INT_MAX + 1); break;
            case 6: xpc_dictionary_set_value(reply, "parent-pid", NULL); break;
            case 7: xpc_dictionary_set_uint64(reply, "parent-pid", 1); break;
            case 8: xpc_dictionary_set_int64(reply, "parent-pid", -1); break;
            case 9: xpc_dictionary_set_int64(reply, "parent-pid", (int64_t)INT_MAX + 1); break;
            case 10: xpc_dictionary_set_int64(reply, "pid", 0); break;
            case 11: xpc_dictionary_set_int64(reply, "pid", -1); break;
            default: assert(false);
        }
    }
    if (variant == 10) xpc_dictionary_set_string(reply, "plan", "malformed plan");
    // Valid ports in the earlier-invalid variants must be released even though decoding never reaches the optional field.
    if (variant == 11) xpc_dictionary_set_string(reply, "launchd-tank-port", "not a Mach send right");
    else if (variant == 12) xpc_dictionary_set_uint64(reply, "launchd-tank-port", tank);
    else if (variant == 13) {
        descriptor = xpc_fd_create(planFD);
        assert(descriptor);
        xpc_dictionary_set_value(reply, "launchd-tank-port", descriptor);
        xpc_release(descriptor);
    } else xpc_dictionary_set_mach_send(reply, "launchd-tank-port", tank);
    if (variant >= 14 + MalformedPortsCount + MalformedIdentityCount) {
        xpc_dictionary_set_uint64(reply, "bootstrap-version", LSB_BOOTSTRAP_VERSION);
        xpc_dictionary_set_uint64(reply, "execution-role", LSB_EXECUTION_SIMULATOR);
        switch (variant - 14 - MalformedPortsCount - MalformedIdentityCount) {
            case 0: xpc_dictionary_set_value(reply, "execution-role", NULL); break;
            case 1: xpc_dictionary_set_value(reply, "bootstrap-version", NULL); break;
            case 2: xpc_dictionary_set_uint64(reply, "bootstrap-version", LSB_BOOTSTRAP_VERSION + 1); break;
            case 3: xpc_dictionary_set_string(reply, "bootstrap-version", "1"); break;
            case 4: xpc_dictionary_set_int64(reply, "bootstrap-version", 1); break;
            case 5: xpc_dictionary_set_string(reply, "execution-role", "native"); break;
            case 6: xpc_dictionary_set_int64(reply, "execution-role", 1); break;
            case 7: xpc_dictionary_set_uint64(reply, "execution-role", LSB_EXECUTION_NATIVE_METAL_SERVICE + 1); break;
            case 8: xpc_dictionary_set_uint64(reply, "execution-role", UINT64_MAX); break;
            case 9: xpc_dictionary_set_uint64(reply, "execution-role", LSB_EXECUTION_NATIVE_METAL_PROBE); break;
            case 10:
                xpc_dictionary_set_uint64(reply, "execution-role", LSB_EXECUTION_NATIVE_METAL_PROBE);
                xpc_dictionary_set_value(reply, "registered-ports", NULL);
                xpc_dictionary_set_value(reply, "bootstrap-port", NULL);
                xpc_dictionary_set_value(reply, "launchd-tank-port", NULL);
                xpc_dictionary_set_int64(reply, "guest-pid", 4243);
                break;
            case 11: xpc_dictionary_set_value(reply, "native-production-status", NULL); break;
            case 12: xpc_dictionary_set_string(reply, "native-production-status", "2"); break;
            case 13: xpc_dictionary_set_int64(reply, "native-production-status", LSB_NATIVE_DEVELOPMENT); break;
            case 14: xpc_dictionary_set_bool(reply, "native-production-status", false); break;
            case 15: xpc_dictionary_set_uint64(reply, "native-production-status", LSB_NATIVE_DEVELOPMENT + 1); break;
            case 16: xpc_dictionary_set_uint64(reply, "native-production-status", UINT64_MAX); break;
            case 17:
                xpc_dictionary_set_uint64(reply, "bootstrap-version", 1);
                xpc_dictionary_set_value(reply, "native-production-status", NULL);
                break;
            default: assert(false);
        }
    }
    if (variant == 1) xpc_dictionary_set_string(reply, "child-status-fd", "not a descriptor");
    else if (variant != 0) {
        int statusFD = variant == 4 ? fds->wake[0] : variant == 7 ? fds->readOnlyStatus : variant == 8 ? fds->shortStatus : fds->status;
        descriptor = xpc_fd_create(statusFD);
        assert(descriptor);
        xpc_dictionary_set_value(reply, "child-status-fd", descriptor);
        xpc_release(descriptor);
    }
    if (variant == 3) xpc_dictionary_set_string(reply, "child-wake-fd", "not a descriptor");
    else if (variant != 2) {
        int wakeFD = variant == 5 ? fds->wake[1] : variant == 6 ? fds->status : variant == 9 ? fds->blockingWake[0] : fds->wake[0];
        descriptor = xpc_fd_create(wakeFD);
        assert(descriptor);
        xpc_dictionary_set_value(reply, "child-wake-fd", descriptor);
        xpc_release(descriptor);
    }
    assert(!xpc_pipe_routine_reply(reply));
    xpc_release(reply);
}

static void registeredPortsHost(mach_port_t broker, const BootstrapFDs *bootstrap) {
    for (unsigned variant = 0; variant < PortRoundTripCount; ++variant) {
        LSBReceivedMessage pending, message;
        assert(!LSBTransportReceive(broker, &pending) && pending.kind == (variant % 2 ? LSB_REQUEST_EXEC : LSB_REQUEST_SPAWN));
        assert(pending.ports.registeredPresent == (variant < 4) && pending.ports.count == (variant < 3 ? 3 : 0));
        assert(pending.ports.bootstrapPresent == (variant < 3 || variant == 4));
        if (variant == 0) assert(MACH_PORT_VALID(pending.ports.registered[0]) && !pending.ports.registered[1] &&
                                 MACH_PORT_VALID(pending.ports.registered[2]) && pending.ports.registered[0] != pending.ports.registered[2]);
        else if (variant == 1) assert(MACH_PORT_VALID(pending.ports.registered[0]) && MACH_PORT_VALID(pending.ports.registered[1]) &&
                                      pending.ports.registered[1] == pending.ports.registered[2] &&
                                      pending.ports.registered[0] != pending.ports.registered[1]);
        else for (unsigned i = 0; i < LSB_MAX_REGISTERED_PORTS; ++i) assert(pending.ports.registered[i] == MACH_PORT_NULL);
        if (variant == 0 || variant == 1) assert(pending.ports.bootstrapPort == pending.ports.registered[0]);
        else if (variant == 4) assert(MACH_PORT_VALID(pending.ports.bootstrapPort));
        else assert(pending.ports.bootstrapPort == MACH_PORT_NULL);
        assert(!LSBTransportReply(&pending, 0, 4242, 80 + variant, NULL, NULL, NULL));
        LSBSpawnPorts anchors = pending.ports; // Borrowed names used only to verify disposal below.
        for (unsigned i = 0; i < pending.ports.count; ++i) {
            mach_port_t port = pending.ports.registered[i];
            if (!MACH_PORT_VALID(port)) continue;
            mach_port_urefs_t references;
            unsigned copies = pending.ports.bootstrapPort == port;
            for (unsigned j = 0; j < pending.ports.count; ++j) copies += pending.ports.registered[j] == port;
            assert(!mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, &references) && references == copies);
            mach_msg_header_t ping = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0), .msgh_size = sizeof(ping),
                                      .msgh_remote_port = port, .msgh_id = (int)(900 + 10 * variant + i)};
            assert(!mach_msg(&ping, MACH_SEND_MSG, sizeof(ping), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL));
        }
        assert(!LSBTransportReceive(broker, &message) && message.kind == LSB_REQUEST_CHECKIN && !message.ports.count);
        if (variant % 2 || variant == 6) {
            LSBBootstrapMetadata metadata = {.guestPID = variant == 6 ? 4242 : variant == 5 ? INT_MAX : 1234 + (int)variant,
                .parentPID = variant == 1 ? 0 : 4567, .ports = &pending.ports,
                .launchdTankPort = variant == 1 ? pending.ports.bootstrapPort : MACH_PORT_NULL,
                .executionRole = variant == 6 ? LSB_EXECUTION_NATIVE_METAL_PROBE : LSB_EXECUTION_SIMULATOR,
                .nativeProductionStatus = variant == 1 ? LSB_NATIVE_DEVELOPMENT : variant == 3 ? LSB_NATIVE_PRODUCTION : LSB_NATIVE_PRODUCTION_UNKNOWN};
            assert(!LSBTransportReplyBootstrapWithMetadata(&message, 0, 4242, 80 + variant, "/tmp/RuntimeRoot", &pending.plan,
                                                           pending.fds, bootstrap->status, bootstrap->wake[0], &metadata));
        } else assert(!LSBTransportReplyBootstrapWithPorts(&message, 0, 4242, 80 + variant, "/tmp/RuntimeRoot", &pending.plan,
                                                           pending.fds, bootstrap->status, bootstrap->wake[0], &pending.ports));
        assert(!memcmp(&anchors, &pending.ports, sizeof(anchors)));
        LSBTransportDisposeMessage(&message);
        assert(!LSBTransportReceive(broker, &message) && message.kind == LSB_REQUEST_READY && message.requestID == 80 + variant);
        LSBTransportDisposeMessage(&pending);
        assert(!pending.ports.count && !pending.ports.registeredPresent && !pending.ports.bootstrapPresent && !pending.ports.bootstrapPort);
        assert(pending.fds[0] == -1);
        for (unsigned i = 0; i < anchors.count; ++i) if (MACH_PORT_VALID(anchors.registered[i])) {
            mach_port_type_t type;
            assert(mach_port_type(mach_task_self(), anchors.registered[i], &type) == KERN_INVALID_NAME);
        }
        if (MACH_PORT_VALID(anchors.bootstrapPort)) {
            mach_port_type_t type;
            assert(mach_port_type(mach_task_self(), anchors.bootstrapPort, &type) == KERN_INVALID_NAME);
        }
        assert(!LSBTransportReply(&message, 0, 4242, 80 + variant, NULL, NULL, NULL));
        LSBTransportDisposeMessage(&message);
    }
    for (unsigned variant = 0; variant < 2; ++variant) {
        LSBReceivedMessage message;
        assert(!LSBTransportReceive(broker, &message) && message.kind == LSB_REQUEST_EXEC);
        assert(!LSBTransportReply(&message, variant == 0 ? ENOTSUP : 0, 0, 0, NULL, NULL, NULL));
        LSBTransportDisposeMessage(&message);
    }
}

static int host(const char *executable) {
    mach_port_t port;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port));
    assert(!mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND));
    posix_spawnattr_t attributes;
    assert(!posix_spawnattr_init(&attributes));
    assert(!posix_spawnattr_set_registered_ports_np(&attributes, &port, 1));
    char *arguments[] = {(char *)executable, "--client", NULL};
    pid_t child;
    assert(!posix_spawn(&child, executable, NULL, &attributes, arguments, environ));
    posix_spawnattr_destroy(&attributes);

    LSBReceivedMessage pending;
    assert(!LSBTransportReceive(port, &pending));
    assert(pending.kind == LSB_REQUEST_SPAWN && pending.audit.val[5] == (unsigned)child);
    assert(pending.plan.fdCount == 1 && pending.fds[0] >= 0);
    char byte = 0;
    assert(read(pending.fds[0], &byte, 1) == 1 && byte == 'a');
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, replyWorker, &pending));
    assert(!pthread_join(worker, NULL));
    assert(pending.object == NULL && fcntl(pending.fds[0], F_GETFD) >= 0);
    LSBReceivedMessage message;
    BootstrapFDs bootstrap = makeBootstrapFDs();
    mach_port_t tank;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &tank));
    assert(!mach_port_insert_right(mach_task_self(), tank, tank, MACH_MSG_TYPE_MAKE_SEND));
    assert(!LSBTransportReceive(port, &message) && message.kind == LSB_REQUEST_CHECKIN);
    assert(message.audit.val[5] == (unsigned)child);
    assert(!LSBTransportReplyBootstrap(&message, 0, child, 77, "/tmp/RuntimeRoot", &pending.plan, pending.fds, bootstrap.status, bootstrap.wake[0]));
    assert(fcntl(bootstrap.status, F_GETFD) >= 0 && fcntl(bootstrap.wake[0], F_GETFD) >= 0);
    LSBTransportDisposeMessage(&message);
    assert(!LSBTransportReceive(port, &message) && message.kind == LSB_REQUEST_READY && message.requestID == 77 && !message.error);
    char shared[6];
    assert(pread(bootstrap.status, shared, sizeof(shared), 0) == sizeof(shared) && !memcmp(shared, "shared", sizeof(shared)));
    assert(!LSBTransportReply(&message, 0, child, 77, NULL, NULL, NULL));
    LSBTransportDisposeMessage(&message);
    for (unsigned index = 0; index < 2; ++index) {
        assert(!LSBTransportReceive(port, &message) && message.kind == LSB_REQUEST_CHECKIN);
        void *request = message.object;
        assert(LSBTransportReplyLaunchdBootstrap(&message, 0, child, 78 + index, "/tmp/RuntimeRoot", &pending.plan,
                                                pending.fds, bootstrap.status, bootstrap.wake[0], MACH_PORT_NULL) == EINVAL);
        assert(LSBTransportReplyLaunchdBootstrap(&message, 0, child, 78 + index, "/tmp/RuntimeRoot", &pending.plan,
                                                pending.fds, bootstrap.status, bootstrap.wake[0], MACH_PORT_DEAD) == EINVAL);
        assert(message.object == request);
        assert(!LSBTransportReplyLaunchdBootstrap(&message, 0, child, 78 + index, "/tmp/RuntimeRoot", &pending.plan,
                                                 pending.fds, bootstrap.status, bootstrap.wake[0], tank));
        mach_port_urefs_t references;
        assert(!mach_port_get_refs(mach_task_self(), tank, MACH_PORT_RIGHT_SEND, &references) && references == 1);
        assert(fcntl(bootstrap.status, F_GETFD) >= 0 && fcntl(bootstrap.wake[0], F_GETFD) >= 0 && !message.object);
        LSBTransportDisposeMessage(&message);
    }
    struct { mach_msg_header_t header; mach_msg_max_trailer_t trailer; } ping;
    assert(!mach_msg(&ping.header, MACH_RCV_MSG, 0, sizeof(ping), tank, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL));
    assert(ping.header.msgh_id == 885 && ping.header.msgh_size == sizeof(mach_msg_header_t));
    registeredPortsHost(port, &bootstrap);
    for (unsigned variant = 0; variant < MalformedPortsCount; ++variant) {
        unsigned before = openDescriptorCount();
        assert(LSBTransportReceive(port, &message) == EPROTO && !message.object && !message.ports.count);
        assert(!message.ports.registeredPresent && !message.ports.bootstrapPresent && !message.ports.bootstrapPort);
        for (unsigned i = 0; i < LSB_MAX_REGISTERED_PORTS; ++i) assert(message.ports.registered[i] == MACH_PORT_NULL);
        for (unsigned i = 0; i < LSB_MAX_FDS; ++i) assert(message.fds[i] == -1);
        mach_port_urefs_t references;
        assert(!mach_port_get_refs(mach_task_self(), tank, MACH_PORT_RIGHT_SEND, &references) && references == 1);
        assert(openDescriptorCount() == before);
    }
    for (unsigned variant = 0; variant < MalformedRoleRequestCount; ++variant) {
        unsigned before = openDescriptorCount();
        assert(LSBTransportReceive(port, &message) == EPROTO && !message.object);
        for (unsigned i = 0; i < LSB_MAX_FDS; ++i) assert(message.fds[i] == -1);
        assert(openDescriptorCount() == before);
    }
    const int signals[] = {0, SIGTERM, SIGKILL, SIGTERM, SIGTERM};
    for (unsigned variant = 0; variant < sizeof(signals) / sizeof(signals[0]); ++variant) {
        assert(!LSBTransportReceive(port, &message) && message.kind == LSB_REQUEST_KILL && !message.requestID && !message.error);
        assert(message.audit.val[5] == (unsigned)child && message.targetPID == 4242 && message.signalNumber == signals[variant]);
        for (unsigned i = 0; i < LSB_MAX_FDS; ++i) assert(message.fds[i] == -1);
        if (variant == 1) {
            assert(!kill(child, SIGCHLD));
            char acknowledged = 0;
            for (unsigned attempt = 0; attempt < 1000 && acknowledged != 's'; ++attempt) {
                ssize_t count = pread(pending.fds[0], &acknowledged, 1, 6);
                assert(count >= 0);
                if (acknowledged != 's') usleep(1000);
            }
            assert(acknowledged == 's');
        }
        assert(!LSBTransportReply(&message, variant == 2 ? ESRCH : 0, variant == 3 ? 4243 : 4242, variant == 4 ? 77 : 0, NULL, NULL, NULL));
        LSBTransportDisposeMessage(&message);
    }
    auditResolutionHost(port, child);
    entitlementHost(port, child, 16);
    entitlementHost(port, child, 7);
    codeHashHost(port, child);
    rebootHost(port, child, pending.fds[0], tank);
    for (unsigned variant = 0; variant < MalformedRebootCount; ++variant) {
        unsigned before = openDescriptorCount();
        assert(LSBTransportReceive(port, &message) == EPROTO && !message.object && !message.nativeServicePort && !message.peerIdentity.taskName);
        assert(!message.ports.registeredPresent && !message.ports.bootstrapPresent && !message.ports.count);
        for (unsigned i = 0; i < LSB_MAX_FDS; ++i) assert(message.fds[i] == -1);
        mach_port_urefs_t references;
        assert(!mach_port_get_refs(mach_task_self(), tank, MACH_PORT_RIGHT_SEND, &references) && references == 1);
        assert(openDescriptorCount() == before);
    }
    for (unsigned variant = 0; variant < MalformedBootstrapCount; ++variant) {
        assert(!LSBTransportReceive(port, &message) && message.kind == LSB_REQUEST_CHECKIN);
        unsigned before = openDescriptorCount();
        replyMalformedBootstrap(&message, &pending.plan, pending.fds[0], &bootstrap, tank, variant);
        LSBTransportDisposeMessage(&message);
        assert(openDescriptorCount() == before);
        mach_port_urefs_t references;
        assert(!mach_port_get_refs(mach_task_self(), tank, MACH_PORT_RIGHT_SEND, &references) && references == 1);
    }
    closeBootstrapFDs(&bootstrap);
    LSBTransportDisposeMessage(&pending);
    assert(pending.fds[0] == -1);

    assert(!LSBTransportReceive(port, &message));
    assert(!LSBTransportReply(&message, EACCES, 0, 0, NULL, NULL, NULL));
    LSBTransportDisposeMessage(&message);
    assert(!LSBTransportReceive(port, &message));
    xpc_object_t invalid = xpc_dictionary_create_reply((xpc_object_t)message.object);
    assert(invalid);
    xpc_dictionary_set_string(invalid, "version", "not an integer");
    assert(!xpc_pipe_routine_reply(invalid));
    xpc_release(invalid);
    LSBTransportDisposeMessage(&message);
    for (unsigned variant = 0; variant < 5; ++variant) {
        unsigned before = openDescriptorCount();
        assert(LSBTransportReceive(port, &message) == EPROTO && message.object == NULL);
        for (unsigned i = 0; i < LSB_MAX_FDS; ++i) assert(message.fds[i] == -1);
        assert(openDescriptorCount() == before);
    }
    for (unsigned variant = 0; variant < MalformedKillCount; ++variant) {
        unsigned before = openDescriptorCount();
        assert(LSBTransportReceive(port, &message) == EPROTO && message.object == NULL);
        for (unsigned i = 0; i < LSB_MAX_FDS; ++i) assert(message.fds[i] == -1);
        assert(openDescriptorCount() == before);
    }
    for (unsigned variant = 0; variant < MalformedAuditCount; ++variant) {
        unsigned before = openDescriptorCount();
        assert(LSBTransportReceive(port, &message) == EPROTO && message.object == NULL);
        for (unsigned i = 0; i < LSB_MAX_FDS; ++i) assert(message.fds[i] == -1);
        assert(openDescriptorCount() == before);
    }
    for (unsigned variant = 0; variant < MalformedEntitlementCount; ++variant) {
        unsigned before = openDescriptorCount();
        assert(LSBTransportReceive(port, &message) == EPROTO && message.object == NULL);
        for (unsigned i = 0; i < LSB_MAX_FDS; ++i) assert(message.fds[i] == -1);
        assert(!message.nativeServicePort && !message.ports.registeredPresent && !message.ports.bootstrapPresent);
        assert(openDescriptorCount() == before);
    }
    assert(!LSBTransportReceive(port, &message) && message.error == ENOEXEC);
    assert(!LSBTransportReply(&message, ENOEXEC, 0, 77, NULL, NULL, NULL));
    LSBTransportDisposeMessage(&message);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(!mach_port_deallocate(mach_task_self(), tank));
    assert(!mach_port_mod_refs(mach_task_self(), tank, MACH_PORT_RIGHT_RECEIVE, -1));
    mach_port_deallocate(mach_task_self(), port);
    mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
    puts("SpawnTransport parent: retained asynchronous reply ownership and audit identity checks passed");
    return 0;
}

int main(int argc, char **argv) {
    _Static_assert(LSB_TRANSPORT_VERSION == 6 && LSB_BOOTSTRAP_VERSION == 3, "audio output and native capture require current transport/bootstrap versions");
    // Bound a failed regression test without adding a signal handler to the production transport.
    alarm(20);
    return argc == 2 && !strcmp(argv[1], "--client") ? client() : host(argv[0]);
}
