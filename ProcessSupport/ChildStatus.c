#include "ChildStatus.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <sys/wait.h>
#include <stddef.h>

enum { Empty, Reserved, Running, Exited, Reaped, Publishing, Exiting, Replacing, Abandoned, PhaseMask = 15,
       PinShift = 4, PinBits = 16, JobShift = PinShift + PinBits, JobBits = 5, GenerationShift = JobShift + JobBits };
#define JOB_MASK (((UINT64_C(1) << JobBits) - 1) << JobShift)
_Static_assert(NSIG <= (1 << JobBits), "Job signal field is too narrow");
#define PIN_UNIT (UINT64_C(1) << PinShift)
#define PIN_MASK (((UINT64_C(1) << PinBits) - 1) << PinShift)
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2,
               "Child status access must remain lock-free in a signal handler");
_Static_assert(sizeof(LSBChildStatus) <= LSB_CHILD_STATUS_MAP_SIZE, "Child status mapping is too small");

static uint64_t tag(LSBChildTicket ticket, unsigned phase) { return (ticket.generation << GenerationShift) | phase; }
static int validTicket(LSBChildTicket ticket) {
    return ticket.index < LSB_CHILD_STATUS_CAPACITY && ticket.generation && ticket.generation <= (UINT64_MAX >> GenerationShift);
}

void LSBChildStatusInitialize(LSBChildStatus *ledger) {
    ledger->magic = LSB_CHILD_STATUS_MAGIC;
    ledger->version = LSB_CHILD_STATUS_VERSION;
    ledger->capacity = LSB_CHILD_STATUS_CAPACITY;
    atomic_init(&ledger->suppressJobSignal, 0);
    atomic_init(&ledger->terminalSessionAndGroup, 0);
    atomic_init(&ledger->terminalStop, 0);
    ledger->nativeStopEvent = (struct kevent64_s){0};
    atomic_init(&ledger->nextGeneration, 1);
    atomic_init(&ledger->discardExited, 0);
    atomic_init(&ledger->parentPID, -1);
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) {
        atomic_init(&ledger->slots[i].state, Empty);
        atomic_init(&ledger->slots[i].execution, 0);
        atomic_init(&ledger->slots[i].pid, 0);
        atomic_init(&ledger->slots[i].group, 0);
        atomic_init(&ledger->slots[i].status, 0);
    }
}

int LSBChildStatusValidate(const LSBChildStatus *ledger) {
    if (!ledger || (uintptr_t)ledger % _Alignof(LSBChildStatus) || ledger->magic != LSB_CHILD_STATUS_MAGIC ||
        ledger->version != LSB_CHILD_STATUS_VERSION || ledger->capacity != LSB_CHILD_STATUS_CAPACITY) return EPROTO;
    return 0;
}

int LSBChildStatusSetParentPID(LSBChildStatus *ledger, pid_t parentPID) {
    if (LSBChildStatusValidate(ledger) || parentPID < 0) return EINVAL;
    atomic_store_explicit(&ledger->parentPID, parentPID, memory_order_release);
    return 0;
}

pid_t LSBChildStatusGetParentPID(const LSBChildStatus *ledger) {
    return LSBChildStatusValidate(ledger) ? -1 : atomic_load_explicit(&ledger->parentPID, memory_order_acquire);
}

static uint64_t phaseTag(uint64_t state, unsigned phase) { return (state & ~(uint64_t)PhaseMask) | phase; }
static int ticketMatches(uint64_t state, LSBChildTicket ticket) { return state >> GenerationShift == ticket.generation; }

// The phase transition preserves concurrent observer pins. Publishers serialize their own updates per ticket.
static int transition(LSBChildSlot *slot, LSBChildTicket ticket, unsigned from, unsigned to, memory_order order) {
    uint64_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
    do {
        if (!ticketMatches(state, ticket) || (state & PhaseMask) != from) return ESTALE;
    } while (!atomic_compare_exchange_weak_explicit(&slot->state, &state, phaseTag(state, to), order, memory_order_acquire));
    return 0;
}

