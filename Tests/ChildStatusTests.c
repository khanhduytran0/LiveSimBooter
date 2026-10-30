#include "../ProcessSupport/ChildStatus.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static LSBChildStatus ledger;

static LSBChildTicket running(pid_t pid, pid_t group) {
    LSBChildTicket ticket;
    assert(!LSBChildStatusReserve(&ledger, &ticket));
    assert(!LSBChildStatusPublishRunning(&ledger, ticket, pid, group));
    return ticket;
}

static void testSelectionAndReuse(void) {
    LSBChildStatusInitialize(&ledger);
    assert(!LSBChildStatusValidate(&ledger));
    pid_t pid = 99;
    int status = 88;
    assert(LSBChildStatusTryWait(&ledger, -1, 0, &pid, &status) == ECHILD && pid == 99 && status == 88);
    LSBChildTicket first = running(10, 7), second = running(11, 8);
    assert(LSBChildStatusTryWait(&ledger, 10, 0, &pid, &status) == EAGAIN && pid == 99 && status == 88);
    assert(LSBChildStatusTryWait(&ledger, 12, 0, &pid, &status) == ECHILD);
    assert(!LSBChildStatusPublishExit(&ledger, first, 37 << 8));
    assert(!LSBChildStatusPublishExit(&ledger, second, SIGTERM));
    assert(LSBChildStatusTryWait(&ledger, 0, 9, &pid, &status) == ECHILD);
    assert(!LSBChildStatusTryWait(&ledger, -8, 0, &pid, &status) && pid == 11 && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    assert(!LSBChildStatusTryWait(&ledger, 0, 7, &pid, &status) && pid == 10 && WIFEXITED(status) && WEXITSTATUS(status) == 37);
    assert(LSBChildStatusTryWait(&ledger, -1, 0, &pid, &status) == ECHILD);
    LSBChildTicket reused = running(12, 7);
    assert(reused.index == first.index && reused.generation != first.generation);
    assert(LSBChildStatusPublishExit(&ledger, first, 0) == ESTALE);
    assert(LSBChildStatusAbandon(&ledger, first) == ESTALE);
    assert(LSBChildStatusTryWait(&ledger, 12, 0, &pid, &status) == EAGAIN);
    assert(!LSBChildStatusPublishExit(&ledger, reused, 0));
    assert(!LSBChildStatusTryWait(&ledger, -1, 0, &pid, NULL) && pid == 12);
    assert(LSBChildStatusTryWait(&ledger, INT_MIN, 0, &pid, NULL) == ECHILD);
}

static void testCapacityAndDiscard(void) {
    LSBChildStatusInitialize(&ledger);
    LSBChildTicket tickets[LSB_CHILD_STATUS_CAPACITY], extra;
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) tickets[i] = running((pid_t)i + 1, 1);
    assert(LSBChildStatusReserve(&ledger, &extra) == EAGAIN);
    // A live 65th child must be visible without consuming/reaping it, even while every ledger slot is occupied.
    LSBChildObservation observation;
    pid_t pid = -1;
    int status = -1;
    assert(tickets[64].index == 64);
    assert(!LSBChildStatusObserve(&ledger, 65, &observation) && observation.ticket.index == 64 && !observation.exited);
    assert(LSBChildStatusTryWait(&ledger, 65, 0, &pid, &status) == EAGAIN && pid == -1 && status == -1);
    // The last slot must retain full observation, exec replacement, wait and reuse behavior too.
    LSBChildTicket last = tickets[LSB_CHILD_STATUS_CAPACITY - 1];
    assert(last.index >= 64);
    assert(!LSBChildStatusPublishExec(&ledger, last, 9000));
    assert(!LSBChildStatusObserveIndex(&ledger, last.index, &observation));
    assert(observation.nativePID == 9000 && observation.execGeneration == 1);
    assert(!LSBChildStatusPublishExit(&ledger, last, SIGTERM));
    assert(!LSBChildStatusTryWait(&ledger, LSB_CHILD_STATUS_CAPACITY, 0, &pid, &status));
    assert(pid == LSB_CHILD_STATUS_CAPACITY && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    assert(!LSBChildStatusReserve(&ledger, &extra) && extra.index == last.index && extra.generation != last.generation);
    assert(!LSBChildStatusPublishRunning(&ledger, extra, 9001, 1));
    assert(LSBChildStatusPublishExit(&ledger, last, 0) == ESTALE);
    assert(LSBChildStatusReserve(&ledger, &extra) == EAGAIN);
    assert(!LSBChildStatusAbandon(&ledger, tickets[4]));
    assert(!LSBChildStatusReserve(&ledger, &extra) && extra.index == tickets[4].index);
    assert(!LSBChildStatusAbandon(&ledger, extra));
    assert(!LSBChildStatusPublishExit(&ledger, tickets[0], 1 << 8));
    LSBChildStatusSetDiscard(&ledger, 1);
    assert(!LSBChildStatusPublishExit(&ledger, tickets[1], 2 << 8));
    assert(LSBChildStatusTryWait(&ledger, 1, 0, &pid, &status) == ECHILD);
    assert(LSBChildStatusTryWait(&ledger, 2, 0, &pid, &status) == ECHILD);
    LSBChildStatusSetDiscard(&ledger, 0);
    assert(!LSBChildStatusPublishExit(&ledger, tickets[2], 3 << 8));
    assert(!LSBChildStatusTryWait(&ledger, 3, 0, &pid, &status) && WEXITSTATUS(status) == 3);
    ledger.version = 99;
    assert(LSBChildStatusValidate(&ledger) == EPROTO);
    ledger.version = LSB_CHILD_STATUS_VERSION;
    atomic_store(&ledger.nextGeneration, UINT64_MAX);
    assert(LSBChildStatusReserve(&ledger, &extra) == EOVERFLOW && atomic_load(&ledger.nextGeneration) == UINT64_MAX);
}

static void testLedgerABI(void) {
    LSBChildStatusInitialize(&ledger);
    assert(sizeof(ledger) == 3872 && sizeof(ledger) <= LSB_CHILD_STATUS_MAP_SIZE && ledger.capacity == 120);
    // Both former layouts and either mismatched version/capacity must fail before any slot is scanned.
    for (unsigned version = 3; version <= 4; ++version) {
        ledger.version = version;
        ledger.capacity = version == 3 ? 16 : 64;
        assert(LSBChildStatusValidate(&ledger) == EPROTO);
        LSBChildTicket ticket = {.index = UINT32_MAX, .generation = UINT64_MAX};
        assert(LSBChildStatusReserve(&ledger, &ticket) == EINVAL);
        assert(ticket.index == UINT32_MAX && ticket.generation == UINT64_MAX);
    }
    ledger.capacity = LSB_CHILD_STATUS_CAPACITY;
    assert(LSBChildStatusValidate(&ledger) == EPROTO);
    ledger.version = LSB_CHILD_STATUS_VERSION;
    ledger.capacity = 64;
    assert(LSBChildStatusValidate(&ledger) == EPROTO);
    ledger.capacity = LSB_CHILD_STATUS_CAPACITY;
    assert(!LSBChildStatusValidate(&ledger));
}

static void testExecutionObservation(void) {
    LSBChildStatusInitialize(&ledger);
    LSBChildObservation observation, unchanged;
    memset(&observation, 0xa5, sizeof(observation));
    memcpy(&unchanged, &observation, sizeof(observation));
    assert(LSBChildStatusObserve(&ledger, 70, &observation) == ECHILD && !memcmp(&observation, &unchanged, sizeof(observation)));
    assert(LSBChildStatusObserve(&ledger, 0, &observation) == EINVAL);
    LSBChildTicket ticket = running(70, 13);
    assert(!LSBChildStatusObserve(&ledger, 70, &observation));
    assert(observation.ticket.index == ticket.index && observation.ticket.generation == ticket.generation);
    assert(observation.nativePID == 70 && observation.execGeneration == 0 && !observation.exited && !observation.status);
    assert(!LSBChildStatusPublishExec(&ledger, ticket, 700));
    assert(!LSBChildStatusObserve(&ledger, 70, &observation) && observation.nativePID == 700 && observation.execGeneration == 1);
    assert(LSBChildStatusObserve(&ledger, 700, &observation) == ECHILD);
    assert(LSBChildStatusPublishExec(&ledger, ticket, 0) == EINVAL);
    assert(!LSBChildStatusPublishExec(&ledger, ticket, 701));
    assert(!LSBChildStatusObserve(&ledger, 70, &observation) && observation.nativePID == 701 && observation.execGeneration == 2);
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 37 << 8));
    for (unsigned i = 0; i < 3; ++i) {
        assert(!LSBChildStatusObserve(&ledger, 70, &observation));
        assert(observation.exited && observation.status == (37 << 8) && observation.nativePID == 701 && observation.execGeneration == 2);
    }
    assert(LSBChildStatusPublishExec(&ledger, ticket, 702) == ESTALE);
    pid_t pid;
    int status;
    assert(!LSBChildStatusTryWait(&ledger, -13, 0, &pid, &status) && pid == 70 && status == (37 << 8));
    unchanged = observation;
    assert(LSBChildStatusObserve(&ledger, 70, &observation) == ECHILD && !memcmp(&observation, &unchanged, sizeof(observation)));
    LSBChildTicket replacement = running(71, 13);
    assert(replacement.index == ticket.index && replacement.generation != ticket.generation);
    assert(LSBChildStatusPublishExec(&ledger, ticket, 703) == ESTALE);
    assert(!LSBChildStatusObserve(&ledger, 71, &observation) && observation.nativePID == 71 && !observation.execGeneration);
    atomic_store(&ledger.slots[replacement.index].execution, ((uint64_t)UINT32_MAX << 32) | 71);
    assert(LSBChildStatusPublishExec(&ledger, replacement, 704) == EOVERFLOW);
    assert(!LSBChildStatusObserve(&ledger, 71, &observation) && observation.nativePID == 71 && observation.execGeneration == UINT32_MAX);
}

