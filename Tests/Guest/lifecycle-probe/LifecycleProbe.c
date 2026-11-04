// Run as a launchd job to exercise process events after waitpid and adoption by simulator launchd. No private ledger APIs or fork.
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
enum { TimeoutSeconds = 45, EventIterations = 3, ChildReportFD = 3, ChildReadyFD = 4 };

static int64_t milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int remainingTime(int64_t deadline) {
    int64_t now = milliseconds();
    if (now < 0) return -1;
    if (now >= deadline) return 0;
    return deadline - now > INT_MAX ? INT_MAX : (int)(deadline - now);
}

static int transfer(int fd, void *buffer, size_t size, bool writing, int64_t deadline) {
    unsigned char *bytes = buffer;
    while (size) {
        int remaining = remainingTime(deadline);
        if (remaining < 0) return errno;
        if (!remaining) return ETIMEDOUT;
        struct pollfd descriptor = {.fd = fd, .events = writing ? POLLOUT : POLLIN};
        int ready = poll(&descriptor, 1, remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return errno;
        if (!ready) return ETIMEDOUT;
        if (descriptor.revents & (POLLNVAL | POLLERR)) return EIO;
        ssize_t count = writing ? write(fd, bytes, size) : read(fd, bytes, size);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) return count < 0 ? errno : EPIPE;
        bytes += count;
        size -= (size_t)count;
    }
    return 0;
}

static int makePipe(int descriptors[2]) {
    int original[2];
    descriptors[0] = descriptors[1] = -1;
    if (pipe(original)) return errno;
    int error = 0;
    for (unsigned i = 0; i < 2; ++i) {
        descriptors[i] = fcntl(original[i], F_DUPFD_CLOEXEC, 10);
        if (descriptors[i] < 0 && !error) error = errno;
    }
    close(original[0]);
    close(original[1]);
    if (error) {
        if (descriptors[0] >= 0) close(descriptors[0]);
        if (descriptors[1] >= 0) close(descriptors[1]);
        descriptors[0] = descriptors[1] = -1;
    }
    return error;
}

static int waitBounded(pid_t child, int *status, int64_t deadline) {
    for (;;) {
        pid_t result = waitpid(child, status, WNOHANG);
        if (result == child) return 0;
        if (result < 0 && errno != EINTR) return errno;
        int remaining = remainingTime(deadline);
        if (remaining < 0) return errno;
        if (!remaining) return ETIMEDOUT;
        (void)poll(NULL, 0, 1);
    }
}

static void stopChild(pid_t child) {
    if (child <= 0) return;
    (void)kill(child, SIGKILL);
    int status;
    (void)waitBounded(child, &status, milliseconds() + 5000);
}

static int waitSemaphore(dispatch_semaphore_t semaphore) {
    return dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, TimeoutSeconds * NSEC_PER_SEC)) ? ETIMEDOUT : 0;
}

typedef struct {
    dispatch_source_t source;
    dispatch_semaphore_t registered, event, canceled;
    uintptr_t eventHandle, cancelHandle;
    unsigned long data, eventMask, cancelMask;
} EventObservation;

static void sourceRegistered(void *context) {
    dispatch_semaphore_signal(((EventObservation *)context)->registered);
}

static void sourceEvent(void *context) {
    EventObservation *observation = context;
    observation->eventHandle = dispatch_source_get_handle(observation->source);
    observation->eventMask = dispatch_source_get_mask(observation->source);
    observation->data = dispatch_source_get_data(observation->source);
    dispatch_semaphore_signal(observation->event);
}

static void sourceCanceled(void *context) {
    EventObservation *observation = context;
    observation->cancelHandle = dispatch_source_get_handle(observation->source);
    observation->cancelMask = dispatch_source_get_mask(observation->source);
    dispatch_semaphore_signal(observation->canceled);
}

