#include "../Guest/Shared/Fork/ForkSignalState.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static volatile sig_atomic_t observed;
static void gotSignal(int number, siginfo_t *info, void *context) {
    sigset_t current;
    if (number == SIGUSR1 && info && context && !sigprocmask(0, NULL, &current) && sigismember(&current, SIGUSR2)) ++observed;
}

static int cover(void *context, uint64_t address, uint64_t size) {
    return address == (uintptr_t)context && size == sizeof(uint32_t) ? 0 : EFAULT;
}

int main(void) {
    ForkSignalState original, saved, reset, verified;
    int wake[2];
    assert(!pipe(wake) && !fcntl(wake[0], F_SETFL, O_NONBLOCK));
    LSBChildStatus ledger;
    LSBChildStatusInitialize(&ledger);
    atomic_store(&ledger.terminalSessionAndGroup, ((uint64_t)(uint32_t)getpid() << 32) | (uint32_t)getpid());
    assert(!LSBChildWaitConfigure(&ledger, wake[0]));
    struct sigaction kernelAction;
    assert(!sigaction(SIGTSTP, NULL, &kernelAction) && kernelAction.sa_handler != SIG_DFL && kernelAction.sa_handler != SIG_IGN);
    assert(!forkReadSignalState(&original));
    assert(original.actions[SIGTSTP].handler == (uintptr_t)SIG_DFL);
    struct sigaction custom = {.sa_sigaction = gotSignal, .sa_flags = SA_SIGINFO | SA_RESTART};
    sigemptyset(&custom.sa_mask);
    sigaddset(&custom.sa_mask, SIGUSR2);
    assert(!sigaction(SIGUSR1, &custom, NULL));
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    assert(!sigprocmask(SIG_SETMASK, &blocked, NULL));
    assert(!forkReadSignalState(&saved));

    struct sigaction ignored = {.sa_handler = SIG_IGN};
    sigemptyset(&ignored.sa_mask);
    assert(!LSBSigaction(SIGTSTP, &ignored, NULL));
    assert(!sigaction(SIGUSR1, &ignored, NULL));
    sigemptyset(&blocked);
    assert(!sigprocmask(SIG_SETMASK, &blocked, NULL));
    assert(!forkReadSignalState(&reset));

    // Deliberately request an earlier valid change before a later invalid callback. The observer must preserve its original actions.
    ForkSignalState invalid = saved;
    invalid.actions[SIGHUP].handler = (uintptr_t)SIG_IGN;
    invalid.actions[SIGUSR2].handler = (uintptr_t)gotSignal + 1;
    assert(forkRestoreSignals(&invalid, (void *)gotSignal, cover) == ENOTSUP);
    assert(!forkReadSignalState(&verified) && !memcmp(&verified, &reset, sizeof(reset)));
    invalid = reset;
    invalid.actions[SIGKILL].handler = (uintptr_t)SIG_IGN;
    assert(forkRestoreSignals(&invalid, (void *)gotSignal, cover) == EPROTO);
    assert(forkRestoreSignals(&saved, NULL, cover) == ENOTSUP); // Valid code in an image outside the accepted snapshot.
    void *data = mmap(NULL, (size_t)getpagesize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(data != MAP_FAILED);
    invalid = reset;
    invalid.actions[SIGUSR1].handler = (uintptr_t)data;
    assert(forkRestoreSignals(&invalid, data, cover) == EACCES); // Authenticated address range, but not executable.
    assert(!munmap(data, (size_t)getpagesize()));

    assert(!forkRestoreSignals(&saved, (void *)gotSignal, cover));
    assert(!forkReadSignalState(&verified));
    assert(verified.actions[SIGTSTP].handler == (uintptr_t)SIG_DFL);
    struct sigaction restored;
    assert(!sigaction(SIGTSTP, NULL, &restored) && restored.sa_handler == kernelAction.sa_handler);
    assert(verified.actions[SIGUSR1].handler == saved.actions[SIGUSR1].handler &&
           verified.actions[SIGUSR1].flags == saved.actions[SIGUSR1].flags && sigismember(&verified.actions[SIGUSR1].mask, SIGUSR2));
    assert(sigismember(&verified.mask, SIGTERM) && !sigismember(&verified.mask, SIGUSR2) && !sigismember(&verified.mask, SIGUSR1));
    assert(!raise(SIGUSR1) && observed == 1);
    assert(!forkRestoreSignals(&original, (void *)(uintptr_t)original.actions[SIGUSR1].handler, cover));
    close(wake[0]); close(wake[1]);
    puts("Fork signal mask, caught action, handler mask and unsupported-target rejection passed");
}
