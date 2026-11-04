#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Exported by the matching simulator CoreFoundation, although its public declaration is hidden on iOS.
extern CFNotificationCenterRef CFNotificationCenterGetDistributedCenter(void);
extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_look_up(mach_port_t bootstrap, const char *name, mach_port_t *port);

static struct {
    CFNotificationCenterRef center;
    CFStringRef name;
    CFStringRef object;
    CFDictionaryRef userInfo;
    atomic_uint delivered;
    atomic_uint invalid;
} probe;

static void timedOut(int signalNumber) {
    (void)signalNumber;
    static const char message[] = "[distributed-notification-probe] timed out after 20 seconds\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static double monotonicTime(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value)) return -1;
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static void lookupService(const char *name) {
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t status = bootstrap_look_up(bootstrap_port, name, &port);
    fprintf(stderr, "[distributed-notification-probe] lookup name=%s status=%d port=%u\n", name, status, port);
    if (MACH_PORT_VALID(port)) {
        status = mach_port_deallocate(mach_task_self(), port);
        if (status) fprintf(stderr, "[distributed-notification-probe] deallocate status=%d\n", status);
    }
}

static void received(CFNotificationCenterRef center, void *observer, CFNotificationName name, const void *object, CFDictionaryRef userInfo) {
    bool valid = observer == &probe && center == probe.center && name && object && userInfo;
    valid = valid && CFGetTypeID(name) == CFStringGetTypeID() && CFGetTypeID(object) == CFStringGetTypeID();
    valid = valid && CFGetTypeID(userInfo) == CFDictionaryGetTypeID();
    valid = valid && CFEqual(name, probe.name) && CFEqual(object, probe.object) && CFEqual(userInfo, probe.userInfo);
    fprintf(stderr, "[distributed-notification-probe] callback valid=%d main_thread=%d\n", valid, pthread_main_np());
    if (!valid) atomic_fetch_add_explicit(&probe.invalid, 1, memory_order_release);
    atomic_fetch_add_explicit(&probe.delivered, 1, memory_order_release);
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

int main(void) {
    setvbuf(stderr, NULL, _IONBF, 0);
    struct sigaction action = {.sa_handler = timedOut};
    sigemptyset(&action.sa_mask);
    sigset_t unblocked;
    sigemptyset(&unblocked);
    sigaddset(&unblocked, SIGALRM);
    if (sigaction(SIGALRM, &action, NULL) || sigprocmask(SIG_UNBLOCK, &unblocked, NULL)) {
        fprintf(stderr, "[distributed-notification-probe] timeout setup failed: %s\n", strerror(errno));
        return 2;
    }
    alarm(20);
    fprintf(stderr, "[distributed-notification-probe] pid=%ld bootstrap=%u\n", (long)getpid(), bootstrap_port);
    lookupService("com.apple.distributed_notifications@0v3");
    lookupService("com.apple.distributed_notifications@1v3");

    uint32_t nonce[4];
    arc4random_buf(nonce, sizeof(nonce));
    char nonceText[33], nameText[128];
    snprintf(nonceText, sizeof(nonceText), "%08x%08x%08x%08x", nonce[0], nonce[1], nonce[2], nonce[3]);
    snprintf(nameText, sizeof(nameText), "com.kdt.LiveSimBooter.distributed-notification-probe.%ld.%s", (long)getpid(), nonceText);
    probe.center = CFNotificationCenterGetDistributedCenter();
    probe.name = CFStringCreateWithCString(NULL, nameText, kCFStringEncodingUTF8);
    probe.object = CFStringCreateWithCString(NULL, nonceText, kCFStringEncodingUTF8);
    int64_t sequenceValue = 0x4c53420012345678;
    CFNumberRef sequence = CFNumberCreate(NULL, kCFNumberSInt64Type, &sequenceValue);
    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)nonce, sizeof(nonce));
    if (sequence && data && probe.object) {
        const void *keys[] = {CFSTR("nonce"), CFSTR("sequence"), CFSTR("bytes")};
        const void *values[] = {probe.object, sequence, data};
        probe.userInfo = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    if (data) CFRelease(data);
    if (sequence) CFRelease(sequence);
    if (!probe.center || probe.center == CFNotificationCenterGetLocalCenter() || !probe.name || !probe.object || !probe.userInfo) {
        fprintf(stderr, "[distributed-notification-probe] distributed center or property-list setup failed\n");
        return 2;
    }

    // The stock distributed path queues both operations to the same distnoted XPC connection; it does not dispatch posts locally.
    CFNotificationCenterAddObserver(probe.center, &probe, received, probe.name, probe.object, CFNotificationSuspensionBehaviorDeliverImmediately);
    fprintf(stderr, "[distributed-notification-probe] registered name=%s object=%s\n", nameText, nonceText);
    double start = monotonicTime();
    if (start < 0) return 2;
    CFNotificationCenterPostNotificationWithOptions(probe.center, probe.name, probe.object, probe.userInfo, kCFNotificationDeliverImmediately);
    fprintf(stderr, "[distributed-notification-probe] post queued; waiting for serialized payload roundtrip\n");
    while (!atomic_load_explicit(&probe.delivered, memory_order_acquire)) {
        double now = monotonicTime();
        if (now < 0 || now - start >= 5.0) break;
        SInt32 result = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
        if (result == kCFRunLoopRunFinished) usleep(10000);
    }
    CFNotificationCenterRemoveObserver(probe.center, &probe, probe.name, probe.object);
    unsigned delivered = atomic_load_explicit(&probe.delivered, memory_order_acquire);
    unsigned invalid = atomic_load_explicit(&probe.invalid, memory_order_acquire);
    double elapsed = monotonicTime() - start;
    fprintf(stderr, "[distributed-notification-probe] result delivered=%u invalid=%u elapsed=%.3f seconds\n", delivered, invalid, elapsed);
    // This one-shot diagnostic keeps expected values alive until exit, including for any already queued callback after removal.
    alarm(0);
    return delivered && !invalid ? 0 : 1;
}
