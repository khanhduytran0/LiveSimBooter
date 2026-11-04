#import "LiveProcessPool.h"
#import "RunningBoardMonitor.h"
#include "../ProcessSupport/ForkStackHint.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { PoolCapacity = 10, PoolParallelLaunches = 2, PoolLaunchTimeout = 15, PoolDrainTimeout = 25 };

@interface LSBWarmProcess : NSObject
@property uint64_t identifier;
@property pid_t pid;
@property LSBReceivedMessage *checkIn;
@property (nonatomic, strong) LSBRunningBoardMonitor *monitor;
@property BOOL retiring;
@end
@implementation LSBWarmProcess
@end

typedef enum { StackDisjoint, StackReclaimable, StackUnknown, StackConflict } StackFit;

static StackFit workerStackFit(LSBWorkerStackGeometry worker, uint64_t bottom, uint64_t top) {
    uint64_t page = vm_page_size;
    if (bottom < page || bottom - page < worker.vmFloor) return StackConflict;
    if (!worker.bottom) return StackUnknown;
    // The donor needs a guard below its copied stack. The native worker can also have a guard below its pthread allocation.
    if (bottom - page >= worker.top || top <= worker.bottom - page) return StackDisjoint;
    if (top <= worker.bottom) return StackUnknown; // Only the possible native guard intersects the offer.
    // trialPlanScratchStack keeps a guard plus headroom below its page-aligned SP, which is at most top - page.
    if (top >= worker.top || worker.top - top < LSBForkStackLandingHeadroom + 2 * page) return StackConflict;
    return StackReclaimable;
}

@interface LSBLiveProcessPool ()
- (void)launchEvent:(uint64_t)identifier pid:(pid_t)pid error:(int)error;
@end

static void warmEvent(uint64_t identifier, pid_t pid, int error, void *context) {
    [(__bridge LSBLiveProcessPool *)context launchEvent:identifier pid:pid error:error];
}

static void rejectCheckIn(LSBReceivedMessage *message, int error) {
    if (!message) return;
    if (message->object) LSBTransportReply(message, error, 0, message->requestID, NULL, NULL, NULL);
    LSBTransportDisposeMessage(message);
    free(message);
}

// An allocation failure leaves this exact launcher session retired, with its assertion held until cancellation succeeds.
static void cancelWarmExtension(uint64_t identifier) {
    if (LSBCancelWarmExtension(identifier) == ENOMEM)
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{ cancelWarmExtension(identifier); });
}

@implementation LSBLiveProcessPool {
    NSMutableDictionary<NSNumber *, LSBWarmProcess *> *_slots;
    LSBPoolPeerValidator _validatePeer;
    NSUInteger _capacity;
    uint64_t _lastDonorBottom, _lastDonorTop;
    BOOL _started, _stopping, _refillScheduled, _trace;
    unsigned _failures;
    dispatch_time_t _retryAt;
    int _drainError;
    void (^_drained)(int);
}

- (instancetype)initWithCapacity:(NSUInteger)capacity validatePeer:(LSBPoolPeerValidator)validatePeer {
    if (!(self = [super init])) return nil;
    _capacity = MIN(capacity, PoolCapacity);
    _validatePeer = validatePeer;
    if (!_validatePeer) return nil;
    _slots = [NSMutableDictionary dictionary];
    const char *trace = getenv("LSB_TRACE_LIVEPROCESS_POOL");
    _trace = trace && !strcmp(trace, "1");
    return self;
}

- (void)start {
    if (_started || _stopping) return;
    _started = YES;
    [self scheduleRefill];
}

- (void)scheduleRefill {
    if (!_started || _stopping || _refillScheduled || _slots.count >= _capacity) return;
    _refillScheduled = YES;
    __weak LSBLiveProcessPool *weak = self;
    dispatch_after(_retryAt ?: DISPATCH_TIME_NOW, dispatch_get_main_queue(), ^{ [weak refill]; });
}

- (void)backOff {
    unsigned delay = 1u << MIN(_failures, 5u);
    if (_failures < 5) ++_failures;
    _retryAt = dispatch_time(DISPATCH_TIME_NOW, (int64_t)delay * NSEC_PER_SEC);
}

