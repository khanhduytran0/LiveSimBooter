#include "../ProcessSupport/SpawnTransport.c"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

static mach_port_t broker;
int LSBTestCopyBrokerPort(mach_port_t *port) {
    *port = broker;
    return mach_port_mod_refs(mach_task_self(), broker, MACH_PORT_RIGHT_SEND, 1) ? EIO : 0;
}
enum { Valid, Denied, WrongType, WrongSize, ExtraRight, BadVersion, Variants };
static void *server(void *unused) {
    (void)unused;
    for (unsigned variant = 0; variant < Variants; ++variant) {
        LSBReceivedMessage message;
        assert(!LSBTransportReceive(broker, &message) && message.kind == LSB_REQUEST_OPEN_AUDIO_OUTPUT);
        size_t size = variant == WrongSize ? LSBAudioMapBytes * 2 : LSBAudioMapBytes;
        void *region = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
        assert(region != MAP_FAILED);
        xpc_object_t memory = xpc_shmem_create(region, size);
        assert(memory);
        ((uint32_t *)region)[0] = 0x1234abcd;
        if (variant == Valid || variant == Denied) assert(!LSBTransportReplyAudioOutput(&message, variant == Denied ? EPERM : 0,
                                                                                     variant == Denied ? NULL : memory));
        else {
            xpc_object_t reply = xpc_dictionary_create_reply(message.object);
            xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION + (variant == BadVersion));
            xpc_dictionary_set_uint64(reply, "request-id", 0);
            xpc_dictionary_set_int64(reply, "pid", 0); xpc_dictionary_set_int64(reply, "error", 0);
            if (variant == WrongType) xpc_dictionary_set_data(reply, "audio-ring", "bad", 3);
            else xpc_dictionary_set_value(reply, "audio-ring", memory);
            if (variant == ExtraRight) xpc_dictionary_set_mach_send(reply, "unexpected-port", broker);
            assert(!xpc_pipe_routine_reply(reply)); xpc_release(reply);
        }
        LSBTransportDisposeMessage(&message);
        if (variant == Valid || variant == WrongType || variant == WrongSize || variant == ExtraRight) {
            assert(!LSBTransportReceive(broker, &message) && message.kind == LSB_REQUEST_CLOSE_AUDIO_OUTPUT);
            if (variant == Valid) assert(((uint32_t *)region)[0] == 0x5678abcd); // Truly shared, not a copy/file-backed audio payload.
            assert(!LSBTransportReply(&message, 0, 0, 0, NULL, NULL, NULL)); LSBTransportDisposeMessage(&message);
        }
        xpc_release(memory); assert(!munmap(region, size));
    }
    LSBReceivedMessage malformed;
    assert(LSBTransportReceive(broker, &malformed) == EPROTO && !malformed.object);
    return NULL;
}
int main(void) {
    alarm(20);
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &broker));
    assert(!mach_port_insert_right(mach_task_self(), broker, broker, MACH_MSG_TYPE_MAKE_SEND));
    pthread_t thread; assert(!pthread_create(&thread, NULL, server, NULL));
    for (unsigned variant = 0; variant < Variants; ++variant) {
        void *mapping = NULL; size_t size = 0;
        int error = LSBTransportOpenAudioOutput(&mapping, &size);
        if (variant == Valid) {
            assert(!error && mapping && size == LSBAudioMapBytes && ((uint32_t *)mapping)[0] == 0x1234abcd);
            ((uint32_t *)mapping)[0] = 0x5678abcd;
            assert(!LSBTransportCloseAudioOutput()); assert(!munmap(mapping, size));
        } else assert(error == (variant == Denied ? EPERM : EPROTO) && !mapping && !size);
    }
    xpc_object_t request = createRequest(LSB_REQUEST_OPEN_AUDIO_OUTPUT, 0, 0), reply = NULL;
    xpc_dictionary_set_mach_send(request, "unexpected-port", broker);
    assert(!callBroker(request, &reply));
    int error; pid_t pid; uint64_t requestID;
    assert(!decodeReplyHeader(reply, &error, &pid, &requestID) && error == EPROTO);
    xpc_release(request); xpc_release(reply);
    assert(!pthread_join(thread, NULL));
    mach_port_mod_refs(mach_task_self(), broker, MACH_PORT_RIGHT_SEND, -1);
    mach_port_mod_refs(mach_task_self(), broker, MACH_PORT_RIGHT_RECEIVE, -1);
    puts("AudioOutputTransport: shared mapping, close, denial, strict size/type/version and extra-capability rejection passed");
}
