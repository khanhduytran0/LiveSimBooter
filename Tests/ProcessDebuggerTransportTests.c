#include "../ProcessSupport/SpawnTransport.h"
#include "../ProcessSupport/PrivateXPC.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/proc.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
extern int posix_spawnattr_set_registered_ports_np(posix_spawnattr_t *, mach_port_t *, uint32_t);
enum { Valid, Denied, Timeout, BadVersion, WrongPID, ShortInfo, MissingInfo, ExtraPort, ErrorInfo, VariantCount };
enum { BadRequests = 12 };

int LSBTestCopyBrokerPort(mach_port_t *port) {
    mach_port_array_t ports = NULL;
    mach_msg_type_number_t count = 0;
    if (mach_ports_lookup(mach_task_self(), &ports, &count)) return EIO;
    *port = count ? ports[0] : MACH_PORT_NULL;
    for (unsigned i = 1; i < count; ++i) if (MACH_PORT_VALID(ports[i])) mach_port_deallocate(mach_task_self(), ports[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)ports, count * sizeof(*ports));
    return MACH_PORT_VALID(*port) ? 0 : ENOENT;
}

static mach_port_t newPort(void) {
    mach_port_t port;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port));
    assert(!mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND));
    return port;
}

static void badRequest(unsigned variant, mach_port_t endpoint) {
    mach_port_t broker;
    assert(!LSBTestCopyBrokerPort(&broker));
    xpc_object_t pipe = xpc_pipe_create_from_port(broker, 0), request = xpc_dictionary_create(NULL, NULL, 0), reply = NULL;
    assert(pipe && request);
    xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(request, "kind", variant < 5 || variant >= 9 ? LSB_REQUEST_REGISTER_DEBUGGER_PORT : LSB_REQUEST_COPY_DEBUGGER_INFO);
    xpc_dictionary_set_uint64(request, "request-id", 0);
    xpc_dictionary_set_int64(request, "error", 0);
    if (variant < 5) xpc_dictionary_set_mach_send(request, "process-debugger-port", endpoint);
    else if (variant < 9) xpc_dictionary_set_int64(request, "target-pid", 42);
    switch (variant) {
        case 0: xpc_dictionary_set_value(request, "process-debugger-port", NULL); break;
        case 1: xpc_dictionary_set_uint64(request, "process-debugger-port", endpoint); break;
        case 2: xpc_dictionary_set_int64(request, "target-pid", 42); break;
        case 3: xpc_dictionary_set_uint64(request, "request-id", 1); break;
        case 4: xpc_dictionary_set_int64(request, "error", EPERM); break;
        case 5: xpc_dictionary_set_int64(request, "target-pid", 0); break;
        case 6: xpc_dictionary_set_mach_send(request, "process-debugger-port", endpoint); break;
        case 7: xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION - 1); break;
        case 8: xpc_dictionary_set_mach_send(request, "unexpected-port", endpoint); break;
        case 9: xpc_dictionary_set_bool(request, "activate-debugger-port", false); break;
        case 10: xpc_dictionary_set_uint64(request, "activate-debugger-port", 1); break;
        case 11: xpc_dictionary_set_bool(request, "activate-debugger-port", true);
                 xpc_dictionary_set_mach_send(request, "process-debugger-port", endpoint); break;
    }
    assert(!xpc_pipe_routine(pipe, request, &reply) && xpc_dictionary_get_int64(reply, "error") == EPROTO);
    xpc_release(reply); xpc_release(request); xpc_release(pipe); mach_port_deallocate(mach_task_self(), broker);
}

static int client(void) {
    mach_port_t endpoint = newPort();
    audit_token_t host = {{0}};
    assert(LSBTransportRegisterDebuggerPort(MACH_PORT_NULL, &host) == EINVAL && !host.val[5]);
    assert(!LSBTransportRegisterDebuggerPort(endpoint, &host));
    assert(host.val[5] == (unsigned)getppid() && host.val[7]); // The pipe reply exposes the real kernel sender, not supplied payload.
    assert(!LSBTransportActivateDebuggerPort(&host));
    struct kinfo_proc info;
    assert(LSBTransportCopyDebuggerInfo(0, &info) == EINVAL && !info.kp_proc.p_pid);
    for (unsigned variant = 0; variant < VariantCount; ++variant) {
        memset(&info, 0xa5, sizeof(info));
        int error = LSBTransportCopyDebuggerInfo(42, &info);
        int expected = variant == Valid ? 0 : variant == Denied ? EPERM : variant == Timeout ? ETIMEDOUT : EPROTO;
        assert(error == expected);
        if (!error) assert(info.kp_proc.p_pid == 42 && info.kp_proc.p_flag == P_TRACED);
        else for (size_t i = 0; i < sizeof(info); ++i) assert(!((unsigned char *)&info)[i]);
    }
    for (unsigned variant = 0; variant < BadRequests; ++variant) badRequest(variant, endpoint);
    assert(!LSBRequireSingleThread());
    mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_SEND, -1);
    mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_RECEIVE, -1);
    return 0;
}