- (void)refill {
    _refillScheduled = NO;
    if (_stopping) return;
    if (_retryAt && dispatch_time(DISPATCH_TIME_NOW, 0) < _retryAt) { [self scheduleRefill]; return; }
    _retryAt = 0;
    unsigned waiting = 0;
    for (LSBWarmProcess *slot in _slots.allValues) if (!slot.checkIn && !slot.retiring) ++waiting;
    static uint64_t nextIdentifier = UINT64_MAX; // Separate from the broker's ascending guest-job IDs.
    while (_slots.count < _capacity && waiting++ < PoolParallelLaunches) {
        LSBWarmProcess *slot = [LSBWarmProcess new];
        uint64_t identifier = nextIdentifier--;
        slot.identifier = identifier;
        _slots[@(identifier)] = slot;
        int error = LSBLaunchWarmExtension(LSB_EXTENSION_IDENTIFIER, identifier, warmEvent, (__bridge void *)self);
        if (error) {
            [_slots removeObjectForKey:@(identifier)];
            [self backOff];
            [self scheduleRefill];
            return;
        }
        __weak LSBLiveProcessPool *weak = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, PoolLaunchTimeout * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            [weak launchTimedOut:identifier];
        });
    }
}

- (void)completeDrainIfEmpty {
    if (!_stopping || _slots.count || !_drained) return;
    void (^completion)(int) = _drained;
    _drained = nil;
    completion(_drainError);
}

- (void)removeSlot:(LSBWarmProcess *)slot {
    if (slot.monitor) LSBRunningBoardMonitorCancel(slot.monitor);
    [_slots removeObjectForKey:@(slot.identifier)];
    [self completeDrainIfEmpty];
    [self scheduleRefill];
}

- (void)retire:(LSBWarmProcess *)slot error:(int)error delayRefill:(BOOL)delay {
    if (slot.retiring) return;
    slot.retiring = YES;
    if (_trace) fprintf(stderr, "[pool] retire pid=%d error=%d\n", slot.pid, error);
    rejectCheckIn(slot.checkIn, error);
    slot.checkIn = NULL;
    cancelWarmExtension(slot.identifier);
    if (delay && !_stopping) [self backOff];
    if (!slot.monitor) {
        // No authenticated death notification exists. Never report a successful drain for an unbound launch.
        if (_stopping && !_drainError) _drainError = error;
        [self removeSlot:slot];
    }
}

- (void)died:(uint64_t)identifier error:(int)error {
    LSBWarmProcess *slot = _slots[@(identifier)];
    if (!slot) return;
    if (error && _stopping) _drainError = error;
    if (!slot.retiring) {
        rejectCheckIn(slot.checkIn, ECHILD);
        slot.checkIn = NULL;
        if (!_stopping) [self backOff];
    }
    if (!error) LSBReleaseExtension(identifier); // Death is confirmed, even if an exact-instance signal had failed to allocate.
    else cancelWarmExtension(identifier);
    [self removeSlot:slot];
}

- (void)launchTimedOut:(uint64_t)identifier {
    LSBWarmProcess *slot = _slots[@(identifier)];
    if (slot && !slot.checkIn && !slot.retiring) [self retire:slot error:ETIMEDOUT delayRefill:YES];
}

- (void)launchEvent:(uint64_t)identifier pid:(pid_t)pid error:(int)error {
    LSBWarmProcess *slot = _slots[@(identifier)];
    if (!slot || slot.retiring) return;
    if (error || pid <= 0 || (slot.pid && slot.pid != pid)) { [self retire:slot error:error ?: EACCES delayRefill:YES]; return; }
    for (LSBWarmProcess *other in _slots.allValues) {
        if (other == slot || other.pid != pid) continue;
        [self retire:other error:ESTALE delayRefill:YES];
        [self retire:slot error:ESTALE delayRefill:YES];
        return;
    }
    slot.pid = pid;
}

- (BOOL)handleCheckIn:(LSBReceivedMessage *)message coldPID:(pid_t)coldPID {
    if (!message || message->kind != LSB_REQUEST_CHECKIN || !message->audit.val[5] || message->audit.val[5] > INT_MAX) return NO;
    pid_t pid = (pid_t)message->audit.val[5];
    if (pid == coldPID) return NO;
    BOOL unbound = NO;
    for (LSBWarmProcess *slot in _slots.allValues) {
        if (!slot.pid && !slot.retiring) unbound = YES;
        if (slot.pid != pid) continue;
        if (slot.retiring || slot.checkIn) { rejectCheckIn(message, EACCES); return YES; }
        int error = _validatePeer(&message->peerIdentity, &message->audit);
        if (error) { rejectCheckIn(message, error); return YES; } // A bad peer cannot consume the expected worker's slot.
        uint64_t identifier = slot.identifier;
        __weak LSBLiveProcessPool *weak = self;
        LSBRunningBoardMonitor *monitor = nil;
        error = LSBRunningBoardMonitorStart(&message->audit, ^(int status, int monitorError) {
            (void)status;
            [weak died:identifier error:monitorError];
        }, &monitor);
        if (error) { rejectCheckIn(message, error); [self retire:slot error:error delayRefill:YES]; return YES; }
        slot.monitor = monitor;
        slot.checkIn = message;
        if (_trace) fprintf(stderr, "[pool] parked pid=%d\n", pid);
        if (_stopping) [self retire:slot error:ECANCELED delayRefill:YES];
        else [self scheduleRefill];
        return YES;
    }
    if (!unbound) return NO;
    // The native starter already handles EAGAIN while pkd's authenticated PID callback is in flight.
    rejectCheckIn(message, EAGAIN);
    return YES;
}

