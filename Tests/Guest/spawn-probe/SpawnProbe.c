#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static volatile sig_atomic_t childSignals;

static void childSignal(int number) {
    (void)number;
    if (childSignals < INT_MAX) ++childSignals;
}

static int64_t milliseconds(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) return -1;
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static int writeOutput(const char *bytes, size_t size) {
    while (size) {
        ssize_t written = write(STDOUT_FILENO, bytes, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        bytes += written;
        size -= (size_t)written;
    }
    return 0;
}

static int expectWait(pid_t child, pid_t selector, int expectedCode, int expectedSignal, sig_atomic_t beforeSignals) {
    int status = -1;
    pid_t result;
    do { result = waitpid(selector, &status, 0); } while (result < 0 && errno == EINTR);
    if (result != child || (expectedSignal ? !WIFSIGNALED(status) || WTERMSIG(status) != expectedSignal :
                                           !WIFEXITED(status) || WEXITSTATUS(status) != expectedCode)) {
        fprintf(stderr, "[spawn-probe] waitpid(%d)=%d status=0x%x errno=%d; unexpected child result\n", selector, result, status, errno);
        return 6;
    }
    if (waitpid(child, NULL, WNOHANG) != -1 || errno != ECHILD) {
        fprintf(stderr, "[spawn-probe] pid=%d did not become ECHILD after reaping\n", child);
        return 6;
    }
    for (unsigned i = 0; childSignals == beforeSignals && i < 100; ++i) usleep(10000);
    if (childSignals == beforeSignals) { fprintf(stderr, "[spawn-probe] missing SIGCHLD for pid=%d\n", child); return 6; }
    fprintf(stderr, "[spawn-probe] reaped pid=%d status=0x%x; SIGCHLD observed and second wait returned ECHILD\n", child, status);
    return 0;
}

static int spawnAndRead(void) {
    sig_atomic_t beforeSignals = childSignals;
    int endpoints[2];
    if (pipe(endpoints)) { perror("[spawn-probe] pipe"); return 1; }
    // Keep source descriptors above stdio even if the caller starts with a closed standard descriptor.
    for (unsigned i = 0; i < 2; ++i) {
        int staged = fcntl(endpoints[i], F_DUPFD_CLOEXEC, 3);
        if (staged < 0) { perror("[spawn-probe] duplicate pipe"); close(endpoints[0]); close(endpoints[1]); return 1; }
        close(endpoints[i]);
        endpoints[i] = staged;
    }
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) {
        fprintf(stderr, "[spawn-probe] file actions: %s\n", strerror(error));
        close(endpoints[0]); close(endpoints[1]);
        return 1;
    }
    error = posix_spawn_file_actions_adddup2(&actions, endpoints[1], STDOUT_FILENO);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, endpoints[0]);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, endpoints[1]);
    char *arguments[] = {"/bin/df", "-h", NULL};
    pid_t child = -1;
    if (!error) error = posix_spawn(&child, "/bin/df", &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(endpoints[1]);
    if (error) {
        fprintf(stderr, "[spawn-probe] posix_spawn: %s (%d)\n", strerror(error), error);
        close(endpoints[0]);
        return 2;
    }
    fprintf(stderr, "[spawn-probe] parent=%d spawned pid=%d; reading redirected stdout\n", getpid(), child);
    int64_t start = milliseconds();
    if (start < 0) { perror("[spawn-probe] clock"); close(endpoints[0]); return 3; }
    int64_t deadline = start + 15000;
    size_t total = 0;
    for (;;) {
        int64_t now = milliseconds();
        if (now < 0 || now >= deadline) {
            fprintf(stderr, "[spawn-probe] timed out before EOF; received %zu bytes\n", total);
            close(endpoints[0]);
            return 3;
        }
        struct pollfd descriptor = {.fd = endpoints[0], .events = POLLIN};
        int ready = poll(&descriptor, 1, (int)(deadline - now));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { perror("[spawn-probe] poll"); close(endpoints[0]); return 4; }
        if (!ready) continue;
        char output[4096];
        ssize_t count = read(endpoints[0], output, sizeof(output));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { perror("[spawn-probe] read"); close(endpoints[0]); return 4; }
        if (!count) break;
        if (writeOutput(output, (size_t)count)) { perror("[spawn-probe] stdout"); close(endpoints[0]); return 4; }
        total += (size_t)count;
    }
    close(endpoints[0]);
    fprintf(stderr, "[spawn-probe] pid=%d stdout reached EOF after %zu bytes\n", child, total);
    int waited = expectWait(child, child, 0, 0, beforeSignals);
    return waited ? waited : total ? 0 : 5;
}

