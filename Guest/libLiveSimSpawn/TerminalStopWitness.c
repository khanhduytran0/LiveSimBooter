#include "TerminalStopWitness.h"

#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#if defined(LSB_FORK_SNAPSHOT_TRIAL)
#include <stdatomic.h>
static _Atomic uint64_t witnessThreadID;
uint64_t LSBTerminalStopWitnessThreadID(void) { return atomic_load_explicit(&witnessThreadID, memory_order_acquire); }
#endif

static struct {
    int queue;
    uint64_t owner;
    struct kevent64_s *event;
} witness;

static int registerSignals(uint32_t generation, unsigned flags) {
    struct kevent64_s changes[2];
    uint64_t stamp = witness.owner | ((uint64_t)generation << 1);
    EV_SET64(changes, SIGSTOP, EVFILT_SIGNAL, flags, 0, 0, stamp | 1, 0, 0);
    EV_SET64(changes + 1, SIGCONT, EVFILT_SIGNAL, flags, 0, 0, stamp, 0, 0);
    return kevent64(witness.queue, changes, 2, NULL, 0, 0, NULL);
}

static void *waitForStop(void *unused) {
    (void)unused;
#if defined(LSB_FORK_SNAPSHOT_TRIAL)
    uint64_t id = 0;
    if (!pthread_threadid_np(NULL, &id)) atomic_store_explicit(&witnessThreadID, id, memory_order_release);
#endif
    // The syscall's result buffer is mapped by the host. XNU wakes this knote for the exact signal before returning
    // any guest thread to userspace. Thus the kernel can publish the stop even while the guest cannot run its own code.
    for (uint32_t generation = 1;;) {
        int count;
        do {
            count = kevent64(witness.queue, NULL, 0, witness.event, 1, 0, NULL);
        } while (count < 0 && errno == EINTR);
        if (count != 1) break;
        if (++generation > INT32_MAX) generation = 1;
        // Keep registration errors out of the shared buffer. EV_ENABLE retags existing knotes without adding to a reused fd.
        if (registerSignals(generation, EV_ENABLE) < 0) break;
    }
    // A guest may have closed or repurposed the private fd. Do not close a replacement on this error path.
#if defined(LSB_FORK_SNAPSHOT_TRIAL)
    atomic_store_explicit(&witnessThreadID, 0, memory_order_release);
#endif
    return NULL;
}

int LSBStartTerminalStopWitness(struct kevent64_s *sharedEvent) {
    audit_token_t audit;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    if (task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&audit, &count) != KERN_SUCCESS || count != TASK_AUDIT_TOKEN_COUNT ||
        audit.val[5] != (uint32_t)getpid() || !audit.val[7]) return EACCES;
    // XNU's kqueue_internal creates a confined, close-on-exec and close-on-fork descriptor.
    int queue = kqueue();
    if (queue < 0) return errno;
    witness.queue = queue;
    witness.owner = (uint64_t)audit.val[7] << 32;
    witness.event = sharedEvent;
    int error = registerSignals(1, EV_ADD | EV_CLEAR) < 0 ? errno : 0;
    // Inherit the mask rather than leaving a window in which a shell's SIGCHLD or tty signal is stolen.
    sigset_t mask, saved;
    sigfillset(&mask);
    if (!error) error = pthread_sigmask(SIG_SETMASK, &mask, &saved);
    if (!error) {
        pthread_t thread;
        error = pthread_create(&thread, NULL, waitForStop, NULL);
        if (!error) pthread_detach(thread);
        (void)pthread_sigmask(SIG_SETMASK, &saved, NULL);
    }
    if (error) close(queue);
    return error;
}
