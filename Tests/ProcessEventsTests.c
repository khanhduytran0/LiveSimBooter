#include "../Guest/ProcessEvents.h"
#include "../Guest/ChildWait.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int observedPID, observedFlavor, observedSize, procResult = 192, procError;
static uint64_t observedArgument;
pid_t LSBGuestPID(void) { return 40000; }
pid_t LSBNativePID(void) { return 41000; }
static int testProcPIDInfo(int pid, int flavor, uint64_t arg, void *buffer, int size) {
    observedPID = pid;
    observedFlavor = flavor;
    observedArgument = arg;
    observedSize = size;
    if (buffer && size > 0) memset(buffer, 0xa5, (size_t)size);
    errno = procError;
    return procResult;
}

static int inspectionGuardPID, inspectionGuardError;
int LSBInspectionUnsupportedPID(int pid) { return pid == inspectionGuardPID ? inspectionGuardError : 0; }
bool LSBPeerProcessCallerEnabled(uintptr_t caller) { (void)caller; return false; }
int LSBPeerProcessPIDInfo(uintptr_t caller, int pid, int flavor, uint64_t arg, void *buffer, int size, int result, int originalErrno) {
    (void)caller; (void)pid; (void)flavor; (void)arg; (void)buffer; (void)size; (void)originalErrno; return result;
}
#define LSB_GUEST_RUNTIME
#define LSB_PROCESS_EVENTS_TESTING
#define proc_pidinfo testProcPIDInfo
#include "../Guest/ProcessEvents.c"
#undef proc_pidinfo

static LSBChildStatus ledger;
static int wakeFDs[2];
static dispatch_queue_t queue;
static const unsigned long processMask = DISPATCH_PROC_EXEC | DISPATCH_PROC_EXIT | DISPATCH_PROC_FORK;

typedef struct {
    dispatch_source_t source;
    dispatch_semaphore_t registration, events, cancellation;
    _Atomic(unsigned long) data;
    _Atomic(unsigned) eventCount;
    pid_t pid;
} Callbacks;

static void registered(void *pointer) {
    Callbacks *context = pointer;
    assert(LSBDispatchSourceGetHandle(context->source) == (uintptr_t)context->pid);
    assert(LSBDispatchSourceGetMask(context->source) == processMask);
    dispatch_semaphore_signal(context->registration);
}

static void event(void *pointer) {
    Callbacks *context = pointer;
    assert(LSBDispatchSourceGetHandle(context->source) == (uintptr_t)context->pid);
    assert(LSBDispatchSourceGetMask(context->source) == processMask);
    atomic_fetch_or(&context->data, dispatch_source_get_data(context->source));
    atomic_fetch_add(&context->eventCount, 1);
    dispatch_semaphore_signal(context->events);
}

static void cancelled(void *pointer) {
    Callbacks *context = pointer;
    assert(LSBDispatchSourceGetHandle(context->source) == (uintptr_t)context->pid);
    dispatch_semaphore_signal(context->cancellation);
}

static void waitFor(dispatch_semaphore_t semaphore) {
    assert(!dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)));
}

static void waitForData(Callbacks *context, unsigned long required) {
    while ((atomic_load(&context->data) & required) != required) waitFor(context->events);
}

static void waitForTimer(dispatch_source_t source) {
    for (unsigned attempt = 0; attempt < 2000; ++attempt) {
        pthread_mutex_lock(&sourceLock);
        ManagedSource *entry = findSource(source);
        bool finished = entry && entry->timerFinished;
        pthread_mutex_unlock(&sourceLock);
        if (finished) return;
        usleep(1000);
    }
    assert(!"polling timer did not finish");
}

static void waitForNoSources(void) {
    for (unsigned attempt = 0; attempt < 2000; ++attempt) {
        pthread_mutex_lock(&sourceLock);
        bool empty = !managedSources;
        pthread_mutex_unlock(&sourceLock);
        if (empty) return;
        usleep(1000);
    }
    assert(!"source callbacks did not retire");
}