static int waitForChildReady(int fd) {
    int64_t start = milliseconds();
    if (start < 0) return -1;
    int64_t deadline = start + 15000;
    for (;;) {
        int64_t now = milliseconds();
        if (now < 0) return -1;
        if (now >= deadline) { errno = ETIMEDOUT; return -1; }
        struct pollfd descriptor = {.fd = fd, .events = POLLIN};
        int result = poll(&descriptor, 1, (int)(deadline - now));
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return -1;
        if (!result) continue;
        char byte;
        ssize_t count = read(fd, &byte, 1);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return -1;
        if (count != 1 || byte != 'r') { errno = EPIPE; return -1; }
        return 0;
    }
}

static int verifyStatusCase(const char *program, unsigned value, int bySignal, int anyChild) {
    int gate[2], ready[2] = {-1, -1};
    if (pipe(gate)) { perror("[spawn-probe] status gate"); return 1; }
    if (bySignal && pipe(ready)) { perror("[spawn-probe] readiness pipe"); close(gate[0]); close(gate[1]); return 1; }
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) { close(gate[0]); close(gate[1]); if (bySignal) { close(ready[0]); close(ready[1]); } return 1; }
    error = posix_spawn_file_actions_addclose(&actions, gate[1]);
    if (!error && bySignal) error = posix_spawn_file_actions_addclose(&actions, ready[0]);
    char valueString[16], gateString[16], readyString[16];
    snprintf(valueString, sizeof(valueString), "%u", value);
    snprintf(gateString, sizeof(gateString), "%d", gate[0]);
    snprintf(readyString, sizeof(readyString), "%d", ready[1]);
    char *arguments[] = {(char *)program, bySignal ? "--wait-signal" : "--exit-code", valueString, "--child-gate-fd", gateString,
                        bySignal ? "--child-ready-fd" : NULL, readyString, NULL};
    pid_t child = -1;
    sig_atomic_t beforeSignals = childSignals;
    if (!error) error = posix_spawn(&child, program, &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(gate[0]);
    if (bySignal) close(ready[1]);
    if (error) {
        fprintf(stderr, "[spawn-probe] status child: %s\n", strerror(error));
        close(gate[1]); if (bySignal) close(ready[0]);
        return 2;
    }
    if (bySignal) {
        int result = waitForChildReady(ready[0]);
        close(ready[0]);
        if (result) { perror("[spawn-probe] wait for child readiness"); close(gate[1]); return 6; }
    }
    int status = 0x1234;
    errno = EDOM;
    if (waitpid(child, &status, WNOHANG) != 0 || status != 0x1234 || errno != EDOM) {
        fprintf(stderr, "[spawn-probe] WNOHANG failed while pid=%d was blocked on its gate\n", child);
        close(gate[1]); return 6;
    }
    fprintf(stderr, "[spawn-probe] pid=%d WNOHANG=0 before releasing child\n", child);
    if (bySignal) {
        errno = EDOM;
        if (kill(child, 0) || errno != EDOM) { perror("[spawn-probe] kill existence check"); close(gate[1]); return 6; }
        if (kill(child, (int)value) || errno != EDOM) { perror("[spawn-probe] kill child"); close(gate[1]); return 6; }
        fprintf(stderr, "[spawn-probe] parent=%d sent signal=%u to ready pid=%d with kill\n", getpid(), value, child);
        // Keep the gate open until the exit is observed: launchd may deliver the signal after kill returns.
        int result = expectWait(child, anyChild ? -1 : child, 0, (int)value, beforeSignals);
        close(gate[1]);
        if (!result && (kill(child, 0) != -1 || errno != ESRCH)) {
            fprintf(stderr, "[spawn-probe] reaped pid=%d did not become ESRCH for kill\n", child);
            return 6;
        }
        return result;
    }
    ssize_t written;
    do { written = write(gate[1], "x", 1); } while (written < 0 && errno == EINTR);
    close(gate[1]);
    if (written != 1) { perror("[spawn-probe] release status child"); return 6; }
    return expectWait(child, anyChild ? -1 : child, (int)value, 0, beforeSignals);
}

static int usage(const char *program) {
    fprintf(stderr, "usage: %s [--stop-before-spawn] [--debug-delay N] [--repeat N] [--verify-statuses]\n"
                    "       %s --exit-code N | --self-signal N | --wait-signal N --child-gate-fd FD --child-ready-fd FD\n"
                    "       debug delay: 1 to 60 seconds; repeat: 1 to 16; exit code: 0 to 255\n", program, program);
    return 64;
}

static int parseCount(const char *number, unsigned minimum, unsigned maximum, unsigned *result) {
    if (!number[0] || strspn(number, "0123456789") != strlen(number)) return -1;
    char *end;
    errno = 0;
    unsigned long value = strtoul(number, &end, 10);
    if (errno || *end || value < minimum || value > maximum) return -1;
    *result = (unsigned)value;
    return 0;
}

