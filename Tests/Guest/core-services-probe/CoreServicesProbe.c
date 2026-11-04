#include "../../../ProcessSupport/PrivateXPC.h"
#include <errno.h>
#include <inttypes.h>
#include <notify.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void timedOut(int signalNumber) {
    (void)signalNumber;
    static const char message[] = "[core-services-probe] timed out after 20 seconds\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124); // Do not attempt potentially blocked IPC or non-signal-safe cleanup in the timeout handler.
}

static int lookupService(const char *name) {
    if (!name || !name[0] || strlen(name) >= 128) {
        fprintf(stderr, "[core-services-probe] invalid bootstrap service name\n");
        return 1;
    }
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t status = bootstrap_look_up(bootstrap_port, name, &port);
    fprintf(stderr, "[core-services-probe] lookup name=%s status=%d port=%u\n", name, status, port);
    int failed = status != KERN_SUCCESS || !MACH_PORT_VALID(port);
    if (MACH_PORT_VALID(port)) {
        kern_return_t release = mach_port_deallocate(mach_task_self(), port);
        if (release) { fprintf(stderr, "[core-services-probe] deallocate name=%s status=%d\n", name, release); failed = 1; }
    }
    return failed;
}

static int checkNotification(int token, int *changed, const char *stage) {
    *changed = -1;
    uint32_t status = notify_check(token, changed);
    if (status || (*changed != 0 && *changed != 1)) {
        fprintf(stderr, "[core-services-probe] notify_check stage=%s status=%u changed=%d\n", stage, status, *changed);
        return 1;
    }
    return 0;
}

static int testNotifications(void) {
    uint32_t nonce[2];
    arc4random_buf(nonce, sizeof(nonce));
    char name[128];
    snprintf(name, sizeof(name), "com.kdt.LiveSimBooter.core-services-probe.%ld.%08x%08x", (long)getpid(), nonce[0], nonce[1]);
    int writer = -1, reader = -1, failed = 1, changed;
    uint32_t status = notify_register_check(name, &writer);
    fprintf(stderr, "[core-services-probe] notify_register_check name=%s status=%u writer=%d\n", name, status, writer);
    if (status || writer < 0) goto finish;
    status = notify_register_check(name, &reader);
    fprintf(stderr, "[core-services-probe] notify_register_check reader status=%u token=%d\n", status, reader);
    if (status || reader < 0) goto finish;
    // notify_check's first observation is always true. Consume it before testing a real post on the separate reader token.
    if (checkNotification(writer, &changed, "writer-initial") || checkNotification(reader, &changed, "reader-initial")) goto finish;

    uint64_t expected = UINT64_C(0x4c53420000000000) | nonce[0], observed = 0;
    status = notify_set_state(writer, expected);
    fprintf(stderr, "[core-services-probe] notify_set_state status=%u state=0x%016" PRIx64 "\n", status, expected);
    if (status) goto finish;
    status = notify_get_state(reader, &observed);
    fprintf(stderr, "[core-services-probe] notify_get_state status=%u state=0x%016" PRIx64 "\n", status, observed);
    if (status || observed != expected) goto finish;
    status = notify_post(name);
    fprintf(stderr, "[core-services-probe] notify_post status=%u\n", status);
    if (status) goto finish;
    for (unsigned attempt = 0; attempt < 300; ++attempt) {
        if (checkNotification(reader, &changed, "after-post")) goto finish;
        if (changed) {
            observed = 0;
            status = notify_get_state(reader, &observed);
            fprintf(stderr, "[core-services-probe] notify roundtrip changed=1 state=0x%016" PRIx64 " status=%u polls=%u\n",
                observed, status, attempt + 1);
            if (!status && observed == expected) failed = 0;
            goto finish;
        }
        usleep(10000);
    }
    fprintf(stderr, "[core-services-probe] notify_check did not observe post within 3 seconds\n");

finish:
    if (reader >= 0) {
        status = notify_cancel(reader);
        fprintf(stderr, "[core-services-probe] notify_cancel reader=%d status=%u\n", reader, status);
        if (status) failed = 1;
    }
    if (writer >= 0) {
        status = notify_cancel(writer);
        fprintf(stderr, "[core-services-probe] notify_cancel writer=%d status=%u\n", writer, status);
        if (status) failed = 1;
    }
    return failed;
}

int main(int argc, char *argv[]) {
    setvbuf(stderr, NULL, _IONBF, 0);
    struct sigaction action = {.sa_handler = timedOut};
    sigemptyset(&action.sa_mask);
    sigset_t unblocked;
    sigemptyset(&unblocked);
    sigaddset(&unblocked, SIGALRM);
    if (sigaction(SIGALRM, &action, NULL) || sigprocmask(SIG_UNBLOCK, &unblocked, NULL)) {
        fprintf(stderr, "[core-services-probe] timeout setup failed: %s\n", strerror(errno));
        return 2;
    }
    alarm(20);
    fprintf(stderr, "[core-services-probe] pid=%ld bootstrap=%u\n", (long)getpid(), bootstrap_port);
    static const char *const defaults[] = {
        "com.apple.system.notification_center", "com.apple.lsd.mapdb", "com.apple.runningboard",
        "com.apple.containermanagerd", "com.apple.fontservicesd", "com.apple.cfprefsd.daemon"
    };
    unsigned lookupFailures = 0;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) lookupFailures += lookupService(argv[i]);
    } else {
        for (size_t i = 0; i < sizeof(defaults) / sizeof(*defaults); ++i) lookupFailures += lookupService(defaults[i]);
    }
    int notificationFailed = testNotifications();
    alarm(0);
    fprintf(stderr, "[core-services-probe] result lookup_failures=%u notify_failed=%d\n", lookupFailures, notificationFailed);
    return lookupFailures || notificationFailed ? 1 : 0;
}
