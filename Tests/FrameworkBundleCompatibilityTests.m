#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include "../Guest/libLiveSimSpawn/FrameworkBundleCompatibility.h"
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static NSString *testDirectory, *runtimeRoot;

static BOOL samePhysicalPath(NSURL *url, NSString *expected) {
    char actualPath[PATH_MAX], expectedPath[PATH_MAX];
    return url && realpath(url.fileSystemRepresentation, actualPath) && realpath(expected.fileSystemRepresentation, expectedPath) &&
           !strcmp(actualPath, expectedPath);
}

static NSString *makeBundle(NSString *relative, BOOL versioned) {
    NSString *bundle = [runtimeRoot stringByAppendingPathComponent:relative];
    NSString *resources = versioned ? [bundle stringByAppendingPathComponent:@"Versions/A/Resources"] : bundle;
    assert([NSFileManager.defaultManager createDirectoryAtPath:resources withIntermediateDirectories:YES attributes:nil error:NULL]);
    NSDictionary *info = @{@"CFBundleIdentifier": @"com.kdt.LiveSimBooter.FrameworkBundleTest", @"CFBundleName": @"BundleTest",
                          @"CFBundleVersion": @"1", @"CFBundlePackageType": @"FMWK"};
    assert([info writeToFile:[resources stringByAppendingPathComponent:@"Info.plist"] atomically:YES]);
    assert([@"guest-resource\n" writeToFile:[resources stringByAppendingPathComponent:@"marker.txt"]
                              atomically:YES encoding:NSUTF8StringEncoding error:NULL]);
    if (versioned) {
        assert(!symlink("A", [[bundle stringByAppendingPathComponent:@"Versions/Current"] fileSystemRepresentation]));
        assert(!symlink("Versions/Current/Resources", [[bundle stringByAppendingPathComponent:@"Resources"] fileSystemRepresentation]));
    }
    return bundle;
}