static int runEventCase(const char *path, unsigned iteration) {
    int gate[2];
    int error = makePipe(gate);
    if (error) return error;
    posix_spawn_file_actions_t actions;
    error = posix_spawn_file_actions_init(&actions);
    if (error) { close(gate[0]); close(gate[1]); return error; }
    error = posix_spawn_file_actions_adddup2(&actions, gate[0], ChildReportFD);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, gate[0]);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, gate[1]);
    char *arguments[] = {(char *)path, "--exit-child", NULL};
    pid_t child = 0;
    if (!error) error = posix_spawn(&child, path, &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(gate[0]);
    if (error) { close(gate[1]); return error; }

    dispatch_queue_t queue = dispatch_queue_create("com.kdt.LiveSimBooter.lifecycle.events", DISPATCH_QUEUE_SERIAL);
    EventObservation observation = {.registered = dispatch_semaphore_create(0), .event = dispatch_semaphore_create(0),
                                    .canceled = dispatch_semaphore_create(0)};
    observation.source = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)child, DISPATCH_PROC_EXIT, queue);
    if (!observation.source) {
        close(gate[1]); stopChild(child);
        dispatch_release(observation.registered); dispatch_release(observation.event); dispatch_release(observation.canceled);
        dispatch_release(queue);
        return errno ? errno : ENOMEM;
    }
    dispatch_set_context(observation.source, &observation);
    dispatch_source_set_registration_handler_f(observation.source, sourceRegistered);
    dispatch_source_set_event_handler_f(observation.source, sourceEvent);
    dispatch_source_set_cancel_handler_f(observation.source, sourceCanceled);
    dispatch_resume(observation.source);
    error = waitSemaphore(observation.registered);
    bool suspended = false, reaped = false;
    int status = 0;
    if (!error) {
        dispatch_suspend(queue);
        suspended = true;
        char release = 'x';
        error = transfer(gate[1], &release, sizeof(release), true, milliseconds() + TimeoutSeconds * 1000);
    }
    close(gate[1]);
    if (!error) {
        error = waitBounded(child, &status, milliseconds() + TimeoutSeconds * 1000);
        reaped = !error;
        if (!error && (!WIFEXITED(status) || WEXITSTATUS(status) != 23)) error = EPROTO;
    }
    // The event callback must run after waitpid has consumed the status. Native unit tests also cover reaping before the shim's poll.
    if (suspended) dispatch_resume(queue);
    if (!error) error = waitSemaphore(observation.event);
    if (!error && (!(observation.data & DISPATCH_PROC_EXIT) || observation.eventHandle != (uintptr_t)child ||
                   observation.eventMask != DISPATCH_PROC_EXIT)) {
        fprintf(stderr, "lifecycle-probe event metadata: pid=%ld handle=%lu mask=0x%lx data=0x%lx\n",
                (long)child, (unsigned long)observation.eventHandle, observation.eventMask, observation.data);
        error = EPROTO;
    }
    dispatch_source_cancel(observation.source);
    if (waitSemaphore(observation.canceled)) {
        fprintf(stderr, "lifecycle-probe: source cancellation timed out\n");
        stopChild(reaped ? 0 : child);
        _exit(126); // Do not leave asynchronous callbacks pointing at a returned stack frame.
    }
    // Native libdispatch clears a PROC source's mask before its cancellation callback; the process handle must remain valid.
    if (!error && observation.cancelHandle != (uintptr_t)child) {
        fprintf(stderr, "lifecycle-probe cancel metadata: pid=%ld handle=%lu mask=0x%lx\n",
                (long)child, (unsigned long)observation.cancelHandle, observation.cancelMask);
        error = EPROTO;
    }
    dispatch_release(observation.source);
    dispatch_release(observation.registered); dispatch_release(observation.event); dispatch_release(observation.canceled);
    dispatch_release(queue);
    if (!reaped) stopChild(child);
    if (!error) printf("lifecycle-probe events %u passed: pid=%ld reaped status=0x%x before EXIT callback, metadata preserved\n",
                       iteration, (long)child, status);
    return error;
}

typedef struct { int32_t child, before, after; } AdoptionReport;

static int orphanChild(pid_t adopter, pid_t oldParent) {
    pid_t before = getppid();
    int64_t deadline = milliseconds() + TimeoutSeconds * 1000;
    char ready = before == oldParent ? 'r' : 'e';
    int error = transfer(ChildReadyFD, &ready, sizeof(ready), true, deadline);
    close(ChildReadyFD);
    if (!error && before != oldParent) error = EPROTO;
    pid_t after = before;
    while (!error && after != adopter) {
        int remaining = remainingTime(deadline);
        if (remaining <= 0) { error = remaining ? errno : ETIMEDOUT; break; }
        (void)poll(NULL, 0, 10);
        after = getppid();
    }
    AdoptionReport report = {.child = getpid(), .before = before, .after = after};
    if (!error) error = transfer(ChildReportFD, &report, sizeof(report), true, deadline);
    close(ChildReportFD);
    if (error) { fprintf(stderr, "lifecycle-probe orphan child: %s (before=%ld after=%ld expected=%ld)\n",
                         strerror(error), (long)before, (long)after, (long)adopter); return 126; }
    printf("lifecycle-probe orphan child pid=%ld adopted by %ld; exiting 37\n", (long)getpid(), (long)after);
    return 37;
}

