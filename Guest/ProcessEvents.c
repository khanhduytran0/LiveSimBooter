#include "ProcessEvents.h"
#include "ChildWait.h"
#include "ProcessIdentity.h"
#ifdef LSB_GUEST_RUNTIME
#include "PeerProcessCompatibility.h"
#include "ProcessInspection.h"
#endif

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Private libproc SPI used by launchd's EXEC handler. The BSD prefix is the public proc_bsdinfo layout.
extern int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int bufferSize);
// launchd uses these libdispatch SPIs instead of the public cancellation setters.
extern void dispatch_source_set_mandatory_cancel_handler(dispatch_source_t source, dispatch_block_t handler);
extern void dispatch_source_set_mandatory_cancel_handler_f(dispatch_source_t source, dispatch_function_t handler);
enum { ProcBSDBasicUniqueID = 18, ProcBSDBasicUniqueIDSize = 192, ProcBSDPIDOffset = 12, ProcBSDParentPIDOffset = 16 };

typedef struct ManagedSource {
    struct ManagedSource *next;
    dispatch_source_t source, timer;
    LSBChildStatus *ledger;
    LSBChildTicket ticket;
    pid_t guestPID;
    unsigned long mask;
    uint32_t seenExecGeneration;
    size_t externalReferences;
    bool timerFinished, sourceFinished, cancellationRequested;
} ManagedSource;

static pthread_mutex_t sourceLock = PTHREAD_MUTEX_INITIALIZER;
static ManagedSource *managedSources;

static ManagedSource *findSource(dispatch_source_t source) {
    for (ManagedSource *entry = managedSources; entry; entry = entry->next) if (entry->source == source) return entry;
    return NULL;
}

#ifdef LSB_PROCESS_EVENTS_TEST_INSPECTION
size_t LSBProcessEventsTestSourceCount(void) {
    pthread_mutex_lock(&sourceLock);
    size_t count = 0;
    for (ManagedSource *entry = managedSources; entry; entry = entry->next) ++count;
    pthread_mutex_unlock(&sourceLock);
    return count;
}
#endif

static bool detachFinishedSource(ManagedSource *entry) {
    if (entry->externalReferences || !entry->timerFinished || !entry->sourceFinished) return false;
    ManagedSource **link = &managedSources;
    while (*link && *link != entry) link = &(*link)->next;
    if (*link) *link = entry->next;
    return true;
}

static void finishSource(ManagedSource *entry, dispatch_block_t block, dispatch_function_t function) {
    pthread_mutex_lock(&sourceLock);
    bool requested = entry->cancellationRequested;
    pthread_mutex_unlock(&sourceLock);
    // This native cancellation barrier runs after event callbacks. An implicit final release must not invent a user cancellation callback.
    if (requested) {
        if (block) block();
        else if (function) function(dispatch_get_context(entry->source));
    }
    pthread_mutex_lock(&sourceLock);
    entry->sourceFinished = true;
    bool destroy = detachFinishedSource(entry);
    pthread_mutex_unlock(&sourceLock);
    if (destroy) free(entry);
}

static void finishTimer(void *context) {
    ManagedSource *entry = context;
    pthread_mutex_lock(&sourceLock);
    dispatch_source_t timer = entry->timer, source = entry->source;
    entry->timer = NULL;
    // Final status remains pinned through the last poll, even if a waiter or SA_NOCLDWAIT consumed it earlier.
    (void)LSBChildStatusReleaseObservation(entry->ledger, entry->ticket);
    entry->timerFinished = true;
    bool destroy = detachFinishedSource(entry);
    pthread_mutex_unlock(&sourceLock);
    // The running cancel handler itself keeps its dispatch object alive. Never touch entry after dropping these refs:
    // a caller's final release may free a finished entry immediately after the lock was released.
    dispatch_release(timer);
    dispatch_release(source);
    if (destroy) free(entry);
}