static void initialize(void) {
    char temporary[] = "/tmp/LiveSimBooter-framework-bundles.XXXXXX", canonical[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    testDirectory = @(canonical);
    runtimeRoot = [testDirectory stringByAppendingPathComponent:@"RuntimeRoot"];
    assert([NSFileManager.defaultManager createDirectoryAtPath:runtimeRoot withIntermediateDirectories:NO attributes:nil error:NULL]);
    assert(!setenv(LSB_FRAMEWORK_BUNDLE_ROOT_ENV, runtimeRoot.fileSystemRepresentation, 1));
    assert(!setenv(LSB_FRAMEWORK_BUNDLE_ENABLE_ENV, "1", 1));
}

static void finish(void) {
    assert([NSFileManager.defaultManager removeItemAtPath:testDirectory error:NULL]);
}

#ifdef LSB_FRAMEWORK_BUNDLE_INTERPOSE_TESTING

int main(void) {
    @autoreleasepool {
        initialize();
        NSString *expected = makeBundle(@"System/Library/Frameworks/QuartzCore.framework", NO);
        NSBundle *bundle = [NSBundle bundleForClass:CALayer.class];
        assert(samePhysicalPath(bundle.bundleURL, expected));
        assert([bundle.bundleIdentifier isEqualToString:@"com.kdt.LiveSimBooter.FrameworkBundleTest"]);
        NSURL *marker = [bundle URLForResource:@"marker" withExtension:@"txt"];
        assert(samePhysicalPath(marker, [expected stringByAppendingPathComponent:@"marker.txt"]));
        assert([[NSString stringWithContentsOfURL:marker encoding:NSUTF8StringEncoding error:NULL] isEqualToString:@"guest-resource\n"]);
        finish();
        puts("FrameworkBundleCompatibility: native NSBundle class lookup, bundle URL, metadata and resource use the staged CFBundle");
    }
    return 0;
}

#else

static CFBundleRef originalCreate(CFAllocatorRef allocator, CFURLRef url) CF_RETURNS_RETAINED;
#define CFBundleCreate originalCreate
#define LSB_FRAMEWORK_BUNDLE_TESTING
#include "../Guest/libLiveSimSpawn/FrameworkBundleCompatibility.c"
#undef CFBundleCreate

static _Atomic(unsigned) originalCalls;
static _Atomic(bool) capture = true;
static bool forceFailure, reenter;
static CFURLRef reentryURL;
static CFAllocatorRef seenAllocator;
static CFURLRef seenURL;
static char seenPaths[4][PATH_MAX];

static CFBundleRef originalCreate(CFAllocatorRef allocator, CFURLRef url) {
    unsigned index = atomic_fetch_add(&originalCalls, 1);
    if (atomic_load(&capture)) {
        seenAllocator = allocator;
        seenURL = url;
        if (index < 4) {
            seenPaths[index][0] = '\0';
            if (url) CFURLGetFileSystemRepresentation(url, true, (UInt8 *)seenPaths[index], sizeof(seenPaths[index]));
            size_t length = strlen(seenPaths[index]);
            while (length > 1 && seenPaths[index][length - 1] == '/') seenPaths[index][--length] = '\0';
        }
    }
    if (reenter) {
        reenter = false;
        CFBundleRef nested = LSBFrameworkBundleCreate(allocator, reentryURL);
        if (nested) CFRelease(nested);
    }
    return forceFailure || !url ? NULL : CFBundleCreate(allocator, url);
}

static CFBundleRef call(NSString *path) CF_RETURNS_RETAINED;
static CFBundleRef call(NSString *path) {
    atomic_store(&originalCalls, 0);
    memset(seenPaths, 0, sizeof(seenPaths));
    NSURL *url = [NSURL fileURLWithPath:path isDirectory:YES];
    return LSBFrameworkBundleCreate(kCFAllocatorDefault, (__bridge CFURLRef)url);
}

static void expectForward(NSString *path) {
    forceFailure = true; // Observe forwarding without asking the native framework loader to interpret malformed fixtures.
    NSURL *url = [NSURL fileURLWithPath:path isDirectory:YES];
    atomic_store(&originalCalls, 0);
    assert(!LSBFrameworkBundleCreate(kCFAllocatorDefault, (__bridge CFURLRef)url));
    assert(atomic_load(&originalCalls) == 1 && seenURL == (__bridge CFURLRef)url && seenAllocator == kCFAllocatorDefault);
    forceFailure = false;
}

static void expectRedirect(NSString *source, NSString *expected) {
    CFBundleRef bundle = call(source);
    assert(bundle && atomic_load(&originalCalls) == 1 && !strcmp(seenPaths[0], expected.fileSystemRepresentation));
    NSURL *url = CFBridgingRelease(CFBundleCopyBundleURL(bundle));
    assert(samePhysicalPath(url, expected));
    NSURL *marker = CFBridgingRelease(CFBundleCopyResourceURL(bundle, CFSTR("marker"), CFSTR("txt"), NULL));
    assert(marker && [[NSString stringWithContentsOfURL:marker encoding:NSUTF8StringEncoding error:NULL] isEqualToString:@"guest-resource\n"]);
    CFRelease(bundle);
}

static void testScopeAndGate(void) {
    NSString *source = @"/System/Library/Frameworks/Example.framework";
    NSString *expected = makeBundle([source substringFromIndex:1], NO);
    const char *disabled[] = {NULL, "", "0", "true", "01", "1 "};
    for (unsigned i = 0; i < sizeof(disabled) / sizeof(disabled[0]); ++i) {
        if (disabled[i]) assert(!setenv(LSB_FRAMEWORK_BUNDLE_ENABLE_ENV, disabled[i], 1));
        else assert(!unsetenv(LSB_FRAMEWORK_BUNDLE_ENABLE_ENV));
        expectForward(source);
    }
    assert(!setenv(LSB_FRAMEWORK_BUNDLE_ENABLE_ENV, "1", 1));
    expectRedirect(source, expected);
    expectRedirect([source stringByAppendingString:@"/"], expected);
    expectForward(expected); // Physical RuntimeRoot paths must not receive a second prefix.
    for (NSString *outside in @[@"/Library/Frameworks/Example.framework", @"/System/Library/FrameworksExtra/Example.framework",
                                @"/System/Library/Frameworks/Example.framework.bundle", @"/Applications/Example.app",
                                @"/System/Library/Frameworks/Absent.framework"]) expectForward(outside);
    forceFailure = true;
    NSURL *web = [NSURL URLWithString:@"https://example.invalid/System/Library/Frameworks/Example.framework"];
    atomic_store(&originalCalls, 0);
    assert(!LSBFrameworkBundleCreate(kCFAllocatorDefault, (__bridge CFURLRef)web));
    assert(atomic_load(&originalCalls) == 1 && seenURL == (__bridge CFURLRef)web);
    assert(!LSBFrameworkBundleCreate(kCFAllocatorDefault, NULL) && !seenURL);
    forceFailure = false;
    assert(!systemFrameworkPath("/System/Library/Frameworks/../PrivateFrameworks/Example.framework"));
    assert(!systemFrameworkPath("/System/Library/Frameworks/./Example.framework"));
    assert(!systemFrameworkPath("/System/Library/Frameworks//Example.framework"));
    const char *invalidRoots[] = {NULL, "", "/", "relative", "/nonexistent-framework-test-root", expected.fileSystemRepresentation};
    for (unsigned i = 0; i < sizeof(invalidRoots) / sizeof(invalidRoots[0]); ++i) {
        if (invalidRoots[i]) assert(!setenv(LSB_FRAMEWORK_BUNDLE_ROOT_ENV, invalidRoots[i], 1));
        else assert(!unsetenv(LSB_FRAMEWORK_BUNDLE_ROOT_ENV));
        expectForward(source);
    }
    assert(!setenv(LSB_FRAMEWORK_BUNDLE_ROOT_ENV, runtimeRoot.fileSystemRepresentation, 1));
}

static void testLayoutsAndContainment(void) {
    for (NSString *source in @[@"/System/Library/PrivateFrameworks/Private.framework",
                               @"/System/Library/Frameworks/Outer.framework/Frameworks/Inner.framework",
                               @"/System/Library/Frameworks/Spaces Here.framework"]) {
        NSString *expected = makeBundle([source substringFromIndex:1], NO);
        expectRedirect(source, expected);
    }
    NSString *versioned = makeBundle(@"System/Library/Frameworks/Versioned.framework", YES);
    expectRedirect(@"/System/Library/Frameworks/Versioned.framework", versioned);
    NSString *empty = [runtimeRoot stringByAppendingPathComponent:@"System/Library/Frameworks/Empty.framework"];
    assert([NSFileManager.defaultManager createDirectoryAtPath:empty withIntermediateDirectories:YES attributes:nil error:NULL]);
    expectForward(@"/System/Library/Frameworks/Empty.framework");

    NSString *outside = [testDirectory stringByAppendingPathComponent:@"RuntimeRootSibling/Outside.framework"];
    assert([NSFileManager.defaultManager createDirectoryAtPath:outside withIntermediateDirectories:YES attributes:nil error:NULL]);
    NSString *escape = [runtimeRoot stringByAppendingPathComponent:@"System/Library/Frameworks/Escape.framework"];
    assert(!symlink(outside.fileSystemRepresentation, escape.fileSystemRepresentation));
    expectForward(@"/System/Library/Frameworks/Escape.framework");
    NSString *wrongNamespace = [runtimeRoot stringByAppendingPathComponent:@"Other.framework"];
    assert([NSFileManager.defaultManager createDirectoryAtPath:wrongNamespace withIntermediateDirectories:YES attributes:nil error:NULL]);
    NSString *internalEscape = [runtimeRoot stringByAppendingPathComponent:@"System/Library/Frameworks/InternalEscape.framework"];
    assert(!symlink(wrongNamespace.fileSystemRepresentation, internalEscape.fileSystemRepresentation));
    expectForward(@"/System/Library/Frameworks/InternalEscape.framework");

    NSString *infoEscape = makeBundle(@"System/Library/Frameworks/InfoEscape.framework", NO);
    NSString *info = [infoEscape stringByAppendingPathComponent:@"Info.plist"];
    NSString *externalInfo = [outside stringByAppendingPathComponent:@"Info.plist"];
    assert([NSFileManager.defaultManager moveItemAtPath:info toPath:externalInfo error:NULL]);
    assert(!symlink(externalInfo.fileSystemRepresentation, info.fileSystemRepresentation));
    expectForward(@"/System/Library/Frameworks/InfoEscape.framework");

    forceFailure = true;
    assert(!call(@"/System/Library/Frameworks/Example.framework") && atomic_load(&originalCalls) == 1);
    assert(!strcmp(seenPaths[0], [[runtimeRoot stringByAppendingPathComponent:@"System/Library/Frameworks/Example.framework"] fileSystemRepresentation]));
    forceFailure = false; // Failure on the selected guest bundle did not fall back to the native bundle.
}

static void testReentrancy(void) {
    NSURL *nested = [NSURL fileURLWithPath:@"/System/Library/Frameworks/Example.framework" isDirectory:YES];
    reentryURL = (__bridge CFURLRef)nested;
    reenter = true;
    CFBundleRef bundle = call(@"/System/Library/Frameworks/Example.framework");
    assert(bundle && atomic_load(&originalCalls) == 2);
    assert(!strcmp(seenPaths[1], "/System/Library/Frameworks/Example.framework"));
    assert(!resolvingBundle);
    CFRelease(bundle);
    reentryURL = NULL;
}

static void *concurrentCreate(void *context) {
    (void)context;
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:@"/System/Library/Frameworks/Example.framework" isDirectory:YES];
        NSString *expected = [runtimeRoot stringByAppendingPathComponent:@"System/Library/Frameworks/Example.framework"];
        for (unsigned i = 0; i < 100; ++i) {
            CFBundleRef bundle = LSBFrameworkBundleCreate(kCFAllocatorDefault, (__bridge CFURLRef)url);
            assert(bundle);
            NSURL *actual = CFBridgingRelease(CFBundleCopyBundleURL(bundle));
            assert(samePhysicalPath(actual, expected));
            CFRelease(bundle);
        }
    }
    return NULL;
}

int main(void) {
    @autoreleasepool {
        initialize();
        testScopeAndGate();
        testLayoutsAndContainment();
        testReentrancy();
        atomic_store(&capture, false);
        atomic_store(&originalCalls, 0);
        pthread_t workers[4];
        for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(&workers[i], NULL, concurrentCreate, NULL));
        for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(workers[i], NULL));
        assert(atomic_load(&originalCalls) == 400);
        finish();
        puts("FrameworkBundleCompatibility: gate, contained flat/nested/versioned bundles, forwarding, failure, recursion and concurrency passed");
    }
    return 0;
}
#endif