static int orphanParent(const char *path, pid_t adopter) {
    int report = fcntl(ChildReportFD, F_DUPFD_CLOEXEC, 10);
    close(ChildReportFD);
    if (report < 0) return errno;
    int ready[2];
    int error = makePipe(ready);
    if (error) { close(report); return error; }
    posix_spawn_file_actions_t actions;
    error = posix_spawn_file_actions_init(&actions);
    if (error) { close(report); close(ready[0]); close(ready[1]); return error; }
    error = posix_spawn_file_actions_adddup2(&actions, report, ChildReportFD);
    if (!error) error = posix_spawn_file_actions_adddup2(&actions, ready[1], ChildReadyFD);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, report);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, ready[0]);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, ready[1]);
    char adopterText[16], parentText[16];
    snprintf(adopterText, sizeof(adopterText), "%ld", (long)adopter);
    snprintf(parentText, sizeof(parentText), "%ld", (long)getpid());
    char *arguments[] = {(char *)path, "--orphan-child", adopterText, parentText, NULL};
    pid_t child = 0;
    if (!error) error = posix_spawn(&child, path, &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(report); close(ready[1]);
    char byte = 0;
    if (!error) error = transfer(ready[0], &byte, sizeof(byte), false, milliseconds() + TimeoutSeconds * 1000);
    close(ready[0]);
    if (!error && byte != 'r') error = EPROTO;
    if (error) stopChild(child);
    else printf("lifecycle-probe intermediate parent pid=%ld exiting; child=%ld confirmed original PPID\n", (long)getpid(), (long)child);
    return error;
}

static int runAdoptionCase(const char *path, pid_t adopter) {
    int report[2];
    int error = makePipe(report);
    if (error) return error;
    posix_spawn_file_actions_t actions;
    error = posix_spawn_file_actions_init(&actions);
    if (error) { close(report[0]); close(report[1]); return error; }
    error = posix_spawn_file_actions_adddup2(&actions, report[1], ChildReportFD);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, report[0]);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, report[1]);
    char adopterText[16];
    snprintf(adopterText, sizeof(adopterText), "%ld", (long)adopter);
    char *arguments[] = {(char *)path, "--orphan-parent", adopterText, NULL};
    pid_t child = 0;
    if (!error) error = posix_spawn(&child, path, &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(report[1]);
    int status = 0;
    bool reaped = false;
    int64_t deadline = milliseconds() + TimeoutSeconds * 1000;
    if (!error) { error = waitBounded(child, &status, deadline); reaped = !error; }
    if (!error && (!WIFEXITED(status) || WEXITSTATUS(status))) error = EPROTO;
    AdoptionReport observation = {0};
    if (!error) error = transfer(report[0], &observation, sizeof(observation), false, deadline);
    close(report[0]);
    if (!error && (observation.child <= 0 || observation.before != child || observation.after != adopter || adopter == child)) error = EPROTO;
    if (!reaped) stopChild(child);
    if (!error) printf("lifecycle-probe adoption passed: child=%d PPID %d -> %d; simulator launchd will reap exit 37\n",
                       observation.child, observation.before, observation.after);
    return error;
}

static pid_t parsePID(const char *text) {
    if (!text[0] || strspn(text, "0123456789") != strlen(text)) return 0;
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    return errno || *end || value <= 0 || value > INT_MAX ? 0 : (pid_t)value;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction ignored = {.sa_handler = SIG_IGN}, timeout = {.sa_handler = SIG_DFL};
    sigemptyset(&ignored.sa_mask); sigemptyset(&timeout.sa_mask);
    sigset_t unblocked;
    sigemptyset(&unblocked); sigaddset(&unblocked, SIGALRM);
    if (sigaction(SIGPIPE, &ignored, NULL) || sigaction(SIGALRM, &timeout, NULL) || sigprocmask(SIG_UNBLOCK, &unblocked, NULL)) return 126;
    alarm(TimeoutSeconds + 15);
    if (argc == 2 && !strcmp(argv[1], "--exit-child")) {
        char byte = 0;
        int error = transfer(ChildReportFD, &byte, sizeof(byte), false, milliseconds() + TimeoutSeconds * 1000);
        close(ChildReportFD);
        return error || byte != 'x' ? 126 : 23;
    }
    if (argc == 3 && !strcmp(argv[1], "--orphan-parent")) {
        pid_t adopter = parsePID(argv[2]);
        int error = adopter ? orphanParent(argv[0], adopter) : EINVAL;
        if (error) fprintf(stderr, "lifecycle-probe orphan parent: %s\n", strerror(error));
        return error ? 126 : 0;
    }
    if (argc == 4 && !strcmp(argv[1], "--orphan-child")) {
        pid_t adopter = parsePID(argv[2]), parent = parsePID(argv[3]);
        return adopter && parent ? orphanChild(adopter, parent) : 126;
    }
    bool eventsOnly = argc == 2 && !strcmp(argv[1], "--events-only");
    if (argc != 1 && !eventsOnly) { fprintf(stderr, "usage: %s [--events-only]\n", argv[0]); return 2; }
    alarm((EventIterations + 2) * (TimeoutSeconds + 15));
    pid_t adopter = getppid();
    if (!eventsOnly && adopter <= 0) return 126;
    int error = 0;
    for (unsigned i = 1; i <= EventIterations && !error; ++i) error = runEventCase(argv[0], i);
    if (!error && !eventsOnly) error = runAdoptionCase(argv[0], adopter);
    if (error) fprintf(stderr, "lifecycle-probe failed: %s\n", strerror(error));
    else printf("lifecycle-probe passed%s\n", eventsOnly ? " (events only)" : "");
    return error ? 1 : 0;
}
