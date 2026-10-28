#import "GuestDeviceSettings.h"
#import "Broker.h"
#import "DeviceConfiguration.h"
#include <errno.h>
#include <string.h>

NSNotificationName const LSBGuestDeviceSettingsDidChangeNotification = @"LSBGuestDeviceSettingsDidChange";
static NSString *const FullWidthPreference = @"LSBSimulatorFullWidth";

@implementation LSBGuestDeviceSettings {
    id _sessionObserver;
    NSUserDefaults *_defaults;
}
+ (instancetype)sharedController {
    static LSBGuestDeviceSettings *controller;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ controller = [self new]; });
    return controller;
}
- (instancetype)init {
    return [self initWithDefaults:NSUserDefaults.standardUserDefaults];
}
- (instancetype)initWithDefaults:(NSUserDefaults *)defaults {
    if ((self = [super init])) {
        _defaults = defaults;
        _fullWidth = [_defaults boolForKey:FullWidthPreference];
        _deviceTypes = LSBGuestDeviceTypes();
        _statusText = @"Apply changes the simulator display and restarts its session. App data is preserved.";
        _hapticsStatusText = @"Off by default. Changing this setting restarts the simulator; running apps will close.";
        _audioOutputStatusText = @"Off by default. Play simulator audio through this device. Changing this setting restarts the simulator.";
        __weak LSBGuestDeviceSettings *weakSelf = self;
        _sessionObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBHostSessionStateDidChangeNotification object:nil
            queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) { (void)notification; [weakSelf refresh]; }];
        [self refresh];
    }
    return self;
}
- (void)setFullWidth:(BOOL)enabled {
    NSAssert(NSThread.isMainThread, @"Device settings are main-thread only");
    if (_fullWidth == enabled) return;
    _fullWidth = enabled;
    [_defaults setBool:enabled forKey:FullWidthPreference];
    [self publish];
}
- (void)dealloc { if (_sessionObserver) [NSNotificationCenter.defaultCenter removeObserver:_sessionObserver]; }
- (void)publish {
    [NSNotificationCenter.defaultCenter postNotificationName:LSBGuestDeviceSettingsDidChangeNotification object:self];
}
- (void)refresh {
    NSAssert(NSThread.isMainThread, @"Device settings are main-thread only");
    _currentIdentifier = LSBHostCurrentDeviceType().identifier;
    _hapticsEnabled = LSBHostCurrentHapticsEnabled();
    _hapticsForced = LSBHapticsForcedByEnvironment();
    _audioOutputEnabled = LSBHostCurrentAudioOutputEnabled();
    _audioOutputForced = LSBAudioOutputForcedByEnvironment();
    _canApply = !_pending && !_outcomeUnknown && LSBHostCanApplyDeviceType();
    [self publish];
}
- (void)applyDeviceIdentifier:(NSString *)identifier {
    NSAssert(NSThread.isMainThread, @"Device settings are main-thread only");
    if (_pending || _outcomeUnknown) return;
    LSBGuestDeviceType *type = LSBGuestDeviceTypeForIdentifier(identifier);
    if (!type || [identifier isEqualToString:LSBHostCurrentDeviceType().identifier]) return;
    _pending = YES;
    _canApply = NO;
    _statusText = @"Applying device type and restarting the simulator…";
    [self publish];
    LSBHostApplyDeviceType(type, ^(int error, BOOL restarted) {
        self->_pending = NO;
        if (error == ENOENT) self->_statusText = @"This device's original capabilities profile is missing. Stage the device profiles during setup.";
        else if (error == EBUSY) self->_statusText = @"The simulator is still starting or restarting. Wait, then try Apply again.";
        else if (error) self->_statusText = [NSString stringWithFormat:@"Device change failed: %s (%d). Reopen the host if the session stopped.",
                                            strerror(error), error];
        else self->_statusText = restarted ? @"Device applied. The simulator is starting with the new display." :
            @"Device selected. Tap Start on IOSurface to boot it.";
        [self refresh];
    });
}
- (void)applyHapticsEnabled:(BOOL)enabled {
    NSAssert(NSThread.isMainThread, @"Device settings are main-thread only");
    if (_pending || _outcomeUnknown || _hapticsForced || enabled == LSBHostCurrentHapticsEnabled()) return;
    _pending = YES;
    _canApply = NO;
    _hapticsStatusText = @"Applying haptic passthrough and restarting the simulator…";
    [self publish];
    LSBHostApplyHapticsEnabled(enabled, ^(int error, BOOL restarted) {
        self->_pending = NO;
        if (error == ENOENT) self->_hapticsStatusText = @"The original device capabilities profile is missing. Stage the device profiles during setup.";
        else if (error == EBUSY) self->_hapticsStatusText = @"The simulator is still starting or restarting. Wait, then try again.";
        else if (error == EPERM) self->_hapticsStatusText = @"The host launch environment controls haptic passthrough.";
        else if (error) self->_hapticsStatusText = [NSString stringWithFormat:@"Haptic change failed: %s (%d). Reopen the host if the session stopped.",
                                                  strerror(error), error];
        else self->_hapticsStatusText = restarted ? @"Haptic setting applied. The simulator is starting a fresh session." :
            @"Haptic setting saved. Tap Start on IOSurface to boot the simulator.";
        [self refresh];
    });
}
- (void)applyAudioOutputEnabled:(BOOL)enabled {
    NSAssert(NSThread.isMainThread, @"Device settings are main-thread only");
    if (_pending || _outcomeUnknown || _audioOutputForced || enabled == LSBHostCurrentAudioOutputEnabled()) return;
    _pending = YES;
    _canApply = NO;
    _audioOutputStatusText = @"Applying audio output and restarting the simulator…";
    [self publish];
    LSBHostApplyAudioOutputEnabled(enabled, ^(int error, BOOL restarted) {
        self->_pending = NO;
        if (error == ENOENT) self->_audioOutputStatusText = @"The original device capabilities profile is missing. Stage the device profiles during setup.";
        else if (error == EBUSY) self->_audioOutputStatusText = @"The simulator is still starting or restarting. Wait, then try again.";
        else if (error == EPERM) self->_audioOutputStatusText = @"The host launch environment controls audio output.";
        else if (error) self->_audioOutputStatusText = [NSString stringWithFormat:@"Audio output change failed: %s (%d). Reopen the host if the session stopped.",
                                                  strerror(error), error];
        else self->_audioOutputStatusText = restarted ? @"Audio output setting applied. The simulator is starting a fresh session." :
            @"Audio output setting saved. Tap Start on IOSurface to boot the simulator.";
        [self refresh];
    });
}
@end
