#include "../Guest/libLiveSimSpawn/ChildKill.h"
#include "../Guest/libLiveSimSpawn/ChildWait.h"
#include "../Guest/libLiveSimSpawn/SpawnShim.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern int LSBSystem(const char *);
extern int LSBSystemNoCancel(const char *);

static bool runtimeShell, bootstrapShell;
static const char *expectedShell;
static int spawnError, waitError;
static sigset_t callerMask;

typedef struct {
    const char *command;
    int ready[2], release[2];
    pid_t child;
    bool gate, nonCancelable, canceledChildKilled;
} Worker;

static _Thread_local Worker *worker;

int LSBResolveExecutableForSpawn(const char *path, char executable[PATH_MAX]) {
    bool available;
    if (!strcmp(path, "/bin/sh")) available = runtimeShell;
    else {
        assert(!strcmp(path, "/var/jb/bin/sh"));
        available = bootstrapShell;
    }
    if (!available) return ENOENT;
    strlcpy(executable, !strcmp(path, "/bin/sh") ? "/fixture/runtime/bin/sh" : "/fixture/bootstrap/bin/sh", PATH_MAX);
    return 0;
}

static void originalHandler(int number) { (void)number; }

static void checkParentSignals(bool running) {
    struct sigaction action;
    assert(!sigaction(SIGINT, NULL, &action));
    assert(action.sa_handler == (running ? SIG_IGN : originalHandler));
    if (!running) {
        assert(action.sa_flags & SA_RESTART);
        assert(sigismember(&action.sa_mask, SIGUSR2));
    }
    assert(!sigaction(SIGQUIT, NULL, &action) && action.sa_handler == SIG_IGN);
}

int LSBPosixSpawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes,
                  char *const argv[], char *const envp[]) {
    assert(expectedShell && !strcmp(path, expectedShell));
    assert(!strcmp(argv[0], "sh") && !strcmp(argv[1], "-c") && !strcmp(argv[2], "--") && argv[3] && !argv[4]);
    checkParentSignals(true);
    sigset_t mask, defaults;
    short flags;
    assert(!pthread_sigmask(SIG_SETMASK, NULL, &mask) && sigismember(&mask, SIGCHLD));
    assert(!posix_spawnattr_getflags(attributes, &flags));
    assert((flags & (POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK)) == (POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK));
    assert(!posix_spawnattr_getsigmask(attributes, &mask) && mask == callerMask);
    assert(!posix_spawnattr_getsigdefault(attributes, &defaults) && sigismember(&defaults, SIGINT) && !sigismember(&defaults, SIGQUIT));
    if (spawnError) return spawnError;
    int error = posix_spawn(pid, "/bin/sh", actions, attributes, argv, envp);
    if (!error && worker) worker->child = *pid;
    return error;
}

pid_t LSBWaitPID(pid_t child, int *status, int options) {
    assert(child > 0 && !options);
    if (worker && worker->gate) {
        worker->gate = false;
        assert(write(worker->ready[1], "r", 1) == 1);
        char byte;
        assert(read(worker->release[0], &byte, 1) == 1);
    }
    if (waitError) {
        int error = waitError;
        waitError = 0;
        if (error != EINTR) assert(waitpid(child, NULL, 0) == child);
        errno = error;
        return -1;
    }
    return waitpid(child, status, options);
}

int LSBKill(pid_t child, int number) {
    assert(worker && child == worker->child && number == SIGKILL);
    worker->canceledChildKilled = true;
    return kill(child, number);
}

static void checkRestored(void) {
    sigset_t mask;
    assert(!pthread_sigmask(SIG_SETMASK, NULL, &mask) && mask == callerMask);
    checkParentSignals(false);
}

static void *runWorker(void *context) {
    worker = context;
    int status = worker->nonCancelable ? LSBSystemNoCancel(worker->command) : LSBSystem(worker->command);
    assert(status == 0);
    sigset_t mask;
    assert(!pthread_sigmask(SIG_SETMASK, NULL, &mask) && mask == callerMask);
    pthread_testcancel();
    return NULL;
}

