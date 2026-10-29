#import "../Host/DeviceConfiguration.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static NSString *fixtureRoot(NSString *parent, NSString *name) {
    NSString *path = [parent stringByAppendingPathComponent:name];
    assert([NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:NULL]);
    return path;
}

static NSString *livePath(NSString *root) {
    return [root stringByAppendingPathComponent:@"Library/Developer/CoreSimulator/DeviceType/capabilities.plist"];
}

static NSString *stagedPath(NSString *root, LSBGuestDeviceType *type) {
    return [type capabilitiesURLInRuntimeRoot:[NSURL fileURLWithPath:root isDirectory:YES]].path;
}

static NSDictionary *profile(LSBGuestDeviceType *type, NSString *key, id replacement) {
    NSMutableDictionary *display = [@{@"deviceName": @"primary", @"width": @(type.width), @"height": @(type.height),
        @"scale": @(type.scale), @"hdpi": @(type.pitch), @"vdpi": @(type.pitch)} mutableCopy];
    if (key) display[key] = replacement;
    return @{@"capabilities": @{@"modelIdentifier": type.modelIdentifier, @"productClass": type.productClass, @"displays": @[display.copy]}};
}

static void writeData(NSString *path, NSData *data) {
    assert([NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent
        withIntermediateDirectories:YES attributes:nil error:NULL]);
    assert([data writeToFile:path options:0 error:NULL]);
}

static void writeProfile(NSString *path, id value) {
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:value format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
    assert(data.length);
    writeData(path, data);
}

static NSDictionary *readProfile(NSString *path) {
    return [NSDictionary dictionaryWithContentsOfURL:[NSURL fileURLWithPath:path] error:NULL];
}

static void assertCopyError(NSString *root, LSBGuestDeviceType *type, int expected) {
    NSDictionary *result = @{@"not": @"returned"};
    int error = LSBCopyGuestDeviceCapabilities(root, type, &result);
    if (!error || (expected && error != expected) || result)
        fprintf(stderr, "copy root=%s type=%s error=%d expected=%d result=%p\n", root.UTF8String, type.identifier.UTF8String, error, expected, result);
    assert(error && (!expected || error == expected) && !result);
}

static void preflightTests(NSString *parent) {
    NSString *root = fixtureRoot(parent, @"preflight");
    LSBGuestDeviceType *device = LSBDefaultGuestDeviceType(), *other = LSBGuestDeviceTypeForIdentifier(@"iphone-11");
    NSDictionary *good = profile(device, nil, nil), *copied = nil;
    assertCopyError(@"relative", device, EINVAL);
    assert(LSBCopyGuestDeviceCapabilities(root, device, NULL) == EINVAL);
    assertCopyError(root, nil, EINVAL);
    assertCopyError(root, device, ENOENT);
    assertCopyError(root, other, ENOENT);

    // Only a genuinely missing staged profile may use the legacy default. Malformed input must not silently fall back.
    writeProfile(livePath(root), good);
    assert(!LSBCopyGuestDeviceCapabilities(root, device, &copied) && [copied isEqual:good]);
    assert(LSBCopyStagedGuestDeviceCapabilities(root, device, &copied) == ENOENT && !copied);
    assertCopyError(root, other, ENOENT);
    for (id invalid in @[profile(other, nil, nil), profile(device, @"scale", @"3"), @[], @{}, @{@"capabilities": @YES}]) {
        writeProfile(stagedPath(root, device), invalid);
        assertCopyError(root, device, EINVAL);
    }
    for (NSData *invalid in @[[ @"not a plist" dataUsingEncoding:NSUTF8StringEncoding], [NSData data],
                              [NSMutableData dataWithLength:2 * 1024 * 1024 + 1]]) {
        writeData(stagedPath(root, device), invalid);
        assertCopyError(root, device, EINVAL);
    }
    for (LSBGuestDeviceType *type in LSBGuestDeviceTypes()) {
        NSDictionary *original = profile(type, nil, nil);
        writeProfile(stagedPath(root, type), original);
        assert(!LSBCopyStagedGuestDeviceCapabilities(root, type, &copied) && [copied isEqual:original]);
        assert(![copied isKindOfClass:NSMutableDictionary.class] && ![copied[@"capabilities"] isKindOfClass:NSMutableDictionary.class]);
        writeData(stagedPath(root, type), [NSData data]);
        assert([copied isEqual:original]); // The parsed preflight snapshot survives later source replacement.
        writeProfile(stagedPath(root, type), original);
    }
}

