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

static NSData *profileData(LSBGuestDeviceType *type, NSString *key, id replacement) {
    NSMutableDictionary *display = [@{@"deviceName": @"primary", @"width": @(type.width), @"height": @(type.height),
        @"scale": @(type.scale), @"hdpi": @(type.pitch), @"vdpi": @(type.pitch)} mutableCopy];
    if (key) display[key] = replacement;
    NSDictionary *profile = @{@"capabilities": @{@"modelIdentifier": type.modelIdentifier, @"productClass": type.productClass,
        @"displays": @[display]}};
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:profile format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    assert(data.length);
    return data;
}

static void writeData(NSString *path, NSData *data) {
    assert([NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent
        withIntermediateDirectories:YES attributes:nil error:NULL]);
    assert([data writeToFile:path options:0 error:NULL]);
}

static void assertCopyError(NSString *root, LSBGuestDeviceType *type, int expected) {
    NSData *data = [@"not returned" dataUsingEncoding:NSUTF8StringEncoding];
    int error = LSBCopyGuestDeviceCapabilities(root, type, &data);
    assert(error && (!expected || error == expected) && !data);
}

static void preflightTests(NSString *parent) {
    NSString *root = fixtureRoot(parent, @"preflight");
    LSBGuestDeviceType *device = LSBDefaultGuestDeviceType(), *other = LSBGuestDeviceTypeForIdentifier(@"iphone-11");
    NSData *good = profileData(device, nil, nil), *copied = nil;
    assert(LSBCopyGuestDeviceCapabilities(@"relative", device, &copied) == EINVAL);
    assert(LSBCopyGuestDeviceCapabilities(root, device, NULL) == EINVAL);
    assert(LSBCopyGuestDeviceCapabilities(root, nil, &copied) == EINVAL);
    assertCopyError(root, device, ENOENT);
    assertCopyError(root, other, ENOENT);

    // Legacy installs serve only the catalog default, and only when the staged original is absent.
    writeData(livePath(root), good);
    assert(!LSBCopyGuestDeviceCapabilities(root, device, &copied) && [copied isEqualToData:good]);
    assertCopyError(root, other, ENOENT);
    writeData(stagedPath(root, device), profileData(other, nil, nil));
    assertCopyError(root, device, EINVAL); // A mismatch cannot quietly use the otherwise valid legacy profile.
    writeData(stagedPath(root, device), profileData(device, @"scale", @"3"));
    assertCopyError(root, device, EINVAL);
    writeData(stagedPath(root, device), [@"not a plist" dataUsingEncoding:NSUTF8StringEncoding]);
    assertCopyError(root, device, EINVAL);
    writeData(stagedPath(root, device), [NSData data]);
    assertCopyError(root, device, EINVAL);
    writeData(stagedPath(root, device), [NSMutableData dataWithLength:2 * 1024 * 1024 + 1]);
    assertCopyError(root, device, EINVAL);

    for (LSBGuestDeviceType *type in LSBGuestDeviceTypes()) {
        NSData *original = profileData(type, nil, nil);
        writeData(stagedPath(root, type), original);
        copied = nil;
        assert(!LSBCopyGuestDeviceCapabilities(root, type, &copied) && [copied isEqualToData:original]);
        writeData(stagedPath(root, type), [NSData data]);
        assert([copied isEqualToData:original]); // A preflight result owns its bytes even if the source changes later.
        writeData(stagedPath(root, type), original);
    }
}

