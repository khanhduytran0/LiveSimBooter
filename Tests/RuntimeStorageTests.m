#import <Foundation/Foundation.h>
#import "../Host/RuntimeStorage.h"
#include "../ProcessSupport/RuntimeLayout.h"
#include <assert.h>
#include <errno.h>
#include <unistd.h>

static NSArray<NSString *> *signedGroups(void) {
    return @[@"group.com.SideStore.SideStore.ABCDE12345", @"group.com.rileytestut.AltStore.ABCDE12345"];
}

@interface StorageFixture : NSObject
@property (nonatomic) NSURL *root, *sideStore, *altStore;
@property (nonatomic) NSMutableArray<NSString *> *lookups;
- (LSBRuntimeStorage *)resolve;
@end
@implementation StorageFixture
- (instancetype)init {
    if (!(self = [super init])) return nil;
    NSString *name = [@"LSBRuntimeStorage-" stringByAppendingString:NSUUID.UUID.UUIDString];
    _root = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:name] isDirectory:YES];
    _sideStore = [_root URLByAppendingPathComponent:@"SideStore" isDirectory:YES];
    _altStore = [_root URLByAppendingPathComponent:@"AltStore" isDirectory:YES];
    _lookups = [NSMutableArray new];
    assert([NSFileManager.defaultManager createDirectoryAtURL:_sideStore withIntermediateDirectories:YES attributes:nil error:NULL]);
    assert([NSFileManager.defaultManager createDirectoryAtURL:_altStore withIntermediateDirectories:YES attributes:nil error:NULL]);
    return self;
}
- (void)dealloc { assert([NSFileManager.defaultManager removeItemAtURL:_root error:NULL]); }
- (LSBRuntimeStorage *)resolve {
    return [[LSBRuntimeStorage alloc] initWithAppGroupIdentifiers:signedGroups() containerProvider:^NSURL *(NSString *identifier) {
        [self.lookups addObject:identifier];
        return [identifier isEqual:signedGroups()[0]] ? self.sideStore : self.altStore;
    }];
}
@end

static NSURL *child(NSURL *url, NSString *name) { return [url URLByAppendingPathComponent:name]; }
static void directory(NSURL *url) {
    assert([NSFileManager.defaultManager createDirectoryAtURL:url withIntermediateDirectories:YES attributes:nil error:NULL]);
}
static void file(NSURL *url) { assert([[@"occupied" dataUsingEncoding:NSUTF8StringEncoding] writeToURL:url atomically:NO]); }
static void removeItem(NSURL *url) { assert([NSFileManager.defaultManager removeItemAtURL:url error:NULL]); }
static void symlinkTo(NSURL *url, NSURL *target) { assert(!symlink(target.fileSystemRepresentation, url.fileSystemRepresentation)); }

static void assertSelected(LSBRuntimeStorage *storage, NSURL *container, NSString *identifier) {
    assert(!storage.error && [storage.appGroupIdentifier isEqual:identifier]);
    assert([storage.containerURL.path isEqual:container.path]);
    assert([storage.storageURL.path isEqual:child(container, @LSB_STORAGE_BUILD_RELATIVE_PATH).path]);
    assert([storage.runtimeRootURL.path isEqual:child(container, @LSB_STORAGE_BUILD_RELATIVE_PATH "/" LSB_RUNTIME_DIRECTORY).path]);
    assert([storage.dataURL.path isEqual:child(container, @LSB_STORAGE_BUILD_RELATIVE_PATH "/" LSB_DATA_DIRECTORY).path]);
}
static void assertFailure(LSBRuntimeStorage *storage, int error) {
    assert(storage.error && [storage.error.domain isEqual:NSPOSIXErrorDomain] && (!error || storage.error.code == error));
    assert(!storage.appGroupIdentifier && !storage.containerURL && !storage.storageURL && !storage.runtimeRootURL && !storage.dataURL);
}

