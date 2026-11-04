#include "../Guest/libLiveSimSpawn/SystemPowerCompatibility.h"
#include <CoreFoundation/CoreFoundation.h>
#include <assert.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static _Atomic(unsigned) callbackCount;

static void callback(void *refcon, io_service_t service, uint32_t message, void *argument) {
    (void)refcon; (void)service; (void)message; (void)argument;
    atomic_fetch_add(&callbackCount, 1);
}

static void assertDeadName(mach_port_name_t port) {
    mach_port_type_t type = 0;
    mach_port_urefs_t references = 0;
    assert(mach_port_type(mach_task_self(), port, &type) == KERN_SUCCESS && type == MACH_PORT_TYPE_DEAD_NAME);
    assert(mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_DEAD_NAME, &references) == KERN_SUCCESS && references == 1);
}

static void assertGone(mach_port_name_t port) {
    mach_port_type_t type = 0;
    assert(mach_port_type(mach_task_self(), port, &type) == KERN_INVALID_NAME);
}

#ifdef LSB_SYSTEM_POWER_INTERPOSE_TESTING

int main(void) {
    assert(!setenv(LSB_SYSTEM_POWER_ENABLE_ENV, "1", 1));
    IONotificationPortRef port = NULL;
    io_object_t notifier = IO_OBJECT_NULL;
    io_connect_t connection = IORegisterForSystemPower(NULL, &port, callback, &notifier);
    assert(connection && port && notifier && connection != notifier);
    assertDeadName(connection);
    assertDeadName(notifier);
    assert(IONotificationPortGetRunLoopSource(port));
    assert(IOAllowPowerChange(connection, 123) == kIOReturnUnsupported);
    assert(IOCancelPowerChange(connection, 456) == kIOReturnUnsupported);
    io_object_t oldNotifier = notifier;
    assert(IODeregisterForSystemPower(&notifier) == KERN_SUCCESS && !notifier);
    assertGone(oldNotifier);
    IONotificationPortDestroy(port);
    assert(IOServiceClose(connection) == KERN_SUCCESS);
    assertGone(connection);
    assert(!atomic_load(&callbackCount));
    puts("SystemPowerCompatibility: native IOKit calls resolve all five interpositions and complete real notification-port teardown");
    return 0;
}

#else

static io_connect_t nativeRegister(void *, IONotificationPortRef *, IOServiceInterestCallback, io_object_t *);
static IOReturn nativeDeregister(io_object_t *);
static kern_return_t nativeClose(io_connect_t);
static IOReturn nativeAllow(io_connect_t, intptr_t);
static IOReturn nativeCancel(io_connect_t, intptr_t);
static IONotificationPortRef createPort(mach_port_t);
static void destroyPort(IONotificationPortRef);
static kern_return_t allocateRight(ipc_space_t, mach_port_right_t, mach_port_name_t *);
static kern_return_t releaseRight(ipc_space_t, mach_port_name_t);

#define IORegisterForSystemPower nativeRegister
#define IODeregisterForSystemPower nativeDeregister
#define IOServiceClose nativeClose
#define IOAllowPowerChange nativeAllow
#define IOCancelPowerChange nativeCancel
#define IONotificationPortCreate createPort
#define IONotificationPortDestroy destroyPort
#define mach_port_allocate allocateRight
#define mach_port_deallocate releaseRight
#define LSB_SYSTEM_POWER_TESTING
#include "../Guest/libLiveSimSpawn/SystemPowerCompatibility.c"
#undef IORegisterForSystemPower
#undef IODeregisterForSystemPower
#undef IOServiceClose
#undef IOAllowPowerChange
#undef IOCancelPowerChange
#undef IONotificationPortCreate
#undef IONotificationPortDestroy
#undef mach_port_allocate
#undef mach_port_deallocate

static _Atomic(unsigned) nativeCalls, portsCreated, portsDestroyed, rightsCreated, rightsReleased, allocationCalls;
static _Atomic(unsigned) failAllocation;
static _Atomic(bool) failPort, failRelease;
static void *observedRefcon;
static IONotificationPortRef *observedPort;
static IOServiceInterestCallback observedCallback;
static io_object_t *observedNotifier;
static mach_port_name_t observedHandle;
static intptr_t observedNotification;

static io_connect_t nativeRegister(void *refcon, IONotificationPortRef *port, IOServiceInterestCallback cb, io_object_t *notifier) {
    atomic_fetch_add(&nativeCalls, 1);
    observedRefcon = refcon;
    observedPort = port;
    observedCallback = cb;
    observedNotifier = notifier;
    return 0x123456;
}

