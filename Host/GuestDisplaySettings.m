#import "GuestDisplaySettings.h"
#import "RuntimeStorage.h"
#include "../ProcessSupport/DisplaySettings.h"
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <uuid/uuid.h>

NSNotificationName const LSBGuestDisplaySettingsDidChangeNotification = @"LSBGuestDisplaySettingsDidChange";

NSArray<NSString *> *LSBPerformanceHUDNames(void) {
    return @[
        NSLocalizedString(@"Off", @"Performance HUD mode: no overlay."),
        NSLocalizedString(@"Basic", @"Performance HUD mode: basic statistics."),
        NSLocalizedString(@"Backdrop", @"Performance HUD mode for backdrop rendering."),
        NSLocalizedString(@"Particles", @"Performance HUD mode for particle rendering."),
        NSLocalizedString(@"Full", @"Performance HUD mode: complete statistics."),
        NSLocalizedString(@"Frequencies", @"Performance HUD mode for clock frequencies."),
        NSLocalizedString(@"Power", @"Performance HUD mode for power usage."),
        NSLocalizedString(@"FPS only", @"Performance HUD mode showing only frames per second."),
        NSLocalizedString(@"Display", @"Performance HUD mode for display statistics."),
        NSLocalizedString(@"Glitches", @"Performance HUD mode for rendering glitches.")
    ];
}

