#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
extern pid_t darwinVfork(void) __asm("_vfork");
static volatile sig_atomic_t phase;
static volatile sig_atomic_t receivedSignal;
static volatile unsigned globals[8];

static void prepare(void) { phase = phase == 0 ? 1 : -1; }
static void parent(void) { phase = phase == 1 ? 2 : -1; }
static void child(void) { phase = phase == 1 ? 3 : -1; }
static void recordSignal(int number) { receivedSignal = number; }

int testForkInLibrary(const char *executable, int shared, int closeOnExec, bool useVfork) {
    pid_t original = getpid();
    unsigned index = (unsigned)original % 8;
    volatile unsigned *heap = calloc(8, sizeof(*heap));
    if (!heap) return 1;
    globals[index] = 17;
    heap[index] = 23;
    volatile unsigned local = 31;
    char sharedArg[16], closeArg[16];
    snprintf(sharedArg, sizeof(sharedArg), "%d", shared);
    snprintf(closeArg, sizeof(closeArg), "%d", closeOnExec);
    char *const args[] = {(char *)executable, "--after-exec", sharedArg, closeArg, NULL};
    char **environment = environ;
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    struct sigaction action = {.sa_handler = recordSignal};
    sigemptyset(&action.sa_mask);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) || sigaction(SIGUSR1, &action, NULL)) { perror("signal setup"); return 1; }
    int error = pthread_atfork(prepare, parent, child);
    if (error) { fprintf(stderr, "pthread_atfork errno=%d\n", error); return 1; }

    // Leave B for the new image. The vfork child checks the handler flag and immediately execs.
    char byte = 0;
    if (useVfork && (read(shared, &byte, 1) != 1 || byte != 'A')) return 1;
    pid_t pid = useVfork ? darwinVfork() : fork();
    if (pid < 0) { fprintf(stderr, "%s errno=%d parent_handler=%d\n", useVfork ? "vfork" : "fork", errno, (int)phase); return 1; }
    if (!pid && useVfork) {
        if (phase != 0) _exit(41);
        execve(executable, args, environment);
        _exit(45);
    }
    if (!pid) {
        if (phase != 3) _exit(41);
        if (getpid() == original || getppid() != original) _exit(42);
        if (globals[index] != 17 || heap[index] != 23 || local != 31) _exit(43);
        if (sigprocmask(0, NULL, &mask) || !sigismember(&mask, SIGUSR2) || raise(SIGUSR1) || receivedSignal != SIGUSR1) _exit(46);
        globals[index] = heap[index] = local = 0;
        if (fcntl(closeOnExec, F_GETFD) != FD_CLOEXEC || read(closeOnExec, &byte, 1) != 1 || byte != 'A') _exit(44);
        execve(executable, args, environment);
        _exit(45);
    }

    int status;
    pid_t reaped;
    do { reaped = waitpid(pid, &status, 0); } while (reaped < 0 && errno == EINTR);
    if (reaped != pid) { perror("waitpid"); return 1; }
    ssize_t count = read(shared, &byte, 1);
    printf("child=%d wait=%#x parent_handler=%d global=%u heap=%u stack=%u next_byte=%c\n",
           pid, status, (int)phase, globals[index], heap[index], local, count == 1 ? byte : '?');
    int passed = WIFEXITED(status) && WEXITSTATUS(status) == 37 && phase == (useVfork ? 0 : 2) && globals[index] == 17 && heap[index] == 23 &&
                 local == 31 && count == 1 && byte == 'C' && !receivedSignal;
    free((void *)heap);
    return passed ? 0 : 1;
}