static void testStagedExecution(void) {
    LSBChildStatusInitialize(&ledger);
    LSBChildTicket ticket = running(80, 14);
    LSBChildExecUpdate update;
    assert(!LSBChildStatusPrepareExec(&ledger, ticket, 800, &update));
    LSBChildObservation observation, unchanged;
    memset(&observation, 0xa5, sizeof(observation));
    memcpy(&unchanged, &observation, sizeof(observation));
    assert(LSBChildStatusObserve(&ledger, 80, &observation) == EAGAIN && !memcmp(&observation, &unchanged, sizeof(observation)));
    pid_t pid = 99;
    int status = 88;
    assert(LSBChildStatusTryWait(&ledger, 80, 0, &pid, &status) == EAGAIN && pid == 99 && status == 88);
    assert(LSBChildStatusPublishExit(&ledger, ticket, 0) == ESTALE);
    assert(LSBChildStatusPublishExec(&ledger, ticket, 801) == ESTALE);
    assert(!LSBChildStatusCancelExec(&ledger, &update));
    assert(LSBChildStatusCancelExec(&ledger, &update) == ESTALE);
    assert(!LSBChildStatusObserve(&ledger, 80, &observation) && observation.nativePID == 80 && observation.execGeneration == 0);
    assert(!LSBChildStatusPrepareExec(&ledger, ticket, 802, &update));
    assert(!LSBChildStatusCommitExec(&ledger, &update));
    assert(LSBChildStatusCommitExec(&ledger, &update) == ESTALE && LSBChildStatusCancelExec(&ledger, &update) == ESTALE);
    assert(!LSBChildStatusObserve(&ledger, 80, &observation) && observation.nativePID == 802 && observation.execGeneration == 1);
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 37 << 8));
    assert(!LSBChildStatusTryWait(&ledger, -14, 0, &pid, &status) && pid == 80 && status == (37 << 8));
}

