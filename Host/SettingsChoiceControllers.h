#pragma once
#import "Private/PreferencesPrivate.h"
@class LSBGuestDeviceSettings;

void LSBOfferSimulatorRestart(UIViewController *controller, LSBGuestDeviceSettings *settings);

@interface LSBDeviceTypePickerViewController : PSListController
@end

@interface LSBHUDSettingsViewController : PSListController
@end