@implementation LSBGuestDisplaySettings {
    NSURL *_dataRoot;
    dispatch_queue_t _queue;
    uint64_t _timeoutNS;
    // Accessed only on _queue. A timeout retains the exact request for later readback; no automatic setter replay.
    LSBDSettingsRequest _unresolved;
    BOOL _hasUnresolved;
}
+ (LSBGuestDisplaySettings *)sharedController {
    static LSBGuestDisplaySettings *controller;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        controller = [[self alloc] initWithDataRoot:LSBRuntimeStorage.sharedStorage.dataURL timeout:8];
    });
    return controller;
}
- (instancetype)initWithDataRoot:(NSURL *)dataRoot timeout:(NSTimeInterval)timeout {
    if (!(self = [super init])) return nil;
    _dataRoot = [dataRoot copy];
    _timeoutNS = (uint64_t)(MAX(0.05, MIN(20.0, timeout)) * 1000000000);
    _queue = dispatch_queue_create("com.kdt.LiveSimBooter.display-settings", DISPATCH_QUEUE_SERIAL);
    _hudMode = _touches = -1;
    _statusText = NSLocalizedString(@"Open Settings after the simulator starts to read its current display options.",
        @"Initial simulator display settings footer.");
    return self;
}
- (void)publish {
    [NSNotificationCenter.defaultCenter postNotificationName:LSBGuestDisplaySettingsDidChangeNotification object:self];
}
- (int)exchange:(uint32_t)operation value:(int32_t)value response:(LSBDSettingsResponse *)response unknown:(BOOL *)unknown {
    *unknown = NO;
    memset(response, 0, sizeof(*response));
    response->hudMode = response->touches = -1;
    response->hudError = response->touchesError = ENODATA;
    int directory = -1;
    int error = LSBDSettingsOpenDirectory(_dataRoot.fileSystemRepresentation, false, &directory);
    LSBDSettingsReady ready;
    if (!error) error = LSBDSettingsReadFile(directory, "ready", &ready, sizeof(ready));
    if (!error) error = LSBDSettingsValidateReady(&ready);
    if (error) { if (directory >= 0) close(directory); *unknown = _hasUnresolved; return error; }
    if (_hasUnresolved && memcmp(_unresolved.generation, ready.generation, 16)) {
        // A new worker never replays an old request. Discard only our in-memory correlation, not another process's files.
        _hasUnresolved = NO;
        close(directory);
        return ESTALE;
    }
    BOOL resuming = _hasUnresolved;
    LSBDSettingsRequest request = _unresolved;
    uint64_t now = LSBDSettingsNow(), stop = now + _timeoutNS;
    if (!resuming) {
        request = (LSBDSettingsRequest){.magic = LSBDSettingsMagic, .version = LSBDSettingsVersion,
            .operation = operation, .value = value, .deadlineNS = stop};
        memcpy(request.generation, ready.generation, 16);
        uuid_generate_random(request.request);
        error = LSBDSettingsValidateRequest(&request, ready.generation, now);
        if (!error) error = LSBDSettingsAbsent(directory, "request");
        if (!error) error = LSBDSettingsAbsent(directory, "active");
        if (!error) error = LSBDSettingsWriteFile(directory, "request", &request, sizeof(request), false);
        if (!error) { _unresolved = request; _hasUnresolved = YES; }
    }
    char name[40];
    LSBDSettingsResultName(request.request, name);
    while (!error) {
        error = LSBDSettingsReadFile(directory, name, response, sizeof(*response));
        if (!error) {
            error = LSBDSettingsValidateResponse(response, &request);
            if (!error) {
                if (unlinkat(directory, name, 0)) error = errno;
                else _hasUnresolved = NO;
            }
            break;
        }
        if (error != ENOENT) break;
        LSBDSettingsReady current;
        error = LSBDSettingsReadFile(directory, "ready", &current, sizeof(current));
        if (!error) error = LSBDSettingsValidateReady(&current);
        if (!error && memcmp(current.generation, request.generation, 16)) { error = ESTALE; _hasUnresolved = NO; }
        if (error) break;
        now = LSBDSettingsNow();
        if (!now || now >= stop) { error = ETIMEDOUT; break; }
        const struct timespec delay = {.tv_nsec = 100000000};
        nanosleep(&delay, NULL);
    }
    *unknown = _hasUnresolved;
    close(directory);
    if (error) {
        // A partially read or malformed response is not readback, even if some of its fields appear successful.
        response->hudMode = response->touches = -1;
        response->hudError = response->touchesError = ENODATA;
        return error;
    }
    return response->error;
}
- (void)perform:(uint32_t)operation value:(int32_t)value {
    NSAssert(NSThread.isMainThread, @"Display settings UI state is main-thread only");
    if (_pending) return;
    if (_outcomeUnknown && operation != LSBDSettingsRead) return;
    _pending = YES;
    _statusText = _outcomeUnknown ?
        NSLocalizedString(@"Checking the previous request; it will not be repeated.", @"Checking a timed-out display setting without applying it twice.") :
        NSLocalizedString(@"Waiting for the simulator…", @"Display settings request in progress.");
    [self publish];
    dispatch_async(_queue, ^{
        LSBDSettingsResponse response;
        BOOL unknown = NO;
        int error = [self exchange:operation value:value response:&response unknown:&unknown];
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_pending = NO;
            self->_outcomeUnknown = unknown;
            self->_hudMode = response.hudError ? -1 : response.hudMode;
            self->_touches = response.touchesError ? -1 : response.touches;
            if (unknown) self->_statusText =
                NSLocalizedString(@"No confirmed reply. Tap Refresh to check again; the previous change will not be repeated.",
                    @"Display settings request timed out with unknown result.");
            else if (error == ENOENT || error == ENOTCONN)
                self->_statusText = NSLocalizedString(@"The simulator display helper or render server is not ready. Start the simulator, then refresh.",
                    @"Display settings unavailable until guest services are ready.");
            else if (error == ESTALE) self->_statusText =
                NSLocalizedString(@"The simulator helper restarted. Refresh to read the new session.", @"Display settings session changed during a request.");
            else if (error) self->_statusText = [NSString stringWithFormat:NSLocalizedString(@"Simulator request failed: %s (%d).",
                @"Display request error: system error description and numeric error code."), strerror(error), error];
            else self->_statusText = NSLocalizedString(@"Values read back from the simulator. The HUD resets when backboardd restarts.",
                @"Display settings confirmed; backboardd is the simulator display daemon's name.");
            [self publish];
        });
    });
}
- (void)refresh { [self perform:LSBDSettingsRead value:0]; }
- (void)setHUDMode:(NSInteger)mode {
    if (mode < 0 || mode > 9) return;
    [self perform:LSBDSettingsHUD value:(int32_t)mode];
}
- (void)setVisualizeTouches:(BOOL)enabled { [self perform:LSBDSettingsTouches value:enabled ? 1 : 0]; }
@end