static void testPreferenceAndNoWrites(void) {
    StorageFixture *fixture = [StorageFixture new];
    NSString *side = signedGroups()[0], *alt = signedGroups()[1];
    assertSelected([fixture resolve], fixture.sideStore, side); // Both empty: SideStore wins deterministically.
    assert(![NSFileManager.defaultManager fileExistsAtPath:child(fixture.sideStore, @LSB_STORAGE_DIRECTORY).path]);
    assert(![NSFileManager.defaultManager fileExistsAtPath:child(fixture.altStore, @LSB_STORAGE_DIRECTORY).path]);
    directory(child(fixture.altStore, @"Apps"));
    assertSelected([fixture resolve], fixture.altStore, alt);
    directory(child(fixture.sideStore, @"Apps"));
    assertSelected([fixture resolve], fixture.sideStore, side);
    directory(child(fixture.altStore, @LSB_STORAGE_DIRECTORY));
    assertSelected([fixture resolve], fixture.altStore, alt); // Existing runtime storage outranks the other active store.
    directory(child(fixture.sideStore, @LSB_STORAGE_DIRECTORY));
    directory(child(fixture.sideStore, @LSB_STORAGE_BUILD_RELATIVE_PATH "/" LSB_RUNTIME_DIRECTORY));
    directory(child(fixture.sideStore, @LSB_STORAGE_BUILD_RELATIVE_PATH "/" LSB_DATA_DIRECTORY));
    assertSelected([fixture resolve], fixture.sideStore, side);
    assert(fixture.lookups.count == 10);
}

static void testUnavailableAndStableSelection(void) {
    StorageFixture *fixture = [StorageFixture new];
    NSURL *side = fixture.sideStore;
    fixture.sideStore = nil;
    assertSelected([fixture resolve], fixture.altStore, signedGroups()[1]);
    fixture.altStore = nil;
    assertFailure([fixture resolve], EACCES);
    fixture.sideStore = side;
    LSBRuntimeStorage *storage = [fixture resolve];
    assertSelected(storage, side, signedGroups()[0]);
    NSUInteger lookups = fixture.lookups.count;
    fixture.altStore = child(fixture.root, @"AltStore");
    directory(child(fixture.altStore, @LSB_STORAGE_DIRECTORY));
    // The resolver's snapshot is immutable; a marker created by any later consumer cannot switch an existing instance.
    assertSelected(storage, side, signedGroups()[0]);
    assert(fixture.lookups.count == lookups);
    assertSelected([fixture resolve], fixture.altStore, signedGroups()[1]);
}

static void testInvalidIdentifiers(void) {
    const char embeddedBytes[] = "group.com.SideStore.SideStore\0other";
    NSString *embedded = [[NSString alloc] initWithBytes:embeddedBytes length:sizeof(embeddedBytes) - 1 encoding:NSUTF8StringEncoding];
    NSArray *invalid = @[@[], @[@7], @[@"group."], @[@"group.com.SideStore.SideStore$(DEVELOPMENT_TEAM_SUFFIX)"],
        @[@"group.com.SideStore.SideStore/wrong"], @[signedGroups()[0], signedGroups()[0]],
        @[@"group.com.SideStore.SideStore", signedGroups()[0]], @[signedGroups()[0], signedGroups()[1], @"group.extra"], @[embedded]];
    for (NSArray *identifiers in invalid) {
        __block unsigned calls = 0;
        LSBRuntimeStorage *storage = [[LSBRuntimeStorage alloc] initWithAppGroupIdentifiers:identifiers containerProvider:^NSURL *(NSString *identifier) {
            (void)identifier; ++calls; return nil;
        }];
        assertFailure(storage, 0);
        assert(!calls);
    }
}

static void testSignedAndResignedIdentifiers(void) {
    StorageFixture *fixture = [StorageFixture new];
    NSArray *pairs = @[@[@"group.com.SideStore.SideStore", @"group.com.rileytestut.AltStore"], signedGroups()];
    for (NSArray<NSString *> *pair in pairs) {
        __block NSMutableArray<NSString *> *lookups = [NSMutableArray new];
        LSBRuntimeStorage *storage = [[LSBRuntimeStorage alloc] initWithAppGroupIdentifiers:@[pair[1], pair[0]]
            containerProvider:^NSURL *(NSString *identifier) {
                [lookups addObject:identifier];
                return [identifier isEqual:pair[0]] ? fixture.sideStore : fixture.altStore;
            }];
        assertSelected(storage, fixture.sideStore, pair[0]);
        assert([lookups isEqual:pair]); // Actual signed identifiers pass through unchanged; the parser provides the order.
        storage = [[LSBRuntimeStorage alloc] initWithAppGroupIdentifiers:@[pair[1]] containerProvider:^NSURL *(NSString *identifier) {
            assert([identifier isEqual:pair[1]]); return fixture.altStore;
        }];
        assertSelected(storage, fixture.altStore, pair[1]);
    }
}

