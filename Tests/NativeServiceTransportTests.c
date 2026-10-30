#include "../ProcessSupport/SpawnTransport.c"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

static mach_port_t broker, endpoint;
static void *client(void *unused) {
    (void)unused;
    assert(LSBTransportReadyNativeServiceOnPort(broker, 9, 0, MACH_PORT_NULL) == EINVAL);
    assert(LSBTransportReadyNativeServiceOnPort(broker, 9, EIO, endpoint) == EINVAL);
    assert(!LSBTransportReadyNativeServiceOnPort(broker, 9, 0, endpoint));
    for (unsigned variant = 0; variant < 5; ++variant) {
        xpc_object_t request = NULL;
        assert(!createRequest(variant == 2 ? LSB_REQUEST_CHECKIN : LSB_REQUEST_READY, variant == 2 ? 0 : 9, variant == 3 ? EIO : 0, &request));
        assert(request);
        if (variant == 0) xpc_dictionary_set_string(request, "native-service-port", "not a port");
        else if (variant == 1) xpc_dictionary_set_uint64(request, "native-service-port", endpoint);
        else xpc_dictionary_set_mach_send(request, "native-service-port", variant == 4 ? MACH_PORT_NULL : endpoint);
        xpc_object_t reply = NULL;
        assert(!callBrokerOnPort(broker, request, &reply));
        int error = 0; pid_t pid = 0; uint64_t identifier = 0;
        assert(!decodeReplyHeader(reply, &error, &pid, &identifier) && error == EPROTO);
        xpc_release(reply);
        xpc_release(request);
    }
    return NULL;
}
int main(void) {
    alarm(10);
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &broker));
    assert(!mach_port_insert_right(mach_task_self(), broker, broker, MACH_MSG_TYPE_MAKE_SEND));
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &endpoint));
    assert(!mach_port_insert_right(mach_task_self(), endpoint, endpoint, MACH_MSG_TYPE_MAKE_SEND));
    pthread_t thread; assert(!pthread_create(&thread, NULL, client, NULL));
    LSBReceivedMessage message;
    assert(!LSBTransportReceive(broker, &message) && message.kind == LSB_REQUEST_READY && message.nativeServicePort == endpoint);
    assert(!LSBTransportReply(&message, 0, 123, 9, NULL, NULL, NULL));
    LSBTransportDisposeMessage(&message);
    assert(message.nativeServicePort == MACH_PORT_NULL);
    for (unsigned i = 0; i < 5; ++i) {
        assert(LSBTransportReceive(broker, &message) == EPROTO && message.object == NULL && !message.nativeServicePort);
    }
    assert(!pthread_join(thread, NULL));
    mach_port_urefs_t references;
    assert(!mach_port_get_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_SEND, &references) && references == 1);
    assert(!mach_port_deallocate(mach_task_self(), endpoint));
    assert(!mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(!mach_port_deallocate(mach_task_self(), broker));
    assert(!mach_port_mod_refs(mach_task_self(), broker, MACH_PORT_RIGHT_RECEIVE, -1));
    puts("NativeServiceTransport: endpoint ownership and malformed READY boundaries passed");
    return 0;
}
