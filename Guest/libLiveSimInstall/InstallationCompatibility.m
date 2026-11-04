#import "InstallationStaging.h"
#import "../../ProcessSupport/RuntimeLayout.h"
#import <objc/runtime.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern uint32_t dyld_get_active_platform(void);
static NSURL *stagingBase;
static const char HelperPath[] = "/System/Library/PrivateFrameworks/MobileInstallation.framework/XPCServices/"
    "com.apple.MobileInstallationHelperService.xpc/com.apple.MobileInstallationHelperService";
static const unsigned char HelperUUID[] = {0x3f, 0x7a, 0xbd, 0x1c, 0x04, 0xd6, 0x3a, 0x63, 0x9c, 0x61, 0x73, 0x11, 0x76, 0xb1, 0x22, 0x63};

static BOOL pinnedMethod(const struct mach_header_64 *header, IMP implementation) {
    // Original 23A343 helper, MIStagingManager method 0x100001290. Subsystem 3 at 0x100001368 hardcodes /private/var/tmp.
    static const uint32_t instructions[] = {0xd10143ff, 0xa9015ff8, 0xa90257f6, 0xa9034ff4};
    if (!header || header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64 || header->filetype != MH_EXECUTE ||
        header->sizeofcmds > 1024 * 1024 || (uintptr_t)implementation != (uintptr_t)header + 0x1290) return NO;
    const unsigned char *cursor = (const unsigned char *)(header + 1), *end = cursor + header->sizeofcmds;
    BOOL uuidMatches = NO, textContainsMethod = NO;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if ((size_t)(end - cursor) < sizeof(struct load_command)) return NO;
        const struct load_command *command = (const void *)cursor;
        if (command->cmdsize < sizeof(*command) || command->cmdsize > (size_t)(end - cursor)) return NO;
        if (command->cmd == LC_UUID && command->cmdsize == sizeof(struct uuid_command))
            uuidMatches = !memcmp(((const struct uuid_command *)command)->uuid, HelperUUID, sizeof(HelperUUID));
        if (command->cmd == LC_SEGMENT_64 && command->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *segment = (const void *)command;
            if (!memcmp(segment->segname, "__TEXT\0", 7) && !segment->fileoff && segment->filesize >= 0x1290 + sizeof(instructions) &&
                segment->vmsize >= segment->filesize && (segment->initprot & VM_PROT_EXECUTE)) textContainsMethod = YES;
        }
        cursor += command->cmdsize;
    }
    return uuidMatches && textContainsMethod && !memcmp((const void *)implementation, instructions, sizeof(instructions));
}

static BOOL matchesHelperPath(const char *path, const char *expected) {
    if (!path) return NO;
    if (!strcmp(path, HelperPath)) return YES;
    char canonical[PATH_MAX];
    return realpath(path, canonical) && !strcmp(canonical, expected);
}