static void hapticsProjectionTests(NSString *parent) {
    const char *priorValue = getenv("LSB_EXPERIMENTAL_HAPTICS");
    NSString *prior = priorValue ? @(priorValue) : nil;
    unsetenv("LSB_EXPERIMENTAL_HAPTICS");
    assert(!LSBExperimentalHapticsEnabled());
    NSArray *answers = @[@YES, @NO, @1, @0, @"true", @{}, NSNull.null];
    for (LSBGuestDeviceType *device in LSBGuestDeviceTypes()) {
        NSData *original = profileData(device, nil, nil);
        NSString *root = fixtureRoot(parent, [@"haptics-stock-" stringByAppendingString:device.identifier]);
        assert(!LSBInstallGuestDeviceCapabilitiesWithNativeHaptics(root, original));
        assert([[NSData dataWithContentsOfFile:livePath(root)] isEqualToData:original]);
        NSDictionary *before = [NSPropertyListSerialization propertyListWithData:original options:0 format:NULL error:NULL];
        for (id answer in answers) {
            CFTypeRef native = answer == NSNull.null ? NULL : (__bridge CFTypeRef)answer;
            BOOL supported = native && CFGetTypeID(native) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)native);
            NSData *adapted = nil;
            assert(!LSBProjectGuestHapticsCapability(original, native, &adapted));
            assert(!LSBValidateGuestDeviceCapabilities(adapted, device));
            NSDictionary *after = [NSPropertyListSerialization propertyListWithData:adapted options:0 format:NULL error:NULL];
            id value = after[@"capabilities"][@"DeviceSupportsClosedLoopHaptics"];
            assert(value && CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() && [value boolValue] == supported);
            NSMutableDictionary *restored = [after mutableCopy], *capabilities = [after[@"capabilities"] mutableCopy];
            [capabilities removeObjectForKey:@"DeviceSupportsClosedLoopHaptics"];
            restored[@"capabilities"] = capabilities;
            assert([restored isEqualToDictionary:before]); // Identity, geometry, and every other profile field are unchanged.
            NSData *repeated = nil;
            assert(!LSBProjectGuestHapticsCapability(adapted, native, &repeated) && [repeated isEqualToData:adapted]);
            NSData *disabled = nil;
            assert(!LSBProjectGuestHapticsCapability(adapted, NULL, &disabled));
            NSDictionary *unknown = [NSPropertyListSerialization propertyListWithData:disabled options:0 format:NULL error:NULL];
            assert(CFGetTypeID((__bridge CFTypeRef)unknown[@"capabilities"][@"DeviceSupportsClosedLoopHaptics"]) == CFBooleanGetTypeID());
            assert(![unknown[@"capabilities"][@"DeviceSupportsClosedLoopHaptics"] boolValue]); // A restored generated profile cannot retain stale true.
        }
    }
    NSData *output = nil;
    assert(LSBProjectGuestHapticsCapability(nil, kCFBooleanTrue, &output) == EINVAL && !output);
    assert(LSBProjectGuestHapticsCapability(profileData(LSBDefaultGuestDeviceType(), nil, nil), kCFBooleanTrue, NULL) == EINVAL);
    for (id invalid in @[@[], @{}, @{@"capabilities": @YES}]) {
        NSData *data = [NSPropertyListSerialization dataWithPropertyList:invalid format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
        output = [NSData data];
        assert(LSBProjectGuestHapticsCapability(data, kCFBooleanTrue, &output) == EINVAL && !output);
    }
    NSString *root = fixtureRoot(parent, @"haptics-publication");
    LSBGuestDeviceType *device = LSBDefaultGuestDeviceType();
    NSData *original = profileData(device, nil, nil);
    writeData(stagedPath(root, device), original);
    for (NSString *disabled in @[@"", @"0", @"true", @"01", @"1 "]) {
        assert(!setenv("LSB_EXPERIMENTAL_HAPTICS", disabled.UTF8String, 1) && !LSBExperimentalHapticsEnabled());
        assert(!LSBInstallGuestDeviceCapabilitiesWithNativeHaptics(root, original));
        assert([[NSData dataWithContentsOfFile:livePath(root)] isEqualToData:original]);
    }
    assert(!setenv("LSB_EXPERIMENTAL_HAPTICS", "1", 1) && LSBExperimentalHapticsEnabled());
    assert(!LSBInstallGuestDeviceCapabilitiesWithNativeHaptics(root, original));
    NSDictionary *installed = [NSPropertyListSerialization propertyListWithData:[NSData dataWithContentsOfFile:livePath(root)]
                                                                      options:0 format:NULL error:NULL];
    assert(CFGetTypeID((__bridge CFTypeRef)installed[@"capabilities"][@"DeviceSupportsClosedLoopHaptics"]) == CFBooleanGetTypeID());
    assert([[NSData dataWithContentsOfFile:stagedPath(root, device)] isEqualToData:original]);
    unsetenv("LSB_EXPERIMENTAL_HAPTICS");
    assert(!LSBInstallGuestDeviceCapabilitiesWithNativeHaptics(root, original));
    assert([[NSData dataWithContentsOfFile:livePath(root)] isEqualToData:original]); // Disabling restores the staged stock bytes.
    if (prior) assert(!setenv("LSB_EXPERIMENTAL_HAPTICS", prior.UTF8String, 1));
}

