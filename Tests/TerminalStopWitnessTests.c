#include "../Guest/TerminalStopWitness.c"

#include <assert.h>
#include <libproc.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t ownedChild;

static void terminateTest(int number) {
    if (ownedChild > 0) kill(ownedChild, SIGKILL);
    _exit(128 + number);
}

static void waitUntilArmed(void) {
    for (unsigned i = 0; i < 2000; ++i) {
        struct kqueue_fdinfo info;
        assert(proc_pidfdinfo(getpid(), witness.queue, PROC_PIDFDKQUEUEINFO, &info, sizeof(info)) == sizeof(info));
        if (info.kqueueinfo.kq_state & PROC_KQUEUE_SLEEP) return;
        usleep(1000);
    }
    assert(!"listener did not reach its kernel wait");
}

static uint64_t waitReceipt(struct kevent64_s *event, uint32_t owner, bool stop, uint64_t previous) {
    for (unsigned i = 0; i < 2000; ++i) {
        uint64_t value = __atomic_load_n(&event->udata, __ATOMIC_ACQUIRE);
        if ((value >> 32) == owner && (value & 1) == stop && value != previous) return value;
        usleep(1000);
    }
    assert(!"kernel did not publish a fresh signal receipt");
    return 0;
}

int main(void) {
    struct kevent64_s *shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
    assert(shared != MAP_FAILED);
    int ready[2], control[2], status;
    assert(!pipe(ready) && !pipe(control));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        char byte;
        close(ready[0]);
        close(control[1]);
        signal(SIGUSR1, SIG_IGN);
        sigset_t before, after;
        assert(!pthread_sigmask(SIG_SETMASK, NULL, &before));
        assert(!LSBStartTerminalStopWitness(shared));
        assert(!pthread_sigmask(SIG_SETMASK, NULL, &after) && before == after);
        audit_token_t token;
        mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
        assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &count));
        assert(write(ready[1], &token, sizeof(token)) == sizeof(token));
        while (read(control[0], &byte, 1) == 1 && byte == 'R') {
            waitUntilArmed();
            assert(write(ready[1], "R", 1) == 1);
        }
        assert(byte == 'M');
        assert(write(ready[1], "M", 1) == 1);
        task_suspend(mach_task_self());
        _exit(87);
    }
    ownedChild = child;
    signal(SIGABRT, terminateTest);
    signal(SIGALRM, terminateTest);
    alarm(12);
    close(ready[1]);
    close(control[0]);
    char byte;
    audit_token_t token;
    assert(read(ready[0], &token, sizeof(token)) == sizeof(token) && token.val[5] == (uint32_t)child && token.val[7]);
    uint64_t previous = 0;
    for (unsigned i = 0; i < 12; ++i) {
        assert(write(control[1], "R", 1) == 1 && read(ready[0], &byte, 1) == 1 && byte == 'R');
        assert(!kill(child, SIGSTOP));
        assert(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
        // The stopped child cannot run user code to tell the host about this event.
        uint64_t stop = waitReceipt(shared, token.val[7], true, previous);
        assert(!kill(child, SIGCONT));
        previous = waitReceipt(shared, token.val[7], false, stop);
    }
    assert(!kill(child, SIGUSR1));
    assert(write(control[1], "M", 1) == 1 && read(ready[0], &byte, 1) == 1 && byte == 'M');
    usleep(10000);
    assert(waitpid(child, &status, WUNTRACED | WNOHANG) == 0);
    assert(__atomic_load_n(&shared->udata, __ATOMIC_ACQUIRE) == previous);
    assert(!kill(child, SIGKILL) && waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    ownedChild = 0;
    alarm(0);
    assert(!munmap(shared, sizeof(*shared)));
    puts("Terminal stop witness: external BSD stops copied out while stopped, continuation retags, plain Mach hold ignored");
}