static LSBChildTicket running(pid_t pid) {
    LSBChildTicket ticket;
    assert(!LSBChildStatusReserve(&ledger, &ticket));
    assert(!LSBChildStatusPublishRunning(&ledger, ticket, pid, 33));
    return ticket;
}

static void configureSource(Callbacks *context, pid_t pid) {
    *context = (Callbacks){.pid = pid, .registration = dispatch_semaphore_create(0), .events = dispatch_semaphore_create(0),
                           .cancellation = dispatch_semaphore_create(0)};
    context->source = LSBDispatchSourceCreate(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)pid, processMask, queue);
    assert(context->source);
    dispatch_set_context(context->source, context);
    dispatch_source_set_registration_handler_f(context->source, registered);
    dispatch_source_set_event_handler_f(context->source, event);
    LSBDispatchSourceSetCancelHandlerF(context->source, cancelled);
}

static void releaseContext(Callbacks *context) {
    waitForTimer(context->source);
    assert(LSBDispatchSourceGetHandle(context->source) == (uintptr_t)context->pid);
    LSBDispatchRelease(context->source);
    dispatch_release(context->registration);
    dispatch_release(context->events);
    dispatch_release(context->cancellation);
}

static void barrier(void *unused) { (void)unused; }

static void testRegistrationSuspensionAndExit(void) {
    LSBChildStatusInitialize(&ledger);
    LSBChildTicket ticket = running(90000);
    Callbacks context;
    configureSource(&context, 90000);
    LSBDispatchRetain(context.source);
    LSBDispatchRelease(context.source);
    assert(!LSBChildStatusPublishExec(&ledger, ticket, 90001));
    assert(dispatch_semaphore_wait(context.registration, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_MSEC)));
    assert(!atomic_load(&context.data));
    dispatch_activate(context.source);
    waitFor(context.registration);
    waitForData(&context, DISPATCH_PROC_EXEC);
    assert(atomic_load(&context.data) == DISPATCH_PROC_EXEC);
    dispatch_suspend(context.source);
    dispatch_sync_f(queue, NULL, barrier);
    atomic_store(&context.data, 0);
    unsigned eventCount = atomic_load(&context.eventCount);
    LSBChildExecUpdate update;
    assert(!LSBChildStatusPrepareExec(&ledger, ticket, 90002, &update));
    usleep(30000);
    assert(!LSBChildStatusCancelExec(&ledger, &update));
    assert(!LSBChildStatusPublishExec(&ledger, ticket, 90003));
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 37 << 8));
    waitForTimer(context.source);
    assert(!atomic_load(&context.data) && atomic_load(&context.eventCount) == eventCount);
    dispatch_resume(context.source);
    waitForData(&context, DISPATCH_PROC_EXEC | DISPATCH_PROC_EXIT);
    assert(atomic_load(&context.data) == (DISPATCH_PROC_EXEC | DISPATCH_PROC_EXIT));
    int status;
    assert(LSBWait4(90000, &status, 0, NULL) == 90000 && WIFEXITED(status) && WEXITSTATUS(status) == 37);
    assert(LSBWait4(90000, &status, WNOHANG, NULL) == -1 && errno == ECHILD);
    LSBDispatchSourceCancel(context.source);
    waitFor(context.cancellation);
    releaseContext(&context);
}

static void testLateRegistrationAndCancellation(void) {
    LSBChildStatusInitialize(&ledger);
    LSBChildTicket ticket = running(90100);
    assert(!LSBChildStatusPublishExec(&ledger, ticket, 90101));
    Callbacks context;
    configureSource(&context, 90100);
    dispatch_resume(context.source);
    waitFor(context.registration);
    waitForData(&context, DISPATCH_PROC_EXEC);
    dispatch_suspend(context.source);
    dispatch_sync_f(queue, NULL, barrier);
    LSBDispatchSourceCancel(context.source);
    waitForTimer(context.source);
    assert(dispatch_source_testcancel(context.source));
    assert(dispatch_semaphore_wait(context.cancellation, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_MSEC)));
    dispatch_resume(context.source);
    waitFor(context.cancellation);
    releaseContext(&context);

    configureSource(&context, 90100);
    LSBDispatchSourceCancel(context.source);
    dispatch_activate(context.source);
    waitFor(context.cancellation);
    assert(!atomic_load(&context.data));
    releaseContext(&context);
}