int main(int argc, char *argv[]) {
    unsigned repeat = 1, debugDelay = 0;
    unsigned exitCode = 0, selfSignal = 0, waitSignal = 0, gateFD = 0, readyFD = 0;
    int stopBeforeSpawn = 0, hasRepeat = 0, verifyStatuses = 0, exitChild = 0, hasGate = 0, hasReady = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--stop-before-spawn") && !stopBeforeSpawn) stopBeforeSpawn = 1;
        else if (!strcmp(argv[i], "--repeat") && !hasRepeat && i + 1 < argc) {
            if (parseCount(argv[++i], 1, 16, &repeat)) return usage(argv[0]);
            hasRepeat = 1;
        } else if (!strcmp(argv[i], "--debug-delay") && !debugDelay && i + 1 < argc) {
            if (parseCount(argv[++i], 1, 60, &debugDelay)) return usage(argv[0]);
        } else if (!strcmp(argv[i], "--verify-statuses") && !verifyStatuses) verifyStatuses = 1;
        else if (!strcmp(argv[i], "--exit-code") && !exitChild && !selfSignal && !waitSignal && i + 1 < argc) {
            if (parseCount(argv[++i], 0, 255, &exitCode)) return usage(argv[0]);
            exitChild = 1;
        } else if (!strcmp(argv[i], "--self-signal") && !selfSignal && !waitSignal && !exitChild && i + 1 < argc) {
            if (parseCount(argv[++i], 1, NSIG - 1, &selfSignal)) return usage(argv[0]);
        } else if (!strcmp(argv[i], "--wait-signal") && !selfSignal && !waitSignal && !exitChild && i + 1 < argc) {
            if (parseCount(argv[++i], 1, NSIG - 1, &waitSignal)) return usage(argv[0]);
        } else if (!strcmp(argv[i], "--child-gate-fd") && !hasGate && i + 1 < argc) {
            if (parseCount(argv[++i], 0, INT_MAX, &gateFD)) return usage(argv[0]);
            hasGate = 1;
        } else if (!strcmp(argv[i], "--child-ready-fd") && !hasReady && i + 1 < argc) {
            if (parseCount(argv[++i], 0, INT_MAX, &readyFD)) return usage(argv[0]);
            hasReady = 1;
        } else return usage(argv[0]);
    }
    if (exitChild || selfSignal || waitSignal) {
        if (verifyStatuses || hasRepeat || debugDelay || stopBeforeSpawn) return usage(argv[0]);
        if (waitSignal ? !hasGate || !hasReady || gateFD == readyFD : hasReady) return usage(argv[0]);
        unsigned signalNumber = selfSignal ? selfSignal : waitSignal;
        if (signalNumber) {
            struct sigaction action = {.sa_handler = SIG_DFL};
            sigemptyset(&action.sa_mask);
            sigset_t unblocked;
            sigemptyset(&unblocked); sigaddset(&unblocked, (int)signalNumber);
            if (signalNumber != SIGKILL && signalNumber != SIGSTOP && sigaction((int)signalNumber, &action, NULL)) return 125;
            if (sigprocmask(SIG_UNBLOCK, &unblocked, NULL)) return 125;
        }
        if (waitSignal) {
            ssize_t count;
            do { count = write((int)readyFD, "r", 1); } while (count < 0 && errno == EINTR);
            close((int)readyFD);
            if (count != 1) return 125;
        }
        if (hasGate) {
            char byte;
            ssize_t count;
            do { count = read((int)gateFD, &byte, 1); } while (count < 0 && errno == EINTR);
            close((int)gateFD);
            if (count != 1) return 125;
        }
        if (exitChild) return (int)exitCode;
        if (selfSignal) raise((int)selfSignal);
        return 125;
    }
    if (hasGate || hasReady) return usage(argv[0]);
    struct sigaction action = {.sa_handler = childSignal};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGCHLD, &action, NULL)) { perror("[spawn-probe] SIGCHLD handler"); return 1; }
    if (stopBeforeSpawn) {
        fprintf(stderr, "[spawn-probe] pid=%d stopping before first spawn; resume with SIGCONT or LLDB\n", getpid());
        fflush(stderr);
        if (raise(SIGSTOP)) { perror("[spawn-probe] SIGSTOP"); return 1; }
    }
    if (debugDelay) {
        // Device debugserver may require a running target to attach; use this instead of SIGSTOP in that case.
        fprintf(stderr, "[spawn-probe] pid=%d delaying first spawn for %u seconds; attach LLDB while running\n", getpid(), debugDelay);
        fflush(stderr);
        for (unsigned remaining = debugDelay; remaining; ) remaining = sleep(remaining);
    }
    for (unsigned i = 0; i < repeat; ++i) {
        if (repeat > 1) fprintf(stderr, "[spawn-probe] iteration %u/%u\n", i + 1, repeat);
        int result = spawnAndRead();
        if (result) return result;
    }
    if (verifyStatuses) {
        int result = verifyStatusCase(argv[0], 0, 0, 0);
        if (!result) result = verifyStatusCase(argv[0], 37, 0, 1);
        if (!result) result = verifyStatusCase(argv[0], SIGTERM, 1, 0);
        if (result) return result;
    }
    return 0;
}