static void noFollowTests(NSString *parent) {
    LSBGuestDeviceType *device = LSBDefaultGuestDeviceType();
    NSData *good = profileData(device, nil, nil);
    NSArray<NSString *> *stagedComponents = @[@"Library", @"Library/Developer", @"Library/Developer/CoreSimulator",
        @"Library/Developer/CoreSimulator/DeviceTypes", @"Library/Developer/CoreSimulator/DeviceTypes/iphone-15-pro-max"];
    unsigned index = 0;
    for (NSString *component in stagedComponents) {
        NSString *root = fixtureRoot(parent, [NSString stringWithFormat:@"source-link-%u", index++]);
        writeData(stagedPath(root, device), good);
        writeData(livePath(root), good);
        NSString *path = [root stringByAppendingPathComponent:component], *target = [path stringByAppendingString:@"-original"];
        assert(!rename(path.fileSystemRepresentation, target.fileSystemRepresentation));
        assert(!symlink(target.fileSystemRepresentation, path.fileSystemRepresentation));
        assertCopyError(root, device, 0); // A forbidden staged path must not trigger the legacy fallback.
    }
    NSString *root = fixtureRoot(parent, @"source-file-link");
    NSString *file = stagedPath(root, device), *target = [root stringByAppendingPathComponent:@"original.plist"];
    writeData(target, good);
    writeData(file, good);
    writeData(livePath(root), good);
    assert(!unlink(file.fileSystemRepresentation));
    assert(!symlink(target.fileSystemRepresentation, file.fileSystemRepresentation));
    assertCopyError(root, device, 0);
    assert(!unlink(file.fileSystemRepresentation));
    assert(!mkdir(file.fileSystemRepresentation, 0700));
    assertCopyError(root, device, EINVAL);
    assert(!rmdir(file.fileSystemRepresentation));
    assert(!mkfifo(file.fileSystemRepresentation, 0600));
    assertCopyError(root, device, EINVAL); // Nonblocking open must reach fstat rather than waiting for a writer on the UI thread.
    assert(!unlink(file.fileSystemRepresentation));
    NSString *legacy = livePath(root);
    assert(!unlink(legacy.fileSystemRepresentation));
    assert(!symlink(target.fileSystemRepresentation, legacy.fileSystemRepresentation));
    assertCopyError(root, device, 0);
    assert(!unlink(legacy.fileSystemRepresentation));
    assert(!mkfifo(legacy.fileSystemRepresentation, 0600));
    assertCopyError(root, device, EINVAL); // The compatibility fallback has the same no-follow/nonblocking rules.
    assert(!unlink(legacy.fileSystemRepresentation));
    writeData(legacy, good);

    NSString *rootLink = [parent stringByAppendingPathComponent:@"runtime-link"];
    assert(!symlink(root.fileSystemRepresentation, rootLink.fileSystemRepresentation));
    assertCopyError(rootLink, device, 0);
    assert(LSBInstallGuestDeviceCapabilities(rootLink, good));

    NSArray<NSString *> *liveComponents = @[@"Library", @"Library/Developer", @"Library/Developer/CoreSimulator",
        @"Library/Developer/CoreSimulator/DeviceType"];
    for (NSString *component in liveComponents) {
        root = fixtureRoot(parent, [NSString stringWithFormat:@"destination-link-%u", index++]);
        writeData(livePath(root), good);
        NSString *path = [root stringByAppendingPathComponent:component], *outside = [path stringByAppendingString:@"-original"];
        assert(!rename(path.fileSystemRepresentation, outside.fileSystemRepresentation));
        assert(!symlink(outside.fileSystemRepresentation, path.fileSystemRepresentation));
        assert(LSBInstallGuestDeviceCapabilities(root, good));
    }
}