int LSBChildStatusReserve(LSBChildStatus *ledger, LSBChildTicket *ticket) {
    if (!ticket || LSBChildStatusValidate(ledger)) return EINVAL;
    uint64_t generation = atomic_load_explicit(&ledger->nextGeneration, memory_order_relaxed);
    for (;;) {
        if (!generation || generation > (UINT64_MAX >> GenerationShift)) return EOVERFLOW;
        if (atomic_compare_exchange_weak_explicit(&ledger->nextGeneration, &generation, generation + 1,
                                                memory_order_relaxed, memory_order_relaxed)) break;
    }
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) {
        uint64_t state = atomic_load_explicit(&ledger->slots[i].state, memory_order_acquire);
        while (!(state & PIN_MASK) && ((state & PhaseMask) == Empty || (state & PhaseMask) == Reaped || (state & PhaseMask) == Abandoned)) {
            uint64_t reserved = (generation << GenerationShift) | Reserved;
            if (atomic_compare_exchange_weak_explicit(&ledger->slots[i].state, &state, reserved,
                                                     memory_order_acq_rel, memory_order_acquire)) {
                *ticket = (LSBChildTicket){i, generation};
                return 0;
            }
        }
    }
    return EAGAIN;
}

int LSBChildStatusPublishRunning(LSBChildStatus *ledger, LSBChildTicket ticket, pid_t pid, pid_t group) {
    if (LSBChildStatusValidate(ledger) || !validTicket(ticket) || pid <= 0 || group < 0) return EINVAL;
    LSBChildSlot *slot = &ledger->slots[ticket.index];
    uint64_t expected = tag(ticket, Reserved);
    if (!atomic_compare_exchange_strong_explicit(&slot->state, &expected, tag(ticket, Publishing),
                                                memory_order_acq_rel, memory_order_acquire)) return ESTALE;
    atomic_store_explicit(&slot->pid, pid, memory_order_relaxed);
    atomic_store_explicit(&slot->group, group, memory_order_relaxed);
    atomic_store_explicit(&slot->status, 0, memory_order_relaxed);
    atomic_store_explicit(&slot->execution, (uint32_t)pid, memory_order_relaxed);
    atomic_store_explicit(&slot->state, tag(ticket, Running), memory_order_release);
    return 0;
}

int LSBChildStatusSetGroup(LSBChildStatus *ledger, LSBChildTicket ticket, pid_t group) {
    if (LSBChildStatusValidate(ledger) || !validTicket(ticket) || group <= 0) return EINVAL;
    LSBChildSlot *slot = &ledger->slots[ticket.index];
    uint64_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
    if (!ticketMatches(state, ticket) || (state & PhaseMask) != Running) return ESTALE;
    atomic_store_explicit(&slot->group, group, memory_order_release);
    return 0;
}

int LSBChildStatusPublishJobEvent(LSBChildStatus *ledger, LSBChildTicket ticket, int signalNumber) {
    if (LSBChildStatusValidate(ledger) || !validTicket(ticket) ||
        (signalNumber != SIGCONT && signalNumber != SIGSTOP && signalNumber != SIGTSTP && signalNumber != SIGTTIN && signalNumber != SIGTTOU)) return EINVAL;
    LSBChildSlot *slot = &ledger->slots[ticket.index];
    uint64_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
    do {
        if (!ticketMatches(state, ticket) || (state & PhaseMask) != Running) return ESTALE;
    } while (!atomic_compare_exchange_weak_explicit(&slot->state, &state, (state & ~JOB_MASK) | ((uint64_t)signalNumber << JobShift),
                                                   memory_order_acq_rel, memory_order_acquire));
    return 0;
}

int LSBChildStatusPrepareExec(LSBChildStatus *ledger, LSBChildTicket ticket, pid_t nativePID, LSBChildExecUpdate *update) {
    if (LSBChildStatusValidate(ledger) || !validTicket(ticket) || nativePID <= 0 || !update) return EINVAL;
    LSBChildSlot *slot = &ledger->slots[ticket.index];
    int error = transition(slot, ticket, Running, Replacing, memory_order_acq_rel);
    if (error) return error;
    uint64_t execution = atomic_load_explicit(&slot->execution, memory_order_relaxed);
    uint32_t generation = (uint32_t)(execution >> 32);
    if (generation == UINT32_MAX) {
        (void)transition(slot, ticket, Replacing, Running, memory_order_acq_rel);
        return EOVERFLOW;
    }
    *update = (LSBChildExecUpdate){ticket, execution, ((uint64_t)(generation + 1) << 32) | (uint32_t)nativePID, 0};
    return 0;
}

