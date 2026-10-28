#import "DeviceConfiguration.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static NSString *const DevicePreference = @"LSBSimulatorDeviceType";

LSBGuestDeviceType *LSBConfiguredGuestDeviceType(void) {
    id identifier = [NSUserDefaults.standardUserDefaults objectForKey:DevicePreference];
    return [identifier isKindOfClass:NSString.class] ? LSBGuestDeviceTypeForIdentifier(identifier) ?: LSBDefaultGuestDeviceType() :
        LSBDefaultGuestDeviceType();
}
void LSBSaveConfiguredGuestDeviceType(LSBGuestDeviceType *type) {
    [NSUserDefaults.standardUserDefaults setObject:type.identifier forKey:DevicePreference];
}

static int openProfileDirectory(NSString *root, NSArray<NSString *> *components, BOOL create) {
    int directory = open(root.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) return -1;
    for (NSString *component in components) {
        if (create && mkdirat(directory, component.fileSystemRepresentation, 0755) && errno != EEXIST) {
            int error = errno; close(directory); errno = error; return -1;
        }
        int next = openat(directory, component.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        int error = errno;
        close(directory);
        if (next < 0) { errno = error; return -1; }
        directory = next;
    }
    return directory;
}

int LSBCopyGuestDeviceCapabilities(NSString *root, LSBGuestDeviceType *type, NSData **output) {
    if (!root.isAbsolutePath || !output || !type || type != LSBGuestDeviceTypeForIdentifier(type.identifier)) return EINVAL;
    *output = nil;
    NSArray *components = @[@"Library", @"Developer", @"CoreSimulator", @"DeviceTypes", type.identifier];
    int directory = openProfileDirectory(root, components, NO);
    int file = directory < 0 ? -1 : openat(directory, "capabilities.plist", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    int error = errno;
    if (directory >= 0) close(directory);
    if (file < 0 && error == ENOENT && type == LSBDefaultGuestDeviceType()) {
        directory = openProfileDirectory(root, @[@"Library", @"Developer", @"CoreSimulator", @"DeviceType"], NO);
        file = directory < 0 ? -1 : openat(directory, "capabilities.plist", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        error = errno;
        if (directory >= 0) close(directory);
    }
    if (file < 0) return error;
    struct stat status;
    error = fstat(file, &status) ? errno : !S_ISREG(status.st_mode) || status.st_size <= 0 || status.st_size > 2 * 1024 * 1024 ? EINVAL : 0;
    NSMutableData *data = error ? nil : [NSMutableData dataWithLength:(NSUInteger)status.st_size];
    size_t offset = 0;
    while (!error && offset < data.length) {
        ssize_t count = read(file, (char *)data.mutableBytes + offset, data.length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = count < 0 ? errno : EIO; break; }
        offset += (size_t)count;
    }
    close(file);
    if (!error) error = LSBValidateGuestDeviceCapabilities(data, type);
    if (!error) *output = data.copy;
    return error;
}

int LSBInstallGuestDeviceCapabilities(NSString *root, NSData *data) {
    if (!root.isAbsolutePath || !data.length || data.length > 2 * 1024 * 1024) return EINVAL;
    int directory = openProfileDirectory(root, @[@"Library", @"Developer", @"CoreSimulator", @"DeviceType"], YES);
    if (directory < 0) return errno;
    NSString *temporary = [@".capabilities-" stringByAppendingString:NSUUID.UUID.UUIDString];
    int file = openat(directory, temporary.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    int error = file < 0 ? errno : 0;
    size_t offset = 0;
    while (!error && offset < data.length) {
        ssize_t count = write(file, (const char *)data.bytes + offset, data.length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = count < 0 ? errno : EIO; break; }
        offset += (size_t)count;
    }
    if (!error && fsync(file)) error = errno;
    if (file >= 0 && close(file) && !error) error = errno;
    if (!error && renameat(directory, temporary.fileSystemRepresentation, directory, "capabilities.plist")) error = errno;
    if (error) unlinkat(directory, temporary.fileSystemRepresentation, 0);
    close(directory);
    return error;
}

int LSBProjectGuestHapticsCapability(NSData *data, CFTypeRef nativeAnswer, NSData **output) {
    if (!output) return EINVAL;
    *output = nil;
    if (![data isKindOfClass:NSData.class] || !data.length || data.length > 2 * 1024 * 1024) return EINVAL;
    id original = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:NULL error:NULL];
    if (![original isKindOfClass:NSDictionary.class] || ![original[@"capabilities"] isKindOfClass:NSDictionary.class]) return EINVAL;
    NSNumber *supported = nativeAnswer && CFGetTypeID(nativeAnswer) == CFBooleanGetTypeID() &&
        CFBooleanGetValue((CFBooleanRef)nativeAnswer) ? @YES : @NO;
    // 23A343 MobileGestalt's closed-loop callback (0xec80) reads this plaintext SimCapabilities key (0x2ccbc).
    // This describes the bridged physical actuator, without changing the selected simulator's identity or display profile.
    NSString *key = @"DeviceSupportsClosedLoopHaptics";
    id previous = original[@"capabilities"][key];
    if (previous && CFGetTypeID((__bridge CFTypeRef)previous) == CFBooleanGetTypeID() && [previous isEqual:supported]) {
        *output = data.copy;
        return 0;
    }
    NSMutableDictionary *profile = [original mutableCopy], *capabilities = [original[@"capabilities"] mutableCopy];
    capabilities[key] = supported;
    profile[@"capabilities"] = capabilities;
    *output = [NSPropertyListSerialization dataWithPropertyList:profile format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    return *output ? 0 : EINVAL;
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

BOOL LSBExperimentalHapticsEnabled(void) {
    const char *value = getenv("LSB_EXPERIMENTAL_HAPTICS");
    return value && !strcmp(value, "1");
}

int LSBInstallGuestDeviceCapabilitiesWithNativeHaptics(NSString *root, NSData *data) {
    if (!LSBExperimentalHapticsEnabled()) return LSBInstallGuestDeviceCapabilities(root, data);
    NSData *adapted = nil;
    int error = LSBProjectGuestHapticsCapability(data, nativeHapticsCapability(), &adapted);
    return error ? error : LSBInstallGuestDeviceCapabilities(root, adapted);
}
