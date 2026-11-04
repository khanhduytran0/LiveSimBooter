#import "../Guest/app-registration/ApplicationRegistrationPlan.h"

#include <assert.h>
#include <errno.h>
#include <mach-o/loader.h>
#include <sys/stat.h>
#include <unistd.h>

static void directory(NSString *path) {
    assert([NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:NULL]);
}

static void writePlist(NSString *path, id value) {
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:value format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    assert(data && [data writeToFile:path atomically:YES]);
}

static void writeJSON(NSString *path, id value) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:value options:0 error:NULL];
    assert(data && [data writeToFile:path atomically:YES]);
}

static void executable(NSString *path) {
    struct mach_header_64 header = {.magic = MH_MAGIC_64, .cputype = CPU_TYPE_ARM64, .filetype = MH_EXECUTE};
    assert([[NSData dataWithBytes:&header length:sizeof(header)] writeToFile:path atomically:YES]);
}

static void expectManifestError(NSString *root, NSString *file, int code) {
    NSError *error = nil;
    assert(!LSBReadApplicationRegistrationManifest(root, file, &error));
    assert([error.domain isEqual:NSPOSIXErrorDomain] && error.code == code);
}

static void expectBundleError(NSString *root, NSString *path, int code) {
    NSError *error = nil;
    NSString *identifier = @"stale", *skip = @"stale";
    assert(!LSBPrepareApplicationRegistration(root, path, &identifier, &skip, &error));
    assert(!identifier && !skip && [error.domain isEqual:NSPOSIXErrorDomain] && error.code == code);
}

