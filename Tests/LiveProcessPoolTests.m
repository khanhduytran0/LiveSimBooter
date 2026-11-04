#import "../Host/LiveProcessPool.m"
#include <assert.h>

typedef struct { uint64_t id; LSBExtensionEvent event; void *context; pid_t pid; BOOL canceled, claimed; } TestSession;
static TestSession sessions[64];
static unsigned submitted, disposed, replies, claims, canceled;
static int replyStatus, cancelError, releaseCount;
static NSMutableDictionary<NSNumber *, LSBRunningBoardMonitor *> *monitors;

@interface LSBRunningBoardMonitor : NSObject
@property pid_t pid;
@property (nonatomic, copy) LSBRunningBoardExitHandler completion;
@end
@implementation LSBRunningBoardMonitor
@end

int LSBRunningBoardMonitorStart(const audit_token_t *audit, LSBRunningBoardExitHandler completion, LSBRunningBoardMonitor **out) {
    assert(audit->val[5] && audit->val[7] == 3 && completion);
    *out = [LSBRunningBoardMonitor new];
    (*out).pid = (pid_t)audit->val[5];
    (*out).completion = completion;
    monitors[@((*out).pid)] = *out;
    return 0;
}
void LSBRunningBoardMonitorCancel(LSBRunningBoardMonitor *monitor) {
    monitor.completion = nil;
    [monitors removeObjectForKey:@(monitor.pid)];
}

int LSBLaunchWarmExtension(const char *identifier, uint64_t id, LSBExtensionEvent event, void *context) {
    assert(!strcmp(identifier, LSB_EXTENSION_IDENTIFIER) && id && event && context && submitted < 64);
    sessions[submitted++] = (TestSession){id, event, context, 0, NO, NO};
    return 0;
}
static TestSession *session(uint64_t id) {
    for (unsigned i = 0; i < submitted; ++i) if (sessions[i].id == id) return &sessions[i];
    abort();
}
int LSBClaimWarmExtension(uint64_t id, uint64_t job, pid_t pid, LSBExtensionEvent event, void *context) {
    TestSession *s = session(id);
    (void)context;
    assert(!s->claimed && !s->canceled && s->pid == pid && event && job);
    s->claimed = YES;
    ++claims;
    return 0;
}
int LSBCancelWarmExtension(uint64_t id) {
    TestSession *s = session(id);
    if (s->canceled) return ESRCH;
    assert(!s->claimed && !s->canceled);
    if (cancelError) return cancelError;
    s->canceled = YES;
    ++canceled;
    return 0;
}
void LSBReleaseExtension(uint64_t id) {
    assert(!session(id)->claimed);
    session(id)->canceled = YES;
    ++releaseCount;
}
int LSBTransportReply(LSBReceivedMessage *m, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot, const LSBSpawnPlan *plan, const int *fds) {
    assert(m->object && error && !pid && !requestID && !runtimeRoot && !plan && !fds);
    m->object = NULL;
    ++replies;
    replyStatus = error;
    return 0;
}
void LSBTransportDisposeMessage(LSBReceivedMessage *m) {
    assert(!m->object);
    ++disposed;
}

static int validate(const LSBPeerProcessIdentity *peer, const audit_token_t *audit) {
    return audit->val[7] == 3 && !memcmp(&peer->audit, audit, sizeof(*audit)) ? 0 : EACCES;
}
static LSBReceivedMessage *checkin(pid_t pid) {
    LSBReceivedMessage *m = calloc(1, sizeof(*m));
    assert(m);
    m->object = m;
    m->kind = LSB_REQUEST_CHECKIN;
    m->peerIdentity.audit = m->audit = (audit_token_t){{0, 0, 0, 0, 0, (uint32_t)pid, 0, 3}};
    return m;
}
static void ready(unsigned i, pid_t pid) {
    TestSession *s = &sessions[i];
    s->pid = pid;
    s->event(s->id, pid, 0, s->context);
}
static void noEvents(uint64_t id, pid_t pid, int error, void *context) { (void)id; (void)pid; (void)error; (void)context; abort(); }

