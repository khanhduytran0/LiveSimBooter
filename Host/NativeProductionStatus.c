#include "NativeProductionStatus.h"
#include "Private/MobileGestaltPrivate.h"
#include <dispatch/dispatch.h>
#include <stdio.h>

static uint32_t statusForAnswer(CFTypeRef answer) {
    if (!answer || CFGetTypeID(answer) != CFBooleanGetTypeID()) return LSB_NATIVE_PRODUCTION_UNKNOWN;
    return CFBooleanGetValue((CFBooleanRef)answer) ? LSB_NATIVE_PRODUCTION : LSB_NATIVE_DEVELOPMENT;
}

static uint32_t cachedStatus;
static void queryProductionStatus(void *context) {
    (void)context;
    CFTypeRef answer = NULL;
    if (MGCopyAnswer) answer = MGCopyAnswer(CFSTR("EffectiveProductionStatusAp"));
    cachedStatus = statusForAnswer(answer);
    const char *type = !answer ? "unavailable" : CFGetTypeID(answer) == CFBooleanGetTypeID() ? "CFBoolean" : "non-Boolean";
    const char *state = cachedStatus == LSB_NATIVE_PRODUCTION ? "production" : cachedStatus == LSB_NATIVE_DEVELOPMENT ? "development" : "unknown";
    fprintf(stderr, "[broker] EffectiveProductionStatusAp type=%s (CFTypeID=%lu) status=%s\n", type,
        answer ? (unsigned long)CFGetTypeID(answer) : 0, state);
    if (answer) CFRelease(answer);
}

uint32_t LSBHostNativeProductionStatus(void) {
    static dispatch_once_t once;
    dispatch_once_f(&once, NULL, queryProductionStatus);
    return cachedStatus;
}
