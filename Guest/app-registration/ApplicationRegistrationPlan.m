#import "ApplicationRegistrationPlan.h"

#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <limits.h>
#include <mach-o/loader.h>
#include <sys/stat.h>
#include <unistd.h>

static const NSUInteger MaximumApplications = 512;
static const off_t MaximumMetadataBytes = 1024 * 1024;

static id fail(NSError **error, int code, NSString *description) {
    if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:code userInfo:@{NSLocalizedDescriptionKey: description}];
    return nil;
}

static BOOL validString(id value) {
    return [value isKindOfClass:NSString.class] && [value length] && [value canBeConvertedToEncoding:NSUTF8StringEncoding] &&
        [value rangeOfString:@"\0"].location == NSNotFound && [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] < PATH_MAX;
}

static BOOL validGuestPath(id value) {
    if (!validString(value) || ![value hasPrefix:@"/"] || [value hasSuffix:@"/"]) return NO;
    NSArray *parts = [value componentsSeparatedByString:@"/"];
    for (NSUInteger i = 1; i < parts.count; ++i) {
        if (![parts[i] length] || [parts[i] isEqual:@"."] || [parts[i] isEqual:@".."]) return NO;
    }
    return parts.count > 1;
}

static BOOL validAppPath(id path) {
    if (!validGuestPath(path) || ![path hasSuffix:@".app"] || [[path lastPathComponent] length] <= 4) return NO;
    return [@[@"/Applications", @"/System/Applications", @"/System/Library/CoreServices"] containsObject:[path stringByDeletingLastPathComponent]];
}

static NSString *canonicalPath(NSString *path, NSError **error) {
    if (!validString(path) || ![path hasPrefix:@"/"]) return fail(error, EINVAL, @"path must be absolute UTF-8");
    char resolved[PATH_MAX];
    if (!realpath(path.fileSystemRepresentation, resolved)) return fail(error, errno, @"cannot resolve path");
    return [NSFileManager.defaultManager stringWithFileSystemRepresentation:resolved length:strlen(resolved)];
}

static NSString *containedPath(NSString *root, NSString *guestPath, NSError **error) {
    if (!validGuestPath(guestPath)) return fail(error, EINVAL, @"invalid absolute guest path");
    NSString *canonicalRoot = canonicalPath(root, error);
    if (!canonicalRoot) return nil;
    if ([canonicalRoot isEqual:@"/"]) return fail(error, EINVAL, @"RuntimeRoot cannot be the native filesystem root");
    struct stat metadata;
    if (stat(canonicalRoot.fileSystemRepresentation, &metadata) || !S_ISDIR(metadata.st_mode))
        return fail(error, ENOTDIR, @"RuntimeRoot is not a directory");
    NSString *physical = [canonicalRoot stringByAppendingString:guestPath];
    NSString *resolved = canonicalPath(physical, error);
    if (!resolved) return nil;
    // Only the root itself may use a harmless /var alias. Internal symlinks cannot change the selected app or manifest.
    if (![resolved isEqual:physical]) return fail(error, ELOOP, @"guest path traverses a symbolic link");
    return resolved;
}

static NSData *readMetadata(NSString *path, NSError **error) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return fail(error, errno, @"cannot open metadata");
    struct stat metadata;
    int code = fstat(fd, &metadata) ? errno : 0;
    if (!code && !S_ISREG(metadata.st_mode)) code = EINVAL;
    if (!code && (metadata.st_size <= 0 || metadata.st_size > MaximumMetadataBytes)) code = EFBIG;
    NSMutableData *data = code ? nil : [NSMutableData dataWithLength:(NSUInteger)metadata.st_size];
    size_t position = 0;
    while (!code && position < data.length) {
        ssize_t count = pread(fd, (char *)data.mutableBytes + position, data.length - position, (off_t)position);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) code = count < 0 ? errno : EIO;
        else position += (size_t)count;
    }
    close(fd);
    if (code) return fail(error, code, @"metadata must be a readable regular file of at most 1 MiB");
    return data;
}