static void testStackPlacement(void) {
    // Real iPhone 17 donor, rejected worker 14604 and successfully reclaimed worker 14620.
    const uint64_t bottom = 0x16b1d0000, top = 0x16b9d0000, page = vm_page_size;
    LSBWorkerStackGeometry conflict = {.bottom = 0x16b02c000, .top = 0x16b82c000}, reclaim = {.bottom = 0x16b3a0000, .top = 0x16bba0000};
    assert(workerStackFit(conflict, bottom, top) == StackConflict);
    assert(workerStackFit(reclaim, bottom, top) == StackReclaimable);
    assert(workerStackFit((LSBWorkerStackGeometry){.bottom = bottom - page * 9, .top = bottom - page}, bottom, top) == StackDisjoint);
    assert(workerStackFit((LSBWorkerStackGeometry){.bottom = bottom - page * 9, .top = bottom}, bottom, top) == StackConflict); // Donor guard.
    assert(workerStackFit((LSBWorkerStackGeometry){.bottom = top + page, .top = top + page * 9}, bottom, top) == StackDisjoint);
    assert(workerStackFit((LSBWorkerStackGeometry){.bottom = top, .top = top + page * 9}, bottom, top) == StackUnknown); // Possible native guard.
    assert(workerStackFit((LSBWorkerStackGeometry){.bottom = bottom + page, .top = top + page}, bottom, top) == StackConflict); // No landing room.

    LSBWorkerStackGeometry disjoint = {.bottom = top + page, .top = top + 0x800000};
    LSBWorkerStackGeometry floor = disjoint;
    floor.vmFloor = bottom; // Stack fits, but the required guard is below the worker's floor.
    LSBWorkerStackGeometry cases[] = {conflict, {0}, reclaim, floor, disjoint};
    enum { count = sizeof(cases) / sizeof(*cases) };
    const unsigned first = submitted;
    LSBLiveProcessPool *pool = [[LSBLiveProcessPool alloc] initWithCapacity:count validatePeer:validate];
    LSBReceivedMessage *workers[count];
    [pool start]; [pool refill];
    for (unsigned i = 0; i < count; ++i) {
        pid_t pid = (pid_t)(550 + i);
        ready(first + i, pid);
        workers[i] = checkin(pid);
        workers[i]->workerStack = cases[i];
        assert([pool handleCheckIn:workers[i] coldPID:0]);
        [pool refill];
    }
    // Selection order is independent of dictionary enumeration. Leave incompatible workers available for another job.
    unsigned expected[] = {4, 2, 1};
    unsigned ordinaryPID = 0;
    for (unsigned i = 0; i < 3; ++i) {
        LSBReceivedMessage *m = [pool takeForRequest:200 + i donorStackBottom:bottom top:top event:noEvents context:NULL];
        assert(m == workers[expected[i]]);
        free(m);
        if (!i) {
            // One reclaimable and one unknown worker are still ready. An ordinary spawn preserves both for the next forks.
            LSBReceivedMessage *ordinary = [pool takeForRequest:199 donorStackBottom:0 top:0 event:noEvents context:NULL];
            assert(ordinary && (ordinary->audit.val[5] == 550 || ordinary->audit.val[5] == 553));
            ordinaryPID = ordinary->audit.val[5];
            free(ordinary);
        }
    }
    assert(![pool takeForRequest:203 donorStackBottom:bottom top:top event:noEvents context:NULL]);
    assert(monitors.count == 1 && !sessions[first].canceled && !sessions[first + 3].canceled);
    LSBReceivedMessage *unused = [pool takeForRequest:204 donorStackBottom:0 top:0 event:noEvents context:NULL];
    assert(unused && unused->audit.val[5] == (ordinaryPID == 550 ? 553u : 550u));
    free(unused);
    assert(!monitors.count);
    [pool drain:nil];

    // A donor repeatedly asking a full incompatible pool must make progress without terminating the entire pool.
    pool = [[LSBLiveProcessPool alloc] initWithCapacity:2 validatePeer:validate];
    unsigned base = submitted, priorCancels = canceled;
    [pool start]; [pool refill];
    for (unsigned i = 0; i < 2; ++i) {
        pid_t pid = (pid_t)(560 + i);
        ready(base + i, pid);
        LSBReceivedMessage *m = checkin(pid);
        m->workerStack = conflict;
        assert([pool handleCheckIn:m coldPID:0]);
    }
    assert(![pool takeForRequest:206 donorStackBottom:bottom top:top event:noEvents context:NULL] && canceled == priorCancels + 1);
    assert(![pool takeForRequest:207 donorStackBottom:bottom top:top event:noEvents context:NULL] && canceled == priorCancels + 1);
    pid_t retired = sessions[base].canceled ? 560 : 561;
    monitors[@(retired)].completion(0, 0);
    [pool refill];
    assert(submitted == base + 3);
    assert(![pool takeForRequest:208 donorStackBottom:bottom top:top event:noEvents context:NULL] && canceled == priorCancels + 1);
    ready(base + 2, 562);
    LSBReceivedMessage *replacement = checkin(562);
    replacement->workerStack = disjoint;
    assert([pool handleCheckIn:replacement coldPID:0]);
    assert([pool takeForRequest:209 donorStackBottom:bottom top:top event:noEvents context:NULL] == replacement);
    free(replacement);
    LSBReceivedMessage *remaining = [pool takeForRequest:210 donorStackBottom:0 top:0 event:noEvents context:NULL];
    assert(remaining && remaining->audit.val[5] == (uint32_t)(retired == 560 ? 561 : 560));
    free(remaining);
    [pool drain:nil];
}

