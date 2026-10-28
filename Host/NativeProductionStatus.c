#include "NativeProductionStatus.h"
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdio.h>

// The private API is loaded only in the native host; no function pointer or CF object is passed into a simulator child.
typedef CFTypeRef (*LSBMGCopyAnswerFunction)(CFStringRef question);

static uint32_t statusForAnswer(CFTypeRef answer) {
    if (!answer || CFGetTypeID(answer) != CFBooleanGetTypeID()) return LSB_NATIVE_PRODUCTION_UNKNOWN;
    return CFBooleanGetValue((CFBooleanRef)answer) ? LSB_NATIVE_PRODUCTION : LSB_NATIVE_DEVELOPMENT;
}

static uint32_t cachedStatus;
static void queryProductionStatus(void *context) {
    (void)context;
    CFTypeRef answer = NULL;
#if defined(LSB_NATIVE_PRODUCTION_STATUS_TESTING)
    extern CFTypeRef LSBTestCopyProductionStatusAnswer(void);
    answer = LSBTestCopyProductionStatusAnswer();
#else
    void *library = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_LAZY | RTLD_LOCAL);
    LSBMGCopyAnswerFunction copyAnswer = library ? (LSBMGCopyAnswerFunction)dlsym(library, "MGCopyAnswer") : NULL;
    if (copyAnswer) answer = copyAnswer(CFSTR("EffectiveProductionStatusAp"));
#endif
    cachedStatus = statusForAnswer(answer);
    const char *type = !answer ? "unavailable" : CFGetTypeID(answer) == CFBooleanGetTypeID() ? "CFBoolean" : "non-Boolean";
    const char *state = cachedStatus == LSB_NATIVE_PRODUCTION ? "production" : cachedStatus == LSB_NATIVE_DEVELOPMENT ? "development" : "unknown";
    fprintf(stderr, "[broker] EffectiveProductionStatusAp type=%s (CFTypeID=%lu) status=%s\n", type,
        answer ? (unsigned long)CFGetTypeID(answer) : 0, state);
    if (answer) CFRelease(answer);
#if !defined(LSB_NATIVE_PRODUCTION_STATUS_TESTING)
    if (library) dlclose(library);
#endif
}

uint32_t LSBHostNativeProductionStatus(void) {
    static dispatch_once_t once;
    dispatch_once_f(&once, NULL, queryProductionStatus);
    return cachedStatus;
}
