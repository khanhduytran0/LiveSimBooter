// Run /usr/local/bin/exec-probe (both cases), --exit-only, or --signal-only. Internal modes re-enter this same signed executable via SETEXEC.
// Uses ordinary libc APIs: no fork and no assumptions about native PIDs. The broker's guest PID must survive replacement.
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
enum { ProbeTimeoutSeconds = 45 };

static int64_t milliseconds(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) return -1;
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static int remainingTime(int64_t deadline) {
    int64_t now = milliseconds();
    if (now < 0) return -1;
    if (now >= deadline) return 0;
    return deadline - now > INT_MAX ? INT_MAX : (int)(deadline - now);
}

static int writeIdentity(const char *stage) {
    char line[64];
    int length = snprintf(line, sizeof(line), "%s %ld\n", stage, (long)getpid());
    if (length < 0 || (size_t)length >= sizeof(line)) return EOVERFLOW;
    size_t offset = 0;
    while (offset < (size_t)length) {
        ssize_t count = write(STDOUT_FILENO, line + offset, (size_t)length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count < 0 ? errno : EIO;
        offset += (size_t)count;
    }
    return 0;
}

static int finalMode(bool terminate) {
    if (terminate) {
        struct sigaction action = {.sa_handler = SIG_DFL};
        sigemptyset(&action.sa_mask);
        sigset_t unblocked;
        sigemptyset(&unblocked);
        sigaddset(&unblocked, SIGTERM);
        if (sigaction(SIGTERM, &action, NULL) || sigprocmask(SIG_UNBLOCK, &unblocked, NULL)) {
            perror("exec-probe final signal setup");
            return 126;
        }
        // Bound an orphaned internal mode too; the parent normally sends SIGTERM immediately after reading its identity.
        alarm(ProbeTimeoutSeconds + 10);
    }
    int error = writeIdentity("after");
    if (error) { fprintf(stderr, "exec-probe after: %s\n", strerror(error)); return 126; }
    if (!terminate) return 23;
    for (;;) pause();
}

static int replacementMode(const char *path, bool terminate) {
    alarm(ProbeTimeoutSeconds + 10);
    int error = writeIdentity("before");
    if (error) { fprintf(stderr, "exec-probe before: %s\n", strerror(error)); return 126; }
    posix_spawnattr_t attributes;
    error = posix_spawnattr_init(&attributes);
    if (error) { fprintf(stderr, "exec-probe attributes: %s\n", strerror(error)); return 126; }
    error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETEXEC);
    char *arguments[] = {(char *)path, "--final", terminate ? "term" : "exit", NULL};
    pid_t ignored = 0;
    if (!error) error = posix_spawn(&ignored, path, NULL, &attributes, arguments, environ);
    posix_spawnattr_destroy(&attributes);
    fprintf(stderr, "exec-probe SETEXEC returned: %s\n", error ? strerror(error) : "unexpected success return");
    return 126;
}

static int makeOutputPipe(int descriptors[2]) {
    int original[2];
    if (pipe(original)) return errno;
    descriptors[0] = fcntl(original[0], F_DUPFD_CLOEXEC, 3);
    int error = descriptors[0] < 0 ? errno : 0;
    descriptors[1] = error ? -1 : fcntl(original[1], F_DUPFD_CLOEXEC, 3);
    if (descriptors[1] < 0 && !error) error = errno;
    close(original[0]);
    close(original[1]);
    if (!error) {
        int flags = fcntl(descriptors[0], F_GETFL);
        if (flags < 0 || fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK)) error = errno;
    }
    if (error) {
        if (descriptors[0] >= 0) close(descriptors[0]);
        if (descriptors[1] >= 0) close(descriptors[1]);
        descriptors[0] = descriptors[1] = -1;
    }
    return error;
}

static int readIdentity(int descriptor, const char *stage, pid_t expected, int64_t deadline) {
    char line[64];
    size_t length = 0;
    for (;;) {
        int remaining = remainingTime(deadline);
        if (remaining < 0) return errno;
        if (!remaining) return ETIMEDOUT;
        struct pollfd input = {.fd = descriptor, .events = POLLIN};
        int result = poll(&input, 1, remaining);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return errno;
        if (!result) return ETIMEDOUT;
        if (input.revents & (POLLNVAL | POLLERR)) return EIO;
        char byte;
        ssize_t count = read(descriptor, &byte, 1);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) return count < 0 ? errno : EPIPE;
        if (byte == '\n') break;
        if (!byte || length == sizeof(line) - 1) return EPROTO;
        line[length++] = byte;
    }
    line[length] = '\0';
    size_t stageLength = strlen(stage);
    if (length <= stageLength || strncmp(line, stage, stageLength) || line[stageLength] != ' ') return EPROTO;
    char *end = NULL;
    errno = 0;
    long value = strtol(line + stageLength + 1, &end, 10);
    if (errno || !end || *end || value <= 0 || value > INT_MAX) return EPROTO;
    if (value != expected) {
        fprintf(stderr, "exec-probe %s guest PID %ld differs from posix_spawn PID %ld\n", stage, value, (long)expected);
        return EPROTO;
    }
    return 0;
}