static void testCorruptMarkersAndRoots(void) {
    StorageFixture *fixture = [StorageFixture new];
    directory(child(fixture.altStore, @LSB_STORAGE_DIRECTORY));
    for (NSString *name in @[@LSB_STORAGE_DIRECTORY, @"Apps"]) {
        NSURL *marker = child(fixture.sideStore, name);
        file(marker);
        assertFailure([fixture resolve], ENOTDIR); // Corruption must not silently send consumers to the other store.
        removeItem(marker);
        symlinkTo(marker, fixture.altStore);
        assertFailure([fixture resolve], ELOOP);
        removeItem(marker);
        symlinkTo(marker, child(fixture.root, @"missing"));
        assertFailure([fixture resolve], ELOOP);
        removeItem(marker);
    }
    directory(child(fixture.sideStore, @LSB_STORAGE_BUILD_RELATIVE_PATH));
    for (NSString *name in @[@LSB_RUNTIME_DIRECTORY, @LSB_DATA_DIRECTORY]) {
        NSURL *root = child(child(fixture.sideStore, @LSB_STORAGE_BUILD_RELATIVE_PATH), name);
        file(root);
        assertFailure([fixture resolve], ENOTDIR);
        removeItem(root);
        symlinkTo(root, fixture.altStore);
        assertFailure([fixture resolve], ELOOP);
        removeItem(root);
    }
    assertSelected([fixture resolve], fixture.sideStore, signedGroups()[0]);
}

static void testEveryHierarchyComponent(void) {
    NSArray<NSString *> *paths = @[@"LSB", @"LSB/iOS", @"LSB/iOS/23A343", @"LSB/iOS/23A343/RuntimeRoot", @"LSB/iOS/23A343/data"];
    for (NSString *path in paths) {
        StorageFixture *fixture = [StorageFixture new];
        NSURL *component = child(fixture.sideStore, path);
        directory(component.URLByDeletingLastPathComponent);
        assertSelected([fixture resolve], fixture.sideStore, signedGroups()[0]);
        assert(![NSFileManager.defaultManager fileExistsAtPath:component.path]); // Missing descendants stay missing.
        file(component);
        assertFailure([fixture resolve], ENOTDIR);
        removeItem(component);
        symlinkTo(component, fixture.altStore);
        assertFailure([fixture resolve], ELOOP);
        removeItem(component);
        symlinkTo(component, child(fixture.root, @"absent"));
        assertFailure([fixture resolve], ELOOP);
        removeItem(component);
        directory(component);
        assertSelected([fixture resolve], fixture.sideStore, signedGroups()[0]);
    }
}

static void testInvalidProvider(void) {
    LSBAppGroupContainerProvider missingProvider = nil;
    assertFailure([[LSBRuntimeStorage alloc] initWithAppGroupIdentifiers:signedGroups() containerProvider:missingProvider], EINVAL);
    StorageFixture *fixture = [StorageFixture new];
    NSURL *side = fixture.sideStore;
    fixture.sideStore = [NSURL URLWithString:@"https://example.invalid/container"];
    assertFailure([fixture resolve], EINVAL);
    fixture.sideStore = child(fixture.root, @"missing");
    assertFailure([fixture resolve], ENOENT);
    fixture.sideStore = child(fixture.root, @"file");
    file(fixture.sideStore);
    assertFailure([fixture resolve], ENOTDIR);
    fixture.sideStore = child(fixture.root, @"linked-container");
    symlinkTo(fixture.sideStore, side);
    assertFailure([fixture resolve], 0);
    fixture.sideStore = [NSURL fileURLWithPath:fixture.sideStore.path isDirectory:YES];
    assertFailure([fixture resolve], 0); // A trailing slash must not bypass the leaf-symlink rejection.
}

int main(void) {
    @autoreleasepool {
        testPreferenceAndNoWrites();
        testUnavailableAndStableSelection();
        testInvalidIdentifiers();
        testSignedAndResignedIdentifiers();
        testCorruptMarkersAndRoots();
        testEveryHierarchyComponent();
        testInvalidProvider();
        puts("RuntimeStorageTests passed");
    }
    return 0;
}