static void pollProcess(void *context) {
    ManagedSource *entry = context;
    if (dispatch_source_testcancel(entry->source)) { dispatch_source_cancel(entry->timer); return; }
    LSBChildObservation observation;
    int error = LSBChildStatusObserveTicket(entry->ledger, entry->ticket, &observation);
    if (error == EAGAIN) return;
    if (error) {
        // Abandoning a live child is not an exit. A pinned ticket cannot migrate to a different process.
        LSBDispatchSourceCancel(entry->source);
        return;
    }
    unsigned long data = 0;
    if (observation.execGeneration > entry->seenExecGeneration) {
        data |= DISPATCH_PROC_EXEC;
        entry->seenExecGeneration = observation.execGeneration;
    }
    if (observation.exited) data |= DISPATCH_PROC_EXIT;
    data &= entry->mask;
    if (data) dispatch_source_merge_data(entry->source, data);
    if (observation.exited) dispatch_source_cancel(entry->timer);
}

dispatch_source_t LSBDispatchSourceCreate(dispatch_source_type_t type, uintptr_t handle, unsigned long mask, dispatch_queue_t queue) {
    LSBChildStatus *ledger = LSBGuestChildStatus();
    if (type != DISPATCH_SOURCE_TYPE_PROC || !ledger || !handle || handle > INT_MAX)
        return dispatch_source_create(type, handle, mask, queue);
    LSBChildObservation observation;
    int error = LSBChildStatusAcquireObservation(ledger, (pid_t)handle, &observation);
    if (error == ECHILD) return dispatch_source_create(type, handle, mask, queue);
    if (error) { errno = error; return NULL; }
    // Fork is not implemented, but accepting its mask preserves launchd's normal EXIT|FORK|EXEC source setup.
    if (mask & ~(DISPATCH_PROC_EXIT | DISPATCH_PROC_EXEC | DISPATCH_PROC_FORK)) {
        (void)LSBChildStatusReleaseObservation(ledger, observation.ticket);
        errno = ENOTSUP;
        return NULL;
    }
    ManagedSource *entry = calloc(1, sizeof(*entry));
    if (!entry) { (void)LSBChildStatusReleaseObservation(ledger, observation.ticket); return NULL; }
    dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_DATA_OR, 0, 0, queue);
    if (!source) { (void)LSBChildStatusReleaseObservation(ledger, observation.ticket); free(entry); return NULL; }
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0));
    if (!timer) {
        (void)LSBChildStatusReleaseObservation(ledger, observation.ticket);
        dispatch_source_cancel(source);
        dispatch_resume(source);
        dispatch_release(source);
        free(entry);
        return NULL;
    }
    *entry = (ManagedSource){.source = source, .timer = timer, .ledger = ledger, .ticket = observation.ticket,
                            .guestPID = (pid_t)handle, .mask = mask, .externalReferences = 1};
    dispatch_retain(source); // A polling/cancel callback cannot outlive this reference.
    dispatch_source_set_cancel_handler(source, ^{ finishSource(entry, NULL, NULL); });
    dispatch_set_context(timer, entry);
    dispatch_source_set_event_handler_f(timer, pollProcess);
    dispatch_source_set_cancel_handler_f(timer, finishTimer);
    dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC, NSEC_PER_MSEC);
    pthread_mutex_lock(&sourceLock);
    entry->next = managedSources;
    managedSources = entry;
    pthread_mutex_unlock(&sourceLock);
    dispatch_resume(timer);
    return source;
}

uintptr_t LSBDispatchSourceGetHandle(dispatch_source_t source) {
    pthread_mutex_lock(&sourceLock);
    ManagedSource *entry = findSource(source);
    uintptr_t handle = entry ? (uintptr_t)entry->guestPID : 0;
    pthread_mutex_unlock(&sourceLock);
    return handle ? handle : dispatch_source_get_handle(source);
}