int main(void) {
    @autoreleasepool {
        monitors = [NSMutableDictionary dictionary];
        LSBLiveProcessPool *pool = [[LSBLiveProcessPool alloc] initWithCapacity:2 validatePeer:validate];
        [pool start]; [pool refill];
        assert(submitted == 2);
        LSBReceivedMessage *cold = checkin(81);
        assert(![pool handleCheckIn:cold coldPID:81]); free(cold);
        assert([pool handleCheckIn:checkin(92) coldPID:81] && replyStatus == EAGAIN && disposed == 1);
        ready(0, 92); ready(1, 93);
        LSBReceivedMessage *invalid = checkin(92);
        ++invalid->peerIdentity.audit.val[7];
        assert([pool handleCheckIn:invalid coldPID:0] && replyStatus == EACCES && disposed == 2);
        LSBReceivedMessage *held = checkin(92);
        assert([pool handleCheckIn:held coldPID:0] && disposed == 2 && monitors.count == 1);
        assert([pool handleCheckIn:checkin(92) coldPID:0] && replyStatus == EACCES && disposed == 3);
        assert([pool takeForRequest:14 donorStackBottom:0 top:0 event:noEvents context:&claims] == held && claims == 1 && monitors.count == 0);
        free(held);
        assert(![pool takeForRequest:15 donorStackBottom:0 top:0 event:noEvents context:&claims]);
        [pool refill];
        assert(submitted == 3);
        assert([pool handleCheckIn:checkin(93) coldPID:0]);
        __block int drain = -1;
        [pool drain:^(int error) { assert(drain == -1); drain = error; }];
        assert(replyStatus == ECANCELED && canceled == 1 && drain == -1);
        monitors[@93].completion(0, 0);
        assert(!monitors.count && drain == -1); // The outstanding pkd request must also exit.
        ready(2, 94);
        assert([pool handleCheckIn:checkin(94) coldPID:0] && replyStatus == ECANCELED && canceled == 2 && drain == -1);
        monitors[@94].completion(0, 0);
        assert(drain == 0 && monitors.count == 0 && releaseCount == 2 && ![pool takeForRequest:16 donorStackBottom:0 top:0 event:noEvents context:NULL]);
        [pool start]; [pool refill]; assert(submitted == 3);

        pool = [[LSBLiveProcessPool alloc] initWithCapacity:1 validatePeer:validate];
        [pool start]; [pool refill];
        ready(3, 96); held = checkin(96);
        assert([pool handleCheckIn:held coldPID:0]); ++held->peerIdentity.audit.val[7];
        assert(![pool takeForRequest:17 donorStackBottom:0 top:0 event:noEvents context:&claims] && canceled == 3 && claims == 1 && replyStatus == EACCES);
        monitors[@96].completion(0, 0);
        [pool refill]; assert(submitted == 4); // A failed claim backs off, without immediately spawning replacements.
        [pool drain:^(int error) { assert(error == 0); }];

        pool = [[LSBLiveProcessPool alloc] initWithCapacity:10 validatePeer:validate];
        [pool start]; [pool refill]; assert(submitted == 6); // At most two unbound starts, including during initial fill.
        drain = -1;
        [pool drain:^(int error) { drain = error; }];
        [pool drainTimedOut];
        assert(drain == ETIMEDOUT && canceled == 5);

        pool = [[LSBLiveProcessPool alloc] initWithCapacity:12 validatePeer:validate];
        const unsigned start = submitted;
        [pool start]; [pool refill];
        for (unsigned i = 0; i < 10; ++i) {
            assert(submitted > start + i && submitted <= start + i + 2);
            ready(start + i, (pid_t)(400 + i));
            assert([pool handleCheckIn:checkin((pid_t)(400 + i)) coldPID:0]);
            [pool refill];
        }
        assert(submitted == start + 10 && monitors.count == 10);
        cancelError = ENOMEM; // Failed signal submission must not leak an assertion on a later confirmed death.
        drain = -1;
        [pool drain:^(int error) { drain = error; }];
        for (unsigned i = 0; i < 10; ++i) monitors[@(400 + i)].completion(0, 0);
        assert(drain == 0 && !monitors.count && releaseCount == 13);
        cancelError = 0;

        pool = [[LSBLiveProcessPool alloc] initWithCapacity:2 validatePeer:validate];
        const unsigned conflict = submitted;
        [pool start]; [pool refill];
        ready(conflict, 450); ready(conflict + 1, 450);
        assert(sessions[conflict].canceled && sessions[conflict + 1].canceled);
        assert(![pool takeForRequest:18 donorStackBottom:0 top:0 event:noEvents context:NULL]);
        assert(replies == disposed);
        testStackPlacement();
        puts("LiveProcess pool authenticated check-ins, ranked fork workers by stack placement, transferred once and drained pending and parked workers");
    }
}
