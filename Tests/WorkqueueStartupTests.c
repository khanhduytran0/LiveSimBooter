#include <assert.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned createdThreads, detachedThreads, swappedHandlers, openedWorkqueues;
static kern_return_t testSwap(task_t task, exception_mask_t mask, mach_port_t port, exception_behavior_t behavior,
                             thread_state_flavor_t flavor, exception_mask_array_t masks, mach_msg_type_number_t *count,
                             exception_handler_array_t ports, exception_behavior_array_t behaviors, exception_flavor_array_t flavors) {
    assert(task == mach_task_self() && mask == EXC_MASK_BAD_ACCESS && MACH_PORT_VALID(port));
    assert(behavior == (exception_behavior_t)(EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES) && flavor == ARM_THREAD_STATE64);
    (void)masks; (void)ports; (void)behaviors; (void)flavors;
    *count = 0;
    ++swappedHandlers;
    return KERN_SUCCESS;
}
static int testCreate(pthread_t *thread, const pthread_attr_t *attributes, void *(*entry)(void *), void *argument) {
    assert(thread && !attributes && entry && !argument);
    *thread = pthread_self();
    ++createdThreads;
    return 0; // Test initialization order without replacing the test process's real exception handler or leaving a server alive.
}
static int testDetach(pthread_t thread) { assert(thread == pthread_self()); ++detachedThreads; return 0; }
static int testOpen(void) { ++openedWorkqueues; return 17; }
void LSBProcessDebuggerHandleMessage(mach_msg_header_t *message, size_t capacity) { (void)message; (void)capacity; abort(); }
uintptr_t LSBThreadSignPC(uintptr_t start, uint32_t flags) { (void)flags; return start; }

#define LSB_WORKQUEUE_BOOTSTRAP_TESTING
#define task_swap_exception_ports testSwap
#define pthread_create testCreate
#define pthread_detach testDetach
#define __workq_open testOpen
#include "../Guest/WorkqueueBootstrap.c"
#undef task_swap_exception_ports
#undef pthread_create
#undef pthread_detach
#undef __workq_open

int main(void) {
    mach_port_t control;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &control));
    assert(LSBWorkqueueAddProcessControlPort(control) == ENOTSUP);
    assert(!createdThreads && !swappedHandlers && !openedWorkqueues); // Native mode must stay untouched.
    assert(!setenv(LSB_HOST_WORKQUEUE_START_ENV, "0x2321218a74", 1));
    assert(!setenv(LSB_HOST_WORKQUEUE_FEATURES_ENV, "0x37", 1));
    assert(!setenv(LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV, "0", 1));
    LSBWorkqueueRegister(0x12b269a74);
    assert(LSBWorkqueueAddProcessControlPort(MACH_PORT_NULL) == EINVAL && !createdThreads);
    // This is the real device order: guest constructor registers its endpoint before libdispatch first opens the workqueue.
    assert(!LSBWorkqueueAddProcessControlPort(control));
    assert(atomic_load(&exceptionServerReady) && createdThreads == 1 && detachedThreads == 1 && swappedHandlers == 1 && !openedWorkqueues);
    mach_port_name_array_t members = NULL;
    mach_msg_type_number_t count = 0;
    assert(!mach_port_get_set_status(mach_task_self(), exceptionPortSet, &members, &count) && count == 2);
    assert((members[0] == control && members[1] == exceptionPort) || (members[1] == control && members[0] == exceptionPort));
    assert(!vm_deallocate(mach_task_self(), (vm_address_t)members, count * sizeof(*members)));
    assert(LSBWorkqueueOpen() == 17 && LSBWorkqueueOpen() == 17);
    assert(createdThreads == 1 && detachedThreads == 1 && swappedHandlers == 1 && openedWorkqueues == 2);
    assert(!mach_port_mod_refs(mach_task_self(), control, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(!mach_port_mod_refs(mach_task_self(), exceptionPortSet, MACH_PORT_RIGHT_PORT_SET, -1));
    assert(!mach_port_mod_refs(mach_task_self(), exceptionPort, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(!mach_port_deallocate(mach_task_self(), exceptionPort));
    puts("Workqueue startup: guest constructor creates one shared server before lazy workqueue open; native callers remain untouched");
    return 0;
}
