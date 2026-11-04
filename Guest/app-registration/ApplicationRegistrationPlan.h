#pragma once

#import <Foundation/Foundation.h>

// These helpers only inspect files. They never contact LaunchServices, including when built for native boundary tests.
// The manifest has exactly {"version": 1, "applications": [absolute guest app paths]}; its own path is also guest-relative.
NSArray<NSString *> *LSBReadApplicationRegistrationManifest(NSString *root, NSString *manifestPath, NSError **error);
// A nil result with skipReason means an original resource-only app stub. Other failures populate error.
NSURL *LSBPrepareApplicationRegistration(NSString *root, NSString *guestPath, NSString **identifier, NSString **skipReason, NSError **error);
BOOL LSBVerifyApplicationRegistration(NSURL *expectedURL, NSString *expectedIdentifier, NSURL *registeredURL,
                                    NSString *registeredIdentifier, BOOL installed, NSError **error);

// The caller supplies a complete guest LaunchServices inventory or an error. Only direct package apps may change.
typedef NSDictionary<NSString *, NSURL *> *(^LSBReadGuestApplications)(NSError **error);
typedef BOOL (^LSBChangeGuestApplication)(NSString *identifier, NSURL *url, BOOL removing, NSError **changeError);
BOOL LSBRefreshDpkgApplications(NSString *runtime, NSString *data, NSString *dpkgRoot, LSBReadGuestApplications readRecords,
                                LSBChangeGuestApplication change, NSError **error);
