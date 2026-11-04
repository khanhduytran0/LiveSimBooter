#include <assert.h>
#include <spawn.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>

static int testSpawn(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const[], char *const[]);
static int testClock(clockid_t, struct timespec *);
static pid_t testWait(pid_t, int *, int);
static int testKill(pid_t, int);
static int testSleep(const struct timespec *, struct timespec *);
#define posix_spawn testSpawn
#define clock_gettime testClock
#define waitpid testWait
#define kill testKill
#define nanosleep testSleep
#define main controlMain
#include "../Guest/launchctl-control/LaunchctlControl.c"
#undef main
#undef posix_spawn
#undef clock_gettime
#undef waitpid
#undef kill
#undef nanosleep

unsigned dyld_get_active_platform(void) { return 7; }
static int spawnError;
static bool installing, simulatedWait, simulatedReap, killed;
static time_t clockSeconds, killedAt;

static int testClock(clockid_t clock, struct timespec *value) {
    if (!simulatedWait) return clock_gettime(clock, value);
    assert(clock == CLOCK_MONOTONIC);
    *value = (struct timespec){ .tv_sec = clockSeconds++ };
    return 0;
}

static pid_t testWait(pid_t child, int *status, int options) {
    if (!simulatedWait) return waitpid(child, status, options);
    assert(child == 12345 && options == WNOHANG);
    if (!killed || !simulatedReap) return 0;
    *status = SIGKILL;
    return child;
}

static int testKill(pid_t child, int signalNumber) {
    assert(simulatedWait && child == 12345 && signalNumber == SIGKILL && !killed);
    killed = true;
    killedAt = clockSeconds - 1;
    return 0;
}

static int testSleep(const struct timespec *duration, struct timespec *remaining) {
    return simulatedWait ? 0 : nanosleep(duration, remaining);
}

static int testSpawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes,
                     char *const arguments[], char *const environment[]) {
    assert(!strcmp(path, installing ? "/usr/local/bin/lsb-app-install" : "/bin/launchctl"));
    assert(!strcmp(arguments[0], path) && !strcmp(arguments[1], installing ? "/data/payload.ipa" : "list") && !arguments[2]);
    if (spawnError) return spawnError;
    if (simulatedWait) { *pid = 12345; return 0; }
    char *childArguments[] = { "/usr/bin/false", NULL };
    return posix_spawn(pid, childArguments[0], actions, attributes, childArguments, environment);
}

static void parseTests(void) {
    char valid[] = "0123456789abcdef0123456789abcdef\0print\0user/501\0";
    char *identifier, *arguments[65];
    assert(!parseRequest(valid, sizeof(valid) - 1, &identifier, arguments));
    assert(identifier == valid && !strcmp(arguments[0], "/bin/launchctl") && !strcmp(arguments[1], "print"));
    assert(!strcmp(arguments[2], "user/501") && !arguments[3]);
    assert(parseRequest(valid, sizeof(valid) - 2, &identifier, arguments) == EINVAL);
    assert(parseRequest(valid, sizeof(valid), &identifier, arguments) == EINVAL);
    valid[0] = 'g';
    assert(parseRequest(valid, sizeof(valid) - 1, &identifier, arguments) == EINVAL);
    char many[33 + 2 * 64];
    memcpy(many, "0123456789abcdef0123456789abcdef", 33);
    for (unsigned i = 0; i < 64; ++i) { many[33 + 2 * i] = 'x'; many[34 + 2 * i] = 0; }
    assert(!parseRequest(many, sizeof(many) - 2, &identifier, arguments));
    assert(!arguments[64]);
    assert(parseRequest(many, sizeof(many), &identifier, arguments) == EINVAL);
    char install[] = "0123456789abcdef0123456789abcdef\0--lsb-install-ipa\0/data/payload.ipa\0";
    assert(!parseRequest(install, sizeof(install) - 1, &identifier, arguments));
    assert(!strcmp(arguments[0], "/usr/local/bin/lsb-app-install") && !strcmp(arguments[1], "/data/payload.ipa") && !arguments[2]);
    char relative[] = "0123456789abcdef0123456789abcdef\0--lsb-install-ipa\0payload.ipa\0";
    assert(parseRequest(relative, sizeof(relative) - 1, &identifier, arguments) == EINVAL);
    char extra[] = "0123456789abcdef0123456789abcdef\0--lsb-install-ipa\0/data/payload.ipa\0extra\0";
    assert(parseRequest(extra, sizeof(extra) - 1, &identifier, arguments) == EINVAL);
    char missing[] = "0123456789abcdef0123456789abcdef\0--lsb-install-ipa\0";
    assert(parseRequest(missing, sizeof(missing) - 1, &identifier, arguments) == EINVAL);
}

