#import "RuntimeStorage.h"
#include "../ProcessSupport/StoreAppGroups.h"
#include "../ProcessSupport/RuntimeLayout.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static NSError *storageError(int code, NSString *message, NSURL *url) {
    NSMutableDictionary *details = [@{NSLocalizedDescriptionKey: message} mutableCopy];
    if (url) details[NSURLErrorKey] = url;
    return [NSError errorWithDomain:NSPOSIXErrorDomain code:code userInfo:details];
}

static NSArray<NSString *> *identifierArray(const LSBStoreAppGroups *groups) {
    NSMutableArray<NSString *> *identifiers = [NSMutableArray arrayWithCapacity:groups->count];
    for (size_t index = 0; index < groups->count; ++index)
        [identifiers addObject:[NSString stringWithUTF8String:groups->identifiers[index]]];
    return identifiers;
}

// Marker and storage-root symlinks are rejected, including dangling symlinks. Following one could select another store's tree.
static int directoryAt(int parent, const char *name, BOOL *exists) {
    struct stat status;
    *exists = NO;
    if (fstatat(parent, name, &status, AT_SYMLINK_NOFOLLOW)) return errno == ENOENT ? 0 : errno;
    if (S_ISLNK(status.st_mode)) return ELOOP;
    if (!S_ISDIR(status.st_mode)) return ENOTDIR;
    *exists = YES;
    return 0;
}

static int validateHierarchy(NSURL *container) {
    int descriptor = open(container.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (descriptor < 0) return errno;
    int error = 0;
    const char *components[] = {LSB_STORAGE_DIRECTORY, LSB_RUNTIME_PLATFORM, LSB_RUNTIME_BUILD};
    for (unsigned index = 0; index < sizeof(components) / sizeof(*components); ++index) {
        BOOL exists;
        error = directoryAt(descriptor, components[index], &exists);
        if (error || !exists) { close(descriptor); return error; } // Absent runtime storage is reported by startup; selection never creates it.
        int next = openat(descriptor, components[index], O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        if (next < 0) error = errno;
        close(descriptor);
        if (error) return error;
        descriptor = next;
    }
    BOOL exists;
    error = directoryAt(descriptor, LSB_RUNTIME_DIRECTORY, &exists);
    if (!error) error = directoryAt(descriptor, LSB_DATA_DIRECTORY, &exists);
    close(descriptor);
    return error;
}

@implementation LSBRuntimeStorage
- (instancetype)initWithResolutionError:(int)error {
    if (!(self = [super init])) return nil;
    _error = storageError(error, @"The signed App Group entitlements do not provide valid SideStore or AltStore groups.", nil);
    return self;
}
+ (LSBRuntimeStorage *)sharedStorage {
    static LSBRuntimeStorage *storage;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        LSBStoreAppGroups groups;
        int error = LSBCopyStoreAppGroups(&groups);
        if (error) { storage = [[self alloc] initWithResolutionError:error]; return; }
        storage = [[self alloc] initWithAppGroupIdentifiers:identifierArray(&groups) containerProvider:^NSURL *(NSString *identifier) {
            return [NSFileManager.defaultManager containerURLForSecurityApplicationGroupIdentifier:identifier];
        }];
    });
    return storage;
}

- (instancetype)initWithAppGroupIdentifiers:(NSArray<NSString *> *)identifiers containerProvider:(LSBAppGroupContainerProvider)provider {
    if (!(self = [super init])) return nil;
    if (!provider) { _error = storageError(EINVAL, @"An App Group container provider is required.", nil); return self; }
    const char *candidates[LSB_STORE_APP_GROUP_COUNT] = {NULL};
    BOOL valid = [identifiers isKindOfClass:NSArray.class] && identifiers.count <= LSB_STORE_APP_GROUP_COUNT;
    for (NSUInteger index = 0; valid && index < identifiers.count; ++index) {
        id identifier = identifiers[index];
        valid = [identifier isKindOfClass:NSString.class] && (candidates[index] = [identifier UTF8String]) != NULL;
        if (valid) valid = strlen(candidates[index]) == [identifier lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    }
    LSBStoreAppGroups groups;
    int error = valid ? LSBParseStoreAppGroups(candidates, identifiers.count, &groups) : EINVAL;
    if (error) {
        _error = storageError(error, @"Signed store App Group identifiers are missing, malformed or ambiguous.", nil);
        return self;
    }
    NSURL *selectedURL = nil;
    NSString *selectedIdentifier = nil;
    unsigned selectedRank = 0;
    for (NSString *identifier in identifierArray(&groups)) {
        NSURL *container = provider(identifier);
        if (!container) continue; // An unprovisioned/unavailable store is not a candidate.
        if (!container.isFileURL || !container.path.isAbsolutePath) {
            _error = storageError(EINVAL, @"The App Group provider returned a non-absolute file URL.", container);
            return self;
        }
        int descriptor = open(container.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        if (descriptor < 0) {
            _error = storageError(errno, @"The available App Group container could not be opened as a real directory.", container);
            return self;
        }
        BOOL hasStorage = NO, hasApps = NO;
        int error = directoryAt(descriptor, LSB_STORAGE_DIRECTORY, &hasStorage);
        if (!error) error = directoryAt(descriptor, "Apps", &hasApps);
        close(descriptor);
        if (error) {
            _error = storageError(error, @"App Group LSB and Apps entries must be directories, not files or symlinks.", container);
            return self;
        }
        unsigned rank = hasStorage ? 3 : hasApps ? 2 : 1;
        if (rank > selectedRank) {
            selectedRank = rank;
            selectedURL = container;
            selectedIdentifier = identifier;
        }
    }
    if (!selectedURL) {
        _error = storageError(EACCES, @"Neither store App Group container is available. Check the signed App Group entitlements and profiles.", nil);
        return self;
    }
    NSURL *storage = [selectedURL URLByAppendingPathComponent:@LSB_STORAGE_BUILD_RELATIVE_PATH isDirectory:YES];
    error = validateHierarchy(selectedURL);
    if (error) {
        _error = storageError(error, @"Every component of LSB/iOS/23A343, RuntimeRoot and data must be a directory, not a file or symlink.", storage);
        return self;
    }
    _appGroupIdentifier = [selectedIdentifier copy];
    _containerURL = [selectedURL copy];
    _storageURL = storage;
    _runtimeRootURL = [storage URLByAppendingPathComponent:@LSB_RUNTIME_DIRECTORY isDirectory:YES];
    _dataURL = [storage URLByAppendingPathComponent:@LSB_DATA_DIRECTORY isDirectory:YES];
    return self;
}
@end