static int finishExec(LSBChildStatus *ledger, const LSBChildExecUpdate *update, int commit) {
    if (LSBChildStatusValidate(ledger) || !update || !validTicket(update->ticket) || (int32_t)update->replacementExecution <= 0 ||
        (uint32_t)(update->previousExecution >> 32) == UINT32_MAX ||
        (uint32_t)(update->replacementExecution >> 32) != (uint32_t)(update->previousExecution >> 32) + 1) return EINVAL;
    LSBChildSlot *slot = &ledger->slots[update->ticket.index];
    uint64_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
    if (!ticketMatches(state, update->ticket) || (state & PhaseMask) != Replacing ||
        atomic_load_explicit(&slot->execution, memory_order_relaxed) != update->previousExecution) return ESTALE;
    if (commit) {
        if (update->replacementGroup > 0) atomic_store_explicit(&slot->group, update->replacementGroup, memory_order_relaxed);
        atomic_store_explicit(&slot->execution, update->replacementExecution, memory_order_release);
    }
    return transition(slot, update->ticket, Replacing, Running, memory_order_acq_rel);
}

int LSBChildStatusCommitExec(LSBChildStatus *ledger, const LSBChildExecUpdate *update) { return finishExec(ledger, update, 1); }
int LSBChildStatusCancelExec(LSBChildStatus *ledger, const LSBChildExecUpdate *update) { return finishExec(ledger, update, 0); }

int LSBChildStatusPublishExec(LSBChildStatus *ledger, LSBChildTicket ticket, pid_t nativePID) {
    LSBChildExecUpdate update;
    int error = LSBChildStatusPrepareExec(ledger, ticket, nativePID, &update);
    return error ? error : LSBChildStatusCommitExec(ledger, &update);
}

static int snapshot(const LSBChildStatus *ledger, unsigned index, const LSBChildTicket *pinned, LSBChildObservation *observation) {
    const LSBChildSlot *slot = &ledger->slots[index];
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        uint64_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
        unsigned phase = state & PhaseMask;
        if (pinned && (!ticketMatches(state, *pinned) || !(state & PIN_MASK))) return ESTALE;
        if (phase == Replacing || phase == Publishing) return EAGAIN;
        if (phase != Running && phase != Exiting && phase != Exited && !(pinned && phase == Reaped)) return pinned ? ESTALE : ECHILD;
        pid_t pid = atomic_load_explicit(&slot->pid, memory_order_relaxed);
        pid_t group = atomic_load_explicit(&slot->group, memory_order_relaxed);
        uint64_t execution = atomic_load_explicit(&slot->execution, memory_order_acquire);
        int status = atomic_load_explicit(&slot->status, memory_order_relaxed);
        // Pin churn cannot change data; every phase or ticket change must be retried.
        if ((atomic_load_explicit(&slot->state, memory_order_acquire) & ~PIN_MASK) != (state & ~PIN_MASK)) continue;
        int exited = phase == Exited || phase == Reaped;
        *observation = (LSBChildObservation){{index, state >> GenerationShift}, (pid_t)(uint32_t)execution,
                                           (uint32_t)(execution >> 32), exited, exited ? status : 0, pid, group};
        return 0;
    }
    return EAGAIN;
}

int LSBChildStatusObserveIndex(const LSBChildStatus *ledger, unsigned index, LSBChildObservation *observation) {
    if (LSBChildStatusValidate(ledger) || index >= LSB_CHILD_STATUS_CAPACITY || !observation) return EINVAL;
    return snapshot(ledger, index, NULL, observation);
}

int LSBChildStatusObserve(const LSBChildStatus *ledger, pid_t guestPID, LSBChildObservation *observation) {
    if (LSBChildStatusValidate(ledger) || guestPID <= 0 || !observation) return EINVAL;
    int unstable = 0;
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) {
        LSBChildObservation found;
        int error = snapshot(ledger, i, NULL, &found);
        if (error == EAGAIN) unstable = 1;
        if (!error && found.guestPID == guestPID) { *observation = found; return 0; }
    }
    return unstable ? EAGAIN : ECHILD;
}

