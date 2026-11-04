#ifndef LSB_FRAMEWORK_BUNDLE_COMPATIBILITY_H
#define LSB_FRAMEWORK_BUNDLE_COMPATIBILITY_H

#include <CoreFoundation/CoreFoundation.h>

#define LSB_FRAMEWORK_BUNDLE_ENABLE_ENV "LSB_SIMULATE_FRAMEWORK_BUNDLES"
#define LSB_FRAMEWORK_BUNDLE_ROOT_ENV "DYLD_ROOT_PATH"

#ifdef __cplusplus
extern "C" {
#endif

// Opt-in resource bundles for staged system frameworks. Missing or uncontained destinations retain native behavior.
// Requires a readable, contained Info.plist; a redirected bundle-creation failure is never retried against the host.
CFBundleRef LSBFrameworkBundleCreate(CFAllocatorRef allocator, CFURLRef url) CF_RETURNS_RETAINED;

#ifdef __cplusplus
}
#endif
#endif
