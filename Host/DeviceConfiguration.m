#import "DeviceConfiguration.h"
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if TARGET_OS_IOS && !TARGET_OS_SIMULATOR
#import <UIKit/UIKit.h>
#include <objc/runtime.h>

// UIDevice's feedback-engine SPI is NSInteger (q16@0:8 in arm64 UIKit); verify the native method before calling it.
// The matching 23A343 simulator method is a zero-returning stub, so the optional guest adapter needs this native value.
@interface UIDevice (LSBNativeFeedbackSupport)
- (NSInteger)_feedbackSupportLevel;
@end
#endif

static NSString *const DevicePreference = @"LSBSimulatorDeviceType";
static NSString *const HapticsPreference = @"LSBExperimentalHapticsEnabled";
static NSString *const AudioOutputPreference = @"LSBExperimentalAudioOutputEnabled";

LSBGuestDeviceType *LSBConfiguredGuestDeviceType(void) {
    id identifier = [NSUserDefaults.standardUserDefaults objectForKey:DevicePreference];
    return [identifier isKindOfClass:NSString.class] ? LSBGuestDeviceTypeForIdentifier(identifier) ?: LSBDefaultGuestDeviceType() :
        LSBDefaultGuestDeviceType();
}
void LSBSaveConfiguredGuestDeviceType(LSBGuestDeviceType *type) {
    [NSUserDefaults.standardUserDefaults setObject:type.identifier forKey:DevicePreference];
}

static int fileError(NSError *error, int fallback) {
    if ([error.domain isEqualToString:NSPOSIXErrorDomain]) return (int)error.code;
    NSError *underlying = error.userInfo[NSUnderlyingErrorKey];
    if ([underlying.domain isEqualToString:NSPOSIXErrorDomain]) return (int)underlying.code;
    if ([error.domain isEqualToString:NSCocoaErrorDomain] &&
        (error.code == NSFileNoSuchFileError || error.code == NSFileReadNoSuchFileError)) return ENOENT;
    return fallback;
}

static int checkProfilePath(NSString *root, NSString *relative) {
    struct stat status;
    if (lstat(root.fileSystemRepresentation, &status)) return errno;
    if (!S_ISDIR(status.st_mode)) return S_ISLNK(status.st_mode) ? ELOOP : ENOTDIR;
    NSString *canonicalRoot = root.stringByResolvingSymlinksInPath;
    NSString *expected = [canonicalRoot stringByAppendingPathComponent:relative];
    NSString *actual = expected.stringByResolvingSymlinksInPath;
    // These are static setup checks, not descriptor-pinned protection against concurrent path replacement.
    return [actual isEqualToString:expected] ? 0 : ELOOP;
}

static int copyProfile(NSString *root, NSString *relative, NSDictionary **output) {
    int error = checkProfilePath(root, relative);
    if (error) return error;
    NSURL *url = [NSURL fileURLWithPath:[root stringByAppendingPathComponent:relative]];
    struct stat status;
    if (lstat(url.fileSystemRepresentation, &status)) return errno;
    if (!S_ISREG(status.st_mode) || status.st_size <= 0 || status.st_size > 2 * 1024 * 1024) return EINVAL;
    NSError *readError = nil;
    NSDictionary *profile = [NSDictionary dictionaryWithContentsOfURL:url error:&readError];
    if (!profile) return fileError(readError, EINVAL);
    // Foundation's convenience loader may return mutable nested containers. Keep the pending reboot snapshot immutable.
    *output = CFBridgingRelease(CFPropertyListCreateDeepCopy(kCFAllocatorDefault, (__bridge CFPropertyListRef)profile, kCFPropertyListImmutable));
    return *output ? 0 : ENOMEM;
}

static int copyGuestDeviceCapabilities(NSString *root, LSBGuestDeviceType *type, BOOL allowLegacy, NSDictionary **output) {
    if (!output) return EINVAL;
    *output = nil;
    if (!root.isAbsolutePath || !type || type != LSBGuestDeviceTypeForIdentifier(type.identifier)) return EINVAL;
    NSString *relative = [NSString stringWithFormat:@"Library/Developer/CoreSimulator/DeviceTypes/%@/capabilities.plist", type.identifier];
    NSDictionary *profile = nil;
    int error = copyProfile(root, relative, &profile);
    if (error == ENOENT && allowLegacy && type == LSBDefaultGuestDeviceType())
        error = copyProfile(root, @"Library/Developer/CoreSimulator/DeviceType/capabilities.plist", &profile);
    if (!error) error = LSBValidateGuestDeviceCapabilities(profile, type);
    if (!error) *output = profile;
    return error;
}

