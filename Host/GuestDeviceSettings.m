#import "GuestDeviceSettings.h"
#import "Broker.h"
#import "DeviceConfiguration.h"
#include <errno.h>
#include <string.h>

NSNotificationName const LSBGuestDeviceSettingsDidChangeNotification = @"LSBGuestDeviceSettingsDidChange";
static NSString *const FullWidthPreference = @"LSBSimulatorFullWidth";

static NSString *saveErrorText(int error, NSString *format) {
    if (error == ENOENT) return NSLocalizedString(@"The original device capabilities profile is missing. Stage the device profiles during setup.",
        @"Settings error when the original simulator device profile has not been installed.");
    if (error == EBUSY) return NSLocalizedString(@"The simulator is starting, restarting, or powering off. Wait, then try again.",
        @"Settings error when a session transition prevents saving a change.");
    if (error == EPERM) return NSLocalizedString(@"The host launch environment controls this setting.",
        @"Settings error for a feature forced by the native host environment.");
    return [NSString stringWithFormat:format, strerror(error), error];
}

@implementation LSBGuestDeviceSettings {
    id _sessionObserver;
    NSUserDefaults *_defaults;
    int _deviceSaveError, _hapticsSaveError, _audioSaveError, _controlError;
    BOOL _poweringOff;
    unsigned _publishedFlags;
}
+ (instancetype)sharedController {
    static LSBGuestDeviceSettings *controller;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ controller = [self new]; });
    return controller;
}
- (instancetype)init { return [self initWithDefaults:NSUserDefaults.standardUserDefaults]; }
- (instancetype)initWithDefaults:(NSUserDefaults *)defaults {
    if ((self = [super init])) {
        _defaults = defaults;
        _fullWidth = [_defaults boolForKey:FullWidthPreference];
        _deviceTypes = LSBGuestDeviceTypes();
        __weak LSBGuestDeviceSettings *weakSelf = self;
        _sessionObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBHostSessionStateDidChangeNotification object:nil
            queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) { (void)notification; [weakSelf refresh]; }];
        [self refresh];
    }
    return self;
}
- (void)dealloc { if (_sessionObserver) [NSNotificationCenter.defaultCenter removeObserver:_sessionObserver]; }
- (void)publish {
    [NSNotificationCenter.defaultCenter postNotificationName:LSBGuestDeviceSettingsDidChangeNotification object:self];
}
- (void)setFullWidth:(BOOL)enabled {
    NSAssert(NSThread.isMainThread, @"Device settings are main-thread only");
    if (_fullWidth == enabled) return;
    _fullWidth = enabled;
    [_defaults setBool:enabled forKey:FullWidthPreference];
    [self publish];
}
- (void)refresh {
    NSAssert(NSThread.isMainThread, @"Device settings are main-thread only");
    NSString *previousIdentifier = _currentIdentifier, *previousStatus = _statusText, *previousSessionStatus = _sessionStatusText;
    NSString *previousHapticsStatus = _hapticsStatusText, *previousAudioStatus = _audioOutputStatusText;
    _currentIdentifier = LSBConfiguredGuestDeviceType().identifier;
    _hapticsEnabled = LSBExperimentalHapticsEnabled();
    _hapticsForced = LSBHapticsForcedByEnvironment();
    _audioOutputEnabled = LSBExperimentalAudioOutputEnabled();
    _audioOutputForced = LSBAudioOutputForcedByEnvironment();
    _simulatorOff = LSBHostSimulatorIsOff();
    _needsRestart = !_simulatorOff && LSBHostConfigurationNeedsRestart();
    _canApply = !_pending && LSBHostCanApplyDeviceType();
    _canControlSimulator = !_pending && !_simulatorOff && LSBHostCanControlSimulator();
    NSString *savedStatus = _simulatorOff ?
        NSLocalizedString(@"Saved choices will apply when you tap Start on IOSurface.", @"Settings footer while the simulator is off; IOSurface is a tab name.") :
        _needsRestart ? NSLocalizedString(@"Saved changes are waiting for a simulator restart.", @"Settings footer for unapplied saved choices.") :
        NSLocalizedString(@"Changes can be saved now and applied with Restart or Later.", @"Settings footer describing the restart prompt choices.");
    _statusText = _deviceSaveError ? saveErrorText(_deviceSaveError,
        NSLocalizedString(@"Could not save device type: %s (%d).", @"Settings error: system error description and numeric error code.")) : savedStatus;
    _hapticsStatusText = _hapticsSaveError ? saveErrorText(_hapticsSaveError,
        NSLocalizedString(@"Could not save haptic passthrough: %s (%d).", @"Settings error: system error description and numeric error code.")) :
        [NSString stringWithFormat:NSLocalizedString(@"Off by default. Use this device's haptic feedback. %@",
            @"Haptic settings description followed by a complete sentence about applying saved choices."), savedStatus];
    _audioOutputStatusText = _audioSaveError ? saveErrorText(_audioSaveError,
        NSLocalizedString(@"Could not save audio output: %s (%d).", @"Settings error: system error description and numeric error code.")) :
        [NSString stringWithFormat:NSLocalizedString(@"Off by default. Play simulator audio through this device. %@",
            @"Audio settings description followed by a complete sentence about applying saved choices."), savedStatus];
    if (_pending) _sessionStatusText = _poweringOff ?
        NSLocalizedString(@"Powering off the simulator…", @"Simulator session operation in progress.") :
        NSLocalizedString(@"Restarting the simulator…", @"Simulator session operation in progress.");
    else if (_controlError) _sessionStatusText = [NSString stringWithFormat:
        NSLocalizedString(@"Simulator control failed: %s (%d). Reopen the host if it cannot recover.",
            @"Session error: system error description and numeric error code; host means the native LiveSimBooter app."), strerror(_controlError), _controlError];
    else if (_simulatorOff) _sessionStatusText = NSLocalizedString(@"Simulator is off. Tap Start on IOSurface when ready. App data is preserved.",
        @"Session settings footer while off; IOSurface is a tab name.");
    else if (_needsRestart) _sessionStatusText =
        NSLocalizedString(@"Saved changes are waiting for a simulator restart. Running apps will close; app data is preserved.",
            @"Session settings footer explaining a pending restart.");
    else if (!_canControlSimulator) _sessionStatusText =
        NSLocalizedString(@"Simulator is starting or changing state. Session controls will become available when ready.",
            @"Session settings footer while controls are unavailable.");
    else _sessionStatusText = NSLocalizedString(@"Power Off and Restart close running simulator apps. App data is preserved.",
        @"Session settings footer describing the two control buttons.");
    // Broker notifications also cover ordinary child launches. Publish only visible settings changes, so boot activity
    // cannot repeatedly replace otherwise identical UIKit controls. Keep pending in the last-published flags because it changes before refresh.
    unsigned flags = _pending | (_canApply << 1) | (_simulatorOff << 2) | (_needsRestart << 3) | (_canControlSimulator << 4) |
        (_hapticsEnabled << 5) | (_hapticsForced << 6) | (_audioOutputEnabled << 7) | (_audioOutputForced << 8);
    BOOL changed = flags != _publishedFlags || ![_currentIdentifier isEqualToString:previousIdentifier] ||
        ![_statusText isEqualToString:previousStatus] || ![_sessionStatusText isEqualToString:previousSessionStatus] ||
        ![_hapticsStatusText isEqualToString:previousHapticsStatus] || ![_audioOutputStatusText isEqualToString:previousAudioStatus];
    _publishedFlags = flags;
    if (changed) [self publish];
}
- (BOOL)saveDeviceIdentifier:(NSString *)identifier {
    NSAssert(NSThread.isMainThread, @"Device settings are main-thread only");
    LSBGuestDeviceType *type = LSBGuestDeviceTypeForIdentifier(identifier);
    if (!type || _pending || [identifier isEqual:LSBConfiguredGuestDeviceType().identifier]) return NO;
    _deviceSaveError = LSBHostSaveDeviceType(type);
    [self refresh];
    return !_deviceSaveError;
}
- (BOOL)saveHapticsEnabled:(BOOL)enabled {
    NSAssert(NSThread.isMainThread, @"Device settings are main-thread only");
    if (_pending || LSBHapticsForcedByEnvironment() || enabled == LSBExperimentalHapticsEnabled()) return NO;
    _hapticsSaveError = LSBHostSaveHapticsEnabled(enabled);
    [self refresh];
    return !_hapticsSaveError;
}
- (BOOL)saveAudioOutputEnabled:(BOOL)enabled {
    NSAssert(NSThread.isMainThread, @"Device settings are main-thread only");
    if (_pending || LSBAudioOutputForcedByEnvironment() || enabled == LSBExperimentalAudioOutputEnabled()) return NO;
    _audioSaveError = LSBHostSaveAudioOutputEnabled(enabled);
    [self refresh];
    return !_audioSaveError;
}
- (void)controlSimulatorPowerOff:(BOOL)powerOff {
    NSAssert(NSThread.isMainThread, @"Device settings are main-thread only");
    if (_pending || LSBHostSimulatorIsOff() || !LSBHostCanControlSimulator()) { [self refresh]; return; }
    _pending = YES;
    _poweringOff = powerOff;
    _controlError = 0;
    [self refresh];
    __weak LSBGuestDeviceSettings *weakSelf = self;
    void (^completion)(int) = ^(int error) {
        LSBGuestDeviceSettings *self = weakSelf;
        if (!self) return;
        self->_pending = NO;
        self->_controlError = error;
        [self refresh];
    };
    if (powerOff) LSBHostPowerOffSimulator(completion);
    else LSBHostRestartSimulator(completion);
}
- (void)restartSimulator { [self controlSimulatorPowerOff:NO]; }
- (void)powerOffSimulator { [self controlSimulatorPowerOff:YES]; }
@end
