#include "../ProcessSupport/TaskPortCapture.h"
#include <mach/arm/thread_status.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern kern_return_t mach_vm_read_overwrite(vm_map_t, mach_vm_address_t, mach_vm_size_t, mach_vm_address_t, mach_vm_size_t *);
#pragma pack(push, 4)
typedef struct {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t port;
    pid_t pid;
    uint64_t address, expected;
} ForwardMessage;
typedef struct { mach_msg_header_t header; pid_t pid; uint64_t address, expected; } HelloMessage;
typedef struct { mach_msg_header_t header; int result; } ResultMessage;
#pragma pack(pop)
typedef union { mach_msg_header_t header; unsigned char bytes[4096]; } MessageBuffer;
enum { ObserverReady = 9101, ChildHello, ForwardTask, ObserverResult };
static volatile sig_atomic_t childPID, observerPID;
static volatile uint64_t sentinel;

static void stopOwnedChildren(void) {
    if (childPID > 0) kill(childPID, SIGKILL);
    if (observerPID > 0) kill(observerPID, SIGKILL);
}
static void timeout(int signalNumber) { stopOwnedChildren(); _exit(128 + signalNumber); }
static void checked(kern_return_t status) {
    if (status) { fprintf(stderr, "Mach failure %d: %s\n", status, mach_error_string(status)); exit(20); }
}
static void releaseEndpoint(mach_port_t port) {
    checked(mach_port_deallocate(mach_task_self(), port));
    checked(mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1));
}
static void sendMessage(mach_msg_header_t *header) {
    checked(mach_msg(header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, header->msgh_size, 0, MACH_PORT_NULL, 2000, MACH_PORT_NULL));
}
static audit_token_t receiveMessage(mach_port_t port, MessageBuffer *buffer) {
    memset(buffer, 0, sizeof(*buffer));
    checked(mach_msg(&buffer->header, MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
        MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT), 0, sizeof(*buffer), port, 3000, MACH_PORT_NULL));
    size_t offset = (buffer->header.msgh_size + 3u) & ~(size_t)3u;
    assert(offset + sizeof(mach_msg_audit_trailer_t) <= sizeof(*buffer));
    mach_msg_audit_trailer_t *trailer = (void *)(buffer->bytes + offset);
    assert(trailer->msgh_trailer_type == MACH_MSG_TRAILER_FORMAT_0 && trailer->msgh_trailer_size == sizeof(*trailer));
    return trailer->msgh_audit;
}
static mach_port_t inheritedEndpoint(void) {
    mach_port_t port;
    checked(task_get_bootstrap_port(mach_task_self(), &port));
    return port;
}
static int inspectTask(mach_port_t task, pid_t pid, uint64_t address, uint64_t expected) {
    audit_token_t audit;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    kern_return_t auditStatus = task_info(task, TASK_AUDIT_TOKEN, (task_info_t)&audit, &count);
    mach_port_t control = MACH_PORT_NULL;
    kern_return_t controlStatus = task_get_special_port(task, TASK_KERNEL_PORT, &control);
    if (MACH_PORT_VALID(control)) mach_port_deallocate(mach_task_self(), control);
    thread_act_array_t threads = NULL;
    mach_msg_type_number_t threadCount = 0;
    kern_return_t threadsStatus = task_threads(task, &threads, &threadCount);
    uint64_t value = 0;
    mach_vm_size_t bytes = 0;
    kern_return_t readStatus = mach_vm_read_overwrite(task, address, sizeof(value), (mach_vm_address_t)&value, &bytes);
    printf("inspection process=%d target=%d audit=%d control=%d threads=%d count=%u read=%d value=0x%llx\n",
        getpid(), pid, auditStatus, controlStatus, threadsStatus, threadCount, readStatus, value);
    for (unsigned i = 0; i < threadCount; ++i) mach_port_deallocate(mach_task_self(), threads[i]);
    if (threads) vm_deallocate(mach_task_self(), (vm_address_t)threads, threadCount * sizeof(*threads));
    return auditStatus || audit.val[5] != (uint32_t)pid || controlStatus || threadsStatus || !threadCount ||
        readStatus || bytes != sizeof(value) || value != expected || task == mach_task_self();
}
static void simpleTests(void) {
    assert(LSBTaskPortCaptureCreate(NULL) == EINVAL);
    LSBTaskPortCaptureOffer empty = {0}, offer;
    audit_token_t expected = {{0}};
    mach_port_t task = 123;
    assert(LSBTaskPortCapturePerform(&empty) == EINVAL);
    assert(LSBTaskPortCaptureReceive(&empty, &expected, &task) == EINVAL && task == MACH_PORT_NULL);
    assert(!LSBTaskPortCaptureCreate(&offer));
    mach_port_type_t type;
    checked(mach_port_type(mach_task_self(), offer.port, &type));
    assert((type & (MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_SEND)) == (MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_SEND));
    assert(LSBTaskPortCaptureReceive(&offer, &expected, &task) == EAGAIN && task == MACH_PORT_NULL);
    ForwardMessage untrusted = {.header = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX,
        .msgh_size = sizeof(untrusted), .msgh_remote_port = offer.port, .msgh_id = 2407}, .body.msgh_descriptor_count = 1,
        .port = {.name = offer.port, .disposition = MACH_MSG_TYPE_COPY_SEND, .type = MACH_MSG_PORT_DESCRIPTOR}};
    sendMessage(&untrusted.header);
    assert(LSBTaskPortCaptureReceive(&offer, &expected, &task) == EACCES && task == MACH_PORT_NULL);
    mach_port_urefs_t references;
    checked(mach_port_get_refs(mach_task_self(), offer.port, MACH_PORT_RIGHT_SEND, &references));
    assert(references == 1); // Rejecting an untrusted message released its descriptor right.
    MessageBuffer oversized = {.header = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0),
        .msgh_size = sizeof(oversized), .msgh_remote_port = offer.port, .msgh_id = 2407}};
    sendMessage(&oversized.header);
    assert(LSBTaskPortCaptureReceive(&offer, &expected, &task) == EMSGSIZE && task == MACH_PORT_NULL);
    assert(LSBTaskPortCaptureReceive(&offer, &expected, &task) == EAGAIN); // Oversized messages cannot pin the queue head.
    releaseEndpoint(offer.port);
}
static int observer(void) {
    childPID = observerPID = 0;
    alarm(10);
    mach_port_t parent = inheritedEndpoint();
    LSBTaskPortCaptureOffer local;
    assert(!LSBTaskPortCaptureCreate(&local));
    ForwardMessage ready = {.header = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX,
        .msgh_size = sizeof(ready), .msgh_remote_port = parent, .msgh_id = ObserverReady}, .body.msgh_descriptor_count = 1,
        .port = {.name = local.port, .disposition = MACH_MSG_TYPE_COPY_SEND, .type = MACH_MSG_PORT_DESCRIPTOR}, .pid = getpid()};
    sendMessage(&ready.header);
    MessageBuffer buffer;
    audit_token_t audit = receiveMessage(local.port, &buffer);
    ForwardMessage *forward = (void *)&buffer;
    assert(forward->header.msgh_id == ForwardTask && forward->header.msgh_size == sizeof(*forward) && audit.val[5] == (uint32_t)getppid());
    assert(forward->body.msgh_descriptor_count == 1 && forward->port.type == MACH_MSG_PORT_DESCRIPTOR);
    int result = inspectTask(forward->port.name, forward->pid, forward->address, forward->expected);
    mach_msg_destroy(&buffer.header);
    ResultMessage reply = {.header = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0), .msgh_size = sizeof(reply),
        .msgh_remote_port = parent, .msgh_id = ObserverResult}, .result = result};
    sendMessage(&reply.header);
    releaseEndpoint(local.port);
    mach_port_deallocate(mach_task_self(), parent);
    return result;
}
static int child(LSBTaskPortCaptureOffer offer, int keepAliveFD) {
    childPID = observerPID = 0;
    alarm(10);
    offer.port = inheritedEndpoint();
    thread_t thread = mach_thread_self();
    LSBTaskPortCaptureOffer prior;
    assert(!LSBTaskPortCaptureCreate(&prior));
    checked(thread_set_exception_ports(thread, EXC_MASK_BREAKPOINT, prior.port, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64));
    sentinel = 0x4c53420000000000ull | (uint32_t)getpid();
    HelloMessage hello = {.header = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0), .msgh_size = sizeof(hello),
        .msgh_remote_port = offer.port, .msgh_id = ChildHello}, .pid = getpid(), .address = (uintptr_t)&sentinel, .expected = sentinel};
    sendMessage(&hello.header);
    assert(!LSBTaskPortCapturePerform(&offer));
    exception_mask_t masks[EXC_TYPES_COUNT];
    mach_port_t ports[EXC_TYPES_COUNT];
    exception_behavior_t behaviors[EXC_TYPES_COUNT];
    thread_state_flavor_t flavors[EXC_TYPES_COUNT];
    mach_msg_type_number_t count = EXC_TYPES_COUNT;
    checked(thread_get_exception_ports(thread, EXC_MASK_BREAKPOINT, masks, &count, ports, behaviors, flavors));
    assert(count == 1 && masks[0] == EXC_MASK_BREAKPOINT && ports[0] == prior.port &&
        behaviors[0] == (exception_behavior_t)(EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES) && flavors[0] == ARM_THREAD_STATE64);
    mach_port_deallocate(mach_task_self(), ports[0]);
    checked(thread_set_exception_ports(thread, EXC_MASK_BREAKPOINT, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE));
    releaseEndpoint(prior.port);
    mach_port_deallocate(mach_task_self(), thread);
    mach_port_deallocate(mach_task_self(), offer.port);
    char byte;
    assert(read(keepAliveFD, &byte, 1) == 1);
    close(keepAliveFD);
    return 0;
}
static void captureAndForwardTest(void) {
    LSBTaskPortCaptureOffer offer;
    assert(!LSBTaskPortCaptureCreate(&offer));
    mach_port_t previous;
    checked(task_get_bootstrap_port(mach_task_self(), &previous));
    checked(task_set_bootstrap_port(mach_task_self(), offer.port));
    observerPID = fork();
    assert(observerPID >= 0);
    if (!observerPID) _exit(observer());
    MessageBuffer buffer;
    audit_token_t audit = receiveMessage(offer.port, &buffer);
    ForwardMessage *ready = (void *)&buffer;
    assert(ready->header.msgh_id == ObserverReady && ready->header.msgh_size == sizeof(*ready) && audit.val[5] == (uint32_t)observerPID);
    mach_port_t observerPort = ready->port.name;
    int keepAlive[2];
    assert(!pipe(keepAlive));
    childPID = fork();
    assert(childPID >= 0);
    if (!childPID) { close(keepAlive[1]); _exit(child(offer, keepAlive[0])); }
    close(keepAlive[0]);
    checked(task_set_bootstrap_port(mach_task_self(), previous));
    mach_port_deallocate(mach_task_self(), previous);
    audit = receiveMessage(offer.port, &buffer);
    HelloMessage hello = *(HelloMessage *)&buffer;
    assert(hello.header.msgh_id == ChildHello && hello.header.msgh_size == sizeof(hello) && audit.val[5] == (uint32_t)childPID && hello.pid == childPID);
    mach_port_t task = MACH_PORT_NULL;
    int result = EAGAIN;
    for (unsigned attempt = 0; attempt < 3000 && result == EAGAIN; ++attempt) {
        result = LSBTaskPortCaptureReceive(&offer, &audit, &task);
        if (result == EAGAIN) usleep(1000);
    }
    assert(!result && MACH_PORT_VALID(task));
    assert(!inspectTask(task, childPID, hello.address, hello.expected));
    ForwardMessage forward = {.header = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX,
        .msgh_size = sizeof(forward), .msgh_remote_port = observerPort, .msgh_id = ForwardTask}, .body.msgh_descriptor_count = 1,
        .port = {.name = task, .disposition = MACH_MSG_TYPE_COPY_SEND, .type = MACH_MSG_PORT_DESCRIPTOR},
        .pid = childPID, .address = hello.address, .expected = hello.expected};
    sendMessage(&forward.header);
    audit = receiveMessage(offer.port, &buffer);
    assert(buffer.header.msgh_id == ObserverResult && buffer.header.msgh_size == sizeof(ResultMessage) && audit.val[5] == (uint32_t)observerPID);
    assert(!((ResultMessage *)&buffer)->result);
    assert(write(keepAlive[1], "x", 1) == 1);
    close(keepAlive[1]);
    int childStatus, observerStatus;
    assert(waitpid(childPID, &childStatus, 0) == childPID);
    childPID = 0;
    assert(waitpid(observerPID, &observerStatus, 0) == observerPID);
    observerPID = 0;
    assert(WIFEXITED(childStatus) && !WEXITSTATUS(childStatus) && WIFEXITED(observerStatus) && !WEXITSTATUS(observerStatus));
    mach_port_deallocate(mach_task_self(), task);
    mach_port_deallocate(mach_task_self(), observerPort);
    releaseEndpoint(offer.port);
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, timeout);
    signal(SIGABRT, timeout);
    signal(SIGTERM, timeout);
    atexit(stopOwnedChildren);
    alarm(20);
    simpleTests();
    captureAndForwardTest();
    alarm(0);
    puts("TaskPortCapture: authenticated kernel capture, existing handler restoration, control right forwarding, memory reads, and rejection cleanup passed");
    return 0;
}