static void replyVariant(LSBReceivedMessage *message, unsigned variant, mach_port_t endpoint) {
    struct kinfo_proc info = {0}; info.kp_proc.p_pid = 42; info.kp_proc.p_flag = P_TRACED;
    if (variant <= Timeout) {
        int error = variant == Denied ? EPERM : variant == Timeout ? ETIMEDOUT : 0;
        assert(!LSBTransportReplyDebuggerInfo(message, error, error ? NULL : &info));
        return;
    }
    xpc_object_t reply = xpc_dictionary_create_reply(message->object);
    assert(reply);
    xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION + (variant == BadVersion));
    xpc_dictionary_set_uint64(reply, "request-id", 0);
    xpc_dictionary_set_int64(reply, "error", variant == ErrorInfo ? EPERM : 0);
    xpc_dictionary_set_int64(reply, "pid", variant == ErrorInfo ? 0 : 42);
    if (variant == WrongPID) info.kp_proc.p_pid = 43;
    if (variant != MissingInfo) xpc_dictionary_set_data(reply, "process-debugger-info", &info, sizeof(info) - (variant == ShortInfo));
    if (variant == ExtraPort) xpc_dictionary_set_mach_send(reply, "unexpected-port", endpoint);
    assert(!xpc_pipe_routine_reply(reply));
    xpc_release(reply);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--client")) return client();
    alarm(30);
    mach_port_t broker = newPort();
    posix_spawnattr_t attributes;
    assert(!posix_spawnattr_init(&attributes) && !posix_spawnattr_set_registered_ports_np(&attributes, &broker, 1));
    char *arguments[] = {argv[0], "--client", NULL}; pid_t child;
    assert(!posix_spawn(&child, argv[0], NULL, &attributes, arguments, environ));
    posix_spawnattr_destroy(&attributes);
    LSBReceivedMessage message;
    assert(!LSBTransportReceive(broker, &message) && message.kind == LSB_REQUEST_REGISTER_DEBUGGER_PORT && message.audit.val[5] == (unsigned)child);
    mach_port_t endpoint = message.processDebuggerPort;
    assert(MACH_PORT_VALID(endpoint) && !mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_SEND, 1));
    assert(!LSBTransportReply(&message, 0, 0, 0, NULL, NULL, NULL));
    LSBTransportDisposeMessage(&message);
    assert(!LSBTransportReceive(broker, &message) && message.kind == LSB_REQUEST_REGISTER_DEBUGGER_PORT && message.activateDebuggerPort &&
           !message.processDebuggerPort && message.audit.val[5] == (unsigned)child);
    assert(!LSBTransportReply(&message, 0, 0, 0, NULL, NULL, NULL));
    LSBTransportDisposeMessage(&message);
    for (unsigned variant = 0; variant < VariantCount; ++variant) {
        assert(!LSBTransportReceive(broker, &message) && message.kind == LSB_REQUEST_COPY_DEBUGGER_INFO && message.targetPID == 42);
        replyVariant(&message, variant, endpoint);
        LSBTransportDisposeMessage(&message);
    }
    for (unsigned variant = 0; variant < BadRequests; ++variant) {
        assert(LSBTransportReceive(broker, &message) == EPROTO && !message.object && !message.processDebuggerPort);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    mach_port_deallocate(mach_task_self(), endpoint);
    mach_port_mod_refs(mach_task_self(), broker, MACH_PORT_RIGHT_SEND, -1);
    mach_port_mod_refs(mach_task_self(), broker, MACH_PORT_RIGHT_RECEIVE, -1);
    puts("ProcessDebuggerTransport: reply kernel audit, endpoint ownership, exact query data, errors and malformed envelopes passed");
    return 0;
}