static void hapticsProjectionTests(NSString *parent) {
    const char *priorValue = getenv("LSB_EXPERIMENTAL_HAPTICS");
    NSString *prior = priorValue ? @(priorValue) : nil;
    unsetenv("LSB_EXPERIMENTAL_HAPTICS");
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSDictionary *priorDefaults = [defaults volatileDomainForName:NSArgumentDomain];
    NSMutableDictionary *domain = [priorDefaults mutableCopy];
    domain[@"LSBExperimentalHapticsEnabled"] = @NO;
    [defaults setVolatileDomain:domain forName:NSArgumentDomain];
    assert(!LSBExperimentalHapticsEnabled());
    NSArray *answers = @[@YES, @NO, @1, @0, @"true", @{}, NSNull.null];
    for (LSBGuestDeviceType *device in LSBGuestDeviceTypes()) {
        NSDictionary *original = profile(device, nil, nil);
        NSString *root = fixtureRoot(parent, [@"haptics-stock-" stringByAppendingString:device.identifier]);
        assert(!LSBInstallGuestDeviceCapabilitiesWithNativeHaptics(root, original, NO));
        assert([readProfile(livePath(root)) isEqual:original]);
        for (id answer in answers) {
            CFTypeRef native = answer == NSNull.null ? NULL : (__bridge CFTypeRef)answer;
            BOOL supported = native && CFGetTypeID(native) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)native);
            NSDictionary *adapted = nil;
            assert(!LSBProjectGuestHapticsCapability(original, native, &adapted));
            assert(!LSBValidateGuestDeviceCapabilities(adapted, device));
            id value = adapted[@"capabilities"][@"DeviceSupportsClosedLoopHaptics"];
            assert(value && CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() && [value boolValue] == supported);
            NSMutableDictionary *restored = [adapted mutableCopy], *capabilities = [adapted[@"capabilities"] mutableCopy];
            [capabilities removeObjectForKey:@"DeviceSupportsClosedLoopHaptics"];
            restored[@"capabilities"] = capabilities;
            assert([restored isEqual:original]); // Every original identity, geometry and unrelated field is unchanged.
            NSDictionary *repeated = nil, *disabled = nil;
            assert(!LSBProjectGuestHapticsCapability(adapted, native, &repeated) && [repeated isEqual:adapted]);
            assert(!LSBProjectGuestHapticsCapability(adapted, NULL, &disabled));
            value = disabled[@"capabilities"][@"DeviceSupportsClosedLoopHaptics"];
            assert(CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() && ![value boolValue]);
        }
    }
    NSDictionary *output = nil;
    assert(LSBProjectGuestHapticsCapability(nil, kCFBooleanTrue, &output) == EINVAL && !output);
    assert(LSBProjectGuestHapticsCapability(profile(LSBDefaultGuestDeviceType(), nil, nil), kCFBooleanTrue, NULL) == EINVAL);
    for (id invalid in @[@[], @{}, @{@"capabilities": @YES}]) {
        output = @{};
        assert(LSBProjectGuestHapticsCapability(invalid, kCFBooleanTrue, &output) == EINVAL && !output);
    }
    NSString *root = fixtureRoot(parent, @"haptics-publication");
    LSBGuestDeviceType *device = LSBDefaultGuestDeviceType();
    NSDictionary *original = profile(device, nil, nil);
    writeProfile(stagedPath(root, device), original);
    NSData *sourceBytes = [NSData dataWithContentsOfFile:stagedPath(root, device)];
    for (NSString *disabled in @[@"", @"0", @"true", @"01", @"1 "]) {
        assert(!setenv("LSB_EXPERIMENTAL_HAPTICS", disabled.UTF8String, 1) && !LSBExperimentalHapticsEnabled());
        assert(!LSBInstallGuestDeviceCapabilitiesWithNativeHaptics(root, original, NO));
        assert([readProfile(livePath(root)) isEqual:original]);
    }
    assert(!setenv("LSB_EXPERIMENTAL_HAPTICS", "1", 1) && LSBExperimentalHapticsEnabled());
    assert(!LSBInstallGuestDeviceCapabilitiesWithNativeHaptics(root, original, YES));
    NSDictionary *installed = readProfile(livePath(root));
    assert(CFGetTypeID((__bridge CFTypeRef)installed[@"capabilities"][@"DeviceSupportsClosedLoopHaptics"]) == CFBooleanGetTypeID());
    assert([[NSData dataWithContentsOfFile:stagedPath(root, device)] isEqual:sourceBytes]);
    unsetenv("LSB_EXPERIMENTAL_HAPTICS");
    assert(!LSBInstallGuestDeviceCapabilitiesWithNativeHaptics(root, original, NO));
    assert([readProfile(livePath(root)) isEqual:original]); // Disabling restores stock values; serialization may be normalized.
    for (id value in @[@NO, @YES, @0, @1, @"true", @{}, @[]]) {
        domain[@"LSBExperimentalHapticsEnabled"] = value;
        [defaults setVolatileDomain:domain forName:NSArgumentDomain];
        BOOL expected = CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() && [value boolValue];
        for (NSString *environment in @[@"", @"0", @"true", @"01", @"1 "]) {
            assert(!setenv("LSB_EXPERIMENTAL_HAPTICS", environment.UTF8String, 1));
            assert(!LSBHapticsForcedByEnvironment() && LSBExperimentalHapticsEnabled() == expected);
        }
        assert(!setenv("LSB_EXPERIMENTAL_HAPTICS", "1", 1));
        assert(LSBHapticsForcedByEnvironment() && LSBExperimentalHapticsEnabled());
        assert([[defaults objectForKey:@"LSBExperimentalHapticsEnabled"] isEqual:value]);
    }
    [defaults setVolatileDomain:priorDefaults forName:NSArgumentDomain];
    unsetenv("LSB_EXPERIMENTAL_HAPTICS");
    if (prior) assert(!setenv("LSB_EXPERIMENTAL_HAPTICS", prior.UTF8String, 1));
}