- (LSBWarmProcess *)selectWorkerForStackBottom:(uint64_t)bottom top:(uint64_t)top {
    LSBWarmProcess *best = nil, *incompatible = nil;
    StackFit selected = StackConflict;
    unsigned conflicts = 0;
    for (NSNumber *key in _slots) {
        LSBWarmProcess *slot = _slots[key];
        if (slot.retiring || !slot.checkIn) continue;
        if (!bottom) {
            if (!_lastDonorBottom) return slot;
            // Give ordinary loads the workers the last fork could not use. Its next fork may then stay on the warm path.
            if (workerStackFit(slot.checkIn->workerStack, _lastDonorBottom, _lastDonorTop) == StackConflict) {
                if (_trace) fprintf(stderr, "[pool] ordinary-placement pid=%d previous-donor-conflict=1\n", slot.pid);
                return slot;
            }
            if (!best) best = slot;
            continue;
        }
        StackFit fit = workerStackFit(slot.checkIn->workerStack, bottom, top);
        if (fit == StackConflict) { ++conflicts; incompatible = slot; continue; }
        if (fit >= selected) continue;
        best = slot;
        selected = fit;
        if (!_trace && selected == StackDisjoint) break;
    }
    if (_trace && bottom) {
        static const char *const choices[] = {"disjoint", "reclaim", "unknown", "none"};
        fprintf(stderr, "[pool] fork-placement donor=%#llx..%#llx choice=%s pid=%d incompatible=%u\n", bottom, top, choices[selected], best.pid, conflicts);
    }
    // A long-lived donor can exhaust every compatible slot. Rotate just one if the entire pool is ready and unsuitable.
    // While its authenticated death or a refill is pending, conflicts is below capacity, so another miss cannot rotate a second slot.
    if (incompatible && conflicts == _capacity) [self retire:incompatible error:ECANCELED delayRefill:NO];
    return best;
}

- (LSBReceivedMessage *)takeForRequest:(uint64_t)jobID donorStackBottom:(uint64_t)bottom top:(uint64_t)top event:(LSBExtensionEvent)event context:(void *)context {
    if (_stopping || !jobID || !event) return NULL;
    if (bottom) { _lastDonorBottom = bottom; _lastDonorTop = top; }
    LSBWarmProcess *slot;
    while ((slot = [self selectWorkerForStackBottom:bottom top:top])) {
        LSBReceivedMessage *message = slot.checkIn;
        int error = _validatePeer(&message->peerIdentity, &message->audit);
        if (!error) error = LSBClaimWarmExtension(slot.identifier, jobID, slot.pid, event, context);
        if (error) { [self retire:slot error:error delayRefill:YES]; continue; }
        slot.checkIn = NULL; // Transfer to the existing authenticated job bootstrap handler.
        _failures = 0;
        _retryAt = 0;
        if (_trace) fprintf(stderr, "[pool] hit request=%llu pid=%d\n", jobID, slot.pid);
        [self removeSlot:slot];
        return message;
    }
    if (_trace && _started) fprintf(stderr, "[pool] miss request=%llu\n", jobID);
    return NULL;
}

- (void)drain:(void (^)(int))completion {
    if (_stopping) { if (completion) completion(EALREADY); return; }
    _stopping = YES;
    _drained = [completion copy];
    for (LSBWarmProcess *slot in _slots.allValues) if (slot.checkIn) [self retire:slot error:ECANCELED delayRefill:YES];
    [self completeDrainIfEmpty];
    if (!_slots.count) return;
    // A pending launch may still check in during shutdown. Wait for its identity and death, with a bounded error if either is unavailable.
    __weak LSBLiveProcessPool *weak = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, PoolDrainTimeout * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ [weak drainTimedOut]; });
}

- (void)drainTimedOut {
    if (!_slots.count || !_stopping) return;
    _drainError = ETIMEDOUT;
    for (LSBWarmProcess *slot in _slots.allValues) {
        if (!slot.retiring) [self retire:slot error:ETIMEDOUT delayRefill:YES];
        if (_slots[@(slot.identifier)]) [self removeSlot:slot];
    }
}
@end
