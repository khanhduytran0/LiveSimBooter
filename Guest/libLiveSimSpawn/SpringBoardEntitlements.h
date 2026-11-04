#ifndef LSB_SPRINGBOARD_ENTITLEMENTS_H
#define LSB_SPRINGBOARD_ENTITLEMENTS_H

#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>

#define LSB_SPRINGBOARD_ENTITLEMENTS_ENABLE_ENV "LSB_SIMULATE_SPRINGBOARD_ENTITLEMENTS"
#define LSB_SPRINGBOARD_UI_ENTITLEMENT "com.apple.springboard-ui.client"

#ifdef __cplusplus
extern "C" {
#endif

// BaseBoard's C API. A weak, dynamically looked-up reference avoids adding BaseBoard to unrelated guest processes.
extern bool BSSelfTaskHasEntitlement(CFStringRef entitlement) __attribute__((weak_import));

// Guest-only report of one entitlement present in stock simulator SpringBoard's __TEXT,__entitlements plist.
// Requires an explicit job opt-in and the canonical RuntimeRoot SpringBoard executable. All other queries forward normally.
// This neither changes native code-signing entitlements nor grants access to a host service or kernel operation.
bool LSBSpringBoardSelfTaskHasEntitlement(CFStringRef entitlement);

#ifdef __cplusplus
}
#endif
#endif
