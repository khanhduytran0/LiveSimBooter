#include "../Guest/libLiveSimSpawn/ChildWait.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static LSBChildStatus ledger;
static int wakeFDs[2];
static volatile sig_atomic_t handlerPID, handlerStatus, handlerError;

static LSBChildTicket running(pid_t pid, pid_t group) {
    LSBChildTicket ticket;
    assert(!LSBChildStatusReserve(&ledger, &ticket));
    assert(!LSBChildStatusPublishRunning(&ledger, ticket, pid, group));
    return ticket;
}

static void reset(void) {
    struct sigaction action = {.sa_handler = SIG_DFL};
    sigemptyset(&action.sa_mask);
    assert(!LSBSigaction(SIGCHLD, &action, NULL));
    LSBChildStatusInitialize(&ledger);
    assert(!LSBChildWaitConfigure(&ledger, wakeFDs[0]));
    char buffer[64];
    while (read(wakeFDs[0], buffer, sizeof(buffer)) > 0) {}
}

static void testNoHangAndSelectors(void) {
    reset();
    LSBChildTicket first = running(100, getpgrp()), second = running(101, 222);
    int status = 0x1234;
    errno = EDOM;
    assert(LSBWaitPID(100, &status, WNOHANG) == 0 && status == 0x1234 && errno == EDOM);
    assert(LSBWaitPID(100, &status, WNOHANG | WUNTRACED) == 0 && status == 0x1234 && errno == EDOM);
    assert(LSBWaitPID(100, &status, WNOHANG | WCONTINUED) == 0 && status == 0x1234 && errno == EDOM);
    assert(LSBWaitPID(100, &status, WNOHANG | WUNTRACED | WCONTINUED) == 0 && status == 0x1234 && errno == EDOM);
    assert(LSBWaitPID(999, &status, WNOHANG) == -1 && errno == ECHILD && status == 0x1234);
    assert(LSBWaitPID(100, &status, WNOHANG | WUNTRACED | WCONTINUED | 0x40000000) == -1 && errno == EINVAL && status == 0x1234);
    assert(!LSBChildStatusPublishExit(&ledger, first, 0));
    errno = ERANGE;
    assert(LSBWaitPID(0, &status, WUNTRACED | WCONTINUED) == 100 && status == 0 && errno == ERANGE);
    assert(LSBWaitPID(100, &status, WNOHANG | WUNTRACED | WCONTINUED) == -1 && errno == ECHILD);
    assert(!LSBChildStatusPublishExit(&ledger, second, SIGTERM));
    assert(LSBWaitPID(-222, &status, WNOHANG | WUNTRACED | WCONTINUED) == 101 && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    assert(LSBWaitPID(101, &status, WUNTRACED | WCONTINUED) == -1 && errno == ECHILD);
    assert(LSBWait(NULL) == -1 && errno == ECHILD);
}

static void testWait4TerminalStatusAndUsage(void) {
    reset();
    const int terminalStatuses[] = {0, 37 << 8, SIGTERM};
    struct rusage usage, unchanged;
    memset(&usage, 0xa5, sizeof(usage));
    memcpy(&unchanged, &usage, sizeof(usage));
    for (unsigned i = 0; i < sizeof(terminalStatuses) / sizeof(terminalStatuses[0]); ++i) {
        pid_t pid = 501 + (pid_t)i;
        LSBChildTicket ticket = running(pid, getpgrp());
        int status = 0x1234;
        errno = EDOM;
        assert(LSBWait4(pid, &status, WNOHANG | WUNTRACED | WCONTINUED, NULL) == 0 && status == 0x1234 && errno == EDOM);
        assert(LSBWait4(pid, &status, 0x40000000, NULL) == -1 && errno == EINVAL && status == 0x1234);
        assert(LSBWait4(pid, &status, WNOHANG, &usage) == -1 && errno == ENOTSUP && status == 0x1234);
        assert(!memcmp(&usage, &unchanged, sizeof(usage)));
        assert(!LSBChildStatusPublishExit(&ledger, ticket, terminalStatuses[i]));
        assert(LSBWait4(pid, &status, 0, &usage) == -1 && errno == ENOTSUP && status == 0x1234);
        assert(!memcmp(&usage, &unchanged, sizeof(usage)));
        errno = ERANGE;
        pid_t selector = i == 0 ? 0 : i == 1 ? -getpgrp() : -1;
        assert(LSBWait4(selector, &status, WUNTRACED | WCONTINUED, NULL) == pid && status == terminalStatuses[i] && errno == ERANGE);
        if (i == 2) assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
        else assert(WIFEXITED(status) && WEXITSTATUS(status) == (i == 1 ? 37 : 0));
        status = 0x2345;
        assert(LSBWait4(pid, &status, WNOHANG, NULL) == -1 && errno == ECHILD && status == 0x2345);
        assert(LSBWaitPID(pid, &status, WNOHANG) == -1 && errno == ECHILD && status == 0x2345);
    }
    assert(LSBWait4(-1, NULL, 0, NULL) == -1 && errno == ECHILD);
}

static void testWait4ExplicitStatusOnly(void) {
    assert(!setenv("LSB_WAIT4_STATUS_ONLY", "1", 1));
    reset();
    struct rusage usage, unchanged, zero = {0};
    memset(&usage, 0xa5, sizeof(usage));
    unchanged = usage;
    int status = 0x1234;
    LSBChildTicket ticket = running(601, getpgrp());
    errno = EDOM;
    assert(LSBWait4(601, &status, WNOHANG, &usage) == 0 && status == 0x1234 && errno == EDOM);
    assert(!memcmp(&usage, &unchanged, sizeof(usage)));
    assert(LSBWait4(601, &status, 0x40000000, &usage) == -1 && errno == EINVAL && status == 0x1234);
    assert(!memcmp(&usage, &unchanged, sizeof(usage)));
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 37 << 8));
    errno = ERANGE;
    assert(LSBWait4(601, &status, 0, &usage) == 601 && WEXITSTATUS(status) == 37 && errno == ERANGE);
    assert(!memcmp(&usage, &zero, sizeof(usage)));
    ticket = running(602, getpgrp());
    assert(!LSBChildStatusPublishExit(&ledger, ticket, SIGTERM));
    assert(LSBWait4(602, &status, 0, &usage) == 602 && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    usage = unchanged;
    assert(LSBWait4(602, &status, WNOHANG, &usage) == -1 && errno == ECHILD);
    assert(!memcmp(&usage, &unchanged, sizeof(usage)));
    assert(!unsetenv("LSB_WAIT4_STATUS_ONLY"));
    reset();
    assert(LSBWait4(-1, NULL, WNOHANG, &usage) == -1 && errno == ENOTSUP);
}