static void atomicInstallTests(NSString *parent) {
    NSString *root = fixtureRoot(parent, @"atomic-install");
    LSBGuestDeviceType *first = LSBDefaultGuestDeviceType(), *second = LSBGuestDeviceTypeForIdentifier(@"ipad-a16");
    NSData *old = profileData(first, nil, nil), *replacement = profileData(second, nil, nil), *copied = nil;
    writeData(stagedPath(root, first), old);
    writeData(stagedPath(root, second), replacement);
    assert(!LSBInstallGuestDeviceCapabilities(root, old));
    NSString *live = livePath(root);
    int reader = open(live.fileSystemRepresentation, O_RDONLY | O_CLOEXEC);
    assert(reader >= 0);
    struct stat before, after;
    assert(!fstat(reader, &before));
    assert(!LSBCopyGuestDeviceCapabilities(root, second, &copied) && [copied isEqualToData:replacement]);
    assert(!LSBInstallGuestDeviceCapabilities(root, copied));
    assert(!stat(live.fileSystemRepresentation, &after) && before.st_ino != after.st_ino);
    assert([[NSData dataWithContentsOfFile:live] isEqualToData:replacement]);
    NSMutableData *stillOld = [NSMutableData dataWithLength:old.length];
    assert(read(reader, stillOld.mutableBytes, stillOld.length) == (ssize_t)stillOld.length && [stillOld isEqualToData:old]);
    assert(!close(reader));
    assert([[NSData dataWithContentsOfFile:stagedPath(root, first)] isEqualToData:old]);
    assert([[NSData dataWithContentsOfFile:stagedPath(root, second)] isEqualToData:replacement]);

    // Replacing a final symlink is safe: its target is never opened or rewritten by installation.
    NSString *target = [root stringByAppendingPathComponent:@"unrelated-original.plist"];
    writeData(target, old);
    assert(!unlink(live.fileSystemRepresentation));
    assert(!symlink(target.fileSystemRepresentation, live.fileSystemRepresentation));
    assert(!LSBInstallGuestDeviceCapabilities(root, replacement));
    assert(!lstat(live.fileSystemRepresentation, &after) && S_ISREG(after.st_mode));
    assert([[NSData dataWithContentsOfFile:target] isEqualToData:old]);
    assert([[NSData dataWithContentsOfFile:live] isEqualToData:replacement]);

    assert(LSBInstallGuestDeviceCapabilities(@"relative", replacement) == EINVAL);
    assert(LSBInstallGuestDeviceCapabilities(root, nil) == EINVAL);
    assert(LSBInstallGuestDeviceCapabilities(root, [NSData data]) == EINVAL);
    assert(LSBInstallGuestDeviceCapabilities(root, [NSMutableData dataWithLength:2 * 1024 * 1024 + 1]) == EINVAL);
    assert([[NSData dataWithContentsOfFile:live] isEqualToData:replacement]);
    assert(!unlink(live.fileSystemRepresentation) && !mkdir(live.fileSystemRepresentation, 0700));
    assert(LSBInstallGuestDeviceCapabilities(root, replacement));
    NSArray *entries = [NSFileManager.defaultManager contentsOfDirectoryAtPath:live.stringByDeletingLastPathComponent error:NULL];
    assert(entries.count == 1 && [entries[0] isEqualToString:@"capabilities.plist"]); // Failed publication cleaned only its temporary.
}

static void preferenceLookupTests(void) {
    // The argument domain is process-local. Do not write the real host's preference domain from a test binary.
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
        noFollowTests(parent);
        atomicInstallTests(parent);
        preferenceLookupTests();
        assert([NSFileManager.defaultManager removeItemAtPath:parent error:NULL]);
    }
    puts("DeviceConfigurationTests passed");
    return 0;
}