int LSBCopyGuestDeviceCapabilities(NSString *root, LSBGuestDeviceType *type, NSDictionary **output) {
    return copyGuestDeviceCapabilities(root, type, YES, output);
}
int LSBCopyStagedGuestDeviceCapabilities(NSString *root, LSBGuestDeviceType *type, NSDictionary **output) {
    return copyGuestDeviceCapabilities(root, type, NO, output);
}

int LSBInstallGuestDeviceCapabilities(NSString *root, NSDictionary *profile) {
    if (!root.isAbsolutePath || ![profile isKindOfClass:NSDictionary.class] || ![profile[@"capabilities"] isKindOfClass:NSDictionary.class])
        return EINVAL;
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:profile format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    if (!data.length || data.length > 2 * 1024 * 1024) return EINVAL;
    NSString *relative = @"Library/Developer/CoreSimulator/DeviceType/capabilities.plist";
    int error = checkProfilePath(root, relative);
    if (error) return error;
    NSURL *url = [NSURL fileURLWithPath:[root stringByAppendingPathComponent:relative]];
    NSError *writeError = nil;
    if (![NSFileManager.defaultManager createDirectoryAtURL:url.URLByDeletingLastPathComponent withIntermediateDirectories:YES
                                                attributes:nil error:&writeError]) return fileError(writeError, EIO);
    struct stat status;
    if (!lstat(url.fileSystemRepresentation, &status)) {
        if (!S_ISREG(status.st_mode)) return S_ISLNK(status.st_mode) ? ELOOP : EINVAL;
    } else if (errno != ENOENT) return errno;
    return [data writeToURL:url options:NSDataWritingAtomic error:&writeError] ? 0 : fileError(writeError, EIO);
}

int LSBProjectGuestHapticsCapabilities(NSDictionary *original, CFTypeRef nativeAnswer, CFTypeRef nativeFeedbackLevel, NSDictionary **output) {
    if (!output) return EINVAL;
    *output = nil;
    if (![original isKindOfClass:NSDictionary.class] || ![original[@"capabilities"] isKindOfClass:NSDictionary.class]) return EINVAL;
    NSNumber *supported = nativeAnswer && CFGetTypeID(nativeAnswer) == CFBooleanGetTypeID() &&
        CFBooleanGetValue((CFBooleanRef)nativeAnswer) ? @YES : @NO;
    // 23A343 MobileGestalt's closed-loop callback (0xec80) reads this plaintext SimCapabilities key (0x2ccbc).
    // This describes the bridged physical actuator, without changing the selected simulator's identity or display profile.
    NSMutableDictionary *profile = [original mutableCopy], *capabilities = [original[@"capabilities"] mutableCopy];
    capabilities[@"DeviceSupportsClosedLoopHaptics"] = supported;
    // Never carry an older session's native level through an unknown/malformed query. Stock profiles have no LSB-only key.
    [capabilities removeObjectForKey:@"LSBUIKitFeedbackSupportLevel"];
    NSInteger feedbackLevel;
    if (nativeFeedbackLevel && CFGetTypeID(nativeFeedbackLevel) == CFNumberGetTypeID() &&
        !CFNumberIsFloatType((CFNumberRef)nativeFeedbackLevel) &&
        CFNumberGetValue((CFNumberRef)nativeFeedbackLevel, kCFNumberNSIntegerType, &feedbackLevel) && feedbackLevel >= 0 && feedbackLevel <= 2)
        capabilities[@"LSBUIKitFeedbackSupportLevel"] = @(feedbackLevel);
    profile[@"capabilities"] = capabilities.copy;
    *output = profile.copy;
    return 0;
}

int LSBProjectGuestHapticsCapability(NSDictionary *original, CFTypeRef nativeAnswer, NSDictionary **output) {
    return LSBProjectGuestHapticsCapabilities(original, nativeAnswer, NULL, output);
}

