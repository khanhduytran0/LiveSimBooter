#include <assert.h>
#include <mach/mach.h>
#include <mach/arm/thread_status.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../ProcessSupport/PeerDebugger.h"

#define LSB_WORKQUEUE_BOOTSTRAP_TESTING
#include "../Guest/WorkqueueBootstrap.c"

static mach_port_t controlPort;
static audit_token_t brokerAudit;
static _Atomic(unsigned) controlQueries;
void LSBProcessDebuggerHandleMessage(mach_msg_header_t *message, size_t capacity) {
    assert(message->msgh_local_port == controlPort);
    atomic_fetch_add(&controlQueries, 1);
    LSBPeerDebuggerReply(message, capacity, &brokerAudit);
}

uintptr_t LSBThreadSignPC(uintptr_t start, uint32_t flags) {
    assert(flags & StateNoPtrauth);
    return start;
}

int main(void) {
    nativeEntry = 0x2321218a74;
    guestEntry = 0x12b269a74;
    assert(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &exceptionPort) == KERN_SUCCESS);
    assert(mach_port_insert_right(mach_task_self(), exceptionPort, exceptionPort, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &exceptionPortSet));
    assert(!mach_port_move_member(mach_task_self(), exceptionPort, exceptionPortSet));
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &controlPort));
    assert(!mach_port_insert_right(mach_task_self(), controlPort, controlPort, MACH_MSG_TYPE_MAKE_SEND));
    atomic_store(&exceptionServerReady, true);
    assert(!LSBWorkqueueAddProcessControlPort(controlPort));
    mach_msg_type_number_t auditCount = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&brokerAudit, &auditCount));
    assert(auditCount == TASK_AUDIT_TOKEN_COUNT);
    pthread_t server;
    assert(pthread_create(&server, NULL, exceptionServer, NULL) == 0);
    assert(pthread_detach(server) == 0);

    ExceptionRequest request = {0};
    request.thread = mach_thread_self();
    request.task = mach_task_self();
    request.exception = EXC_BAD_ACCESS;
    request.codeCount = 2;
    request.code[0] = KERN_INVALID_ADDRESS;
    request.code[1] = nativeEntry;
    request.state.__pc = nativeEntry;
    request.state.__pad = StateNoPtrauth;
    request.state.__x[0] = 0x104008000;
    request.state.__x[1] = request.thread;
    request.state.__x[2] = 0x104000000;
    request.state.__x[4] = WorkqueueTSDBaseSet;
    request.state.__sp = request.state.__x[0];

    mach_port_urefs_t beforeThread, beforeTask, afterThread, afterTask;
    assert(mach_port_get_refs(mach_task_self(), request.thread, MACH_PORT_RIGHT_SEND, &beforeThread) == KERN_SUCCESS);
    assert(mach_port_get_refs(mach_task_self(), request.task, MACH_PORT_RIGHT_SEND, &beforeTask) == KERN_SUCCESS);
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
        arm_thread_state64_t state = request.state;
        int flavor = ARM_THREAD_STATE64;
        mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
        assert(forwardMach64(exceptionPort, EXCEPTION_STATE_IDENTITY, &request, &flavor, (thread_state_t)&state, &count) == KERN_SUCCESS);
        assert(flavor == ARM_THREAD_STATE64 && count == ARM_THREAD_STATE64_COUNT);
        arm_thread_state64_t expected = request.state;
        expected.__pc = guestEntry;
        assert(!memcmp(&state, &expected, sizeof(state)));
    }
    // A real unrelated fault gets a MIG error reply; no matching saved handler means it is not swallowed.
    request.code[1] += 4;
    arm_thread_state64_t state = request.state;
    int flavor = ARM_THREAD_STATE64;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    assert(forwardMach64(exceptionPort, EXCEPTION_STATE_IDENTITY, &request, &flavor, (thread_state_t)&state, &count) == KERN_FAILURE);
    assert(mach_port_get_refs(mach_task_self(), request.thread, MACH_PORT_RIGHT_SEND, &afterThread) == KERN_SUCCESS);
    assert(mach_port_get_refs(mach_task_self(), request.task, MACH_PORT_RIGHT_SEND, &afterTask) == KERN_SUCCESS);
    assert(beforeThread == afterThread && beforeTask == afterTask);
    struct kinfo_proc info;
    assert(!LSBPeerDebuggerQuery(controlPort, &brokerAudit, 500, &info));
    assert(info.kp_proc.p_pid == getpid() && atomic_load(&controlQueries) == 1);
    // Servicing a cooperative request must leave the original exception transport functional with its identity rights intact.
    request.code[1] = nativeEntry;
    state = request.state; count = ARM_THREAD_STATE64_COUNT;
    assert(!forwardMach64(exceptionPort, EXCEPTION_STATE_IDENTITY, &request, &flavor, (thread_state_t)&state, &count));
    assert(state.__pc == guestEntry);
    assert(mach_port_deallocate(mach_task_self(), request.thread) == KERN_SUCCESS);
    puts("Workqueue Mach: exception and authenticated control requests share one server thread with intact exception forwarding");
    return 0;
}