static IOReturn nativeDeregister(io_object_t *notifier) {
    atomic_fetch_add(&nativeCalls, 1);
    observedNotifier = notifier;
    if (notifier && !*notifier) return kIOReturnSuccess;
    return kIOReturnNoDevice;
}

static kern_return_t nativeClose(io_connect_t connection) {
    atomic_fetch_add(&nativeCalls, 1);
    observedHandle = connection;
    return kIOReturnNotOpen;
}

static IOReturn nativeAllow(io_connect_t connection, intptr_t notification) {
    atomic_fetch_add(&nativeCalls, 1);
    observedHandle = connection;
    observedNotification = notification;
    return kIOReturnNoDevice;
}

static IOReturn nativeCancel(io_connect_t connection, intptr_t notification) {
    atomic_fetch_add(&nativeCalls, 1);
    observedHandle = connection;
    observedNotification = notification;
    return kIOReturnNotOpen;
}

static IONotificationPortRef createPort(mach_port_t mainPort) {
    assert(mainPort == MACH_PORT_NULL);
    if (atomic_load(&failPort)) return NULL;
    IONotificationPortRef port = IONotificationPortCreate(mainPort);
    assert(port);
    atomic_fetch_add(&portsCreated, 1);
    return port;
}

static void destroyPort(IONotificationPortRef port) {
    assert(port);
    IONotificationPortDestroy(port);
    atomic_fetch_add(&portsDestroyed, 1);
}

static kern_return_t allocateRight(ipc_space_t task, mach_port_right_t kind, mach_port_name_t *name) {
    assert(task == mach_task_self() && kind == MACH_PORT_RIGHT_DEAD_NAME && name);
    unsigned attempt = atomic_fetch_add(&allocationCalls, 1) + 1;
    if (attempt == atomic_load(&failAllocation)) return KERN_RESOURCE_SHORTAGE;
    kern_return_t result = mach_port_allocate(task, kind, name);
    assert(result == KERN_SUCCESS);
    atomic_fetch_add(&rightsCreated, 1);
    return result;
}

static kern_return_t releaseRight(ipc_space_t task, mach_port_name_t name) {
    assert(task == mach_task_self());
    if (atomic_exchange(&failRelease, false)) return KERN_RESOURCE_SHORTAGE;
    assertDeadName(name);
    kern_return_t result = mach_port_deallocate(task, name);
    assert(result == KERN_SUCCESS);
    atomic_fetch_add(&rightsReleased, 1);
    return result;
}

typedef struct { IONotificationPortRef port; io_connect_t connection; io_object_t notifier; } Subscription;

static Subscription subscribe(void) {
    Subscription subscription = {0};
    subscription.connection = LSBRegisterForSystemPower(NULL, &subscription.port, callback, &subscription.notifier);
    assert(subscription.connection && subscription.port && subscription.notifier && subscription.connection != subscription.notifier);
    assertDeadName(subscription.connection);
    assertDeadName(subscription.notifier);
    return subscription;
}

static void finish(Subscription subscription, bool closeFirst) {
    if (closeFirst) assert(LSBSystemPowerServiceClose(subscription.connection) == KERN_SUCCESS);
    assert(LSBDeregisterForSystemPower(&subscription.notifier) == KERN_SUCCESS && !subscription.notifier);
    destroyPort(subscription.port);
    if (!closeFirst) assert(LSBSystemPowerServiceClose(subscription.connection) == KERN_SUCCESS);
}

static void assertEmpty(void) {
    assert(atomic_load(&portsCreated) == atomic_load(&portsDestroyed));
    assert(atomic_load(&rightsCreated) == atomic_load(&rightsReleased));
    assert(!atomic_load(&callbackCount));
    for (unsigned i = 0; i < LSB_SYSTEM_POWER_MAX_REGISTRATIONS; ++i)
        assert(!registrations[i].occupied && !registrations[i].connection && !registrations[i].notifier);
}