NSArray<NSString *> *LSBReadApplicationRegistrationManifest(NSString *root, NSString *manifestPath, NSError **error) {
    if (error) *error = nil;
    NSString *physical = containedPath(root, manifestPath, error);
    if (!physical) return nil;
    NSData *data = readMetadata(physical, error);
    if (!data) return nil;
    id manifest = [NSJSONSerialization JSONObjectWithData:data options:0 error:error];
    if (![manifest isKindOfClass:NSDictionary.class] || [manifest count] != 2 || ![manifest[@"version"] isEqual:@1] ||
        ![manifest[@"applications"] isKindOfClass:NSArray.class]) return fail(error, EINVAL, @"invalid application manifest schema");
    NSArray *applications = manifest[@"applications"];
    if (!applications.count || applications.count > MaximumApplications) return fail(error, E2BIG, @"manifest must select 1 through 512 apps");
    NSMutableSet *seen = [NSMutableSet set];
    for (id path in applications) {
        if (!validAppPath(path) || [seen containsObject:path]) return fail(error, EINVAL, @"invalid or duplicate top-level app path");
        [seen addObject:path];
    }
    return [applications copy];
}

static NSURL *prepareBundle(NSString *physical, NSString **identifier, NSString **skipReason, NSError **error) {
    if (identifier) *identifier = nil;
    if (skipReason) *skipReason = nil;
    if (error) *error = nil;
    struct stat metadata;
    if (stat(physical.fileSystemRepresentation, &metadata) || !S_ISDIR(metadata.st_mode)) return fail(error, ENOTDIR, @"app is not a directory");
    NSString *infoPath = [physical stringByAppendingPathComponent:@"Info.plist"];
    if (lstat(infoPath.fileSystemRepresentation, &metadata)) {
        if (errno != ENOENT) return fail(error, errno, @"cannot inspect Info.plist");
        if (skipReason) *skipReason = @"missing-info-plist";
        return nil;
    }
    NSData *data = readMetadata(infoPath, error);
    if (!data) return nil;
    id info = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:NULL error:error];
    if (![info isKindOfClass:NSDictionary.class]) return fail(error, EINVAL, @"invalid Info.plist dictionary");
    NSString *bundleID = info[@"CFBundleIdentifier"], *executable = info[@"CFBundleExecutable"];
    if (!validString(bundleID)) return fail(error, EINVAL, @"missing or invalid CFBundleIdentifier");
    if (!executable) {
        if (skipReason) *skipReason = @"no-executable-declaration";
        return nil;
    }
    if (!validString(executable) || [executable containsString:@"/"] || [@[@".", @".."] containsObject:executable])
        return fail(error, EINVAL, @"CFBundleExecutable must be a single filename");
    NSString *executablePath = [physical stringByAppendingPathComponent:executable];
    int fd = open(executablePath.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno != ENOENT) return fail(error, errno, @"cannot open app executable");
        if (skipReason) *skipReason = @"missing-executable";
        return nil;
    }
    struct mach_header_64 header;
    BOOL regular = !fstat(fd, &metadata) && S_ISREG(metadata.st_mode);
    ssize_t count;
    do { count = pread(fd, &header, sizeof(header), 0); } while (count < 0 && errno == EINTR);
    close(fd);
    if (!regular || count != sizeof(header) || header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_ARM64 || header.filetype != MH_EXECUTE)
        return fail(error, ENOEXEC, @"app executable is not a thin arm64 Mach-O executable");
    if (identifier) *identifier = bundleID;
    return [NSURL fileURLWithPath:physical isDirectory:YES];
}

NSURL *LSBPrepareApplicationRegistration(NSString *root, NSString *guestPath, NSString **identifier, NSString **skipReason, NSError **error) {
    if (identifier) *identifier = nil;
    if (skipReason) *skipReason = nil;
    if (error) *error = nil;
    if (!validAppPath(guestPath)) return fail(error, EINVAL, @"app must be in an allowed top-level stock directory");
    NSString *physical = containedPath(root, guestPath, error);
    return physical ? prepareBundle(physical, identifier, skipReason, error) : nil;
}

BOOL LSBVerifyApplicationRegistration(NSURL *expectedURL, NSString *expectedIdentifier, NSURL *registeredURL,
                                    NSString *registeredIdentifier, BOOL installed, NSError **error) {
    if (error) *error = nil;
    if (!installed || ![registeredIdentifier isEqual:expectedIdentifier] || !registeredURL.isFileURL || !expectedURL.isFileURL) {
        fail(error, EIO, @"LaunchServices record is missing or has a different identifier");
        return NO;
    }
    NSString *expected = canonicalPath(expectedURL.path, error);
    if (!expected) return NO;
    NSString *registered = canonicalPath(registeredURL.path, error);
    if (!registered) return NO;
    if (![registered isEqual:expected]) {
        fail(error, EXDEV, @"LaunchServices record points to another bundle");
        return NO;
    }
    return YES;
}