static void testReusedSlotIsNotFollowed(void) {
    LSBChildStatusInitialize(&ledger);
    LSBChildTicket ticket = running(90200);
    Callbacks context;
    configureSource(&context, 90200);
    dispatch_activate(context.source);
    waitFor(context.registration);
    // A live child can disappear only through another owner abandoning it. Reuse with the same PID must not migrate this source.
    assert(!LSBChildStatusAbandon(&ledger, ticket));
    LSBChildTicket reused = running(90200);
    assert(reused.generation != ticket.generation);
    // The old slot stays pinned until its polling cancellation finishes; reuse may occur on another slot meanwhile.
    assert(!LSBChildStatusPublishExec(&ledger, reused, 90201));
    assert(!LSBChildStatusPublishExit(&ledger, reused, SIGTERM));
    waitFor(context.cancellation);
    assert(!atomic_load(&context.data));
    releaseContext(&context);
}

static void testExitSurvivesReapingAndDiscard(void) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        waitForNoSources();
        LSBChildStatusInitialize(&ledger);
        pid_t guestPID = 90600 + (pid_t)mode;
        LSBChildTicket ticket = running(guestPID);
        Callbacks contexts[2];
        dispatch_suspend(queue);
        for (unsigned i = 0; i < 2; ++i) {
            configureSource(&contexts[i], guestPID);
            dispatch_activate(contexts[i].source);
        }
        if (mode == 1) LSBChildStatusSetDiscard(&ledger, 1);
        assert(!LSBChildStatusPublishExit(&ledger, ticket, 24 << 8));
        if (mode == 2) LSBChildStatusSetDiscard(&ledger, 1);
        int status;
        if (mode == 0) assert(LSBWait4(guestPID, &status, 0, NULL) == guestPID && WEXITSTATUS(status) == 24);
        else assert(LSBWait4(guestPID, &status, WNOHANG, NULL) == -1 && errno == ECHILD);
        // A later process with the same logical PID must not contaminate either old source's queued events.
        LSBChildTicket replacement = running(guestPID);
        assert(replacement.generation != ticket.generation);
        assert(!LSBChildStatusPublishExec(&ledger, replacement, guestPID + 100));
        assert(!LSBChildStatusPublishExit(&ledger, replacement, SIGTERM));
        for (unsigned i = 0; i < 2; ++i) {
            waitForTimer(contexts[i].source);
            assert(!atomic_load(&contexts[i].data));
        }
        dispatch_resume(queue);
        for (unsigned i = 0; i < 2; ++i) {
            waitFor(contexts[i].registration);
            waitForData(&contexts[i], DISPATCH_PROC_EXIT);
            assert(atomic_load(&contexts[i].data) == DISPATCH_PROC_EXIT);
            LSBDispatchSourceCancel(contexts[i].source);
            waitFor(contexts[i].cancellation);
            releaseContext(&contexts[i]);
        }
    }
    waitForNoSources();
}

static void *retainRelease(void *pointer) {
    dispatch_source_t source = pointer;
    for (unsigned i = 0; i < 1000; ++i) { LSBDispatchRetain(source); LSBDispatchRelease(source); }
    return NULL;
}

