#import <Foundation/Foundation.h>
#include <assert.h>
#define LSB_INSTALLATION_COMPATIBILITY_TESTING 1
#define MIStagingManager LSBTestStagingManager
#define MIStagingRecord LSBTestStagingRecord
#include "../Guest/InstallationCompatibility.m"

static NSUInteger originalCalls;
static BOOL incompatibleRecord;

@implementation MIStagingRecord
- (instancetype)initWithStagingBase:(NSURL *)base relativePath:(NSString *)relativePath {
    if ((self = [super init])) { _stagingBase = base; _relativePath = relativePath; }
    return self;
}
@end

@implementation MIStagingManager
- (MIStagingRecord *)stagingRecordForSystemContentWithinSubsystem:(NSUInteger)subsystem error:(NSError **)error {
    ++originalCalls;
    if (subsystem == 99) {
        if (error) *error = [NSError errorWithDomain:@"OriginalStagingError" code:7 userInfo:nil];
        return nil;
    }
    return [[MIStagingRecord alloc] initWithStagingBase:[NSURL fileURLWithPath:subsystem == 3 ? @"/private/var/tmp" : @"/unchanged"]
        relativePath:subsystem == 3 && !incompatibleRecord ? @"com.apple.appinstall.temp" : @"original"];
}
@end

static void pinTests(void) {
    unsigned char *image = calloc(1, 0x4000);
    assert(image);
    struct mach_header_64 *header = (void *)image;
    *header = (struct mach_header_64){ .magic = MH_MAGIC_64, .cputype = CPU_TYPE_ARM64, .filetype = MH_EXECUTE, .ncmds = 2,
        .sizeofcmds = sizeof(struct uuid_command) + sizeof(struct segment_command_64) };
    struct uuid_command *uuid = (void *)(header + 1);
    *uuid = (struct uuid_command){ .cmd = LC_UUID, .cmdsize = sizeof(*uuid) };
    memcpy(uuid->uuid, HelperUUID, sizeof(HelperUUID));
    struct segment_command_64 *text = (void *)(uuid + 1);
    *text = (struct segment_command_64){ .cmd = LC_SEGMENT_64, .cmdsize = sizeof(*text), .filesize = 0x4000, .vmsize = 0x4000,
        .initprot = VM_PROT_READ | VM_PROT_EXECUTE };
    memcpy(text->segname, "__TEXT", 7);
    uint32_t *instructions = (void *)(image + 0x1290);
    const uint32_t expected[] = {0xd10143ff, 0xa9015ff8, 0xa90257f6, 0xa9034ff4};
    memcpy(instructions, expected, sizeof(expected));
    IMP method = (IMP)instructions;
    assert(pinnedMethod(header, method));
    assert(!pinnedMethod(header, (IMP)(image + 0x1294)));
    uuid->uuid[0] ^= 1;
    assert(!pinnedMethod(header, method));
    uuid->uuid[0] ^= 1;
    instructions[0] ^= 1;
    assert(!pinnedMethod(header, method));
    instructions[0] ^= 1;
    text->filesize = 0x1290;
    assert(!pinnedMethod(header, method));
    text->filesize = 0x4000;
    text->initprot = VM_PROT_READ;
    assert(!pinnedMethod(header, method));
    text->initprot |= VM_PROT_EXECUTE;
    uuid->cmdsize = 0;
    assert(!pinnedMethod(header, method));
    free(image);
}