typedef struct { LSBChildTicket ticket; int status, wake; } Completion;
static void *completeLater(void *pointer) {
    Completion *completion = pointer;
    usleep(30000);
    assert(!LSBChildStatusPublishExit(&ledger, completion->ticket, completion->status));
    if (completion->wake) assert(write(wakeFDs[1], "x", 1) == 1);
    return NULL;
}

static void testBlockingWait(void) {
    for (unsigned variant = 0; variant < 4; ++variant) {
        reset();
        int wake = variant & 1;
        Completion completion = {running(201, getpgrp()), 37 << 8, wake};
        pthread_t worker;
        assert(!pthread_create(&worker, NULL, completeLater, &completion));
        int status;
        int options = wake ? WUNTRACED | WCONTINUED : 0;
        errno = EDOM;
        pid_t result = variant & 2 ? LSBWait4(-1, &status, options, NULL) : LSBWaitPID(-1, &status, options);
        assert(result == 201 && WIFEXITED(status) && WEXITSTATUS(status) == 37 && errno == EDOM);
        assert(!pthread_join(worker, NULL));
    }
}

static void reapInHandler(int number) {
    (void)number;
    int saved = errno, status = -1;
    handlerPID = LSBWaitPID(-1, &status, WNOHANG);
    handlerStatus = status;
    handlerError = errno;
    errno = saved;
}

static void testSignalHandlerReapAndDiscard(void) {
    reset();
    struct sigaction action = {.sa_handler = reapInHandler};
    sigemptyset(&action.sa_mask);
    assert(!LSBSigaction(SIGCHLD, &action, NULL));
    LSBChildTicket ticket = running(301, getpgrp());
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 42 << 8));
    errno = ENOTTY;
    assert(!raise(SIGCHLD));
    assert(handlerPID == 301 && WEXITSTATUS(handlerStatus) == 42 && handlerError == ENOTTY && errno == ENOTTY);
    assert(LSBWaitPID(301, NULL, WNOHANG) == -1 && errno == ECHILD);

    ticket = running(302, getpgrp());
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 0));
    action.sa_handler = SIG_IGN;
    // Native sigaction permits its input and output to alias. The shim must publish the requested policy, not the old one.
    assert(!LSBSigaction(SIGCHLD, &action, &action));
    assert(LSBWaitPID(302, NULL, WNOHANG) == -1 && errno == ECHILD);
    ticket = running(303, getpgrp());
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 0));
    assert(LSBWaitPID(303, NULL, WNOHANG) == -1 && errno == ECHILD);
    assert(LSBSignal(SIGCHLD, SIG_DFL) != SIG_ERR);
    ticket = running(304, getpgrp());
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 9 << 8));
    int status;
    assert(LSBWaitPID(304, &status, WNOHANG) == 304 && WEXITSTATUS(status) == 9);
    action = (struct sigaction){.sa_handler = SIG_DFL, .sa_flags = SA_NOCLDWAIT};
    sigemptyset(&action.sa_mask);
    assert(!LSBSigaction(SIGCHLD, &action, NULL));
    ticket = running(305, getpgrp());
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 0));
    assert(LSBWaitPID(305, NULL, WNOHANG) == -1 && errno == ECHILD);
}

