#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../ProcessSupport/RuntimeLayout.h"

extern char **environ;
extern unsigned dyld_get_active_platform(void);

static const struct timespec pollDelay = { .tv_nsec = 250000000 };

static int parseRequest(char *bytes, size_t count, char **identifier, char *arguments[65]) {
    if (count < 35 || bytes[count - 1]) return EINVAL;
    size_t idLength = strnlen(bytes, count);
    if (idLength != 32) return EINVAL;
    for (size_t i = 0; i < idLength; ++i)
        if (!((bytes[i] >= '0' && bytes[i] <= '9') || (bytes[i] >= 'a' && bytes[i] <= 'f'))) return EINVAL;
    *identifier = bytes;
    arguments[0] = "/bin/launchctl";
    size_t offset = idLength + 1, argumentCount = 1;
    while (offset < count) {
        if (argumentCount >= 64 || !bytes[offset]) return EINVAL;
        size_t length = strnlen(bytes + offset, count - offset);
        if (length == count - offset) return EINVAL;
        arguments[argumentCount++] = bytes + offset;
        offset += length + 1;
    }
    arguments[argumentCount] = NULL;
    return argumentCount > 1 ? 0 : EINVAL;
}

static bool elapsedAtLeast(const struct timespec *start, const struct timespec *now, time_t seconds) {
    time_t elapsed = now->tv_sec - start->tv_sec;
    return elapsed > seconds || (elapsed == seconds && now->tv_nsec >= start->tv_nsec);
}

static int waitForCommand(pid_t child, int *status, bool *timedOut) {
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return errno;
    for (;;) {
        pid_t result = waitpid(child, status, WNOHANG);
        if (result == child) return 0;
        if (result < 0 && errno != EINTR) return errno;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return errno;
        if (elapsedAtLeast(&start, &now, *timedOut ? 65 : 60)) {
            if (*timedOut) return ETIMEDOUT;
            *timedOut = true;
            if (kill(child, SIGKILL) && errno != ESRCH) return errno;
        }
        nanosleep(&pollDelay, NULL);
    }
}

static int absent(int directory, const char *name) {
    struct stat metadata;
    if (!fstatat(directory, name, &metadata, AT_SYMLINK_NOFOLLOW)) return EEXIST;
    return errno == ENOENT ? 0 : errno;
}