static void audioPreferenceTests(void) {
    const char *priorValue = getenv("LSB_AUDIO_OUTPUT");
    NSString *prior = priorValue ? @(priorValue) : nil;
    unsetenv("LSB_AUDIO_OUTPUT");
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSDictionary *priorDefaults = [defaults volatileDomainForName:NSArgumentDomain];
    NSMutableDictionary *domain = [priorDefaults mutableCopy];
    for (id value in @[@NO, @YES, @0, @1, @"true", @{}, @[]]) {
        domain[@"LSBExperimentalAudioOutputEnabled"] = value;
        [defaults setVolatileDomain:domain forName:NSArgumentDomain];
        BOOL expected = CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() && [value boolValue];
        for (NSString *environment in @[@"", @"0", @"true", @"01", @"1 "]) {
            assert(!setenv("LSB_AUDIO_OUTPUT", environment.UTF8String, 1));
            assert(!LSBAudioOutputForcedByEnvironment() && LSBExperimentalAudioOutputEnabled() == expected);
        }
        assert(!setenv("LSB_AUDIO_OUTPUT", "1", 1));
        assert(LSBAudioOutputForcedByEnvironment() && LSBExperimentalAudioOutputEnabled());
        assert([[defaults objectForKey:@"LSBExperimentalAudioOutputEnabled"] isEqual:value]);
    }
    [defaults setVolatileDomain:priorDefaults forName:NSArgumentDomain];
    unsetenv("LSB_AUDIO_OUTPUT");
    if (prior) assert(!setenv("LSB_AUDIO_OUTPUT", prior.UTF8String, 1));
}

