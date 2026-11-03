#include "../Host/ChildMonitor.h"
#include "../Host/TerminalJobState.h"
#include <sys/event.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static void observeGrandchild(int signalNumber, int exitCode) {
    int report[2], release[2];
    assert(!pipe(report) && !pipe(release));
    pid_t parent = fork();
    assert(parent >= 0);
    if (!parent) {
        close(report[0]); close(release[1]);
        pid_t child = fork();
        if (child < 0) _exit(90);
        if (!child) {
            close(report[1]);
            char byte;
            if (read(release[0], &byte, 1) != 1) _exit(91);
            if (signalNumber) { signal(signalNumber, SIG_DFL); raise(signalNumber); }
            _exit(exitCode);
        }
        close(release[0]);
        if (write(report[1], &child, sizeof(child)) != sizeof(child)) _exit(92);
        int status;
        if (waitpid(child, &status, 0) != child || write(report[1], &status, sizeof(status)) != sizeof(status)) _exit(93);
        _exit(0);
    }
    close(report[1]); close(release[0]);
    pid_t child;
    assert(read(report[0], &child, sizeof(child)) == sizeof(child));
    int monitor = -1, observed = -1;
    assert(LSBChildMonitorCreate(child, &monitor) == 0);
    assert(LSBChildMonitorRead(monitor, child, &observed) == EAGAIN && observed == -1);
    assert(write(release[1], "x", 1) == 1);
    close(release[1]);
    struct pollfd ready = {.fd = monitor, .events = POLLIN};
    assert(poll(&ready, 1, 5000) == 1);
    assert(LSBChildMonitorRead(monitor, child, &observed) == 0);
    int actual;
    assert(read(report[0], &actual, sizeof(actual)) == sizeof(actual));
    assert(observed == actual);
    if (signalNumber) assert(WIFSIGNALED(observed) && WTERMSIG(observed) == signalNumber);
    else assert(WIFEXITED(observed) && WEXITSTATUS(observed) == exitCode);
    close(monitor); close(report[0]);
    assert(waitpid(parent, &actual, 0) == parent && WIFEXITED(actual) && WEXITSTATUS(actual) == 0);
}

static void observeUnmediatedStop(void) {
    int report[2], release[2];
    assert(!pipe(report) && !pipe(release));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(report[0]); close(release[1]);
        audit_token_t identity;
        mach_msg_type_number_t words = TASK_AUDIT_TOKEN_COUNT;
        if (task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&identity, &words) ||
            write(report[1], &identity, sizeof(identity)) != sizeof(identity)) _exit(90);
        char byte;
        _exit(read(release[0], &byte, 1) == 1 ? 37 : 99);
    }
    close(report[1]); close(release[0]);
    audit_token_t identity;
    assert(read(report[0], &identity, sizeof(identity)) == sizeof(identity));
    int queue = kqueue();
    struct kevent change;
    EV_SET(&change, (uintptr_t)child, EVFILT_PROC, EV_ADD | EV_CLEAR, NOTE_SIGNAL, 0, NULL);
    assert(queue >= 0 && !kevent(queue, &change, 1, NULL, 0, NULL));
    int status, stop;
    assert(!LSBReadTerminalJobState(&identity, &stop) && !stop);
    ++identity.val[7];
    assert(LSBReadTerminalJobState(&identity, &stop) == ESRCH);
    --identity.val[7];
    assert(!kill(child, SIGSTOP) && waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status));
    struct timespec timeout = {.tv_sec = 1};
    assert(kevent(queue, NULL, 0, &change, 1, &timeout) == 1 && (change.fflags & NOTE_SIGNAL));
    assert(!LSBReadTerminalJobState(&identity, &stop) && stop == SIGSTOP);
    assert(!kill(child, SIGCONT) && waitpid(child, &status, WCONTINUED) == child && WIFCONTINUED(status));
    assert(kevent(queue, NULL, 0, &change, 1, &timeout) == 1 && (change.fflags & NOTE_SIGNAL));
    assert(!LSBReadTerminalJobState(&identity, &stop) && !stop);
    assert(write(release[1], "x", 1) == 1);
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 37);
    close(queue); close(report[0]); close(release[1]);
}

static void checkSoftwareExceptionHandler(void) {
    assert(!LSBTerminalTaskHasNoSoftwareHandler(MACH_PORT_NULL));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        // Change only this disposable process's exception delivery, including if the test runner inherited a handler.
        mach_port_t task = mach_task_self(), port;
        assert(!task_set_exception_ports(task, EXC_MASK_SOFTWARE, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE));
        assert(LSBTerminalTaskHasNoSoftwareHandler(task));
        assert(!mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &port));
        assert(!mach_port_insert_right(task, port, port, MACH_MSG_TYPE_MAKE_SEND));
        assert(!task_set_exception_ports(task, EXC_MASK_SOFTWARE, port, EXCEPTION_STATE_IDENTITY_PROTECTED | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64));
        assert(!LSBTerminalTaskHasNoSoftwareHandler(task));
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
}

int main(void) {
    audit_token_t missing = {{0}};
    missing.val[5] = INT_MAX;
    // The libproc API returns a positive errno; treating only negative values as failures loses sandbox denials.
    assert(LSBChildSignal(&missing, SIGCHLD) == ESRCH);
    checkSoftwareExceptionHandler();
    observeUnmediatedStop();
    observeGrandchild(0, 0);
    observeGrandchild(0, 37);
    observeGrandchild(SIGTERM, 0);
    puts("Child monitor: real non-child exit 0, exit 37, and SIGTERM statuses passed");
    return 0;
}