enum { ExecRounds = 20000, ExecGuestPID = 100000 };
static LSBChildTicket executionTicket;
static _Atomic(unsigned) executionFinished;
static void *replaceExecutables(void *unused) {
    (void)unused;
    for (unsigned i = 1; i <= ExecRounds; ++i) {
        LSBChildExecUpdate cancelled;
        assert(!LSBChildStatusPrepareExec(&ledger, executionTicket, ExecGuestPID + ExecRounds + (pid_t)i, &cancelled));
        assert(!LSBChildStatusCancelExec(&ledger, &cancelled));
        assert(!LSBChildStatusPublishExec(&ledger, executionTicket, ExecGuestPID + (pid_t)i));
    }
    assert(!LSBChildStatusPublishExit(&ledger, executionTicket, SIGTERM));
    atomic_store_explicit(&executionFinished, 1, memory_order_release);
    return NULL;
}

static void testConcurrentExecutionObservation(void) {
    LSBChildStatusInitialize(&ledger);
    executionTicket = running(ExecGuestPID, 29);
    atomic_store(&executionFinished, 0);
    pthread_t publisher;
    assert(!pthread_create(&publisher, NULL, replaceExecutables, NULL));
    uint32_t lastGeneration = 0;
    do {
        LSBChildObservation observation;
        int error = LSBChildStatusObserve(&ledger, ExecGuestPID, &observation);
        assert(!error || error == EAGAIN);
        if (!error) {
            assert(observation.ticket.index == executionTicket.index && observation.ticket.generation == executionTicket.generation);
            assert(observation.nativePID == ExecGuestPID + (pid_t)observation.execGeneration);
            assert(observation.execGeneration >= lastGeneration && observation.execGeneration <= ExecRounds);
            if (observation.exited) assert(observation.execGeneration == ExecRounds && observation.status == SIGTERM);
            else assert(!observation.status);
            lastGeneration = observation.execGeneration;
        }
    } while (!atomic_load_explicit(&executionFinished, memory_order_acquire));
    assert(!pthread_join(publisher, NULL));
    LSBChildObservation observation;
    assert(!LSBChildStatusObserve(&ledger, ExecGuestPID, &observation) && observation.execGeneration == ExecRounds && observation.exited);
    pid_t pid;
    int status;
    assert(!LSBChildStatusTryWait(&ledger, -29, 0, &pid, &status) && pid == ExecGuestPID && status == SIGTERM);
}

