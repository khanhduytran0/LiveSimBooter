#pragma once
#import <Foundation/Foundation.h>
#import "DeviceType.h"

FOUNDATION_EXPORT NSNotificationName const LSBGuestDeviceSettingsDidChangeNotification;

// Main-thread settings state. Device selection remains a UI draft until saved; saves never start or restart a session.
@interface LSBGuestDeviceSettings : NSObject
@property (class, nonatomic, readonly) LSBGuestDeviceSettings *sharedController;
@property (nonatomic, readonly, copy) NSArray<LSBGuestDeviceType *> *deviceTypes;
// Configured choices, which may differ from the running session until an explicit restart.
@property (nonatomic, readonly, copy) NSString *currentIdentifier, *statusText;
@property (nonatomic, readonly) BOOL pending, canApply, simulatorOff, needsRestart, canControlSimulator;
@property (nonatomic, readonly, copy) NSString *sessionStatusText;
// Saved host-presentation preference; effective only for home-button devices and iPads. It never restarts a guest.
@property (nonatomic, readonly) BOOL fullWidth;
@property (nonatomic, readonly) BOOL hapticsEnabled, hapticsForced;
@property (nonatomic, readonly, copy) NSString *hapticsStatusText;
@property (nonatomic, readonly) BOOL audioOutputEnabled, audioOutputForced;
@property (nonatomic, readonly, copy) NSString *audioOutputStatusText;
- (instancetype)initWithDefaults:(NSUserDefaults *)defaults;
- (void)refresh;
// Return YES only when a changed choice was successfully saved. Errors remain in the corresponding footer.
- (BOOL)saveDeviceIdentifier:(NSString *)identifier;
- (BOOL)saveHapticsEnabled:(BOOL)enabled;
- (BOOL)saveAudioOutputEnabled:(BOOL)enabled;
- (void)setFullWidth:(BOOL)enabled;
- (void)restartSimulator;
- (void)powerOffSimulator;
@end
