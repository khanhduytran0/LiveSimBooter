#include "ChildKill.h"
#include "ChildWait.h"
#include "ProcessIdentity.h"
#include "../ProcessSupport/SpawnTransport.h"

#include <errno.h>
#include <signal.h>
#include <unistd.h>

int LSBKill(pid_t pid, int signalNumber) {
    if (signalNumber < 0 || signalNumber >= NSIG) { errno = EINVAL; return -1; }
    if (pid == -1) { errno = ENOTSUP; return -1; }
    // References from this image retain the original implementation, as with the signal and sigaction interposers.
    if (pid == LSBGuestPID()) return signalNumber == SIGSTOP ? LSBTerminalStopSelf(SIGSTOP) : kill(getpid(), signalNumber);
    int savedErrno = errno;
    int error = LSBTransportKill(pid, signalNumber);
    if (error) { errno = error; return -1; }
    errno = savedErrno;
    return 0;
}

int LSBKillPG(pid_t group, int signalNumber) {
    if (group < 0) { errno = EINVAL; return -1; }
    return LSBKill(-group, signalNumber);
}

#ifndef LSB_CHILD_KILL_TESTING
static int terminalRaise(int number) {
    LSBChildStatus *ledger = LSBGuestChildStatus();
    if (number == SIGSTOP && ledger && atomic_load_explicit(&ledger->terminalSessionAndGroup, memory_order_acquire))
        return LSBTerminalStopSelf(SIGSTOP);
    return raise(number);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBKill, (const void *)&kill}, {(const void *)&LSBKillPG, (const void *)&killpg},
    {(const void *)&terminalRaise, (const void *)&raise}
};
#endif
