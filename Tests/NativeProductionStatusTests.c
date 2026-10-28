#define LSB_NATIVE_PRODUCTION_STATUS_TESTING 1
#include "../Host/NativeProductionStatus.c"
#include <assert.h>

static CFTypeRef testAnswer;
static unsigned queryCount;
CFTypeRef LSBTestCopyProductionStatusAnswer(void) {
    ++queryCount;
    return testAnswer ? CFRetain(testAnswer) : NULL;
}

int main(void) {
    assert(statusForAnswer(NULL) == LSB_NATIVE_PRODUCTION_UNKNOWN);
    assert(statusForAnswer(kCFBooleanTrue) == LSB_NATIVE_PRODUCTION);
    assert(statusForAnswer(kCFBooleanFalse) == LSB_NATIVE_DEVELOPMENT);
    assert(statusForAnswer(CFSTR("false")) == LSB_NATIVE_PRODUCTION_UNKNOWN);
    int zero = 0;
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &zero);
    assert(number && statusForAnswer(number) == LSB_NATIVE_PRODUCTION_UNKNOWN);
    CFRelease(number);
    CFDictionaryRef dictionary = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    assert(dictionary && statusForAnswer(dictionary) == LSB_NATIVE_PRODUCTION_UNKNOWN);
    CFRelease(dictionary);
    testAnswer = kCFBooleanFalse;
    assert(LSBHostNativeProductionStatus() == LSB_NATIVE_DEVELOPMENT && queryCount == 1);
    testAnswer = kCFBooleanTrue;
    assert(LSBHostNativeProductionStatus() == LSB_NATIVE_DEVELOPMENT && queryCount == 1);
    puts("Native production-status type and cache tests passed");
    return 0;
}