static _Atomic(unsigned) gate, winners;
static void *competingWaiter(void *unused) {
    (void)unused;
    while (!atomic_load(&gate)) sched_yield();
    pid_t pid;
    int status;
    int result = LSBChildStatusTryWait(&ledger, 42, 0, &pid, &status);
    assert(result == 0 || result == ECHILD);
    if (!result) { assert(pid == 42 && status == 0); atomic_fetch_add(&winners, 1); }
    return NULL;
}

static void testConcurrentReaping(void) {
    LSBChildStatusInitialize(&ledger);
    LSBChildTicket ticket = running(42, 1);
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 0));
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_create(&threads[i], NULL, competingWaiter, NULL));
    atomic_store(&gate, 1);
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_join(threads[i], NULL));
    assert(atomic_load(&winners) == 1);
}

enum { StressCount = 4000 };
static void *reuseProducer(void *unused) {
    (void)unused;
    for (int i = 0; i < StressCount; ++i) {
        LSBChildTicket ticket;
        int error;
        do { error = LSBChildStatusReserve(&ledger, &ticket); if (error == EAGAIN) sched_yield(); } while (error == EAGAIN);
        assert(!error && !LSBChildStatusPublishRunning(&ledger, ticket, 100 + i, 9));
        assert(!LSBChildStatusPublishExit(&ledger, ticket, (i & 255) << 8));
    }
    return NULL;
}

static void testConcurrentReuse(void) {
    LSBChildStatusInitialize(&ledger);
    pthread_t producer;
    assert(!pthread_create(&producer, NULL, reuseProducer, NULL));
    unsigned reaped = 0;
    while (reaped < StressCount) {
        pid_t pid;
        int status;
        int result = LSBChildStatusTryWait(&ledger, -1, 0, &pid, &status);
        if (!result) { assert(pid >= 100 && pid < 100 + StressCount && WEXITSTATUS(status) == ((pid - 100) & 255)); ++reaped; }
        else { assert(result == EAGAIN || result == ECHILD); sched_yield(); }
    }
    assert(!pthread_join(producer, NULL));
}

enum { PolicyRounds = 4000 };
static _Atomic(unsigned) policyRound, policyFinished;
static LSBChildTicket policyTicket;
static void *racePolicyPublication(void *pointer) {
    int publish = pointer != NULL;
    for (unsigned round = 1; round <= PolicyRounds; ++round) {
        while (atomic_load_explicit(&policyRound, memory_order_acquire) != round) sched_yield();
        if (publish) assert(!LSBChildStatusPublishExit(&ledger, policyTicket, 17 << 8));
        else LSBChildStatusSetDiscard(&ledger, 1);
        atomic_fetch_add_explicit(&policyFinished, 1, memory_order_release);
    }
    return NULL;
}

