#include "../Guest/libLiveSimSpawn/ChildKill.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static unsigned transportCalls;
static pid_t requestedPID;
static int requestedSignal, transportError;
static volatile sig_atomic_t selfSignals;

int LSBTransportKill(pid_t pid, int signalNumber) {
    ++transportCalls;
    requestedPID = pid;
    requestedSignal = signalNumber;
    errno = ENOTTY;
    return transportError;
}

static void selfSignal(int number) { if (number == SIGUSR1) ++selfSignals; }

int main(void) {
    assert(LSBKill(42, -1) == -1 && errno == EINVAL);
    assert(LSBKill(42, NSIG) == -1 && errno == EINVAL);
    assert(LSBKillPG(-123, 0) == -1 && errno == EINVAL);
    assert(LSBKill(-1, SIGTERM) == -1 && errno == ENOTSUP);
    assert(!transportCalls);

    errno = EDOM;
    assert(!LSBKill(42, 0) && errno == EDOM);
    assert(transportCalls == 1 && requestedPID == 42 && requestedSignal == 0);
    assert(!LSBKill(42, SIGTERM) && errno == EDOM);
    assert(transportCalls == 2 && requestedSignal == SIGTERM);
    assert(!LSBKill(0, SIGTERM) && requestedPID == 0);
    assert(!LSBKillPG(123, SIGCONT) && requestedPID == -123 && requestedSignal == SIGCONT);
    transportError = ESRCH;
    assert(LSBKill(42, SIGTERM) == -1 && errno == ESRCH);
    transportError = EPERM;
    assert(LSBKill(42, SIGTERM) == -1 && errno == EPERM);
    transportError = EPIPE;
    assert(LSBKill(42, SIGTERM) == -1 && errno == EPIPE);

    unsigned before = transportCalls;
    errno = ERANGE;
    assert(!LSBKill(getpid(), 0) && errno == ERANGE);
    struct sigaction action = {.sa_handler = selfSignal}, previous;
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGUSR1, &action, &previous));
    assert(!LSBKill(getpid(), SIGUSR1));
    assert(selfSignals == 1 && transportCalls == before);
    assert(!sigaction(SIGUSR1, &previous, NULL));
    puts("ChildKill: signal validation, unsupported selectors, broker PID/signal, errno translation and native self-signals passed");
    return 0;
}