static void testGateAndNativeForwarding(void) {
    const char *disabled[] = {NULL, "", "0", "true", "01", "1 "};
    for (unsigned i = 0; i < sizeof(disabled) / sizeof(disabled[0]); ++i) {
        if (disabled[i]) assert(!setenv(LSB_SYSTEM_POWER_ENABLE_ENV, disabled[i], 1));
        else assert(!unsetenv(LSB_SYSTEM_POWER_ENABLE_ENV));
        IONotificationPortRef port = (IONotificationPortRef)(uintptr_t)0x1234;
        io_object_t notifier = 0x5678;
        assert(LSBRegisterForSystemPower(&notifier, &port, callback, &notifier) == 0x123456);
        assert(observedRefcon == &notifier && observedPort == &port && observedCallback == callback && observedNotifier == &notifier);
        assert(port == (IONotificationPortRef)(uintptr_t)0x1234 && notifier == 0x5678);
    }
    assert(!setenv(LSB_SYSTEM_POWER_ENABLE_ENV, "1", 1));
    io_object_t unknown = 0x1234;
    assert(LSBDeregisterForSystemPower(&unknown) == kIOReturnNoDevice && observedNotifier == &unknown && unknown == 0x1234);
    assert(LSBSystemPowerServiceClose(unknown) == kIOReturnNotOpen && observedHandle == unknown);
    assert(LSBAllowPowerChange(unknown, -123) == kIOReturnNoDevice && observedHandle == unknown && observedNotification == -123);
    assert(LSBCancelPowerChange(unknown, 456) == kIOReturnNotOpen && observedHandle == unknown && observedNotification == 456);
    assertEmpty();
}

static void testFailures(void) {
    unsigned nativeBefore = atomic_load(&nativeCalls);
    IONotificationPortRef port = (IONotificationPortRef)(uintptr_t)1;
    io_object_t notifier = 1;
    assert(!LSBRegisterForSystemPower(NULL, NULL, callback, &notifier) && !notifier);
    assert(!LSBRegisterForSystemPower(NULL, &port, callback, NULL) && !port);
    assert(!LSBRegisterForSystemPower(NULL, &port, NULL, &notifier) && !port && !notifier);
    assert(LSBDeregisterForSystemPower(NULL) == kIOReturnBadArgument);
    atomic_store(&failPort, true);
    assert(!LSBRegisterForSystemPower(NULL, &port, callback, &notifier) && !port && !notifier);
    atomic_store(&failPort, false);
    assertEmpty();
    for (unsigned allocation = 1; allocation <= 2; ++allocation) {
        atomic_store(&failAllocation, atomic_load(&allocationCalls) + allocation);
        port = (IONotificationPortRef)(uintptr_t)1;
        notifier = 1;
        assert(!LSBRegisterForSystemPower(NULL, &port, callback, &notifier) && !port && !notifier);
        assertEmpty();
    }
    atomic_store(&failAllocation, 0);
    Subscription subscription = subscribe();
    atomic_store(&failRelease, true);
    assert(LSBSystemPowerServiceClose(subscription.connection) == KERN_RESOURCE_SHORTAGE);
    assertDeadName(subscription.connection);
    atomic_store(&failRelease, true);
    assert(LSBDeregisterForSystemPower(&subscription.notifier) == KERN_RESOURCE_SHORTAGE && subscription.notifier);
    assertDeadName(subscription.notifier);
    finish(subscription, false);
    assert(atomic_load(&nativeCalls) == nativeBefore);
    assertEmpty();
}

static void testOwnershipAndCapacity(void) {
    unsigned nativeBefore = atomic_load(&nativeCalls);
    Subscription subscriptions[LSB_SYSTEM_POWER_MAX_REGISTRATIONS];
    for (unsigned i = 0; i < LSB_SYSTEM_POWER_MAX_REGISTRATIONS; ++i) subscriptions[i] = subscribe();
    IONotificationPortRef port = (IONotificationPortRef)(uintptr_t)1;
    io_object_t notifier = 1;
    unsigned created = atomic_load(&portsCreated);
    assert(!LSBRegisterForSystemPower(NULL, &port, callback, &notifier) && !port && !notifier);
    assert(atomic_load(&portsCreated) == created);
    Subscription *first = &subscriptions[0];
    assert(LSBSystemPowerServiceClose(first->notifier) == kIOReturnBadArgument);
    io_object_t wrongKind = first->connection;
    assert(LSBDeregisterForSystemPower(&wrongKind) == kIOReturnBadArgument && wrongKind == first->connection);
    assert(LSBAllowPowerChange(first->connection, 1) == kIOReturnUnsupported);
    assert(LSBCancelPowerChange(first->connection, 2) == kIOReturnUnsupported);
    assert(LSBAllowPowerChange(first->notifier, 1) == kIOReturnUnsupported);
    assert(LSBSystemPowerServiceClose(first->connection) == KERN_SUCCESS);
    assertGone(first->connection);
    assert(!LSBRegisterForSystemPower(NULL, &port, callback, &notifier)); // The notifier still occupies the table slot.
    assert(LSBDeregisterForSystemPower(&first->notifier) == KERN_SUCCESS && !first->notifier);
    destroyPort(first->port);
    *first = subscribe(); // Fully released slots are reusable.
    assert(!unsetenv(LSB_SYSTEM_POWER_ENABLE_ENV)); // Existing subscription ownership survives a later environment change.
    for (unsigned i = 0; i < LSB_SYSTEM_POWER_MAX_REGISTRATIONS; ++i) finish(subscriptions[i], i % 2);
    assert(!setenv(LSB_SYSTEM_POWER_ENABLE_ENV, "1", 1));
    assert(atomic_load(&nativeCalls) == nativeBefore);
    assertEmpty();
}