static BOOL reject(NSError **error, int code, NSString *detail) {
    fail(error, code, detail);
    return NO;
}

static NSString *dpkgApplicationsPath(NSString *runtime, NSString *data, NSString *root, NSError **error) {
    if (!validGuestPath(runtime) || ![runtime.lastPathComponent isEqual:@"RuntimeRoot"] || !validGuestPath(data) || !validGuestPath(root) ||
        ![[runtime.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"data"] isEqual:data] ||
        (![root isEqual:data] && ![root hasPrefix:[data stringByAppendingString:@"/"]]))
        return fail(error, EACCES, @"expected a dpkg root inside the selected simulator data directory");
    if (![canonicalPath(runtime, error) isEqual:runtime]) return fail(error, EACCES, @"RuntimeRoot must be canonical");
    NSString *bootstrap = [root stringByAppendingString:@"/var/jb"];
    if (![canonicalPath(bootstrap, error) isEqual:bootstrap]) return fail(error, EACCES, @"dpkg bootstrap must exist without symlinks");
    struct stat s;
    if (stat(runtime.fileSystemRepresentation, &s) || !S_ISDIR(s.st_mode) ||
        stat(bootstrap.fileSystemRepresentation, &s) || !S_ISDIR(s.st_mode)) return fail(error, ENOTDIR, @"invalid runtime or dpkg bootstrap");
    return [bootstrap stringByAppendingPathComponent:@"Applications"];
}

// LaunchServices can inspect nested bundle metadata and plug-ins. Permit internal framework links, but never hand it
// a bundle with a dangling link or a link into another application, transaction root or native directory.
static BOOL checkBundleLinks(NSString *path, NSError **error) {
    char *paths[] = {(char *)path.fileSystemRepresentation, NULL};
    FTS *walk = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    if (!walk) return reject(error, errno, @"cannot inspect package bundle");
    int code = 0;
    const size_t length = strlen(paths[0]);
    FTSENT *entry;
    while ((entry = fts_read(walk))) {
        if (entry->fts_info == FTS_ERR || entry->fts_info == FTS_DNR || entry->fts_info == FTS_NS) { code = entry->fts_errno ?: EIO; break; }
        if (entry->fts_info != FTS_SL && entry->fts_info != FTS_SLNONE) continue;
        char resolved[PATH_MAX];
        if (!realpath(entry->fts_accpath, resolved)) { code = errno; break; }
        if (strncmp(resolved, paths[0], length) || resolved[length] != '/') { code = EACCES; break; }
    }
    if (!entry) code = errno;
    fts_close(walk);
    return !code || reject(error, code, [NSString stringWithFormat:@"cannot confine all bundle entries to %@", path]);
}

static NSDictionary<NSString *, NSURL *> *dpkgInventory(NSString *directory, NSError **error) {
    struct stat s;
    if (lstat(directory.fileSystemRepresentation, &s)) {
        if (errno == ENOENT) return @{};
        return fail(error, errno, @"cannot inspect package Applications directory");
    }
    if (!S_ISDIR(s.st_mode)) return fail(error, EINVAL, @"package Applications must be a directory, not a symlink");
    NSArray<NSString *> *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:directory error:error];
    if (!names) return nil;
    NSMutableDictionary *apps = [NSMutableDictionary dictionary];
    for (NSString *name in names) {
        if (![name hasSuffix:@".app"]) continue;
        NSString *path = [directory stringByAppendingPathComponent:name], *identifier = nil, *skip = nil;
        if (!validGuestPath(path) || name.length <= 4 || ![canonicalPath(path, error) isEqual:path])
            return fail(error, EACCES, @"package app is not a canonical bundle");
        if (!checkBundleLinks(path, error)) return nil;
        NSURL *url = prepareBundle(path, &identifier, &skip, error);
        if (!url) return skip ? fail(error, ENOEXEC, [NSString stringWithFormat:@"%@: %@", name, skip]) : nil;
        if ([identifier.lowercaseString hasPrefix:@"com.apple."]) return fail(error, EACCES, @"reserved Apple app identifier");
        if (apps[identifier]) return fail(error, EEXIST, @"duplicate package app identifier");
        if (apps.count >= MaximumApplications) return fail(error, E2BIG, @"too many package apps");
        apps[identifier] = url;
    }
    return apps;
}