int LSBChildStatusAcquireObservation(LSBChildStatus *ledger, pid_t guestPID, LSBChildObservation *observation) {
    if (LSBChildStatusValidate(ledger) || guestPID <= 0 || !observation) return EINVAL;
    int unstable = 0;
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) {
        LSBChildSlot *slot = &ledger->slots[i];
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            uint64_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
            unsigned phase = state & PhaseMask;
            if (phase == Publishing || phase == Replacing) { unstable = 1; break; }
            if (phase != Running && phase != Exiting && phase != Exited) break;
            if (atomic_load_explicit(&slot->pid, memory_order_relaxed) != guestPID) break;
            if ((state & PIN_MASK) == PIN_MASK) return EOVERFLOW;
            if (!atomic_compare_exchange_weak_explicit(&slot->state, &state, state + PIN_UNIT,
                                                       memory_order_acq_rel, memory_order_acquire)) { unstable = 1; continue; }
            LSBChildTicket ticket = {i, state >> GenerationShift};
            int error = snapshot(ledger, i, &ticket, observation);
            if (error) (void)LSBChildStatusReleaseObservation(ledger, ticket);
            return error;
        }
    }
    return unstable ? EAGAIN : ECHILD;
}

int LSBChildStatusObserveTicket(const LSBChildStatus *ledger, LSBChildTicket ticket, LSBChildObservation *observation) {
    if (LSBChildStatusValidate(ledger) || !validTicket(ticket) || !observation) return EINVAL;
    return snapshot(ledger, ticket.index, &ticket, observation);
}

int LSBChildStatusReleaseObservation(LSBChildStatus *ledger, LSBChildTicket ticket) {
    if (LSBChildStatusValidate(ledger) || !validTicket(ticket)) return EINVAL;
    LSBChildSlot *slot = &ledger->slots[ticket.index];
    uint64_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
    do {
        if (!ticketMatches(state, ticket) || !(state & PIN_MASK)) return ESTALE;
    } while (!atomic_compare_exchange_weak_explicit(&slot->state, &state, state - PIN_UNIT, memory_order_acq_rel, memory_order_acquire));
    return 0;
}

int LSBChildStatusImport(LSBChildStatus *ledger, const LSBChildObservation *observation, LSBChildTicket *ticket) {
    if (!observation || !ticket || observation->guestPID <= 0 || observation->nativePID <= 0 || observation->group < 0 ||
        (observation->exited != 0 && observation->exited != 1) || observation->status < 0 || observation->status > UINT16_MAX ||
        (!observation->exited && observation->status)) return EINVAL;
    LSBChildTicket reserved;
    int error = LSBChildStatusReserve(ledger, &reserved);
    if (error) return error;
    LSBChildSlot *slot = &ledger->slots[reserved.index];
    (void)transition(slot, reserved, Reserved, Publishing, memory_order_acq_rel);
    atomic_store_explicit(&slot->pid, observation->guestPID, memory_order_relaxed);
    atomic_store_explicit(&slot->group, observation->group, memory_order_relaxed);
    atomic_store_explicit(&slot->status, observation->status, memory_order_relaxed);
    atomic_store_explicit(&slot->execution, ((uint64_t)observation->execGeneration << 32) | (uint32_t)observation->nativePID, memory_order_relaxed);
    atomic_store_explicit(&slot->state, tag(reserved, observation->exited ? Exited : Running), memory_order_seq_cst);
    if (observation->exited && atomic_load_explicit(&ledger->discardExited, memory_order_seq_cst))
        (void)transition(slot, reserved, Exited, Reaped, memory_order_acq_rel);
    *ticket = reserved;
    return 0;
}

int LSBChildStatusPublishExit(LSBChildStatus *ledger, LSBChildTicket ticket, int waitStatus) {
    if (LSBChildStatusValidate(ledger) || !validTicket(ticket) || waitStatus < 0 || waitStatus > UINT16_MAX) return EINVAL;
    LSBChildSlot *slot = &ledger->slots[ticket.index];
    int error = transition(slot, ticket, Running, Exiting, memory_order_acq_rel);
    if (error) return error;
    atomic_store_explicit(&slot->status, waitStatus, memory_order_relaxed);
    // These SC operations pair with SetDiscard's store+scan: neither side may miss the other's publication on ARM.
    (void)transition(slot, ticket, Exiting, Exited, memory_order_seq_cst);
    if (atomic_load_explicit(&ledger->discardExited, memory_order_seq_cst))
        (void)transition(slot, ticket, Exited, Reaped, memory_order_acq_rel);
    return 0;
}