static void feedbackSupportProjectionTests(NSString *parent) {
    NSString *key = @"LSBUIKitFeedbackSupportLevel";
    NSArray *levels = @[@0, @1, @2, @(-1), @3, @(NSIntegerMax), @YES, @NO, @0.0, @2.0, @1.5, @"2", @{}, @[], NSNull.null];
    for (LSBGuestDeviceType *device in LSBGuestDeviceTypes()) {
        NSDictionary *stock = profile(device, nil, nil);
        NSMutableDictionary *original = stock.mutableCopy, *values = [stock[@"capabilities"] mutableCopy];
        values[key] = @2;
        original[@"capabilities"] = values.copy; // An invalid new answer must not retain an earlier session's override.
        NSDictionary *before = original.copy;
        for (NSUInteger index = 0; index < levels.count; ++index) {
            id level = levels[index];
            CFTypeRef nativeLevel = level == NSNull.null ? NULL : (__bridge CFTypeRef)level;
            for (id nativeSupport in @[@YES, @NO, @1, NSNull.null]) {
                CFTypeRef closedLoop = nativeSupport == NSNull.null ? NULL : (__bridge CFTypeRef)nativeSupport;
                NSDictionary *adapted = nil;
                assert(!LSBProjectGuestHapticsCapabilities(original, closedLoop, nativeLevel, &adapted));
                assert(!LSBValidateGuestDeviceCapabilities(adapted, device) && [original isEqual:before]);
                assert(![adapted isKindOfClass:NSMutableDictionary.class] && ![adapted[@"capabilities"] isKindOfClass:NSMutableDictionary.class]);
                NSData *serialized = [NSPropertyListSerialization dataWithPropertyList:adapted format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
                NSDictionary *roundTrip = [NSPropertyListSerialization propertyListWithData:serialized options:NSPropertyListImmutable format:NULL error:NULL];
                id emitted = roundTrip[@"capabilities"][key];
                if (index < 3) {
                    assert(emitted && CFGetTypeID((__bridge CFTypeRef)emitted) == CFNumberGetTypeID());
                    assert(!CFNumberIsFloatType((__bridge CFNumberRef)emitted) && [emitted integerValue] == (NSInteger)index);
                } else assert(!emitted);
                id supported = roundTrip[@"capabilities"][@"DeviceSupportsClosedLoopHaptics"];
                BOOL expected = closedLoop && CFGetTypeID(closedLoop) == CFBooleanGetTypeID() && CFBooleanGetValue(closedLoop);
                assert(CFGetTypeID((__bridge CFTypeRef)supported) == CFBooleanGetTypeID() && [supported boolValue] == expected);
                NSMutableDictionary *restored = adapted.mutableCopy, *capabilities = [adapted[@"capabilities"] mutableCopy];
                [capabilities removeObjectForKey:key];
                [capabilities removeObjectForKey:@"DeviceSupportsClosedLoopHaptics"];
                restored[@"capabilities"] = capabilities;
                assert([restored isEqual:stock]); // Selected device identity, geometry and unrelated capabilities remain stock.
            }
        }
        NSString *root = fixtureRoot(parent, [@"feedback-level-" stringByAppendingString:device.identifier]);
        NSDictionary *adapted = nil;
        assert(!LSBProjectGuestHapticsCapabilities(stock, kCFBooleanTrue, (__bridge CFTypeRef)@2, &adapted));
        assert(!LSBInstallGuestDeviceCapabilities(root, adapted) && [readProfile(livePath(root)) isEqual:adapted]);
        assert(!LSBInstallGuestDeviceCapabilitiesWithNativeHaptics(root, stock, NO));
        assert([readProfile(livePath(root)) isEqual:stock]); // Disabling publishes the untouched staged profile, with no LSB-only level.
    }
    NSDictionary *output = @{};
    assert(LSBProjectGuestHapticsCapabilities(nil, kCFBooleanTrue, (__bridge CFTypeRef)@2, &output) == EINVAL && !output);
    assert(LSBProjectGuestHapticsCapabilities(profile(LSBDefaultGuestDeviceType(), nil, nil), kCFBooleanTrue, NULL, NULL) == EINVAL);
}

static void staticPathTests(NSString *parent) {
    LSBGuestDeviceType *device = LSBDefaultGuestDeviceType();
    NSDictionary *good = profile(device, nil, nil);
    NSArray<NSString *> *components = @[@"Library", @"Library/Developer", @"Library/Developer/CoreSimulator",
        @"Library/Developer/CoreSimulator/DeviceTypes", @"Library/Developer/CoreSimulator/DeviceTypes/iphone-15-pro-max"];
    unsigned index = 0;
    for (NSString *component in components) {
        NSString *root = fixtureRoot(parent, [NSString stringWithFormat:@"source-link-%u", index++]);
        writeProfile(stagedPath(root, device), good);
        writeProfile(livePath(root), good);
        NSString *path = [root stringByAppendingPathComponent:component], *target = [path stringByAppendingString:@"-original"];
        assert(!rename(path.fileSystemRepresentation, target.fileSystemRepresentation));
        assert(!symlink(target.fileSystemRepresentation, path.fileSystemRepresentation));
        assertCopyError(root, device, ELOOP);
    }
    NSString *root = fixtureRoot(parent, @"source-special-file");
    NSString *file = stagedPath(root, device), *target = [root stringByAppendingPathComponent:@"original.plist"];
    writeProfile(target, good); writeProfile(file, good); writeProfile(livePath(root), good);
    assert(!unlink(file.fileSystemRepresentation) && !symlink(target.fileSystemRepresentation, file.fileSystemRepresentation));
    assertCopyError(root, device, ELOOP);
    assert(!unlink(file.fileSystemRepresentation) && !mkdir(file.fileSystemRepresentation, 0700));
    assertCopyError(root, device, EINVAL);
    assert(!rmdir(file.fileSystemRepresentation) && !mkfifo(file.fileSystemRepresentation, 0600));
    assertCopyError(root, device, EINVAL); // Static regular-file check prevents blocking on a FIFO.
    assert(!unlink(file.fileSystemRepresentation));
    NSString *legacy = livePath(root);
    assert(!unlink(legacy.fileSystemRepresentation) && !symlink(target.fileSystemRepresentation, legacy.fileSystemRepresentation));
    assertCopyError(root, device, ELOOP);
    assert(!unlink(legacy.fileSystemRepresentation) && !mkfifo(legacy.fileSystemRepresentation, 0600));
    assertCopyError(root, device, EINVAL);
    assert(!unlink(legacy.fileSystemRepresentation));
    writeProfile(legacy, good);

    NSString *rootLink = [parent stringByAppendingPathComponent:@"runtime-link"];
    assert(!symlink(root.fileSystemRepresentation, rootLink.fileSystemRepresentation));
    assertCopyError(rootLink, device, ELOOP);
    assert(LSBInstallGuestDeviceCapabilities(rootLink, good) == ELOOP);
    for (NSString *component in @[@"Library", @"Library/Developer", @"Library/Developer/CoreSimulator",
                                 @"Library/Developer/CoreSimulator/DeviceType"]) {
        root = fixtureRoot(parent, [NSString stringWithFormat:@"destination-link-%u", index++]);
        writeProfile(livePath(root), good);
        NSString *path = [root stringByAppendingPathComponent:component], *outside = [path stringByAppendingString:@"-original"];
        assert(!rename(path.fileSystemRepresentation, outside.fileSystemRepresentation));
        assert(!symlink(outside.fileSystemRepresentation, path.fileSystemRepresentation));
        assert(LSBInstallGuestDeviceCapabilities(root, good) == ELOOP);
    }
}

static void atomicInstallTests(NSString *parent) {
    NSString *root = fixtureRoot(parent, @"atomic-install");
    LSBGuestDeviceType *first = LSBDefaultGuestDeviceType(), *second = LSBGuestDeviceTypeForIdentifier(@"ipad-a16");
    NSDictionary *old = profile(first, nil, nil), *replacement = profile(second, nil, nil), *copied = nil;
    writeProfile(stagedPath(root, first), old); writeProfile(stagedPath(root, second), replacement);
    assert(!LSBInstallGuestDeviceCapabilities(root, old));
    NSString *live = livePath(root);
    NSData *oldBytes = [NSData dataWithContentsOfFile:live];
    int reader = open(live.fileSystemRepresentation, O_RDONLY | O_CLOEXEC);
    assert(reader >= 0);
    struct stat before, after;
    assert(!fstat(reader, &before));
    assert(!LSBCopyGuestDeviceCapabilities(root, second, &copied) && [copied isEqual:replacement]);
    assert(!LSBInstallGuestDeviceCapabilities(root, copied));
    assert(!stat(live.fileSystemRepresentation, &after) && before.st_ino != after.st_ino);
    assert([readProfile(live) isEqual:replacement]);
    NSMutableData *stillOld = [NSMutableData dataWithLength:oldBytes.length];
    assert(read(reader, stillOld.mutableBytes, stillOld.length) == (ssize_t)stillOld.length && [stillOld isEqual:oldBytes]);
    assert(!close(reader));
    assert([readProfile(stagedPath(root, first)) isEqual:old] && [readProfile(stagedPath(root, second)) isEqual:replacement]);

    NSString *target = [root stringByAppendingPathComponent:@"unrelated-original.plist"];
    writeProfile(target, old);
    assert(!unlink(live.fileSystemRepresentation) && !symlink(target.fileSystemRepresentation, live.fileSystemRepresentation));
    assert(LSBInstallGuestDeviceCapabilities(root, replacement) == ELOOP);
    assert([readProfile(target) isEqual:old]); // Refuse an existing destination symlink; do not write its target.
    assert(!unlink(live.fileSystemRepresentation) && !LSBInstallGuestDeviceCapabilities(root, replacement));
    assert(LSBInstallGuestDeviceCapabilities(@"relative", replacement) == EINVAL);
    assert(LSBInstallGuestDeviceCapabilities(root, nil) == EINVAL && LSBInstallGuestDeviceCapabilities(root, @{}) == EINVAL);
    assert(LSBInstallGuestDeviceCapabilities(root, @{@"capabilities": @{}, @"large": [NSMutableData dataWithLength:2 * 1024 * 1024]}) == EINVAL);
    assert([readProfile(live) isEqual:replacement]);
    if (geteuid()) {
        NSString *directory = live.stringByDeletingLastPathComponent;
        assert(!chmod(directory.fileSystemRepresentation, 0500));
        assert(LSBInstallGuestDeviceCapabilities(root, old));
        assert(!chmod(directory.fileSystemRepresentation, 0755));
        assert([readProfile(live) isEqual:replacement]); // Failed atomic publication leaves the active profile intact.
    }
    assert(!unlink(live.fileSystemRepresentation) && !mkdir(live.fileSystemRepresentation, 0700));
    assert(LSBInstallGuestDeviceCapabilities(root, replacement));
    NSArray *entries = [NSFileManager.defaultManager contentsOfDirectoryAtPath:live.stringByDeletingLastPathComponent error:NULL];
    assert(entries.count == 1 && [entries[0] isEqualToString:@"capabilities.plist"]);
}

static void preferenceLookupTests(void) {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSDictionary *previous = [defaults volatileDomainForName:NSArgumentDomain];
    for (id value in @[@42, @"", @"unknown-profile", @"../iphone-8", @"iphone-8", @"ipad-a16"]) {
        NSMutableDictionary *domain = [previous mutableCopy];
        domain[@"LSBSimulatorDeviceType"] = value;
        [defaults setVolatileDomain:domain forName:NSArgumentDomain];
        LSBGuestDeviceType *expected = [value isKindOfClass:NSString.class] ? LSBGuestDeviceTypeForIdentifier(value) : nil;
        assert(LSBConfiguredGuestDeviceType() == (expected ?: LSBDefaultGuestDeviceType()));
    }
    [defaults setVolatileDomain:previous forName:NSArgumentDomain];
}

int main(void) {
    alarm(20);
    @autoreleasepool {
        char directory[] = "/private/tmp/LSBDeviceConfiguration.XXXXXX";
        assert(mkdtemp(directory));
        NSString *parent = @(directory);
        preflightTests(parent);
        hapticsProjectionTests(parent);
        feedbackSupportProjectionTests(parent);
        audioPreferenceTests();
        staticPathTests(parent);
        atomicInstallTests(parent);
        preferenceLookupTests();
        assert([NSFileManager.defaultManager removeItemAtPath:parent error:NULL]);
    }
    puts("DeviceConfigurationTests passed");
    return 0;
}
