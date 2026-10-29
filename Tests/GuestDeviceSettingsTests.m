#import <Foundation/Foundation.h>
#import "../Host/GuestDeviceSettings.h"
#import "../Host/Broker.h"
#import "../Host/DeviceConfiguration.h"
#include <assert.h>
#include <errno.h>

NSString *const LSBHostSessionStateDidChangeNotification = @"LSBHostSessionStateDidChangeTests";
static LSBGuestDeviceType *configuredDevice, *runningDevice;
static BOOL savedHaptics, savedAudio, runningHaptics, runningAudio, hapticsForced, audioForced;
static BOOL simulatorOff, ready, pendingPowerOff;
static unsigned saveCount, restartCount, powerOffCount;
static int saveError, controlError;
static void (^pendingCompletion)(int);

@interface MemoryDeviceDefaults : NSUserDefaults
@property(nonatomic) NSMutableDictionary<NSString *, NSNumber *> *values;
@property(nonatomic) unsigned writes;
@end
@implementation MemoryDeviceDefaults
- (instancetype)init { if ((self = [super init])) _values = [NSMutableDictionary new]; return self; }
- (BOOL)boolForKey:(NSString *)key { return [self.values[key] boolValue]; }
- (void)setBool:(BOOL)value forKey:(NSString *)key { self.values[key] = value ? @YES : @NO; ++self.writes; }
@end

static void notifyBroker(void) {
    [NSNotificationCenter.defaultCenter postNotificationName:LSBHostSessionStateDidChangeNotification object:nil];
}

LSBGuestDeviceType *LSBConfiguredGuestDeviceType(void) { return configuredDevice; }
BOOL LSBExperimentalHapticsEnabled(void) { return hapticsForced || savedHaptics; }
BOOL LSBHapticsForcedByEnvironment(void) { return hapticsForced; }
BOOL LSBExperimentalAudioOutputEnabled(void) { return audioForced || savedAudio; }
BOOL LSBAudioOutputForcedByEnvironment(void) { return audioForced; }
BOOL LSBHostSimulatorIsOff(void) { return simulatorOff; }
BOOL LSBHostCanControlSimulator(void) { return !simulatorOff && ready; }
BOOL LSBHostCanApplyDeviceType(void) { return simulatorOff || ready; }
BOOL LSBHostConfigurationNeedsRestart(void) {
    return !simulatorOff && (![configuredDevice.identifier isEqual:runningDevice.identifier] ||
        LSBExperimentalHapticsEnabled() != runningHaptics || LSBExperimentalAudioOutputEnabled() != runningAudio);
}
static int savePreflight(void) {
    assert(NSThread.isMainThread);
    ++saveCount;
    return !LSBHostCanApplyDeviceType() ? EBUSY : saveError;
}
int LSBHostSaveDeviceType(LSBGuestDeviceType *type) {
    int error = savePreflight();
    if (!error) { configuredDevice = type; notifyBroker(); }
    return error;
}
int LSBHostSaveHapticsEnabled(BOOL enabled) {
    int error = savePreflight();
    if (!error && hapticsForced) error = EPERM;
    if (!error) { savedHaptics = enabled; notifyBroker(); }
    return error;
}
int LSBHostSaveAudioOutputEnabled(BOOL enabled) {
    int error = savePreflight();
    if (!error && audioForced) error = EPERM;
    if (!error) { savedAudio = enabled; notifyBroker(); }
    return error;
}
static void beginControl(BOOL powerOff, void (^completion)(int)) {
    assert(NSThread.isMainThread && completion && !pendingCompletion && LSBHostCanControlSimulator());
    if (powerOff) ++powerOffCount;
    else ++restartCount;
    if (controlError) { completion(controlError); return; }
    ready = NO;
    pendingPowerOff = powerOff;
    pendingCompletion = [completion copy];
    notifyBroker();
}
void LSBHostRestartSimulator(void (^completion)(int)) { beginControl(NO, completion); }
void LSBHostPowerOffSimulator(void (^completion)(int)) { beginControl(YES, completion); }

static void completeControl(int error) {
    assert(pendingCompletion);
    void (^completion)(int) = pendingCompletion;
    pendingCompletion = nil;
    if (!error) {
        simulatorOff = pendingPowerOff;
        if (!pendingPowerOff) {
            runningDevice = configuredDevice;
            runningHaptics = LSBExperimentalHapticsEnabled();
            runningAudio = LSBExperimentalAudioOutputEnabled();
        }
    }
    completion(error);
}
static void reset(BOOL off) {
    assert(!pendingCompletion);
    configuredDevice = runningDevice = LSBDefaultGuestDeviceType();
    savedHaptics = savedAudio = runningHaptics = runningAudio = hapticsForced = audioForced = NO;
    simulatorOff = off;
    ready = !off;
    saveError = controlError = 0;
    saveCount = restartCount = powerOffCount = 0;
}
static LSBGuestDeviceSettings *newModel(void) {
    return [[LSBGuestDeviceSettings alloc] initWithDefaults:[MemoryDeviceDefaults new]];
}