int main(void) {
    @autoreleasepool {
        pinTests();
        char temporary[] = "/tmp/LSB-installation-staging.XXXXXX", canonical[PATH_MAX];
        assert(mkdtemp(temporary) && realpath(temporary, canonical));
        NSString *base = @(canonical);
        NSString *runtime = [base stringByAppendingPathComponent:@"LSB/iOS/23A343/RuntimeRoot"];
        NSString *data = [base stringByAppendingPathComponent:@"LSB/iOS/23A343/data"];
        NSString *tmp = [data stringByAppendingPathComponent:@"tmp"];
        NSString *helper = [runtime stringByAppendingString:@(HelperPath)];
        assert([NSFileManager.defaultManager createDirectoryAtPath:helper.stringByDeletingLastPathComponent
            withIntermediateDirectories:YES attributes:nil error:NULL]);
        assert([NSData.data writeToFile:helper atomically:NO]);
        assert([NSFileManager.defaultManager createDirectoryAtPath:tmp withIntermediateDirectories:YES attributes:nil error:NULL]);
        stagingBase = validatedStagingBase(runtime.fileSystemRepresentation, data.fileSystemRepresentation, helper.fileSystemRepresentation, helper.fileSystemRepresentation);
        assert([stagingBase.path isEqual:tmp]);
        assert(validatedStagingBase(runtime.fileSystemRepresentation, data.fileSystemRepresentation, HelperPath, HelperPath));
        assert(!validatedStagingBase(runtime.fileSystemRepresentation, data.fileSystemRepresentation, HelperPath, "/usr/bin/other"));
        assert(!validatedStagingBase(runtime.fileSystemRepresentation, data.fileSystemRepresentation, "/usr/lib/libLiveSimSpawn.dylib", HelperPath));
        assert(!validatedStagingBase(runtime.fileSystemRepresentation, data.fileSystemRepresentation, NULL, HelperPath));
        assert(!validatedStagingBase(runtime.fileSystemRepresentation, data.fileSystemRepresentation, HelperPath, NULL));
        assert(!validatedStagingBase(runtime.fileSystemRepresentation, base.fileSystemRepresentation, HelperPath, HelperPath));
        assert(!validatedStagingBase(data.fileSystemRepresentation, data.fileSystemRepresentation, HelperPath, HelperPath));
        assert(!rmdir(tmp.fileSystemRepresentation));
        assert(!validatedStagingBase(runtime.fileSystemRepresentation, data.fileSystemRepresentation, HelperPath, HelperPath));
        assert(!symlink(base.fileSystemRepresentation, tmp.fileSystemRepresentation));
        assert(!validatedStagingBase(runtime.fileSystemRepresentation, data.fileSystemRepresentation, HelperPath, HelperPath));
        assert(!unlink(tmp.fileSystemRepresentation));
        assert([NSFileManager.defaultManager createDirectoryAtPath:tmp withIntermediateDirectories:NO attributes:nil error:NULL]);

        MIStagingManager *manager = [MIStagingManager new];
        Method original = class_getInstanceMethod(MIStagingManager.class, @selector(stagingRecordForSystemContentWithinSubsystem:error:));
        assert(installAdapter(MIStagingManager.class, original));
        assert(!installAdapter(MIStagingManager.class, original)); // Never exchange the selectors twice.
        NSError *error = nil;
        MIStagingRecord *record = [manager stagingRecordForSystemContentWithinSubsystem:3 error:&error];
        assert(record && !error && [record.stagingBase.path isEqual:tmp] && [record.relativePath isEqual:@"com.apple.appinstall.temp"]);
        assert(originalCalls == 1);
        record = [manager stagingRecordForSystemContentWithinSubsystem:1 error:&error];
        assert(record && !error && [record.stagingBase.path isEqual:@"/unchanged"] && [record.relativePath isEqual:@"original"]);
        record = [manager stagingRecordForSystemContentWithinSubsystem:2 error:&error];
        assert(record && !error && [record.stagingBase.path isEqual:@"/unchanged"]);
        assert(![manager stagingRecordForSystemContentWithinSubsystem:99 error:&error]);
        assert([error.domain isEqual:@"OriginalStagingError"] && error.code == 7 && originalCalls == 4);
        incompatibleRecord = YES;
        error = nil;
        assert(![manager stagingRecordForSystemContentWithinSubsystem:3 error:&error] && error.code == ENOTSUP);
        incompatibleRecord = NO;
        stagingBase = nil;
        error = nil;
        assert(![manager stagingRecordForSystemContentWithinSubsystem:3 error:&error] && error.code == ENOTSUP);
        assert([NSFileManager.defaultManager removeItemAtPath:base error:NULL]);
        puts("InstallationCompatibility tests passed");
    }
    return 0;
}