static pthread_t startWorker(Worker *entry) {
    pthread_t thread;
    entry->gate = true;
    assert(!pipe(entry->ready) && !pipe(entry->release));
    assert(!pthread_create(&thread, NULL, runWorker, entry));
    char byte;
    assert(read(entry->ready[0], &byte, 1) == 1);
    return thread;
}

static void closeWorker(Worker *entry) {
    close(entry->ready[0]);
    close(entry->ready[1]);
    close(entry->release[0]);
    close(entry->release[1]);
}

static void testConcurrentCancellation(void) {
    Worker canceled = {.command = "exec /bin/sleep 8"}, other = {.command = "exit 0", .nonCancelable = true};
    pthread_t first = startWorker(&canceled), second = startWorker(&other);
    void *result;
    assert(!pthread_cancel(first) && !pthread_join(first, &result) && result == PTHREAD_CANCELED);
    assert(canceled.canceledChildKilled && waitpid(canceled.child, NULL, WNOHANG) == -1 && errno == ECHILD);
    checkParentSignals(true);
    assert(!pthread_cancel(second));
    assert(write(other.release[1], "r", 1) == 1 && !pthread_join(second, &result) && result == PTHREAD_CANCELED);
    assert(!other.canceledChildKilled && waitpid(other.child, NULL, WNOHANG) == -1 && errno == ECHILD);
    checkRestored();
    closeWorker(&canceled);
    closeWorker(&other);
}

int main(void) {
    alarm(15);
    struct sigaction action = {.sa_handler = originalHandler, .sa_flags = SA_RESTART};
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGUSR2);
    assert(!sigaction(SIGINT, &action, NULL));
    action.sa_handler = SIG_IGN;
    assert(!sigaction(SIGQUIT, &action, NULL));
    sigemptyset(&callerMask);
    sigaddset(&callerMask, SIGUSR1);
    assert(!pthread_sigmask(SIG_SETMASK, &callerMask, NULL));

    assert(LSBSystem(NULL) == 0 && LSBSystem("exit 0") == (127 << 8));
    bootstrapShell = true;
    expectedShell = "/fixture/bootstrap/bin/sh";
    assert(LSBSystem(NULL) != 0);
    assert(!setenv("LSB_SYSTEM_TEST_EXIT", "37", 1));
    waitError = EINTR;
    assert(LSBSystem("exit \"$LSB_SYSTEM_TEST_EXIT\"") == (37 << 8) && !waitError);
    checkRestored();

    runtimeShell = true;
    expectedShell = "/fixture/runtime/bin/sh";
    sigaddset(&callerMask, SIGCHLD);
    assert(!pthread_sigmask(SIG_SETMASK, &callerMask, NULL));
    int status = LSBSystemNoCancel("kill -TERM $$");
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    checkRestored();
    sigdelset(&callerMask, SIGCHLD);
    assert(!pthread_sigmask(SIG_SETMASK, &callerMask, NULL));

    int shellErrors[] = {ENOENT, ENOEXEC};
    for (size_t i = 0; i < sizeof(shellErrors) / sizeof(*shellErrors); ++i) {
        spawnError = shellErrors[i];
        assert(LSBSystem("exit 0") == (127 << 8));
    }
    int childCreationErrors[] = {EAGAIN, ENOMEM, EPIPE, EMFILE};
    for (size_t i = 0; i < sizeof(childCreationErrors) / sizeof(*childCreationErrors); ++i) {
        spawnError = childCreationErrors[i];
        assert(LSBSystem("exit 0") == -1 && errno == spawnError);
    }
    spawnError = 0;
    waitError = ECHILD;
    assert(LSBSystem("exit 0") == -1 && errno == ECHILD);
    checkRestored();
    testConcurrentCancellation();
    return 0;
}