static void testConcurrentDiscard(void) {
    pthread_t publisher, policy;
    assert(!pthread_create(&publisher, NULL, racePolicyPublication, &ledger));
    assert(!pthread_create(&policy, NULL, racePolicyPublication, NULL));
    for (unsigned round = 1; round <= PolicyRounds; ++round) {
        LSBChildStatusInitialize(&ledger);
        policyTicket = running(55, 1);
        LSBChildObservation observation;
        assert(!LSBChildStatusAcquireObservation(&ledger, 55, &observation));
        atomic_store_explicit(&policyFinished, 0, memory_order_relaxed);
        atomic_store_explicit(&policyRound, round, memory_order_release);
        while (atomic_load_explicit(&policyFinished, memory_order_acquire) != 2) sched_yield();
        pid_t pid;
        int status;
        assert(LSBChildStatusTryWait(&ledger, -1, 0, &pid, &status) == ECHILD);
        assert(!LSBChildStatusObserveTicket(&ledger, policyTicket, &observation) && observation.exited && observation.status == (17 << 8));
        assert(!LSBChildStatusReleaseObservation(&ledger, policyTicket));
    }
    assert(!pthread_join(publisher, NULL) && !pthread_join(policy, NULL));
}

enum { PinRaceRounds = 4000 };
static _Atomic(unsigned) pinRound, pinFinished;
static LSBChildTicket pinTicket;
static void *racePinnedReuse(void *unused) {
    (void)unused;
    for (unsigned round = 1; round <= PinRaceRounds; ++round) {
        while (atomic_load_explicit(&pinRound, memory_order_acquire) != round) sched_yield();
        assert(!LSBChildStatusPublishExec(&ledger, pinTicket, 4100));
        assert(!LSBChildStatusPublishExit(&ledger, pinTicket, 42 << 8));
        pid_t pid;
        int status;
        assert(!LSBChildStatusTryWait(&ledger, 4000, 0, &pid, &status) && status == (42 << 8));
        LSBChildTicket replacement = running(4000, 40);
        assert(replacement.index != pinTicket.index);
        assert(!LSBChildStatusPublishExec(&ledger, replacement, 4200));
        assert(!LSBChildStatusPublishExit(&ledger, replacement, SIGTERM));
        assert(!LSBChildStatusTryWait(&ledger, 4000, 0, &pid, &status) && status == SIGTERM);
        atomic_store_explicit(&pinFinished, round, memory_order_release);
    }
    return NULL;
}

static void testConcurrentPinnedReuse(void) {
    LSBChildStatusInitialize(&ledger);
    pthread_t producer;
    assert(!pthread_create(&producer, NULL, racePinnedReuse, NULL));
    for (unsigned round = 1; round <= PinRaceRounds; ++round) {
        pinTicket = running(4000, 40);
        LSBChildObservation observation;
        assert(!LSBChildStatusAcquireObservation(&ledger, 4000, &observation));
        atomic_store_explicit(&pinRound, round, memory_order_release);
        do {
            int error = LSBChildStatusObserveTicket(&ledger, pinTicket, &observation);
            assert(!error || error == EAGAIN);
            if (!error) {
                assert(observation.guestPID == 4000 && observation.group == 40);
                assert(observation.nativePID == 4000 + (pid_t)observation.execGeneration * 100 && observation.execGeneration <= 1);
                assert(observation.status == (observation.exited ? (42 << 8) : 0));
            }
        } while (atomic_load_explicit(&pinFinished, memory_order_acquire) != round);
        assert(!LSBChildStatusObserveTicket(&ledger, pinTicket, &observation) && observation.exited && observation.status == (42 << 8));
        assert(!LSBChildStatusReleaseObservation(&ledger, pinTicket));
    }
    assert(!pthread_join(producer, NULL));
}