static void noOperation(void *context) { (void)context; }

static void testNotificationDeliverySetup(void) {
    Subscription subscription = subscribe();
    CFRunLoopSourceRef source = IONotificationPortGetRunLoopSource(subscription.port);
    assert(source && CFRunLoopSourceIsValid(source));
    CFRetain(source);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, true);
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
    finish(subscription, false);
    assert(!CFRunLoopSourceIsValid(source));
    CFRelease(source);

    subscription = subscribe();
    mach_port_t receive = IONotificationPortGetMachPort(subscription.port);
    dispatch_queue_t queue = dispatch_queue_create("SystemPowerCompatibilityTests", DISPATCH_QUEUE_SERIAL);
    assert(queue && receive);
    IONotificationPortSetDispatchQueue(subscription.port, queue);
    dispatch_sync_f(queue, NULL, noOperation);
    finish(subscription, true);
    // Native IOKit keeps the port alive until the dispatch source's asynchronous cancellation handler finishes.
    mach_port_type_t type;
    for (unsigned attempt = 0; attempt < 1000 && mach_port_type(mach_task_self(), receive, &type) == KERN_SUCCESS; ++attempt) {
        dispatch_sync_f(queue, NULL, noOperation);
        usleep(1000);
    }
    assertGone(receive);
    dispatch_release(queue);
    assertEmpty();
}

static void *concurrentRegistrations(void *context) {
    uintptr_t worker = (uintptr_t)context;
    for (unsigned iteration = 0; iteration < 100; ++iteration) finish(subscribe(), (worker + iteration) % 2);
    return NULL;
}

static void testConcurrentRegistrations(void) {
    unsigned nativeBefore = atomic_load(&nativeCalls);
    pthread_t workers[4];
    for (uintptr_t i = 0; i < 4; ++i) assert(!pthread_create(&workers[i], NULL, concurrentRegistrations, (void *)i));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(workers[i], NULL));
    assert(atomic_load(&nativeCalls) == nativeBefore);
    assertEmpty();
}

static void *concurrentClose(void *context) {
    Subscription *subscription = context;
    assert(LSBSystemPowerServiceClose(subscription->connection) == KERN_SUCCESS);
    return NULL;
}

static void *concurrentDeregister(void *context) {
    Subscription *subscription = context;
    assert(LSBDeregisterForSystemPower(&subscription->notifier) == KERN_SUCCESS && !subscription->notifier);
    return NULL;
}

static void testConcurrentTeardown(void) {
    unsigned nativeBefore = atomic_load(&nativeCalls);
    for (unsigned iteration = 0; iteration < 40; ++iteration) {
        Subscription subscription = subscribe();
        mach_port_name_t connection = subscription.connection, notifier = subscription.notifier;
        pthread_t closeWorker, deregisterWorker;
        assert(!pthread_create(&closeWorker, NULL, concurrentClose, &subscription));
        assert(!pthread_create(&deregisterWorker, NULL, concurrentDeregister, &subscription));
        assert(!pthread_join(closeWorker, NULL) && !pthread_join(deregisterWorker, NULL));
        assertGone(connection);
        assertGone(notifier);
        destroyPort(subscription.port);
    }
    assert(atomic_load(&nativeCalls) == nativeBefore);
    assertEmpty();
}

int main(void) {
    testGateAndNativeForwarding();
    testFailures();
    testOwnershipAndCapacity();
    testNotificationDeliverySetup();
    testConcurrentRegistrations();
    testConcurrentTeardown();
    puts("SystemPowerCompatibility: opt-in, forwarding, real ports/rights, rollback, capacity, teardown orders and concurrency passed");
    return 0;
}
#endif