static void interruptHandler(int number) { (void)number; }
static void *interruptLater(void *pointer) {
    usleep(30000);
    assert(!pthread_kill(*(pthread_t *)pointer, SIGUSR1));
    return NULL;
}

static void testInterruptedWait(void) {
    for (unsigned variant = 0; variant < 2; ++variant) {
        reset();
        LSBChildTicket ticket = running(401, getpgrp());
        struct sigaction action = {.sa_handler = interruptHandler};
        sigemptyset(&action.sa_mask);
        assert(!sigaction(SIGUSR1, &action, NULL));
        pthread_t mainThread = pthread_self(), worker;
        assert(!pthread_create(&worker, NULL, interruptLater, &mainThread));
        int status = 0x2222;
        pid_t result = variant ? LSBWait4(401, &status, 0, NULL) : LSBWaitPID(401, &status, 0);
        assert(result == -1 && errno == EINTR && status == 0x2222);
        assert(!pthread_join(worker, NULL));
        assert(!LSBChildStatusPublishExit(&ledger, ticket, 37 << 8));
        assert(LSBWait4(401, &status, 0, NULL) == 401 && WIFEXITED(status) && WEXITSTATUS(status) == 37);
    }
}

static void testTerminalStopDisposition(void) {
    reset();
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        // With a new native session the kernel ignores the original default TSTP. The guest foreground group still has a parent.
        assert(setsid() == getpid());
        atomic_store(&ledger.terminalSessionAndGroup, ((uint64_t)(uint32_t)getpid() << 32) | (uint32_t)getpid());
        assert(!LSBChildWaitConfigure(&ledger, wakeFDs[0]));
        assert(LSBSignal(SIGTSTP, SIG_IGN) == SIG_DFL && !raise(SIGTSTP) && !atomic_load(&ledger.terminalStop));
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGTSTP);
        assert(!sigprocmask(SIG_BLOCK, &mask, NULL));
        assert(LSBSignal(SIGTSTP, SIG_DFL) == SIG_IGN && !raise(SIGTSTP) && !atomic_load(&ledger.terminalStop));
        struct sigaction current;
        assert(!LSBSigaction(SIGTSTP, NULL, &current) && current.sa_handler == SIG_DFL);
        assert(!sigprocmask(SIG_UNBLOCK, &mask, NULL));
        assert(atomic_load(&ledger.terminalStop) == 32);
        assert(!LSBTerminalStopSelf(SIGSTOP));
        _exit(atomic_load(&ledger.terminalStop) == 64 ? 37 : 100);
    }
    int status;
    assert(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
    assert(!kill(child, SIGCONT));
    assert(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
    assert(!kill(child, SIGCONT));
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 37);
}

static void testReusedWakeDescriptor(void) {
    reset();
    int replacement[2];
    assert(!pipe(replacement));
    assert(LSBGuestChildWakeFD() == wakeFDs[0]);
    assert(dup2(replacement[0], wakeFDs[0]) == wakeFDs[0]);
    errno = ERANGE;
    assert(LSBGuestChildWakeFD() == -1 && errno == ERANGE);
    assert(write(replacement[1], "x", 1) == 1);
    char byte;
    assert(read(wakeFDs[0], &byte, 1) == 1 && byte == 'x');
    close(replacement[0]);
    close(replacement[1]);
}

int main(void) {
    alarm(15);
    assert(!unsetenv("LSB_WAIT4_STATUS_ONLY"));
    assert(!pipe(wakeFDs) && !fcntl(wakeFDs[0], F_SETFL, O_NONBLOCK));
    testNoHangAndSelectors();
    testWait4TerminalStatusAndUsage();
    testWait4ExplicitStatusOnly();
    testBlockingWait();
    testSignalHandlerReapAndDiscard();
    testInterruptedWait();
    testTerminalStopDisposition();
    testReusedWakeDescriptor();
    close(wakeFDs[0]); close(wakeFDs[1]);
    puts("ChildWait: waitpid/wait4, groups, status, strict/opt-in rusage, blocking/lost wakes, signal reaping, discard and EINTR passed");
    return 0;
}