unsigned long LSBDispatchSourceGetMask(dispatch_source_t source) {
    pthread_mutex_lock(&sourceLock);
    ManagedSource *entry = findSource(source);
    bool managed = entry != NULL;
    unsigned long mask = managed ? entry->mask : 0;
    pthread_mutex_unlock(&sourceLock);
    return managed ? mask : dispatch_source_get_mask(source);
}

void LSBDispatchSourceCancel(dispatch_source_t source) {
    pthread_mutex_lock(&sourceLock);
    ManagedSource *entry = findSource(source);
    if (entry) entry->cancellationRequested = true;
    dispatch_source_t timer = entry ? entry->timer : NULL;
    if (timer) dispatch_retain(timer);
    pthread_mutex_unlock(&sourceLock);
    dispatch_source_cancel(source);
    if (timer) { dispatch_source_cancel(timer); dispatch_release(timer); }
}

void LSBDispatchSourceSetCancelHandler(dispatch_source_t source, dispatch_block_t handler) {
    pthread_mutex_lock(&sourceLock);
    ManagedSource *entry = findSource(source);
    pthread_mutex_unlock(&sourceLock);
    if (entry) dispatch_source_set_cancel_handler(source, ^{ finishSource(entry, handler, NULL); });
    else dispatch_source_set_cancel_handler(source, handler);
}

void LSBDispatchSourceSetCancelHandlerF(dispatch_source_t source, dispatch_function_t handler) {
    pthread_mutex_lock(&sourceLock);
    ManagedSource *entry = findSource(source);
    pthread_mutex_unlock(&sourceLock);
    if (entry) dispatch_source_set_cancel_handler(source, ^{ finishSource(entry, NULL, handler); });
    else dispatch_source_set_cancel_handler_f(source, handler);
}

void LSBDispatchSourceSetMandatoryCancelHandler(dispatch_source_t source, dispatch_block_t handler) {
    pthread_mutex_lock(&sourceLock);
    ManagedSource *entry = findSource(source);
    pthread_mutex_unlock(&sourceLock);
    // The native mandatory setter replaces the same cancellation slot as the public setter. Keep our lifetime barrier
    // around it too, or launchd can free a source while its stale metadata later matches an unrelated reused allocation.
    if (entry) dispatch_source_set_mandatory_cancel_handler(source, ^{ finishSource(entry, handler, NULL); });
    else dispatch_source_set_mandatory_cancel_handler(source, handler);
}

void LSBDispatchSourceSetMandatoryCancelHandlerF(dispatch_source_t source, dispatch_function_t handler) {
    pthread_mutex_lock(&sourceLock);
    ManagedSource *entry = findSource(source);
    pthread_mutex_unlock(&sourceLock);
    if (entry) dispatch_source_set_mandatory_cancel_handler(source, ^{ finishSource(entry, NULL, handler); });
    else dispatch_source_set_mandatory_cancel_handler_f(source, handler);
}

void LSBDispatchRetain(dispatch_object_t object) {
    dispatch_retain(object);
    pthread_mutex_lock(&sourceLock);
    ManagedSource *entry = findSource(object._ds);
    if (entry) ++entry->externalReferences;
    pthread_mutex_unlock(&sourceLock);
}

void LSBDispatchRelease(dispatch_object_t object) {
    dispatch_source_t timer = NULL;
    ManagedSource *destroy = NULL;
    bool cancel = false;
    pthread_mutex_lock(&sourceLock);
    ManagedSource **link = &managedSources;
    while (*link && (*link)->source != object._ds) link = &(*link)->next;
    if (*link && --(*link)->externalReferences == 0) {
        ManagedSource *entry = *link;
        timer = entry->timer;
        if (timer) dispatch_retain(timer);
        cancel = !entry->sourceFinished;
        if (detachFinishedSource(entry)) destroy = entry;
    }
    pthread_mutex_unlock(&sourceLock);
    // Keep the metadata until the native source's callback barrier, even when the caller drops its last reference inside an event.
    if (cancel) dispatch_source_cancel(object._ds);
    dispatch_release(object);
    if (timer) { dispatch_source_cancel(timer); dispatch_release(timer); }
    free(destroy);
}