static void testOffEditsNeverStart(void) {
    reset(YES);
    LSBGuestDeviceSettings *model = newModel();
    assert(model.simulatorOff && model.canApply && !model.canControlSimulator && !model.pending && !model.needsRestart);
    assert(model.deviceTypes.count == 7 && [model.deviceTypes isEqual:LSBGuestDeviceTypes()]);
    assert([model.statusText containsString:@"Start"] && [model.sessionStatusText containsString:@"off"]);
    assert(![model saveDeviceIdentifier:@"unknown"] && ![model saveDeviceIdentifier:model.currentIdentifier]);
    assert(![model saveHapticsEnabled:NO] && ![model saveAudioOutputEnabled:NO] && !saveCount);
    assert([model saveDeviceIdentifier:@"ipad-9"]);
    assert([model saveHapticsEnabled:YES]);
    assert([model saveAudioOutputEnabled:YES]);
    assert([model.currentIdentifier isEqual:@"ipad-9"] && model.hapticsEnabled && model.audioOutputEnabled && saveCount == 3);
    assert(model.simulatorOff && model.canApply && !model.canControlSimulator && !model.needsRestart && !model.pending);
    assert(runningDevice == LSBDefaultGuestDeviceType() && !runningHaptics && !runningAudio);
    [model restartSimulator];
    [model powerOffSimulator];
    assert(!restartCount && !powerOffCount && !pendingCompletion);
    LSBGuestDeviceSettings *reopened = newModel();
    assert([reopened.currentIdentifier isEqual:@"ipad-9"] && reopened.hapticsEnabled && reopened.audioOutputEnabled && reopened.simulatorOff);
}

static void testDeferredChoicesComposeAndReverse(void) {
    reset(NO);
    LSBGuestDeviceSettings *model = newModel();
    assert(model.canApply && model.canControlSimulator && !model.simulatorOff && !model.needsRestart);
    assert([model saveDeviceIdentifier:@"iphone-8"] && [model saveHapticsEnabled:YES] && [model saveAudioOutputEnabled:YES]);
    assert(model.needsRestart && !model.pending && model.canControlSimulator);
    assert([model.statusText containsString:@"waiting"] && [model.sessionStatusText containsString:@"waiting"]);
    assert([model.currentIdentifier isEqual:@"iphone-8"] && model.hapticsEnabled && model.audioOutputEnabled);
    assert(runningDevice == LSBDefaultGuestDeviceType() && !runningHaptics && !runningAudio && !restartCount && !powerOffCount);
    // Each later save starts from the saved configuration, not the old running snapshot.
    assert([model saveDeviceIdentifier:runningDevice.identifier] && model.needsRestart);
    assert([model saveHapticsEnabled:NO] && model.needsRestart);
    assert([model saveAudioOutputEnabled:NO] && !model.needsRestart);
    assert(saveCount == 6 && !restartCount && !powerOffCount && !pendingCompletion);
}

static void testRestartAppliesAllSavedChoicesAndBusyState(void) {
    reset(NO);
    LSBGuestDeviceSettings *model = newModel();
    assert([model saveDeviceIdentifier:@"ipad-a16"] && [model saveHapticsEnabled:YES] && [model saveAudioOutputEnabled:YES]);
    [model restartSimulator];
    assert(model.pending && !model.canApply && !model.canControlSimulator && !model.simulatorOff && restartCount == 1);
    assert([model.sessionStatusText containsString:@"Restarting"] && model.needsRestart);
    assert(![model saveDeviceIdentifier:@"iphone-8"] && ![model saveHapticsEnabled:NO] && ![model saveAudioOutputEnabled:NO]);
    [model restartSimulator];
    [model powerOffSimulator];
    assert(saveCount == 3 && restartCount == 1 && !powerOffCount);
    ready = YES;
    notifyBroker(); // Readiness cannot clear an operation owned by the model.
    assert(model.pending && !model.canApply && !model.canControlSimulator);
    ready = NO;
    completeControl(0); // Guest submission completed; fresh launchd is still starting.
    assert(!model.pending && !model.canApply && !model.canControlSimulator && !model.simulatorOff && !model.needsRestart);
    assert([runningDevice.identifier isEqual:@"ipad-a16"] && runningHaptics && runningAudio);
    ready = YES;
    notifyBroker();
    assert(model.canApply && model.canControlSimulator && !model.needsRestart);
}