static NSNumber *nativeFeedbackSupportLevel(void) {
#if TARGET_OS_IOS && !TARGET_OS_SIMULATOR
    static NSNumber *cachedLevel;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Method method = class_getInstanceMethod(UIDevice.class, @selector(_feedbackSupportLevel));
        const char *signature = method ? method_getTypeEncoding(method) : NULL;
        if (!signature || strcmp(signature, "q16@0:8")) {
            fprintf(stderr, "[haptics] native UIKit feedback support=unknown method signature unavailable or incompatible\n");
            return;
        }
        @try {
            NSInteger level = UIDevice.currentDevice._feedbackSupportLevel;
            if (level >= 0 && level <= 2) cachedLevel = @(level);
            fprintf(stderr, "[haptics] native UIKit feedback support=%ld known=%d\n", (long)level, cachedLevel != nil);
        } @catch (NSException *exception) {
            fprintf(stderr, "[haptics] native UIKit feedback support=unknown exception=%s\n", exception.name.UTF8String);
        }
    });
    return cachedLevel;
#else
    return nil; // Desktop profile tests and simulator builds have no native iPhone capability to publish.
#endif
}

static CFBooleanRef nativeHapticsCapability(void) {
    static CFBooleanRef cachedAnswer;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        // Native host only: no MobileGestalt pointer or CF object crosses into the guest.
        typedef CFTypeRef (*MGCopyAnswerFunction)(CFStringRef question);
        void *library = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_LAZY | RTLD_LOCAL);
        MGCopyAnswerFunction copyAnswer = library ? (MGCopyAnswerFunction)dlsym(library, "MGCopyAnswer") : NULL;
        CFTypeRef answer = copyAnswer ? copyAnswer(CFSTR("gqDnklGQnpv5ilgh5uHckw")) : NULL;
        BOOL known = answer && CFGetTypeID(answer) == CFBooleanGetTypeID();
        if (known) cachedAnswer = CFBooleanGetValue((CFBooleanRef)answer) ? kCFBooleanTrue : kCFBooleanFalse;
        fprintf(stderr, "[haptics] native closed-loop support=%s answer-type=%lu\n",
                known ? CFBooleanGetValue(cachedAnswer) ? "true" : "false" : "unknown", answer ? (unsigned long)CFGetTypeID(answer) : 0);
        if (answer) CFRelease(answer);
        if (library) dlclose(library);
    });
    return cachedAnswer;
}

BOOL LSBHapticsForcedByEnvironment(void) {
    const char *value = getenv("LSB_EXPERIMENTAL_HAPTICS");
    return value && !strcmp(value, "1");
}
BOOL LSBExperimentalHapticsEnabled(void) {
    if (LSBHapticsForcedByEnvironment()) return YES;
    id value = [NSUserDefaults.standardUserDefaults objectForKey:HapticsPreference];
    return value && CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() && CFBooleanGetValue((__bridge CFBooleanRef)value);
}
void LSBSaveConfiguredHapticsEnabled(BOOL enabled) {
    [NSUserDefaults.standardUserDefaults setObject:enabled ? @YES : @NO forKey:HapticsPreference];
}

BOOL LSBAudioOutputForcedByEnvironment(void) {
    const char *value = getenv("LSB_AUDIO_OUTPUT");
    return value && !strcmp(value, "1");
}
BOOL LSBExperimentalAudioOutputEnabled(void) {
    if (LSBAudioOutputForcedByEnvironment()) return YES;
    id value = [NSUserDefaults.standardUserDefaults objectForKey:AudioOutputPreference];
    return value && CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() && CFBooleanGetValue((__bridge CFBooleanRef)value);
}
void LSBSaveConfiguredAudioOutputEnabled(BOOL enabled) {
    [NSUserDefaults.standardUserDefaults setObject:enabled ? @YES : @NO forKey:AudioOutputPreference];
}

int LSBInstallGuestDeviceCapabilitiesWithNativeHaptics(NSString *root, NSDictionary *profile, BOOL enabled) {
    if (!enabled) return LSBInstallGuestDeviceCapabilities(root, profile);
    NSDictionary *adapted = nil;
    int error = LSBProjectGuestHapticsCapabilities(profile, nativeHapticsCapability(), (__bridge CFTypeRef)nativeFeedbackSupportLevel(), &adapted);
    return error ? error : LSBInstallGuestDeviceCapabilities(root, adapted);
}