static void testDpkgRefresh(NSString *base) {
    // mkdtemp's /tmp spelling aliases /private/tmp on macOS. The guest caller uses canonical absolute paths.
    char canonical[PATH_MAX];
    assert(realpath(base.fileSystemRepresentation, canonical));
    NSString *build = [[NSString stringWithUTF8String:canonical] stringByAppendingPathComponent:@"dpkg"];
    NSString *runtime = [build stringByAppendingPathComponent:@"RuntimeRoot"], *data = [build stringByAppendingPathComponent:@"data"];
    NSString *clone = [data stringByAppendingPathComponent:@"tmp/clone"], *bootstrap = [clone stringByAppendingPathComponent:@"var/jb"];
    NSString *apps = [bootstrap stringByAppendingPathComponent:@"Applications"];
    NSString *path = [apps stringByAppendingPathComponent:@"New.app"];
    NSURL *new = [NSURL fileURLWithPath:path isDirectory:YES];
    NSURL *gone = [NSURL fileURLWithPath:[apps stringByAppendingPathComponent:@"Old.app"] isDirectory:YES];
    NSURL *foreign = [NSURL fileURLWithPath:@"/Applications/Foreign.app" isDirectory:YES];
    NSString *outside = [data stringByAppendingPathComponent:@"var/jb/Applications/Elsewhere.app"];
    NSMutableDictionary *registry = [@{@"test.old": gone, @"outside": foreign, @"sibling": [NSURL fileURLWithPath:outside]} mutableCopy];
    __block NSUInteger writes = 0;
    __block BOOL offline = NO, skipMutation = NO;
    LSBReadGuestApplications read = ^NSDictionary *(NSError **error) {
        if (offline) {
            if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOTCONN userInfo:nil];
            return nil;
        }
        return registry.copy;
    };
    LSBChangeGuestApplication change = ^BOOL(NSString *identifier, NSURL *url, BOOL removing, NSError **changeError) {
        (void)changeError; ++writes;
        if (skipMutation) return YES;
        if (removing) {
            assert([registry[identifier] isEqual:url]);
            [registry removeObjectForKey:identifier];
        } else {
            assert([identifier isEqual:@"test.new"] && [url isEqual:new]);
            registry[identifier] = url;
        }
        return YES;
    };
    directory(runtime);
    directory(path);
    NSError *error = nil;
    assert(!LSBRefreshDpkgApplications(runtime, data, clone, read, change, &error) && !writes); // Invalid Info.plist, preserve stale record too.
    writePlist([path stringByAppendingPathComponent:@"Info.plist"], @{@"CFBundleExecutable": @"New", @"CFBundleIdentifier": @"test.new"});
    executable([path stringByAppendingPathComponent:@"New"]);
    registry[@"test.new"] = foreign;
    assert(!LSBRefreshDpkgApplications(runtime, data, clone, read, change, &error) && error.code == EEXIST && !writes);
    [registry removeObjectForKey:@"test.new"];
    offline = YES;
    assert(!LSBRefreshDpkgApplications(runtime, data, clone, read, change, &error) && error.code == ENOTCONN && !writes);
    offline = NO;
    NSString *alias = [apps stringByAppendingPathComponent:@"Link.app"];
    assert(!symlink(path.fileSystemRepresentation, alias.fileSystemRepresentation));
    assert(!LSBRefreshDpkgApplications(runtime, data, clone, read, change, &error) && !writes);
    assert(!unlink(alias.fileSystemRepresentation));
    NSString *plugins = [path stringByAppendingPathComponent:@"PlugIns"];
    assert(!symlink(build.fileSystemRepresentation, plugins.fileSystemRepresentation));
    assert(!LSBRefreshDpkgApplications(runtime, data, clone, read, change, &error) && error.code == EACCES && !writes);
    assert(!unlink(plugins.fileSystemRepresentation));
    assert(!chmod(apps.fileSystemRepresentation, 0000));
    if (geteuid() != 0) assert(!LSBRefreshDpkgApplications(runtime, data, clone, read, change, &error) && !writes);
    assert(!chmod(apps.fileSystemRepresentation, 0700));
    skipMutation = YES;
    assert(!LSBRefreshDpkgApplications(runtime, data, clone, read, change, &error) && error.code == EIO && writes == 1 && registry[@"test.old"]);
    skipMutation = NO;
    assert(LSBRefreshDpkgApplications(runtime, data, clone, read, change, &error) && writes == 3 && !registry[@"test.old"] && registry[@"test.new"]);
    assert(LSBRefreshDpkgApplications(runtime, data, clone, read, change, &error) && writes == 3); // Idempotent.
    assert([NSFileManager.defaultManager removeItemAtPath:apps error:NULL]);
    assert(LSBRefreshDpkgApplications(runtime, data, clone, read, change, &error) && writes == 4 && !registry[@"test.new"]);
    assert([registry[@"outside"] isEqual:foreign] && registry[@"sibling"]); // Native, IPA and other transaction URLs are never stale here.
    assert(!LSBRefreshDpkgApplications(runtime, data, build, read, change, &error) && error.code == EACCES && writes == 4);
    assert(!symlink(build.fileSystemRepresentation, apps.fileSystemRepresentation));
    assert(!LSBRefreshDpkgApplications(runtime, data, clone, read, change, &error) && writes == 4);
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        char pattern[] = "/tmp/LSBAppRegistration.XXXXXX";
        char *created = mkdtemp(pattern);
        assert(created);
        NSString *base = [NSString stringWithUTF8String:created];
        NSString *root = [base stringByAppendingPathComponent:@"RuntimeRoot"];
        NSString *metadata = [root stringByAppendingPathComponent:@"usr/share/LiveSimBooter"];
        directory(metadata);
        NSString *manifest = [metadata stringByAppendingPathComponent:@"applications.json"];
        NSString *manifestGuest = @"/usr/share/LiveSimBooter/applications.json";
        NSArray *paths = @[@"/Applications/Test App.app", @"/System/Applications/Other.app", @"/System/Library/CoreServices/Service.app"];
        writeJSON(manifest, @{@"version": @1, @"applications": paths});
        NSError *error = nil;
        assert([LSBReadApplicationRegistrationManifest(root, manifestGuest, &error) isEqual:paths] && !error);

        // Root aliases are valid; aliases below the selected root are rejected even if they point back inside it.
        NSString *alias = [base stringByAppendingPathComponent:@"root-alias"];
        assert(!symlink(root.fileSystemRepresentation, alias.fileSystemRepresentation));
        assert([LSBReadApplicationRegistrationManifest(alias, manifestGuest, &error) isEqual:paths]);
        expectManifestError(@"/", manifest, EINVAL);
        expectManifestError(root, @"/../applications.json", EINVAL);
        for (id invalid in @[@"/Applications/Test.app/../Other.app", @"/Applications//Test.app", @"/Applications/./Test.app",
                             @"/Users/duy/Test.app", @"/System/Applications/Test.app/Nested.app", @"/Applications/.app", @42]) {
            writeJSON(manifest, @{@"version": @1, @"applications": @[invalid]});
            expectManifestError(root, manifestGuest, EINVAL);
        }
        writeJSON(manifest, @{@"version": @1, @"applications": @[paths[0], paths[0]]});
        expectManifestError(root, manifestGuest, EINVAL);
        writeJSON(manifest, @{@"version": @2, @"applications": paths});
        expectManifestError(root, manifestGuest, EINVAL);
        writeJSON(manifest, @{@"version": @1, @"applications": @[]});
        expectManifestError(root, manifestGuest, E2BIG);
        NSMutableArray *many = [NSMutableArray array];
        for (NSUInteger i = 0; i < 513; ++i) [many addObject:[NSString stringWithFormat:@"/Applications/App%lu.app", (unsigned long)i]];
        writeJSON(manifest, @{@"version": @1, @"applications": many});
        expectManifestError(root, manifestGuest, E2BIG);
        [many removeLastObject];
        writeJSON(manifest, @{@"version": @1, @"applications": many});
        assert(LSBReadApplicationRegistrationManifest(root, manifestGuest, &error).count == 512);
        assert([[NSMutableData dataWithLength:1024 * 1024 + 1] writeToFile:manifest atomically:YES]);
        expectManifestError(root, manifestGuest, EFBIG);
        assert(!unlink(manifest.fileSystemRepresentation));
        NSString *outside = [base stringByAppendingPathComponent:@"outside.json"];
        writeJSON(outside, @{@"version": @1, @"applications": paths});
        assert(!symlink(outside.fileSystemRepresentation, manifest.fileSystemRepresentation));
        expectManifestError(root, manifestGuest, ELOOP);
        assert(!unlink(manifest.fileSystemRepresentation));
        // No producer opens these FIFOs. A blocking inspection would hang before the regular-file check.
        assert(!mkfifo(manifest.fileSystemRepresentation, 0600));
        alarm(2);
        expectManifestError(root, manifestGuest, EINVAL);
        alarm(0);
        assert(!unlink(manifest.fileSystemRepresentation));

        NSString *app = [root stringByAppendingString:paths[0]];
        directory(app);
        NSString *identifier = nil, *skip = nil;
        assert(!LSBPrepareApplicationRegistration(root, paths[0], &identifier, &skip, &error));
        assert([skip isEqual:@"missing-info-plist"] && !error && !identifier);
        NSString *info = [app stringByAppendingPathComponent:@"Info.plist"];
        writePlist(info, @{@"CFBundleIdentifier": @"test.bundle", @"CFBundleExecutable": @"Test"});
        assert(!LSBPrepareApplicationRegistration(root, paths[0], &identifier, &skip, &error));
        assert([skip isEqual:@"missing-executable"] && !error && !identifier);
        NSString *binary = [app stringByAppendingPathComponent:@"Test"];
        executable(binary);
        NSURL *prepared = LSBPrepareApplicationRegistration(root, paths[0], &identifier, &skip, &error);
        assert(prepared && [identifier isEqual:@"test.bundle"] && !skip && !error);
        assert(LSBVerifyApplicationRegistration(prepared, identifier, prepared, identifier, YES, &error));
        assert(!LSBVerifyApplicationRegistration(prepared, identifier, prepared, identifier, NO, &error) && error.code == EIO);
        assert(!LSBVerifyApplicationRegistration(prepared, identifier, prepared, @"different.bundle", YES, &error) && error.code == EIO);
        assert(!LSBVerifyApplicationRegistration(prepared, identifier, [NSURL URLWithString:@"https://example.invalid"], identifier, YES, &error));
        assert(!LSBVerifyApplicationRegistration(prepared, identifier, [NSURL fileURLWithPath:root], identifier, YES, &error) && error.code == EXDEV);

        writePlist(info, @{@"CFBundleIdentifier": @"test.bundle", @"CFBundleExecutable": @"../../outside"});
        expectBundleError(root, paths[0], EINVAL);
        writePlist(info, @{@"CFBundleIdentifier": @"test.bundle"});
        assert(!LSBPrepareApplicationRegistration(root, paths[0], &identifier, &skip, &error));
        assert([skip isEqual:@"no-executable-declaration"] && !error);
        writePlist(info, @{@"CFBundleExecutable": @"Test"});
        expectBundleError(root, paths[0], EINVAL);
        writePlist(info, @{@"CFBundleIdentifier": @"test.bundle", @"CFBundleExecutable": @"Test"});
        assert([[NSData dataWithBytes:"bad" length:3] writeToFile:binary atomically:YES]);
        expectBundleError(root, paths[0], ENOEXEC);
        assert(!unlink(binary.fileSystemRepresentation));
        assert(!mkfifo(binary.fileSystemRepresentation, 0600));
        alarm(2);
        expectBundleError(root, paths[0], ENOEXEC);
        alarm(0);
        assert(!unlink(binary.fileSystemRepresentation));
        assert(!symlink(outside.fileSystemRepresentation, binary.fileSystemRepresentation));
        expectBundleError(root, paths[0], ELOOP);
        assert(!unlink(info.fileSystemRepresentation));
        assert(!mkfifo(info.fileSystemRepresentation, 0600));
        alarm(2);
        expectBundleError(root, paths[0], EINVAL);
        alarm(0);
        assert(!unlink(info.fileSystemRepresentation));
        assert(!symlink(outside.fileSystemRepresentation, info.fileSystemRepresentation));
        expectBundleError(root, paths[0], ELOOP);
        NSString *linked = [root stringByAppendingPathComponent:@"Applications/Linked.app"];
        assert(!symlink(app.fileSystemRepresentation, linked.fileSystemRepresentation));
        expectBundleError(root, @"/Applications/Linked.app", ELOOP);
        expectBundleError(root, @"/System/Library/CoreServices/Service.app/Nested.app", EINVAL);

        testDpkgRefresh(base);
        assert([NSFileManager.defaultManager removeItemAtPath:base error:&error]);
        puts("ApplicationRegistrationTests passed (filesystem and record-validation only; no native LaunchServices calls)");
        if (argc == 2) {
            NSString *stockRoot = [NSString stringWithUTF8String:argv[1]];
            unsigned ready = 0, skipped = 0, failed = 0;
            for (NSString *scope in @[@"/Applications", @"/System/Applications", @"/System/Library/CoreServices"]) {
                NSArray *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:[stockRoot stringByAppendingString:scope] error:&error];
                assert(names);
                for (NSString *name in [names sortedArrayUsingSelector:@selector(compare:)]) {
                    if (![name hasSuffix:@".app"]) continue;
                    NSString *path = [scope stringByAppendingPathComponent:name];
                    if (LSBPrepareApplicationRegistration(stockRoot, path, &identifier, &skip, &error)) ++ready;
                    else if (skip) ++skipped;
                    else { ++failed; fprintf(stderr, "stock preflight failed %s: %s\n", path.UTF8String, error.description.UTF8String); }
                }
            }
            printf("Stock read-only preflight: ready=%u skipped=%u failed=%u\n", ready, skipped, failed);
            assert(!failed);
        }
    }
    return 0;
}