static void testPowerOffRetainsSavedChoices(void) {
    reset(NO);
    LSBGuestDeviceSettings *model = newModel();
    assert([model saveHapticsEnabled:YES]);
    [model powerOffSimulator];
    assert(model.pending && !model.simulatorOff && !model.canApply && !model.canControlSimulator && powerOffCount == 1);
    assert([model.sessionStatusText containsString:@"Powering off"]);
    completeControl(0);
    assert(!model.pending && model.simulatorOff && model.canApply && !model.canControlSimulator && !model.needsRestart && model.hapticsEnabled);
    assert([model saveAudioOutputEnabled:YES] && [model saveDeviceIdentifier:@"iphone-se-2"]);
    assert(model.simulatorOff && !model.needsRestart && model.audioOutputEnabled && model.hapticsEnabled);
    [model powerOffSimulator];
    [model restartSimulator];
    assert(powerOffCount == 1 && !restartCount);
}

static void testSaveErrorsAndFreshReadiness(void) {
    reset(NO);
    LSBGuestDeviceSettings *model = newModel();
    const struct { int error; const char *text; } cases[] = {
        {ENOENT, "profile is missing"}, {EBUSY, "starting"}, {EPERM, "launch environment"}, {EINVAL, "Could not save"}, {EIO, "Could not save"}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        saveError = cases[i].error;
        assert(![model saveDeviceIdentifier:@"iphone-8"]);
        assert(![model saveHapticsEnabled:YES] && ![model saveAudioOutputEnabled:YES]);
        assert([model.currentIdentifier isEqual:runningDevice.identifier] && !model.hapticsEnabled && !model.audioOutputEnabled && !model.needsRestart);
        assert([model.statusText containsString:@(cases[i].text)] && [model.hapticsStatusText containsString:@(cases[i].text)]);
        assert([model.audioOutputStatusText containsString:@(cases[i].text)] && !model.pending && !pendingCompletion);
    }
    saveError = 0;
    assert([model saveHapticsEnabled:YES] && ![model.hapticsStatusText containsString:@"Could not save"]);
    ready = NO; // A lifecycle transition began after the last UI snapshot.
    assert(model.canApply && ![model saveDeviceIdentifier:@"ipad-9"]);
    assert(!model.canApply && !model.canControlSimulator && !model.simulatorOff);
    assert(![model saveHapticsEnabled:NO] && ![model saveAudioOutputEnabled:YES] && model.hapticsEnabled && !model.audioOutputEnabled);
    [model restartSimulator];
    [model powerOffSimulator];
    assert(!restartCount && !powerOffCount);
    ready = YES;
    notifyBroker();
    assert(model.canApply && model.canControlSimulator && [model saveDeviceIdentifier:@"ipad-9"]);
    assert([model.currentIdentifier isEqual:@"ipad-9"] && ![model.statusText containsString:@"starting"]);
}

static void testFailedControlIsNotOff(void) {
    reset(NO);
    LSBGuestDeviceSettings *model = newModel();
    assert([model saveHapticsEnabled:YES]);
    [model powerOffSimulator];
    completeControl(ETIMEDOUT);
    assert(!model.pending && !model.simulatorOff && !model.canApply && !model.canControlSimulator && model.needsRestart);
    assert([model.sessionStatusText containsString:@"failed"] && [model.sessionStatusText containsString:@"Reopen"] && model.hapticsEnabled);
    [model restartSimulator];
    assert(!restartCount);
    ready = YES;
    notifyBroker();
    controlError = EIO; // Synchronous preflight failure also clears pending.
    [model restartSimulator];
    assert(!model.pending && model.canControlSimulator && !model.simulatorOff && restartCount == 1);
    assert([model.sessionStatusText containsString:@"failed"] && model.needsRestart && model.hapticsEnabled);
    controlError = 0;
    [model restartSimulator];
    assert(model.pending && ![model.sessionStatusText containsString:@"failed"]);
    completeControl(0);
    assert(!model.needsRestart);
}

static void testForcedFlagsAndExternalSavedChanges(void) {
    reset(YES);
    hapticsForced = audioForced = YES;
    LSBGuestDeviceSettings *model = newModel();
    assert(model.hapticsEnabled && model.hapticsForced && model.audioOutputEnabled && model.audioOutputForced);
    assert(![model saveHapticsEnabled:NO] && ![model saveAudioOutputEnabled:NO] && !saveCount);
    assert([model saveDeviceIdentifier:@"iphone-11"] && model.canApply);
    hapticsForced = audioForced = NO;
    savedHaptics = savedAudio = YES;
    configuredDevice = LSBGuestDeviceTypeForIdentifier(@"ipad-a16");
    notifyBroker();
    assert(!model.hapticsForced && !model.audioOutputForced && model.hapticsEnabled && model.audioOutputEnabled);
    assert([model.currentIdentifier isEqual:@"ipad-a16"] && !model.needsRestart && !restartCount);
    assert([model saveHapticsEnabled:NO] && model.audioOutputEnabled && [model.currentIdentifier isEqual:@"ipad-a16"]);
}