// LaunchServices may use the /var spelling of a physical /private/var file URL. Never resolve an untrusted or already-removed URL through a symlink.
static NSString *recordPath(NSURL *url) {
    if (!url.isFileURL || (url.host.length && ![url.host isEqual:@"localhost"])) return nil;
    NSString *path = url.path;
    if (!validGuestPath(path)) return nil;
    if ([path hasPrefix:@"/var/"] || [path hasPrefix:@"/tmp/"]) return [@"/private" stringByAppendingString:path];
    return path;
}

BOOL LSBRefreshDpkgApplications(NSString *runtime, NSString *data, NSString *dpkgRoot, LSBReadGuestApplications readRecords,
                                LSBChangeGuestApplication change, NSError **error) {
    if (error) *error = nil;
    NSString *directory = dpkgApplicationsPath(runtime, data, dpkgRoot, error);
    if (!directory) return NO;
    NSDictionary<NSString *, NSURL *> *installed = dpkgInventory(directory, error);
    if (!installed) return NO;
    NSDictionary<NSString *, NSURL *> *records = readRecords(error);
    if (!records) return NO;
    NSMutableDictionary<NSString *, NSString *> *idsByPath = [NSMutableDictionary dictionary];
    NSMutableDictionary<NSString *, NSURL *> *stale = [NSMutableDictionary dictionary];
    for (NSString *identifier in records) {
        NSURL *record = records[identifier];
        NSString *path = recordPath(record);
        if (installed[identifier] && ![path isEqual:installed[identifier].path]) return reject(error, EEXIST, @"app identifier already belongs to another URL");
        if (![path.stringByDeletingLastPathComponent isEqual:directory] || ![path.lastPathComponent hasSuffix:@".app"]) continue;
        if (idsByPath[path] || [identifier.lowercaseString hasPrefix:@"com.apple."]) return reject(error, EACCES, @"ambiguous or protected app record");
        idsByPath[path] = identifier;
        if (!installed[identifier]) {
            struct stat s;
            if (!lstat(path.fileSystemRepresentation, &s) || errno != ENOENT) return reject(error, EEXIST, @"registered bundle is not confirmed absent");
            stale[identifier] = record;
        }
    }

    NSUInteger added = 0, removed = 0;
    // All bundles and conflicts were checked before changing LaunchServices. Registration must succeed before removing any stale entry.
    for (NSString *identifier in installed) {
        NSURL *url = installed[identifier];
        if (records[identifier]) continue;
        if (![canonicalPath(url.path, error) isEqual:url.path]) return reject(error, EAGAIN, @"package bundle moved before registration");
        if (!change(identifier, url, NO, error)) return NO;
        ++added;
    }
    // Verify all additions together. A failed registration must not cause removal of another app.
    if (added) {
        records = readRecords(error);
        if (!records) return NO;
        for (NSString *identifier in installed) {
            if (![recordPath(records[identifier]) isEqual:installed[identifier].path])
                return reject(error, EIO, [NSString stringWithFormat:@"guest registry did not confirm %@", identifier]);
        }
    }
    for (NSString *identifier in stale) {
        NSString *path = recordPath(stale[identifier]);
        if (!records[identifier]) continue;
        if (![recordPath(records[identifier]) isEqual:path] || ![dpkgApplicationsPath(runtime, data, dpkgRoot, error) isEqual:directory])
            return reject(error, EAGAIN, @"registration or installation root changed before removal");
        struct stat s;
        if (lstat(directory.fileSystemRepresentation, &s) ? errno != ENOENT : !S_ISDIR(s.st_mode))
            return reject(error, EAGAIN, @"package Applications directory changed before removal");
        if (!lstat(path.fileSystemRepresentation, &s) || errno != ENOENT) return reject(error, EAGAIN, @"bundle is no longer confirmed absent");
        if (!change(identifier, [NSURL fileURLWithPath:path isDirectory:YES], YES, error)) return NO;
        ++removed;
    }
    if (removed) {
        records = readRecords(error);
        if (!records) return NO;
        for (NSString *identifier in stale) if (records[identifier]) return reject(error, EIO, @"guest registry did not confirm removal");
    }
    printf("[app-registration] dpkg packages=%lu added=%lu removed=%lu\n", (unsigned long)installed.count,
           (unsigned long)added, (unsigned long)removed);
    return YES;
}
