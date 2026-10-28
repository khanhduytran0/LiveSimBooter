#include "../ProcessSupport/SpawnTransport.h"
#include "../ProcessSupport/PrivateXPC.h"
#include <assert.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
extern int posix_spawnattr_set_registered_ports_np(posix_spawnattr_t *, mach_port_t *, uint32_t);

enum { Valid, Denied, Empty, EncodingFailure, BadVersion, Reserved, BadCount, DuplicatePID, BadPath, MissingData, WrongType,
       ExtraFD, ErrorData, BadEnvelope, BadReplyPID, BadRequestID, ShortData, Variants };
enum { MalformedRequests = 7 };

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
static void addFD(xpc_object_t object) {
    xpc_object_t fd = xpc_fd_create(STDERR_FILENO);
    assert(fd); xpc_dictionary_set_value(object, "unexpected-fd", fd); xpc_release(fd);
}
static LSBProcessList *newList(void) {
    LSBProcessList *list = calloc(1, LSBProcessListSize(2));
    assert(list);
    list->version = LSB_PROCESS_LIST_VERSION; list->count = 2; list->recordSize = sizeof(LSBProcessRecord);
    for (unsigned i = 0; i < 2; ++i) {
        LSBProcessRecord *record = &list->records[i];
        record->guestPID = 42 + i; record->parentPID = 1;
        record->audit.val[5] = 142 + i; record->audit.val[7] = 900 + i;
        memcpy(record->shortInfo, &record->audit.val[5], sizeof(uint32_t));
        strcpy(record->executablePath, i ? "/bin/df" : "/sbin/launchd_sim");
    }
    return list;
}
static void replyVariant(LSBReceivedMessage *message, unsigned variant) {
    LSBProcessList *list = newList();
    if (variant <= EncodingFailure) {
        if (variant == Empty) list->count = 0;
        int error = variant == Denied ? EPERM : 0;
        if (variant == EncodingFailure) ++list->version;
        assert(LSBTransportReplyProcessList(message, -1, NULL, 0) == EINVAL && message->object);
        int result = LSBTransportReplyProcessList(message, error, error ? NULL : list, error ? 0 : LSBProcessListSize(list->count));
        assert(result == (variant == EncodingFailure ? EINVAL : 0) && !message->object);
        free(list); return;
    }
    if (variant == BadVersion) ++list->version;
    if (variant == Reserved) list->reserved = 1;
    if (variant == BadCount) list->count = LSB_PROCESS_LIST_CAPACITY + 1;
    if (variant == DuplicatePID) list->records[1].guestPID = list->records[0].guestPID;
    if (variant == BadPath) strcpy(list->records[0].executablePath, "/bin/../bad");
    xpc_object_t reply = xpc_dictionary_create_reply(message->object);
    assert(reply);
    xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION + (variant == BadEnvelope));
    xpc_dictionary_set_uint64(reply, "request-id", variant == BadRequestID);
    xpc_dictionary_set_int64(reply, "error", variant == ErrorData ? EPERM : 0);
    xpc_dictionary_set_int64(reply, "pid", variant == BadReplyPID);
    if (variant != MissingData) xpc_dictionary_set_data(reply, "process-list", list, LSBProcessListSize(2) - (variant == ShortData));
    if (variant == WrongType) xpc_dictionary_set_string(reply, "process-list", "not a process list");
    if (variant == ExtraFD) addFD(reply);
    assert(!xpc_pipe_routine_reply(reply));
    xpc_release(reply); xpc_release(message->object); message->object = NULL; free(list);
}
static void malformedRequest(unsigned variant) {
    mach_port_t port = MACH_PORT_NULL;
    assert(!LSBTestCopyBrokerPort(&port));
    xpc_object_t pipe = xpc_pipe_create_from_port(port, 0);
    assert(pipe);
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0), reply = NULL;
    assert(request);
    xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(request, "kind", LSB_REQUEST_LIST_PROCESSES);
    xpc_dictionary_set_uint64(request, "request-id", 0);
    xpc_dictionary_set_int64(request, "error", 0);
    switch (variant) {
        case 0: xpc_dictionary_set_int64(request, "target-pid", 42); break;
        case 1: xpc_dictionary_set_uint64(request, "operation", 0); break;
        case 2: addFD(request); break;
        case 3: xpc_dictionary_set_mach_send(request, "unexpected-port", port); break;
        case 4: xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION - 1); break;
        case 5: xpc_dictionary_set_uint64(request, "request-id", 1); break;
        case 6: xpc_dictionary_set_int64(request, "error", EPERM); break;
    }
    assert(!xpc_pipe_routine(pipe, request, &reply) && reply && xpc_dictionary_get_int64(reply, "error") == EPROTO);
    xpc_release(reply); xpc_release(request); xpc_release(pipe); mach_port_deallocate(mach_task_self(), port);
}
static int client(void) {
    LSBProcessList *list = (void *)1; size_t size = 1;
    assert(LSBTransportListProcesses(&list, NULL) == EINVAL && !list);
    assert(LSBTransportListProcesses(NULL, &size) == EINVAL && !size);
    for (unsigned variant = 0; variant < Variants; ++variant) {
        unsigned descriptors = descriptorCount();
        list = (void *)1; size = 1;
        int result = LSBTransportListProcesses(&list, &size);
        int expected = variant == Valid || variant == Empty ? 0 : variant == Denied ? EPERM : variant == EncodingFailure ? EINVAL : EPROTO;
        assert(result == expected);
        if (!result) {
            assert(!LSBValidateProcessList(list, size) && list->count == (variant == Empty ? 0u : 2u));
            if (list->count) assert(list->records[0].guestPID == 42 && list->records[0].audit.val[5] == 142);
            free(list);
        } else assert(!list && !size);
        assert(descriptorCount() == descriptors);
    }
    for (unsigned variant = 0; variant < MalformedRequests; ++variant) malformedRequest(variant);
    assert(!LSBRequireSingleThread());
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--client")) return client();
    alarm(30);
    mach_port_t broker = MACH_PORT_NULL;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &broker));
    assert(!mach_port_insert_right(mach_task_self(), broker, broker, MACH_MSG_TYPE_MAKE_SEND));
    posix_spawnattr_t attributes;
    assert(!posix_spawnattr_init(&attributes) && !posix_spawnattr_set_registered_ports_np(&attributes, &broker, 1));
    char *arguments[] = {argv[0], "--client", NULL}; pid_t child = 0;
    assert(!posix_spawn(&child, argv[0], NULL, &attributes, arguments, environ));
    posix_spawnattr_destroy(&attributes);
    for (unsigned variant = 0; variant < Variants; ++variant) {
        LSBReceivedMessage message;
        assert(!LSBTransportReceive(broker, &message) && message.kind == LSB_REQUEST_LIST_PROCESSES);
        assert(message.audit.val[5] == (unsigned)child && !message.requestID && !message.nativeServicePort && !message.peerIdentity.taskName);
        replyVariant(&message, variant);
        LSBTransportDisposeMessage(&message);
    }
    for (unsigned variant = 0; variant < MalformedRequests; ++variant) {
        unsigned descriptors = descriptorCount();
        LSBReceivedMessage message;
        assert(LSBTransportReceive(broker, &message) == EPROTO && !message.object && !message.nativeServicePort && !message.peerIdentity.taskName);
        assert(descriptorCount() == descriptors);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(!mach_port_mod_refs(mach_task_self(), broker, MACH_PORT_RIGHT_SEND, -1));
    assert(!mach_port_mod_refs(mach_task_self(), broker, MACH_PORT_RIGHT_RECEIVE, -1));
    puts("ProcessInspectionTransport: authenticated no-rights request, bounded list, malformed envelopes/records and cleanup passed");
    return 0;
}