static void testFullWidthRemainsImmediateDuringControl(void) {
    reset(NO);
    MemoryDeviceDefaults *defaults = [MemoryDeviceDefaults new];
    LSBGuestDeviceSettings *model = [[LSBGuestDeviceSettings alloc] initWithDefaults:defaults];
    __block unsigned changes = 0;
    id observer = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDeviceSettingsDidChangeNotification object:model
        queue:nil usingBlock:^(NSNotification *notification) { assert(notification.object == model && NSThread.isMainThread); ++changes; }];
    assert(!model.fullWidth);
    [model setFullWidth:NO];
    assert(!defaults.writes && !changes);
    [model setFullWidth:YES];
    assert(model.fullWidth && defaults.writes == 1 && changes == 1 && !saveCount);
    [model setFullWidth:YES];
    assert(defaults.writes == 1 && changes == 1);
    assert(defaults.values.count == 1 && [defaults.values[@"LSBSimulatorFullWidth"] isEqual:@YES]);
    LSBGuestDeviceSettings *reopened = [[LSBGuestDeviceSettings alloc] initWithDefaults:defaults];
    assert(reopened.fullWidth);
    [model restartSimulator];
    [model setFullWidth:NO];
    assert(!model.fullWidth && model.pending && !model.canApply && defaults.writes == 2 && !saveCount);
    completeControl(0);
    assert(!model.fullWidth && !model.pending);
    reopened = [[LSBGuestDeviceSettings alloc] initWithDefaults:defaults];
    assert(!reopened.fullWidth && defaults.writes == 2);
    [NSNotificationCenter.defaultCenter removeObserver:observer];
}

static void testObserverDoesNotRetainModel(void) {
    reset(NO);
    __weak LSBGuestDeviceSettings *weakModel;
    @autoreleasepool { LSBGuestDeviceSettings *model = newModel(); weakModel = model; assert(weakModel); }
    assert(!weakModel);
    notifyBroker();
    @autoreleasepool {
        LSBGuestDeviceSettings *model = newModel();
        weakModel = model;
        [model restartSimulator];
    }
    assert(!weakModel);
    completeControl(0); // A pending broker callback also must not keep the settings controller alive.
}

static void testUnchangedBrokerActivityDoesNotRedrawSettings(void) {
    reset(NO);
    ready = NO;
    LSBGuestDeviceSettings *model = newModel();
    __block unsigned changes = 0;
    id observer = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDeviceSettingsDidChangeNotification object:model
        queue:nil usingBlock:^(NSNotification *notification) { (void)notification; ++changes; }];
    for (unsigned i = 0; i < 20; ++i) { notifyBroker(); [model refresh]; }
    assert(!changes && !model.canApply && !model.canControlSimulator);
    ready = YES;
    notifyBroker();
    assert(changes == 1 && model.canApply && model.canControlSimulator);
    for (unsigned i = 0; i < 20; ++i) notifyBroker();
    assert(changes == 1);
    // Saving triggers both a broker notification and the model's explicit refresh; the visible change appears once.
    assert([model saveHapticsEnabled:YES] && changes == 2 && model.needsRestart);
    [model refresh];
    assert(changes == 2);
    [model restartSimulator];
    assert(changes == 3 && model.pending && !model.canControlSimulator);
    for (unsigned i = 0; i < 20; ++i) notifyBroker();
    assert(changes == 3);
    completeControl(0);
    assert(changes == 4 && !model.pending && !model.canControlSimulator);
    ready = YES;
    notifyBroker();
    assert(changes == 5 && model.canControlSimulator);
    saveError = ENOENT;
    assert(![model saveAudioOutputEnabled:YES] && changes == 6);
    assert(![model saveAudioOutputEnabled:YES] && changes == 6); // Equal newly allocated error strings are still unchanged UI state.
    audioForced = YES;
    notifyBroker();
    assert(changes == 7 && model.audioOutputForced && model.audioOutputEnabled);
    [model setFullWidth:YES];
    assert(changes == 8 && model.fullWidth);
    [model refresh];
    assert(changes == 8);
    [NSNotificationCenter.defaultCenter removeObserver:observer];
}

int main(void) {
    @autoreleasepool {
        testOffEditsNeverStart();
        testDeferredChoicesComposeAndReverse();
        testRestartAppliesAllSavedChoicesAndBusyState();
        testPowerOffRetainsSavedChoices();
        testSaveErrorsAndFreshReadiness();
        testFailedControlIsNotOff();
        testForcedFlagsAndExternalSavedChanges();
        testFullWidthRemainsImmediateDuringControl();
        testObserverDoesNotRetainModel();
        testUnchangedBrokerActivityDoesNotRedrawSettings();
        puts("GuestDeviceSettings tests passed");
    }
    return 0;
}