static void testConcurrentLifetimeAndPassthrough(void) {
    LSBChildStatusInitialize(&ledger);
    (void)running(90300);
    for (unsigned round = 0; round < 16; ++round) {
        dispatch_source_t source = LSBDispatchSourceCreate(DISPATCH_SOURCE_TYPE_PROC, 90300, processMask, queue);
        assert(source);
        dispatch_activate(source);
        pthread_t threads[4];
        for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, retainRelease, source));
        LSBDispatchSourceCancel(source);
        for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
        waitForTimer(source);
        assert(LSBDispatchSourceGetHandle(source) == 90300);
        LSBDispatchRelease(source);
    }
    for (unsigned i = 0; i < 128; ++i) {
        dispatch_source_t source = LSBDispatchSourceCreate(DISPATCH_SOURCE_TYPE_PROC, 90300, processMask, queue);
        assert(source);
        dispatch_activate(source);
        LSBDispatchRelease(source); // Final release races an active poll/cancel handler without a caller cancel.
    }
    waitForNoSources();
    dispatch_source_t native = LSBDispatchSourceCreate(DISPATCH_SOURCE_TYPE_DATA_ADD, 0, 0, queue);
    assert(native && !LSBDispatchSourceGetHandle(native) && !LSBDispatchSourceGetMask(native));
    LSBDispatchRetain(native);
    LSBDispatchRelease(native);
    LSBDispatchSourceCancel(native);
    dispatch_activate(native);
    LSBDispatchRelease(native);
    errno = 0;
    assert(!LSBDispatchSourceCreate(DISPATCH_SOURCE_TYPE_PROC, 90300, DISPATCH_PROC_SIGNAL, queue) && errno == ENOTSUP);
}

typedef struct {
    dispatch_source_t source;
    dispatch_semaphore_t entered, proceed, completed;
    pid_t pid;
    _Atomic(unsigned) cancellations;
} ReleaseCallbacks;

static void releaseEvent(void *pointer) {
    ReleaseCallbacks *context = pointer;
    dispatch_semaphore_signal(context->entered);
    waitFor(context->proceed);
    // The caller released its last external reference while this native event callback was running.
    assert(LSBDispatchSourceGetHandle(context->source) == (uintptr_t)context->pid);
    assert(LSBDispatchSourceGetMask(context->source) == processMask);
    dispatch_semaphore_signal(context->completed);
}

static void releaseCancellation(void *pointer) {
    ReleaseCallbacks *context = pointer;
    assert(LSBDispatchSourceGetHandle(context->source) == (uintptr_t)context->pid);
    assert(LSBDispatchSourceGetMask(context->source) == processMask);
    atomic_fetch_add(&context->cancellations, 1);
    dispatch_semaphore_signal(context->completed);
}

static void testCallbacksAfterFinalRelease(void) {
    waitForNoSources();
    LSBChildStatusInitialize(&ledger);
    LSBChildTicket ticket = running(90500);
    ReleaseCallbacks context = {.pid = 90500, .entered = dispatch_semaphore_create(0), .proceed = dispatch_semaphore_create(0),
                                .completed = dispatch_semaphore_create(0)};
    context.source = LSBDispatchSourceCreate(DISPATCH_SOURCE_TYPE_PROC, context.pid, processMask, queue);
    assert(context.source);
    dispatch_set_context(context.source, &context);
    dispatch_source_set_event_handler_f(context.source, releaseEvent);
    LSBDispatchSourceSetCancelHandlerF(context.source, releaseCancellation);
    dispatch_activate(context.source);
    assert(!LSBChildStatusPublishExec(&ledger, ticket, 90501));
    waitFor(context.entered);
    LSBDispatchRelease(context.source);
    dispatch_semaphore_signal(context.proceed);
    waitFor(context.completed);
    waitForNoSources();
    assert(!atomic_load(&context.cancellations)); // Native final release does not invoke a user cancellation handler.

    // A canceled source's handler can remain queued long after both caller and polling timer have released their references.
    for (unsigned useBlock = 0; useBlock < 2; ++useBlock) {
        dispatch_suspend(queue);
        context.source = LSBDispatchSourceCreate(DISPATCH_SOURCE_TYPE_PROC, context.pid, processMask, queue);
        assert(context.source);
        ReleaseCallbacks *callbackContext = &context;
        if (useBlock) LSBDispatchSourceSetCancelHandler(context.source, ^{ releaseCancellation(callbackContext); });
        else {
            dispatch_set_context(context.source, NULL);
            LSBDispatchSourceSetCancelHandlerF(context.source, releaseCancellation);
            dispatch_set_context(context.source, &context); // Function handlers use the context current when cancellation runs.
        }
        dispatch_activate(context.source);
        LSBDispatchSourceCancel(context.source);
        LSBDispatchRelease(context.source);
        usleep(30000);
        pthread_mutex_lock(&sourceLock);
        ManagedSource *entry = findSource(context.source);
        assert(entry && entry->externalReferences == 0 && !entry->sourceFinished);
        pthread_mutex_unlock(&sourceLock);
        dispatch_resume(queue);
        waitFor(context.completed);
        waitForNoSources();
        assert(atomic_load(&context.cancellations) == useBlock + 1);
    }
    dispatch_release(context.entered);
    dispatch_release(context.proceed);
    dispatch_release(context.completed);
}

