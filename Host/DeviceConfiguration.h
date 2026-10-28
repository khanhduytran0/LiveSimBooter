#pragma once
#import "DeviceType.h"

// Preferences select only an entry in the built-in catalog. Runtime profile files are supplied separately by setup.
LSBGuestDeviceType *LSBConfiguredGuestDeviceType(void);
void LSBSaveConfiguredGuestDeviceType(LSBGuestDeviceType *type);
// Read and validate a staged original profile. The old single-profile layout is accepted only for the default device.
int LSBCopyGuestDeviceCapabilities(NSString *runtimeRoot, LSBGuestDeviceType *type, NSData **data);
// Call only before any guest starts or after all old guest processes have exited. Atomic replacement preserves readers.
int LSBInstallGuestDeviceCapabilities(NSString *runtimeRoot, NSData *data);
// Native-host diagnostic opt-in; the unfinished service bridge and capability projection share this exact gate.
BOOL LSBExperimentalHapticsEnabled(void);
// Without the opt-in, publish the stock profile unchanged. Enabled projection changes only physical closed-loop support.
int LSBInstallGuestDeviceCapabilitiesWithNativeHaptics(NSString *runtimeRoot, NSData *data);
// Pure profile adaptation. Only CFBoolean true enables support; false/unavailable/malformed native answers publish false.
int LSBProjectGuestHapticsCapability(NSData *data, CFTypeRef nativeAnswer, NSData **output);
