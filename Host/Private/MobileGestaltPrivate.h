#ifndef LSB_MOBILE_GESTALT_PRIVATE_H
#define LSB_MOBILE_GESTALT_PRIVATE_H

#include <CoreFoundation/CoreFoundation.h>

#ifdef __cplusplus
extern "C" {
#endif

// Native host only. Missing MobileGestalt support must remain an unknown capability or production status.
CFTypeRef MGCopyAnswer(CFStringRef question) __attribute__((weak_import)) CF_RETURNS_RETAINED;

#ifdef __cplusplus
}
#endif

#endif
