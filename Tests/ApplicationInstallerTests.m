#import <Foundation/Foundation.h>
#include <assert.h>

#define LSB_APPLICATION_INSTALLER_TESTING 1
#define MIInstallOptions LSBTestInstallOptions
#define IXAppInstallCoordinator LSBTestInstallCoordinator
#define LSApplicationRecord LSBTestApplicationRecord
#define main installerMain
#include "../Guest/lsb-app-install/ApplicationInstaller.m"
#undef main

static uint32_t testPlatform = 7;
uint32_t dyld_get_active_platform(void) { return testPlatform; }

static NSURL *recordBundle, *recordData;
static NSString *recordIdentifier = @"example.test.installed";
static NSError *coordinatorError;
static BOOL missingRecord, mutateSource;
static NSUInteger installCalls;
static NSURL *installedSource;

@implementation MIInstallOptions
@end

@implementation IXAppInstallCoordinator
+ (void)installApplication:(NSURL *)url consumeSource:(BOOL)consumeSource options:(MIInstallOptions *)options
                completion:(void (^)(NSString *, NSError *))completion {
    assert(!consumeSource && options.installTargetType == 1 && options.developerInstall && options.userInitiated);
    ++installCalls;
    installedSource = url;
    if (mutateSource) {
        int fd = open(url.fileSystemRepresentation, O_WRONLY | O_APPEND);
        assert(fd >= 0 && write(fd, "x", 1) == 1 && !close(fd));
    }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{ completion(recordIdentifier, coordinatorError); });
}
@end

@implementation LSApplicationRecord
- (instancetype)initWithBundleIdentifier:(NSString *)identifier allowPlaceholder:(BOOL)allowPlaceholder error:(NSError **)error {
    assert([identifier isEqual:recordIdentifier] && !allowPlaceholder);
    if (missingRecord) {
        if (error) *error = [NSError errorWithDomain:@"TestLaunchServices" code:42 userInfo:nil];
        return nil;
    }
    return [super init];
}
- (NSString *)bundleIdentifier { return recordIdentifier; }
- (NSURL *)URL { return recordBundle; }
- (NSURL *)dataContainerURL { return recordData; }
@end

static void createDirectory(NSString *path) {
    assert([NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:NULL]);
}

static void createFile(NSString *path) {
    assert([[@"PK\003\004fixture" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:path atomically:NO]);
}

static NSDictionary *captureResult(NSDictionary *installed, NSError *error, int expectedStatus) {
    FILE *file = tmpfile();
    assert(file);
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    assert(saved >= 0 && dup2(fileno(file), STDOUT_FILENO) == STDOUT_FILENO);
    assert(reportResult(installed, error) == expectedStatus);
    assert(dup2(saved, STDOUT_FILENO) == STDOUT_FILENO && !close(saved));
    assert(!fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length > 0 && length < 8192 && !fseek(file, 0, SEEK_SET));
    NSMutableData *bytes = [NSMutableData dataWithLength:(NSUInteger)length];
    assert(fread(bytes.mutableBytes, 1, bytes.length, file) == bytes.length && !fclose(file));
    const char prefix[] = "LSB_INSTALL_RESULT ";
    assert(bytes.length > sizeof(prefix) && !memcmp(bytes.bytes, prefix, sizeof(prefix) - 1));
    assert(((char *)bytes.bytes)[bytes.length - 1] == '\n');
    NSData *json = [bytes subdataWithRange:NSMakeRange(sizeof(prefix) - 1, bytes.length - sizeof(prefix))];
    assert(!memchr(json.bytes, '\n', json.length));
    NSDictionary *result = [NSJSONSerialization JSONObjectWithData:json options:0 error:NULL];
    assert([result isKindOfClass:NSDictionary.class] && [result[@"version"] isEqual:@1]);
    assert(CFGetTypeID((__bridge CFTypeRef)result[@"success"]) == CFBooleanGetTypeID());
    assert([result[@"success"] boolValue] == (expectedStatus == 0));
    return result;
}

static void errorChainTests(void) {
    NSError *error = nil;
    for (unsigned depth = 0; depth < 8; ++depth) {
        NSMutableDictionary *details = [@{NSLocalizedDescriptionKey: @"Failure\ncontinued", @"PrivateOpaqueObject": @"DO_NOT_LOG"} mutableCopy];
        if (error) details[NSUnderlyingErrorKey] = error;
        error = [NSError errorWithDomain:@"TestChain" code:depth userInfo:details];
    }
    FILE *file = tmpfile();
    assert(file);
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fileno(file), STDERR_FILENO) == STDERR_FILENO);
    reportErrorChain(error);
    fflush(stderr);
    assert(dup2(saved, STDERR_FILENO) == STDERR_FILENO && !close(saved) && !fseek(file, 0, SEEK_SET));
    char bytes[8192];
    size_t count = fread(bytes, 1, sizeof(bytes) - 1, file);
    bytes[count] = 0;
    assert(!fclose(file) && !strstr(bytes, "DO_NOT_LOG") && !strstr(bytes, "error-depth=6"));
    assert(strstr(bytes, "error-depth=0 domain=TestChain code=7") && strstr(bytes, "error-depth=5 domain=TestChain code=2"));
    assert(strstr(bytes, "Failure\\ncontinued"));
    unsigned lines = 0;
    for (size_t i = 0; i < count; ++i) if (bytes[i] == '\n') ++lines;
    assert(lines == 6);
}