static void testPinnedExitRetention(void) {
    LSBChildStatusInitialize(&ledger);
    LSBChildTicket ticket = running(91, 15);
    LSBChildObservation first, second, observed;
    assert(!LSBChildStatusAcquireObservation(&ledger, 91, &first));
    assert(!LSBChildStatusAcquireObservation(&ledger, 91, &second));
    assert(!LSBChildStatusPublishExec(&ledger, ticket, 910));
    assert(!LSBChildStatusPublishExit(&ledger, ticket, SIGTERM));
    pid_t pid;
    int status;
    assert(!LSBChildStatusTryWait(&ledger, 91, 0, &pid, &status) && status == SIGTERM);
    assert(LSBChildStatusObserve(&ledger, 91, &observed) == ECHILD);
    assert(!LSBChildStatusObserveTicket(&ledger, first.ticket, &observed));
    assert(observed.exited && observed.status == SIGTERM && observed.execGeneration == 1 && observed.nativePID == 910);
    assert(observed.guestPID == 91 && observed.group == 15);
    LSBChildTicket reused = running(91, 16);
    assert(reused.index != ticket.index);
    assert(!LSBChildStatusReleaseObservation(&ledger, first.ticket));
    assert(!LSBChildStatusObserveTicket(&ledger, second.ticket, &observed) && observed.status == SIGTERM);
    assert(!LSBChildStatusReleaseObservation(&ledger, second.ticket));
    assert(LSBChildStatusObserveTicket(&ledger, ticket, &observed) == ESTALE);
    assert(LSBChildStatusReleaseObservation(&ledger, ticket) == ESTALE);
    LSBChildTicket recycled = running(92, 15);
    assert(recycled.index == ticket.index && recycled.generation != ticket.generation);
    assert(LSBChildStatusObserveTicket(&ledger, ticket, &observed) == ESTALE);
    assert(LSBChildStatusReleaseObservation(&ledger, ticket) == ESTALE);

    LSBChildStatusInitialize(&ledger);
    ticket = running(93, 15);
    assert(!LSBChildStatusAcquireObservation(&ledger, 93, &first));
    assert(!LSBChildStatusAbandon(&ledger, ticket));
    assert(LSBChildStatusObserveTicket(&ledger, first.ticket, &observed) == ESTALE);
    reused = running(93, 15);
    assert(reused.index != ticket.index);
    assert(!LSBChildStatusReleaseObservation(&ledger, first.ticket));
    recycled = running(94, 15);
    assert(recycled.index == ticket.index);

    for (unsigned discardBeforeExit = 0; discardBeforeExit < 2; ++discardBeforeExit) {
        LSBChildStatusInitialize(&ledger);
        ticket = running(95, 15);
        assert(!LSBChildStatusAcquireObservation(&ledger, 95, &first));
        if (discardBeforeExit) LSBChildStatusSetDiscard(&ledger, 1);
        assert(!LSBChildStatusPublishExit(&ledger, ticket, 21 << 8));
        if (!discardBeforeExit) LSBChildStatusSetDiscard(&ledger, 1);
        assert(LSBChildStatusTryWait(&ledger, 95, 0, &pid, &status) == ECHILD);
        assert(!LSBChildStatusObserveTicket(&ledger, first.ticket, &observed) && observed.exited && observed.status == (21 << 8));
        assert(!LSBChildStatusReleaseObservation(&ledger, first.ticket));
    }
}

static void testObserverBounds(void) {
    LSBChildStatusInitialize(&ledger);
    LSBChildTicket tickets[LSB_CHILD_STATUS_CAPACITY], extra;
    LSBChildObservation observation;
    pid_t pid;
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) {
        tickets[i] = running(1000 + (pid_t)i, 1);
        assert(!LSBChildStatusAcquireObservation(&ledger, 1000 + (pid_t)i, &observation));
        assert(!LSBChildStatusPublishExit(&ledger, tickets[i], 0));
        assert(!LSBChildStatusTryWait(&ledger, 1000 + (pid_t)i, 0, &pid, NULL));
    }
    assert(LSBChildStatusReserve(&ledger, &extra) == EAGAIN);
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) assert(!LSBChildStatusReleaseObservation(&ledger, tickets[i]));
    assert(!LSBChildStatusReserve(&ledger, &extra));
    assert(!LSBChildStatusAbandon(&ledger, extra));
    LSBChildTicket ticket = running(2000, 2);
    for (unsigned i = 0; i < UINT16_MAX; ++i) assert(!LSBChildStatusAcquireObservation(&ledger, 2000, &observation));
    assert(LSBChildStatusAcquireObservation(&ledger, 2000, &observation) == EOVERFLOW);
    for (unsigned i = 0; i < UINT16_MAX; ++i) assert(!LSBChildStatusReleaseObservation(&ledger, ticket));
    assert(LSBChildStatusReleaseObservation(&ledger, ticket) == ESTALE);
    atomic_store(&ledger.nextGeneration, UINT64_MAX >> 20);
    assert(!LSBChildStatusReserve(&ledger, &extra) && extra.generation == (UINT64_MAX >> 20));
    assert(LSBChildStatusReserve(&ledger, &extra) == EOVERFLOW);
}