// An error leaves active intact: the caller must inspect it, never automatically replay a possibly completed operation.
static int runRequest(int directory) {
    char bytes[16384], *identifier = NULL, *arguments[65];
    int input = openat(directory, "active", O_RDONLY | O_CLOEXEC | O_NOFOLLOW), error = input < 0 ? errno : 0;
    struct stat metadata;
    size_t size = 0;
    if (!error && fstat(input, &metadata)) error = errno;
    if (!error && (!S_ISREG(metadata.st_mode) || metadata.st_size <= 0 || metadata.st_size > (off_t)sizeof(bytes)))
        error = EINVAL;
    if (!error) {
        size = (size_t)metadata.st_size;
        size_t readCount = 0;
        while (readCount < size) {
            ssize_t amount = read(input, bytes + readCount, size - readCount);
            if (amount < 0 && errno == EINTR) continue;
            if (amount <= 0) { error = amount < 0 ? errno : EIO; break; }
            readCount += (size_t)amount;
        }
    }
    if (input >= 0) close(input);
    if (!error) error = parseRequest(bytes, size, &identifier, arguments);
    if (error) {
        fprintf(stderr, "[launchctl-control] malformed request error=%d\n", error);
        return error;
    }
    char logName[80], statusName[80], temporaryName[80];
    snprintf(logName, sizeof(logName), "%s.log", identifier);
    snprintf(statusName, sizeof(statusName), "%s.json", identifier);
    snprintf(temporaryName, sizeof(temporaryName), "%s.json.new", identifier);
    error = absent(directory, logName);
    if (!error) error = absent(directory, statusName);
    if (!error) error = absent(directory, temporaryName);
    if (error) return error;
    int output = openat(directory, logName, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    error = output < 0 ? errno : 0;
    if (!error && output <= STDERR_FILENO) {
        int replacement = fcntl(output, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (replacement < 0) error = errno;
        close(output);
        output = replacement;
    }
    posix_spawn_file_actions_t actions;
    bool initialized = false;
    if (!error) { error = posix_spawn_file_actions_init(&actions); initialized = !error; }
    if (!error) error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!error) error = posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
    if (!error) error = posix_spawn_file_actions_adddup2(&actions, output, STDERR_FILENO);
    if (!error && output > STDERR_FILENO) error = posix_spawn_file_actions_addclose(&actions, output);
    pid_t child = 0;
    if (!error) error = posix_spawn(&child, arguments[0], &actions, NULL, arguments, environ);
    if (initialized) posix_spawn_file_actions_destroy(&actions);
    if (output >= 0) close(output);
    int status = 0, waitError = 0;
    bool timedOut = false;
    if (!error) waitError = waitForCommand(child, &status, &timedOut);
    int result = openat(directory, temporaryName, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    int publicationError = result < 0 ? errno : 0;
    if (result >= 0) {
        if (dprintf(result, "{\"id\":\"%s\",\"pid\":%d,\"spawn_error\":%d,\"wait_error\":%d,\"status\":%d,"
                "\"exit\":%d,\"signal\":%d,\"timed_out\":%s}\n", identifier, child, error, waitError, status,
                !error && !waitError && WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                !error && !waitError && WIFSIGNALED(status) ? WTERMSIG(status) : 0, timedOut ? "true" : "false") < 0) publicationError = errno;
        if (!publicationError && fsync(result)) publicationError = errno;
        if (close(result) && !publicationError) publicationError = errno;
        if (!publicationError && renameatx_np(directory, temporaryName, directory, statusName, RENAME_EXCL)) publicationError = errno;
    }
    fprintf(stderr, "[launchctl-control] id=%s child=%d spawn=%d wait=%d status=%#x timeout=%d\n",
            identifier, child, error, waitError, status, timedOut);
    fflush(stderr);
    if (waitError || publicationError) return waitError ?: publicationError;
    return unlinkat(directory, "active", 0) ? errno : 0;
}

static int openControlDirectory(char path[PATH_MAX]) {
    const char *data = getenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY"), *root = getenv("DYLD_ROOT_PATH");
    char canonical[PATH_MAX], canonicalRoot[PATH_MAX], expected[PATH_MAX];
    struct stat metadata;
    if (dyld_get_active_platform() != 7 || !root || root[0] != '/' || !data || data[0] != '/' ||
        !realpath(root, canonicalRoot) || stat(canonicalRoot, &metadata) || !S_ISDIR(metadata.st_mode) || !realpath(data, canonical)) return -1;
    char *component = strrchr(canonicalRoot, '/');
    if (!component || strcmp(component, "/" LSB_RUNTIME_DIRECTORY)) return -1;
    *component = '\0';
    int length = snprintf(expected, sizeof(expected), "%s/" LSB_DATA_DIRECTORY, canonicalRoot);
    if (length <= 0 || (size_t)length >= sizeof(expected) || strcmp(expected, canonical)) return -1;
    length = snprintf(path, PATH_MAX, "%s/var/run/launchctl-control", canonical);
    if (length <= 0 || length >= PATH_MAX) return -1;
    int directory = open(canonical, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) return -1;
    const char *components[] = { "var", "run", "launchctl-control" };
    for (unsigned i = 0; i < 3; ++i) {
        if (i == 2 && mkdirat(directory, components[i], 0700) && errno != EEXIST) { close(directory); return -1; }
        int next = openat(directory, components[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        close(directory);
        if (next < 0) return -1;
        directory = next;
    }
    return directory;
}

int main(void) {
    char path[PATH_MAX];
    int directory = openControlDirectory(path);
    if (directory < 0) {
        fprintf(stderr, "[launchctl-control] requires simulator platform and sibling " LSB_RUNTIME_DIRECTORY "/" LSB_DATA_DIRECTORY " directories\n");
        return 1;
    }
    int lock = openat(directory, "worker.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        fprintf(stderr, "[launchctl-control] worker lock failed error=%d\n", errno);
        return 1;
    }
    int error = absent(directory, "active");
    if (error) {
        fprintf(stderr, "[launchctl-control] active request requires operator inspection error=%d\n", error);
        return 1;
    }
    fprintf(stderr, "[launchctl-control] ready pid=%d uid=%d directory=%s\n", getpid(), getuid(), path);
    fflush(stderr);
    for (;;) {
        if (!renameatx_np(directory, "request", directory, "active", RENAME_EXCL)) error = runRequest(directory);
        else error = errno == ENOENT ? 0 : errno;
        if (error) {
            fprintf(stderr, "[launchctl-control] stopped with preserved request evidence error=%d\n", error);
            return 1;
        }
        nanosleep(&pollDelay, NULL);
    }
}