int LSBChildStatusAbandon(LSBChildStatus *ledger, LSBChildTicket ticket) {
    if (LSBChildStatusValidate(ledger) || !validTicket(ticket)) return EINVAL;
    LSBChildSlot *slot = &ledger->slots[ticket.index];
    uint64_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
    for (;;) {
        unsigned phase = state & PhaseMask;
        if (!ticketMatches(state, ticket) || (phase != Reserved && phase != Running && phase != Exited)) return ESTALE;
        if (atomic_compare_exchange_weak_explicit(&slot->state, &state, phaseTag(state, phase == Exited ? Reaped : Abandoned),
                                                 memory_order_acq_rel, memory_order_acquire)) return 0;
    }
}

static int matches(pid_t selector, pid_t currentGroup, pid_t pid, pid_t group) {
    if (selector > 0) return pid == selector;
    if (selector == -1) return 1;
    return group == (selector == 0 ? (int64_t)currentGroup : -(int64_t)selector);
}

int LSBChildStatusTryWaitOptions(LSBChildStatus *ledger, pid_t selector, pid_t currentGroup, int options, pid_t *pid, int *status) {
    if (!ledger || !pid) return EINVAL;
    int live = 0;
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) {
        LSBChildSlot *slot = &ledger->slots[i];
        uint64_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
        for (;;) {
            unsigned phase = state & PhaseMask;
            if (phase == Reserved || phase == Publishing) { if (selector == -1) live = 1; break; }
            if (phase != Running && phase != Replacing && phase != Exiting && phase != Exited) break;
            pid_t child = atomic_load_explicit(&slot->pid, memory_order_relaxed);
            pid_t group = atomic_load_explicit(&slot->group, memory_order_relaxed);
            int value = atomic_load_explicit(&slot->status, memory_order_relaxed);
            // Fields can change when another waiter reaps and the host reuses a slot. Never match a mixed generation.
            uint64_t confirmed = atomic_load_explicit(&slot->state, memory_order_acquire);
            if (confirmed != state) { state = confirmed; continue; }
            if (!matches(selector, currentGroup, child, group)) break;
            if (phase != Exited) {
                live = 1;
                unsigned signalNumber = (unsigned)((state & JOB_MASK) >> JobShift);
                if (phase != Running || !signalNumber || !(options & (signalNumber == SIGCONT ? WCONTINUED : WUNTRACED))) break;
                if (!atomic_compare_exchange_weak_explicit(&slot->state, &state, state & ~JOB_MASK, memory_order_acq_rel, memory_order_acquire)) continue;
                *pid = child;
                if (status) *status = W_STOPCODE(signalNumber);
                return 0;
            }
            uint64_t reaped = (state & ~(uint64_t)PhaseMask) | Reaped;
            int discard = atomic_load_explicit(&ledger->discardExited, memory_order_seq_cst) != 0;
            if (!atomic_compare_exchange_weak_explicit(&slot->state, &state, reaped, memory_order_acq_rel, memory_order_acquire)) continue;
            if (discard) break;
            *pid = child;
            if (status) *status = value;
            return 0;
        }
    }
    return live ? EAGAIN : ECHILD;
}

int LSBChildStatusTryWait(LSBChildStatus *ledger, pid_t selector, pid_t currentGroup, pid_t *pid, int *status) {
    return LSBChildStatusTryWaitOptions(ledger, selector, currentGroup, 0, pid, status);
}

void LSBChildStatusSetDiscard(LSBChildStatus *ledger, int discard) {
    if (!ledger) return;
    atomic_store_explicit(&ledger->discardExited, discard != 0, memory_order_seq_cst);
    if (!discard) return;
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) {
        LSBChildSlot *slot = &ledger->slots[i];
        uint64_t state = atomic_load_explicit(&slot->state, memory_order_seq_cst);
        while ((state & PhaseMask) == Exited) {
            uint64_t reaped = (state & ~(uint64_t)PhaseMask) | Reaped;
            if (atomic_compare_exchange_weak_explicit(&slot->state, &state, reaped, memory_order_acq_rel, memory_order_acquire)) break;
        }
    }
}