static void testParentAndImport(void) {
    LSBChildStatusInitialize(&ledger);
    assert(LSBChildStatusGetParentPID(&ledger) == -1);
    assert(LSBChildStatusSetParentPID(&ledger, -1) == EINVAL);
    assert(!LSBChildStatusSetParentPID(&ledger, 0) && LSBChildStatusGetParentPID(&ledger) == 0);
    assert(!LSBChildStatusSetParentPID(&ledger, 43) && LSBChildStatusGetParentPID(&ledger) == 43);
    LSBChildTicket ticket = running(3000, 30);
    assert(!LSBChildStatusPublishExec(&ledger, ticket, 3100));
    LSBChildObservation observation, imported;
    assert(!LSBChildStatusObserveIndex(&ledger, ticket.index, &observation));
    LSBChildStatus destination;
    LSBChildStatusInitialize(&destination);
    LSBChildTicket newTicket;
    assert(!LSBChildStatusImport(&destination, &observation, &newTicket));
    assert(!LSBChildStatusObserve(&destination, 3000, &imported));
    assert(imported.nativePID == 3100 && imported.execGeneration == 1 && imported.group == 30 && !imported.exited);
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 33 << 8));
    assert(!LSBChildStatusObserveIndex(&ledger, ticket.index, &observation));
    LSBChildStatusInitialize(&destination);
    assert(!LSBChildStatusImport(&destination, &observation, &newTicket));
    pid_t pid;
    int status;
    assert(!LSBChildStatusTryWait(&destination, 3000, 0, &pid, &status) && pid == 3000 && status == (33 << 8));
    assert(!LSBChildStatusAbandon(&ledger, ticket));
    assert(LSBChildStatusObserveIndex(&ledger, ticket.index, &imported) == ECHILD);
    LSBChildStatusSetDiscard(&destination, 1);
    assert(!LSBChildStatusImport(&destination, &observation, &newTicket));
    assert(LSBChildStatusTryWait(&destination, 3000, 0, &pid, &status) == ECHILD);
    observation.status = -1;
    assert(LSBChildStatusImport(&destination, &observation, &newTicket) == EINVAL);
}

static void testSharedAcrossProcesses(void) {
    LSBChildStatus *shared = mmap(NULL, LSB_CHILD_STATUS_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    assert(shared != MAP_FAILED);
    LSBChildStatusInitialize(shared);
    LSBChildTicket ticket;
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) assert(!LSBChildStatusReserve(shared, &ticket));
    assert(ticket.index == LSB_CHILD_STATUS_CAPACITY - 1);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        assert(!LSBChildStatusPublishRunning(shared, ticket, getpid(), getpgrp()));
        assert(!LSBChildStatusPublishExit(shared, ticket, 23 << 8));
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    pid_t reported;
    assert(!LSBChildStatusTryWait(shared, child, 0, &reported, &status) && reported == child && WEXITSTATUS(status) == 23);
    assert(!munmap(shared, LSB_CHILD_STATUS_MAP_SIZE));
}

int main(void) {
    alarm(15);
    testSelectionAndReuse();
    testCapacityAndDiscard();
    testLedgerABI();
    testExecutionObservation();
    testStagedExecution();
    testConcurrentExecutionObservation();
    testConcurrentReaping();
    testConcurrentReuse();
    testConcurrentDiscard();
    testConcurrentPinnedReuse();
    testPinnedExitRetention();
    testObserverBounds();
    testParentAndImport();
    testSharedAcrossProcesses();
    puts("ChildStatus: 120-child capacity, ABI rejection, exec observations, pinned/reaped EXIT, adoption, discard races, concurrent reuse and shared mapping passed");
    return 0;
}