static NSURL *validatedStagingBase(const char *runtimeBytes, const char *dataBytes, const char *ownerPath, const char *executable) {
    if (!runtimeBytes || runtimeBytes[0] != '/' || !dataBytes || dataBytes[0] != '/') return nil;
    char runtime[PATH_MAX], data[PATH_MAX], expected[PATH_MAX], canonical[PATH_MAX];
    if (!realpath(runtimeBytes, runtime) || !realpath(dataBytes, data)) return nil;
    const char *last = strrchr(runtime, '/');
    if (!last || strcmp(last, "/" LSB_RUNTIME_DIRECTORY)) return nil;
    size_t parentLength = (size_t)(last - runtime);
    static const char buildSuffix[] = "/" LSB_RUNTIME_BUILD;
    if (parentLength < sizeof(buildSuffix) - 1 || memcmp(last - sizeof(buildSuffix) + 1, buildSuffix, sizeof(buildSuffix) - 1)) return nil;
    int length = snprintf(expected, sizeof(expected), "%.*s/" LSB_DATA_DIRECTORY, (int)parentLength, runtime);
    if (length < 0 || (size_t)length >= sizeof(expected) || strcmp(data, expected)) return nil;
    length = snprintf(expected, sizeof(expected), "%s%s", runtime, HelperPath);
    if (length < 0 || (size_t)length >= sizeof(expected)) return nil;
    // Check both the method's owning image and the process executable; image index zero may be an inserted library.
    if (!matchesHelperPath(ownerPath, expected) || !matchesHelperPath(executable, expected)) return nil;
    int root = open(data, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root < 0) return nil;
    int temporary = openat(root, "tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(root);
    if (temporary < 0) return nil;
    struct stat metadata;
    BOOL valid = !fstat(temporary, &metadata) && S_ISDIR(metadata.st_mode);
    close(temporary);
    if (!valid) return nil;
    length = snprintf(expected, sizeof(expected), "%s/tmp", data);
    if (length < 0 || (size_t)length >= sizeof(expected) || !realpath(expected, canonical) || strcmp(expected, canonical)) return nil;
    return [NSURL fileURLWithPath:@(expected) isDirectory:YES];
}

@interface LSBInstallationStagingAdapter : NSObject
- (MIStagingRecord *)lsb_stagingRecordForSystemContentWithinSubsystem:(NSUInteger)subsystem error:(NSError **)error;
@end

@implementation LSBInstallationStagingAdapter
- (MIStagingRecord *)lsb_stagingRecordForSystemContentWithinSubsystem:(NSUInteger)subsystem error:(NSError **)error {
    MIStagingRecord *original = [(MIStagingManager *)self lsb_stagingRecordForSystemContentWithinSubsystem:subsystem error:error];
    if (subsystem != 3 || !original) return original;
    if (!stagingBase || ![original.stagingBase.path isEqual:@"/private/var/tmp"] || ![original.relativePath isEqual:@"com.apple.appinstall.temp"]) {
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOTSUP
            userInfo:@{NSLocalizedDescriptionKey: @"The simulator installation staging record does not match the pinned layout."}];
        return nil;
    }
    // Return a physical base at the producer. Every consumer and sandbox-extension request then sees the same guest-owned URL.
    return [[original.class alloc] initWithStagingBase:stagingBase relativePath:original.relativePath];
}
@end

static BOOL installAdapter(Class manager, Method original) {
    SEL alias = @selector(lsb_stagingRecordForSystemContentWithinSubsystem:error:);
    if (!manager || !original || strcmp(method_getTypeEncoding(original), "@32@0:8Q16^@24") || class_getInstanceMethod(manager, alias)) return NO;
    Method replacement = class_getInstanceMethod(LSBInstallationStagingAdapter.class, alias);
    if (!replacement || !class_addMethod(manager, alias, method_getImplementation(replacement), method_getTypeEncoding(original))) return NO;
    method_exchangeImplementations(original, class_getInstanceMethod(manager, alias));
    return YES;
}

#ifndef LSB_INSTALLATION_COMPATIBILITY_TESTING
__attribute__((constructor)) static void installStagingCompatibility(void) {
    // This role-only Objective-C dylib runs after guest libSystem/Foundation initialization; the core early shim stays pure C.
    @autoreleasepool {
        if (dyld_get_active_platform() != 7) return;
        Class manager = objc_lookUpClass("MIStagingManager");
        Method method = class_getInstanceMethod(manager, @selector(stagingRecordForSystemContentWithinSubsystem:error:));
        IMP implementation = method ? method_getImplementation(method) : NULL;
        Dl_info owner = {0};
        if (!implementation || !dladdr((const void *)implementation, &owner) || !pinnedMethod(owner.dli_fbase, implementation)) {
            fprintf(stderr, "[app-install] unsupported MobileInstallationHelperService staging method\n");
            return;
        }
        char executable[PATH_MAX];
        uint32_t capacity = sizeof(executable);
        if (!_NSGetExecutablePath(executable, &capacity))
            stagingBase = validatedStagingBase(getenv("DYLD_ROOT_PATH"), getenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY"), owner.dli_fname, executable);
        if (!stagingBase || !installAdapter(manager, method)) {
            fprintf(stderr, "[app-install] could not configure guest-owned installation staging\n");
            stagingBase = nil;
        }
    }
}
#endif