int main(void) {
    @autoreleasepool {
        errorChainTests();
        char temporary[] = "/tmp/LSB-application-installer.XXXXXX", resolved[PATH_MAX];
        assert(mkdtemp(temporary) && realpath(temporary, resolved));
        NSString *base = [NSString stringWithUTF8String:resolved];
        NSString *runtime = [base stringByAppendingPathComponent:@"LSB/iOS/23A343/RuntimeRoot"];
        NSString *data = [base stringByAppendingPathComponent:@"LSB/iOS/23A343/data"];
        createDirectory(runtime);
        createDirectory(data);
        assert(!setenv("DYLD_ROOT_PATH", runtime.fileSystemRepresentation, 1));
        assert(!setenv("SIMULATOR_ROOT", runtime.fileSystemRepresentation, 1));
        assert(!setenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY", data.fileSystemRepresentation, 1));
        NSError *error = nil;
        assert([guestDataRoot(&error) isEqual:data] && !error);
        testPlatform = 2;
        assert(!guestDataRoot(&error) && error.code == EINVAL);
        testPlatform = 7;
        assert(!setenv("SIMULATOR_ROOT", data.fileSystemRepresentation, 1));
        assert(!guestDataRoot(&error) && error.code == EINVAL);
        assert(!setenv("SIMULATOR_ROOT", runtime.fileSystemRepresentation, 1));
        assert(!setenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY", base.fileSystemRepresentation, 1));
        assert(!guestDataRoot(&error) && error.code == EINVAL);
        assert(!setenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY", data.fileSystemRepresentation, 1));

        NSString *importDirectory = [data stringByAppendingPathComponent:@"Library/LiveSimBooter/Imports/0123456789abcdef0123456789abcdef"];
        createDirectory(importDirectory);
        NSString *source = [importDirectory stringByAppendingPathComponent:@"payload.ipa"];
        createFile(source);
        struct stat snapshot;
        int fd = -1;
        error = nil;
        NSURL *url = openImport(source, data, &fd, &snapshot, &error);
        assert(url && !error && fd >= 0 && sameSource(fd, url, &snapshot) && !close(fd));
        assert(!openImport(@"relative/payload.ipa", data, &fd, &snapshot, &error) && fd == -1 && error.code == EINVAL);
        NSString *outside = [base stringByAppendingPathComponent:@"payload.ipa"];
        createFile(outside);
        assert(!openImport(outside, data, &fd, &snapshot, &error) && error.code == EPERM);
        NSString *other = [importDirectory stringByAppendingPathComponent:@"other.ipa"];
        createFile(other);
        assert(!openImport(other, data, &fd, &snapshot, &error) && error.code == EINVAL);
        NSString *badDirectory = [data stringByAppendingPathComponent:@"Library/LiveSimBooter/Imports/ABCDEF01234567890123456789abcdef"];
        createDirectory(badDirectory);
        NSString *badSource = [badDirectory stringByAppendingPathComponent:@"payload.ipa"];
        createFile(badSource);
        assert(!openImport(badSource, data, &fd, &snapshot, &error) && error.code == EINVAL);
        NSString *alias = [importDirectory stringByAppendingPathComponent:@"alias.ipa"];
        assert(!symlink(source.fileSystemRepresentation, alias.fileSystemRepresentation));
        assert(!openImport(alias, data, &fd, &snapshot, &error) && error.code == ELOOP);
        NSString *traversal = [importDirectory stringByAppendingString:@"/../0123456789abcdef0123456789abcdef/payload.ipa"];
        assert(!openImport(traversal, data, &fd, &snapshot, &error));
        assert(!unlink(source.fileSystemRepresentation));
        assert(!symlink(other.fileSystemRepresentation, source.fileSystemRepresentation));
        assert(!openImport(source, data, &fd, &snapshot, &error));
        assert(!unlink(source.fileSystemRepresentation));
        assert([NSData.data writeToFile:source atomically:NO]);
        assert(!openImport(source, data, &fd, &snapshot, &error) && error.code == EINVAL);
        createFile(source);

        NSString *bundle = [data stringByAppendingPathComponent:@"Containers/Bundle/Application/00000000-1111-2222-3333-444444444444/Test.app"];
        NSString *home = [data stringByAppendingPathComponent:@"Containers/Data/Application/55555555-1111-2222-3333-444444444444"];
        createDirectory(bundle);
        createDirectory(home);
        recordBundle = [NSURL fileURLWithPath:bundle];
        recordData = [NSURL fileURLWithPath:home];
        error = nil;
        assert(!chmod(source.fileSystemRepresentation, 0400));
        NSDictionary *result = installApplication(source, data, &error);
        assert(result && !error && installCalls == 1 && [installedSource.path isEqual:source]);
        assert([result[@"bundleIdentifier"] isEqual:recordIdentifier]);
        assert([NSURL URLWithString:result[@"bundleURL"]].isFileURL);
        assert([[NSURL URLWithString:result[@"dataURL"]].path isEqual:home]);
        assert([NSFileManager.defaultManager fileExistsAtPath:source]);
        assert([captureResult(result, nil, 0)[@"bundleIdentifier"] isEqual:recordIdentifier]);
        assert(!chmod(source.fileSystemRepresentation, 0600));

        coordinatorError = [NSError errorWithDomain:@"TestInstallError" code:19
            userInfo:@{NSLocalizedDescriptionKey: @"Rejected \"archive\"\nwith a reason"}];
        error = nil;
        assert(!installApplication(source, data, &error) && error == coordinatorError && installCalls == 2);
        NSDictionary *failure = captureResult(nil, error, 1);
        assert([failure[@"errorDomain"] isEqual:@"TestInstallError"] && [failure[@"errorCode"] isEqual:@19]);
        assert([failure[@"errorDescription"] isEqual:error.localizedDescription]);
        coordinatorError = nil;
        mutateSource = YES;
        error = nil;
        assert(!installApplication(source, data, &error) && error.code == ESTALE);
        mutateSource = NO;
        missingRecord = YES;
        error = nil;
        assert(!installApplication(source, data, &error) && [error.domain isEqual:@"TestLaunchServices"] && error.code == 42);
        missingRecord = NO;
        recordBundle = [NSURL fileURLWithPath:runtime];
        error = nil;
        assert(!inspectApplication(recordIdentifier, data, &error) && error.code == EPERM);
        recordBundle = [NSURL fileURLWithPath:bundle];
        recordData = [NSURL fileURLWithPath:runtime];
        error = nil;
        assert(!inspectApplication(recordIdentifier, data, &error) && error.code == EPERM);
        recordData = nil;
        assert(inspectApplication(recordIdentifier, data, &error));
        recordBundle = [NSURL URLWithString:@"https://example.invalid/Test.app"];
        error = nil;
        assert(!inspectApplication(recordIdentifier, data, &error) && error.code == EIO);
        assert([NSFileManager.defaultManager removeItemAtPath:base error:NULL]);
        puts("ApplicationInstaller tests passed");
    }
    return 0;
}