static void timeoutTests(void) {
    simulatedWait = true;
    for (unsigned install = 0; install < 2; ++install) {
        for (unsigned reaped = 0; reaped < 2; ++reaped) {
            clockSeconds = 100;
            killed = false;
            simulatedReap = reaped;
            int status = 0;
            bool timedOut = false;
            time_t deadline = install ? 180 : 60;
            assert(waitForCommand(12345, &status, &timedOut, deadline) == (reaped ? 0 : ETIMEDOUT));
            assert(timedOut && killed && killedAt == 100 + deadline);
            if (reaped) assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
            else assert(clockSeconds == 100 + deadline + 6);
        }
    }
    simulatedWait = false;
}

static void deadlineTests(void) {
    struct timespec start = { .tv_sec = 100, .tv_nsec = 900000000 };
    struct timespec now = { .tv_sec = 160, .tv_nsec = 899999999 };
    assert(!elapsedAtLeast(&start, &now, 60));
    now.tv_nsec = start.tv_nsec;
    assert(elapsedAtLeast(&start, &now, 60));
    now = (struct timespec){ .tv_sec = 161 };
    assert(elapsedAtLeast(&start, &now, 60));
    now = (struct timespec){ .tv_sec = 165, .tv_nsec = 899999999 };
    assert(!elapsedAtLeast(&start, &now, 65));
    now.tv_nsec = start.tv_nsec;
    assert(elapsedAtLeast(&start, &now, 65));
    start.tv_nsec = 0;
    now = (struct timespec){ .tv_sec = 160 };
    assert(elapsedAtLeast(&start, &now, 60));
}