static void signalCancellation(void *context) { dispatch_semaphore_signal(context); }

static void testMandatoryCancellationLifetime(void) {
    waitForNoSources();
    LSBChildStatusInitialize(&ledger);
    LSBChildTicket ticket = running(90700);
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 0));
    ReleaseCallbacks context = {.pid = 90700, .completed = dispatch_semaphore_create(0)};
    ReleaseCallbacks *callbackContext = &context;
    for (unsigned round = 0; round < 64; ++round) {
        dispatch_suspend(queue);
        context.source = LSBDispatchSourceCreate(DISPATCH_SOURCE_TYPE_PROC, context.pid, processMask, queue);
        assert(context.source);
        if (round & 1) {
            LSBDispatchSourceSetMandatoryCancelHandler(context.source, ^{ releaseCancellation(callbackContext); });
        } else {
            dispatch_set_context(context.source, NULL);
            LSBDispatchSourceSetMandatoryCancelHandlerF(context.source, releaseCancellation);
            dispatch_set_context(context.source, &context);
        }
        dispatch_activate(context.source);
        LSBDispatchSourceCancel(context.source);
        waitForTimer(context.source);
        LSBDispatchRelease(context.source);
        pthread_mutex_lock(&sourceLock);
        ManagedSource *entry = findSource(context.source);
        assert(entry && entry->timerFinished && !entry->sourceFinished && entry->externalReferences == 0);
        pthread_mutex_unlock(&sourceLock);
        dispatch_resume(queue);
        waitFor(context.completed);
        waitForNoSources(); // The device failure left this entry alive after native source disposal and allocator reuse.
        assert(atomic_load(&context.cancellations) == round + 1);
    }
    dispatch_release(context.completed);

    // Unmanaged sources retain native mandatory cancellation behavior and callback context.
    for (unsigned useBlock = 0; useBlock < 2; ++useBlock) {
        dispatch_source_t native = dispatch_source_create(DISPATCH_SOURCE_TYPE_DATA_OR, 0, 0, queue);
        dispatch_semaphore_t completed = dispatch_semaphore_create(0);
        dispatch_set_context(native, completed);
        if (useBlock) LSBDispatchSourceSetMandatoryCancelHandler(native, ^{ dispatch_semaphore_signal(completed); });
        else LSBDispatchSourceSetMandatoryCancelHandlerF(native, signalCancellation);
        dispatch_activate(native);
        LSBDispatchSourceCancel(native);
        waitFor(completed);
        LSBDispatchRelease(native);
        dispatch_release(completed);
    }
}

static void testSelfProcPIDTranslation(void) {
    unsigned char bytes[192];
    procResult = 56;
    procError = EDOM;
    assert(LSBProcPIDInfo(40000, 17, 1, bytes, 56) == 56 && errno == EDOM);
    assert(observedPID == 41000 && observedFlavor == 17 && observedArgument == 1 && observedSize == 56);
    for (unsigned i = 0; i < 56; ++i) assert(bytes[i] == 0xa5); // Preserve unique PID, parent unique PID, pidversion, and UUID.
    procResult = 192;
    assert(LSBProcPIDInfo(40000, 18, 0, bytes, sizeof(bytes)) == 192 && observedPID == 41000);
    for (unsigned i = 0; i < sizeof(bytes); ++i) assert(bytes[i] == 0xa5);
    procResult = 0;
    procError = ESRCH;
    assert(!LSBProcPIDInfo(40000, 17, 1, bytes, 56) && errno == ESRCH && observedPID == 41000);
    procResult = 192;
    procError = 0;
}

