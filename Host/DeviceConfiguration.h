#pragma once
#import "DeviceType.h"

// Preferences select only an entry in the built-in catalog. Runtime profile files are supplied separately by setup.
LSBGuestDeviceType *LSBConfiguredGuestDeviceType(void);
void LSBSaveConfiguredGuestDeviceType(LSBGuestDeviceType *type);
// Read and validate an immutable staged profile dictionary. The old single-profile layout is accepted only for the default device.
int LSBCopyGuestDeviceCapabilities(NSString *runtimeRoot, LSBGuestDeviceType *type, NSDictionary **profile);
// Feature changes must restore the original catalog entry, never the generated active/legacy profile.
int LSBCopyStagedGuestDeviceCapabilities(NSString *runtimeRoot, LSBGuestDeviceType *type, NSDictionary **profile);
// Call only before any guest starts or after all old guest processes have exited. Atomic replacement preserves readers.
int LSBInstallGuestDeviceCapabilities(NSString *runtimeRoot, NSDictionary *profile);
// Native preference defaults off and accepts only a real Boolean. Exact host environment "1" forces on without saving it.
BOOL LSBHapticsForcedByEnvironment(void);
BOOL LSBExperimentalHapticsEnabled(void);
void LSBSaveConfiguredHapticsEnabled(BOOL enabled);
BOOL LSBAudioOutputForcedByEnvironment(void);
BOOL LSBExperimentalAudioOutputEnabled(void);
void LSBSaveConfiguredAudioOutputEnabled(BOOL enabled);
// Consume the broker's session snapshot; disabled publication preserves the supplied stock profile values unchanged.
int LSBInstallGuestDeviceCapabilitiesWithNativeHaptics(NSString *runtimeRoot, NSDictionary *profile, BOOL enabled);
// Pure profile adaptation. Only CFBoolean true enables support; false/unavailable/malformed native answers publish false.
int LSBProjectGuestHapticsCapability(NSDictionary *profile, CFTypeRef nativeAnswer, NSDictionary **output);
// Additionally publish the native UIKit support level as capabilities.LSBUIKitFeedbackSupportLevel: integer 0..2, never Boolean/float.
// Unknown or malformed levels remove this LSB-only override; the closed-loop capability follows the same Boolean policy above.
int LSBProjectGuestHapticsCapabilities(NSDictionary *profile, CFTypeRef nativeAnswer, CFTypeRef nativeFeedbackLevel, NSDictionary **output);