static void requestTests(void) {
    const char *identifier = "0123456789abcdef0123456789abcdef";
    char directoryPath[] = "/tmp/LiveSimBooter-launchctl-tests.XXXXXX";
    assert(mkdtemp(directoryPath));
    int directory = open(directoryPath, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(directory >= 0);
    for (unsigned failure = 0; failure < 2; ++failure) {
        int request = openat(directory, "active", O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(request >= 0);
        assert(write(request, identifier, 33) == 33 && write(request, "list", 5) == 5 && !close(request));
        spawnError = failure ? EAGAIN : 0;
        assert(!runRequest(directory));
        assert(absent(directory, "active") == 0);
        char name[80], contents[1024];
        snprintf(name, sizeof(name), "%s.json", identifier);
        int result = openat(directory, name, O_RDONLY | O_NOFOLLOW);
        assert(result >= 0);
        ssize_t length = read(result, contents, sizeof(contents) - 1);
        assert(length > 0 && !close(result));
        contents[length] = 0;
        assert(strstr(contents, "\"wait_error\":0") && strstr(contents, "\"timed_out\":false"));
        assert(strstr(contents, failure ? "\"spawn_error\":35" : "\"spawn_error\":0"));
        assert(strstr(contents, failure ? "\"exit\":-1" : "\"exit\":1"));
        assert(!unlinkat(directory, name, 0));
        snprintf(name, sizeof(name), "%s.log", identifier);
        assert(!unlinkat(directory, name, 0));
    }
    // Even a reaped installer client may have left an accepted operation running in installd. Do not replay it.
    installing = true;
    simulatedWait = true;
    simulatedReap = true;
    spawnError = 0;
    killed = false;
    clockSeconds = 100;
    int installRequest = openat(directory, "active", O_WRONLY | O_CREAT | O_EXCL, 0600);
    const char installArguments[] = "--lsb-install-ipa\0/data/payload.ipa\0";
    assert(installRequest >= 0 && write(installRequest, identifier, 33) == 33);
    assert(write(installRequest, installArguments, sizeof(installArguments) - 1) == sizeof(installArguments) - 1 && !close(installRequest));
    assert(runRequest(directory) == ETIMEDOUT && absent(directory, "active") == EEXIST && killedAt == 280);
    assert(!unlinkat(directory, "active", 0));
    char resultName[80], resultBytes[1024];
    snprintf(resultName, sizeof(resultName), "%s.json", identifier);
    int timeoutResult = openat(directory, resultName, O_RDONLY);
    assert(timeoutResult >= 0);
    ssize_t resultLength = read(timeoutResult, resultBytes, sizeof(resultBytes) - 1);
    assert(resultLength > 0 && !close(timeoutResult));
    resultBytes[resultLength] = 0;
    assert(strstr(resultBytes, "\"timed_out\":true") && strstr(resultBytes, "\"signal\":9"));
    assert(!unlinkat(directory, resultName, 0));
    snprintf(resultName, sizeof(resultName), "%s.log", identifier);
    assert(!unlinkat(directory, resultName, 0));
    installing = simulatedWait = false;
    // Malformed requests must remain available for inspection and must not invoke launchctl.
    int request = openat(directory, "active", O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(request >= 0 && write(request, "invalid", 7) == 7 && !close(request));
    assert(runRequest(directory) == EINVAL && absent(directory, "active") == EEXIST);
    assert(!unlinkat(directory, "active", 0) && !close(directory) && !rmdir(directoryPath));
}

static void directoryTests(void) {
    char temporary[] = "/tmp/LiveSimBooter-launchctl-paths.XXXXXX", base[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, base));
    int parent = open(base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(parent >= 0 && !mkdirat(parent, "RuntimeRoot", 0700) && !mkdirat(parent, "data", 0700));
    assert(!mkdirat(parent, "data/var", 0700) && !mkdirat(parent, "data/var/run", 0700));
    assert(!mkdirat(parent, "SimulatorData", 0700) && !mkdirat(parent, "RuntimeRoot-other", 0700));
    char root[PATH_MAX], data[PATH_MAX], previous[PATH_MAX], wrongRoot[PATH_MAX], result[PATH_MAX], expected[PATH_MAX];
    assert(snprintf(root, sizeof(root), "%s/RuntimeRoot", base) < (int)sizeof(root));
    assert(snprintf(data, sizeof(data), "%s/data", base) < (int)sizeof(data));
    assert(snprintf(previous, sizeof(previous), "%s/SimulatorData", base) < (int)sizeof(previous));
    assert(snprintf(wrongRoot, sizeof(wrongRoot), "%s/RuntimeRoot-other", base) < (int)sizeof(wrongRoot));
    assert(snprintf(expected, sizeof(expected), "%s/data/var/run/launchctl-control", base) < (int)sizeof(expected));
    assert(!setenv("DYLD_ROOT_PATH", root, 1) && !setenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY", previous, 1));
    assert(openControlDirectory(result) == -1); // The previous directory spelling is not an implicit migration.
    assert(!setenv("DYLD_ROOT_PATH", wrongRoot, 1) && !setenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY", data, 1));
    assert(openControlDirectory(result) == -1);
    assert(!setenv("DYLD_ROOT_PATH", root, 1));
    int directory = openControlDirectory(result);
    assert(directory >= 0 && !strcmp(result, expected));
    struct stat opened, named;
    assert(!fstat(directory, &opened) && !stat(expected, &named));
    assert(opened.st_dev == named.st_dev && opened.st_ino == named.st_ino && !close(directory));
    assert(!unlinkat(parent, "data/var/run/launchctl-control", AT_REMOVEDIR));
    assert(!unlinkat(parent, "data/var/run", AT_REMOVEDIR) && !unlinkat(parent, "data/var", AT_REMOVEDIR));
    assert(!unlinkat(parent, "data", AT_REMOVEDIR) && !unlinkat(parent, "RuntimeRoot", AT_REMOVEDIR));
    assert(!unlinkat(parent, "SimulatorData", AT_REMOVEDIR) && !unlinkat(parent, "RuntimeRoot-other", AT_REMOVEDIR));
    assert(!close(parent) && !rmdir(base));
}

int main(void) {
    parseTests();
    deadlineTests();
    timeoutTests();
    requestTests();
    directoryTests();
    puts("LaunchctlControl tests passed");
    return 0;
}