static int processPIDInfo(int pid, int flavor, uint64_t arg, void *buffer, int bufferSize) {
    // os_log/firehose queries its unique kernel identity with getpid(). A SETEXEC guest still sees the retired
    // logical PID, so translate self before consulting the child ledger. Leave the kernel's identity payload intact.
    if (pid > 0 && pid == LSBGuestPID()) return proc_pidinfo(LSBNativePID(), flavor, arg, buffer, bufferSize);
    LSBChildStatus *ledger = LSBGuestChildStatus();
    if (!ledger || pid <= 0) return proc_pidinfo(pid, flavor, arg, buffer, bufferSize);
    LSBChildObservation observation;
    int error = LSBChildStatusObserve(ledger, pid, &observation);
    if (error == ECHILD) return proc_pidinfo(pid, flavor, arg, buffer, bufferSize);
    if (error) { errno = error; return 0; }
    int result = proc_pidinfo(observation.nativePID, flavor, arg, buffer, bufferSize);
    if (flavor == ProcBSDBasicUniqueID && buffer && bufferSize >= ProcBSDBasicUniqueIDSize && result >= ProcBSDBasicUniqueIDSize) {
        int savedErrno = errno;
        uint32_t logicalPID = (uint32_t)pid, logicalParent = (uint32_t)LSBGuestPID();
        memcpy((char *)buffer + ProcBSDPIDOffset, &logicalPID, sizeof(logicalPID));
        memcpy((char *)buffer + ProcBSDParentPIDOffset, &logicalParent, sizeof(logicalParent));
        errno = savedErrno;
    }
    return result;
}

int LSBProcPIDInfo(int pid, int flavor, uint64_t arg, void *buffer, int bufferSize) {
#ifdef LSB_GUEST_RUNTIME
    uintptr_t caller = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
    int inspectionError = LSBInspectionUnsupportedPID(pid);
    if (inspectionError) { errno = inspectionError; return 0; }
#endif
    int result = processPIDInfo(pid, flavor, arg, buffer, bufferSize);
#ifdef LSB_GUEST_RUNTIME
    if (!LSBPeerProcessCallerEnabled(caller)) return result;
    return LSBPeerProcessPIDInfo(caller, pid, flavor, arg, buffer, bufferSize, result, errno);
#else
    return result;
#endif
}

#ifndef LSB_PROCESS_EVENTS_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBDispatchSourceCreate, (const void *)&dispatch_source_create},
    {(const void *)&LSBDispatchSourceGetHandle, (const void *)&dispatch_source_get_handle},
    {(const void *)&LSBDispatchSourceGetMask, (const void *)&dispatch_source_get_mask},
    {(const void *)&LSBDispatchSourceCancel, (const void *)&dispatch_source_cancel},
    {(const void *)&LSBDispatchSourceSetCancelHandler, (const void *)&dispatch_source_set_cancel_handler},
    {(const void *)&LSBDispatchSourceSetCancelHandlerF, (const void *)&dispatch_source_set_cancel_handler_f},
    {(const void *)&LSBDispatchSourceSetMandatoryCancelHandler, (const void *)&dispatch_source_set_mandatory_cancel_handler},
    {(const void *)&LSBDispatchSourceSetMandatoryCancelHandlerF, (const void *)&dispatch_source_set_mandatory_cancel_handler_f},
    {(const void *)&LSBDispatchRetain, (const void *)&dispatch_retain}, {(const void *)&LSBDispatchRelease, (const void *)&dispatch_release},
    {(const void *)&LSBProcPIDInfo, (const void *)&proc_pidinfo}
};
#endif