static void testProcPIDTranslation(void) {
    // No new sources are created here; allow canceled timer callbacks from the lifetime test to finish before resetting the ledger.
    usleep(50000);
    LSBChildStatusInitialize(&ledger);
    LSBChildTicket ticket = running(90400);
    assert(!LSBChildStatusPublishExec(&ledger, ticket, 90401));
    unsigned char bytes[192], expected[192];
    memset(expected, 0xa5, sizeof(expected));
    uint32_t child = 90400, parent = 40000;
    memcpy(expected + 12, &child, sizeof(child));
    memcpy(expected + 16, &parent, sizeof(parent));
    procError = EDOM;
    assert(LSBProcPIDInfo(90400, 18, 1, bytes, sizeof(bytes)) == 192 && errno == EDOM);
    assert(observedPID == 90401 && observedFlavor == 18 && observedArgument == 1 && observedSize == sizeof(bytes));
    assert(!memcmp(bytes, expected, sizeof(bytes)));
    assert(LSBProcPIDInfo(90400, 99, 2, bytes, sizeof(bytes)) == 192 && observedPID == 90401);
    for (unsigned i = 0; i < sizeof(bytes); ++i) assert(bytes[i] == 0xa5);
    assert(LSBProcPIDInfo(90499, 18, 3, bytes, sizeof(bytes)) == 192 && observedPID == 90499);
    observedPID = 0; inspectionGuardPID = 90499; inspectionGuardError = ESRCH;
    assert(!LSBProcPIDInfo(90499, 18, 3, bytes, sizeof(bytes)) && errno == ESRCH && !observedPID);
    inspectionGuardPID = 90400; inspectionGuardError = ENOTSUP;
    assert(!LSBProcPIDInfo(90400, 18, 3, bytes, sizeof(bytes)) && errno == ENOTSUP && !observedPID);
    inspectionGuardPID = inspectionGuardError = 0;
    for (unsigned i = 0; i < sizeof(bytes); ++i) assert(bytes[i] == 0xa5);
    LSBChildExecUpdate update;
    assert(!LSBChildStatusPrepareExec(&ledger, ticket, 90402, &update));
    observedPID = 0;
    assert(!LSBProcPIDInfo(90400, 18, 1, bytes, sizeof(bytes)) && errno == EAGAIN && !observedPID);
    assert(!LSBChildStatusCancelExec(&ledger, &update));
    procResult = 0;
    procError = ESRCH;
    assert(!LSBProcPIDInfo(90400, 18, 1, bytes, sizeof(bytes)) && errno == ESRCH && observedPID == 90401);
    for (unsigned i = 0; i < sizeof(bytes); ++i) assert(bytes[i] == 0xa5);
}

int main(void) {
    alarm(20);
    assert(!pipe(wakeFDs) && !fcntl(wakeFDs[0], F_SETFL, O_NONBLOCK));
    LSBChildStatusInitialize(&ledger);
    assert(!LSBChildWaitConfigure(&ledger, wakeFDs[0]));
    queue = dispatch_queue_create("LSBProcessEventsTests", DISPATCH_QUEUE_SERIAL);
    testRegistrationSuspensionAndExit();
    testLateRegistrationAndCancellation();
    testReusedSlotIsNotFollowed();
    testExitSurvivesReapingAndDiscard();
    testConcurrentLifetimeAndPassthrough();
    testCallbacksAfterFinalRelease();
    testMandatoryCancellationLifetime();
    testSelfProcPIDTranslation();
    testProcPIDTranslation();
    dispatch_release(queue);
    close(wakeFDs[0]);
    close(wakeFDs[1]);
    puts("ProcessEvents: native source registration/suspension, staged EXEC/EXIT, reaped/discarded EXIT retention, multiple observers, PID reuse, cancellation, concurrent lifetime and proc translation passed");
    return 0;
}