static int waitBounded(pid_t child, int *status, int64_t deadline, bool *reaped) {
    for (;;) {
        pid_t result = waitpid(child, status, WNOHANG);
        if (result == child) { *reaped = true; return 0; }
        if (result < 0 && errno != EINTR) { if (errno == ECHILD) *reaped = true; return errno; }
        int remaining = remainingTime(deadline);
        if (remaining < 0) return errno;
        if (!remaining) return ETIMEDOUT;
        (void)poll(NULL, 0, remaining > 10 ? 10 : remaining);
    }
}

static int runCase(const char *path, bool terminate) {
    const char *name = terminate ? "signal" : "exit";
    int output[2] = {-1, -1};
    int error = makeOutputPipe(output);
    if (error) { fprintf(stderr, "exec-probe pipe: %s\n", strerror(error)); return 1; }
    posix_spawn_file_actions_t actions;
    error = posix_spawn_file_actions_init(&actions);
    if (error) { close(output[0]); close(output[1]); fprintf(stderr, "exec-probe actions: %s\n", strerror(error)); return 1; }
    error = posix_spawn_file_actions_addclose(&actions, output[0]);
    if (!error) error = posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, output[1]);
    char *arguments[] = {(char *)path, "--replace", terminate ? "term" : "exit", NULL};
    pid_t child = 0;
    int64_t start = milliseconds();
    if (!error && start < 0) error = errno;
    if (!error) {
        error = posix_spawn(&child, path, &actions, NULL, arguments, environ);
        if (error) child = 0;
    }
    posix_spawn_file_actions_destroy(&actions);
    close(output[1]);
    bool reaped = false;
    int status = 0;
    int64_t deadline = start + ProbeTimeoutSeconds * 1000;
    if (!error) error = readIdentity(output[0], "before", child, deadline);
    if (!error) error = readIdentity(output[0], "after", child, deadline);
    close(output[0]);
    if (!error && terminate && kill(child, SIGTERM)) error = errno;
    if (!error) error = waitBounded(child, &status, deadline, &reaped);
    if (!error && (terminate ? !WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM : !WIFEXITED(status) || WEXITSTATUS(status) != 23)) {
        fprintf(stderr, "exec-probe %s: unexpected wait status 0x%x\n", name, status);
        error = EPROTO;
    }
    if (error) {
        fprintf(stderr, "exec-probe %s failed: %s\n", name, strerror(error));
        if (child > 0 && !reaped) {
            (void)kill(child, SIGKILL);
            int64_t cleanup = milliseconds();
            if (cleanup >= 0) {
                int cleanupError = waitBounded(child, &status, cleanup + 5000, &reaped);
                if (cleanupError) fprintf(stderr, "exec-probe cleanup: %s\n", strerror(cleanupError));
            }
        }
        return 1;
    }
    printf("exec-probe %s passed: guest PID %ld survived SETEXEC, wait status 0x%x\n", name, (long)child, status);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && (!strcmp(argv[1], "--replace") || !strcmp(argv[1], "--final"))) {
        if (strcmp(argv[2], "exit") && strcmp(argv[2], "term")) return 2;
        bool terminate = !strcmp(argv[2], "term");
        return !strcmp(argv[1], "--replace") ? replacementMode(argv[0], terminate) : finalMode(terminate);
    }
    bool exitOnly = argc == 2 && !strcmp(argv[1], "--exit-only"), signalOnly = argc == 2 && !strcmp(argv[1], "--signal-only");
    if (argc > 1 && !exitOnly && !signalOnly) {
        fprintf(stderr, "usage: %s [--exit-only | --signal-only]\n", argv[0]);
        return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 2;
    }
    if (!signalOnly && runCase(argv[0], false)) return 1;
    return exitOnly ? 0 : runCase(argv[0], true);
}
