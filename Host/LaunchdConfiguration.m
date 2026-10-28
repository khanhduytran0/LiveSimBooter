#include "../ProcessSupport/RuntimeLayout.h"
#include "../ProcessSupport/NativeRuntimePolicy.h"
#include "NativeProductionStatus.h"
#import <Foundation/Foundation.h>
#import "DeviceType.h"
#include <CommonCrypto/CommonDigest.h>
#include "LaunchdConfiguration.h"
#include "ChildStatus.h"
#include "DisplaySettings.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Keep the initial service selection bounded independently of slots needed later for apps, helpers and adopted children.
// Backboardd, cfprefsd and the terminal/settings helpers are direct launchd children alongside the selected services.
enum { BaselineLaunchdChildren = 4, MaximumSelectedServices = 20 };
_Static_assert(BaselineLaunchdChildren + MaximumSelectedServices < LSB_CHILD_STATUS_CAPACITY, "leave child-ledger slots for dynamic jobs");

static LSBGuestDeviceType *configurationDeviceType;
void LSBLaunchdConfigurationSetDeviceType(LSBGuestDeviceType *type) { configurationDeviceType = type; }

static int readStockPlist(NSString *path, int readFailure, NSDictionary **output) {
    NSError *error = nil;
    *output = [NSDictionary dictionaryWithContentsOfURL:[NSURL fileURLWithPath:path] error:&error];
    if (*output) return 0;
    // Preserve the existing distinction between unreadable files and invalid dictionary contents.
    return !error || ([error.domain isEqualToString:NSCocoaErrorDomain] && error.code == NSFileReadCorruptFileError) ? EINVAL : readFailure;
}

static int openDirectory(int parent, const char *name) {
    if (mkdirat(parent, name, 0700) && errno != EEXIST) return -1;
    return openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

static int writeConfiguration(int directory, const char *name, NSData *data) {
    struct stat status;
    if (!fstatat(directory, name, &status, AT_SYMLINK_NOFOLLOW)) {
        if (!S_ISREG(status.st_mode)) return S_ISLNK(status.st_mode) ? ELOOP : EINVAL;
    } else if (errno != ENOENT) return errno;
    char temporary[80];
    int length = snprintf(temporary, sizeof(temporary), ".launchd-bootstrap.%s", NSUUID.UUID.UUIDString.UTF8String);
    if (length < 0 || (size_t)length >= sizeof(temporary)) return ENAMETOOLONG;
    int fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return errno;
    int error = 0;
    const unsigned char *bytes = data.bytes;
    size_t remaining = data.length;
    while (remaining) {
        ssize_t count = write(fd, bytes, remaining);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = count < 0 ? errno : EIO; break; }
        bytes += count;
        remaining -= (size_t)count;
    }
    if (close(fd) && !error) error = errno;
    if (!error && renameat(directory, temporary, directory, name)) error = errno;
    if (error) unlinkat(directory, temporary, 0);
    return error;
}

static int checkJobDirectory(int directory, NSSet<NSString *> *jobNames) {
    int duplicate = fcntl(directory, F_DUPFD_CLOEXEC, 3);
    if (duplicate < 0) return errno;
    DIR *entries = fdopendir(duplicate);
    if (!entries) { int error = errno; close(duplicate); return error; }
    int error = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(entries);
        if (!entry) { error = errno; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        NSString *name = [NSString stringWithUTF8String:entry->d_name];
        if (name && [jobNames containsObject:name]) continue;
        // Never bootstrap or remove an unexpected job that was placed beside the generated probe.
        error = EEXIST;
        break;
    }
    closedir(entries);
    return error;
}

static int openRegularLog(int directory, const char *name) {
    int fd = openat(directory, name, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd < 0) return -1;
    struct stat status;
    int error = 0;
    if (fstat(fd, &status)) error = errno;
    else if (!S_ISREG(status.st_mode) || status.st_nlink != 1 || status.st_uid != geteuid()) error = EINVAL;
    else if (fcntl(fd, F_SETFL, O_WRONLY | O_APPEND)) error = errno;
    if (error) { close(fd); errno = error; return -1; }
    return fd;
}

static int prepareTouchPreferences(int directory, NSData **output) {
    *output = nil;
    NSMutableDictionary *preferences = [NSMutableDictionary dictionary];
    int fd = openat(directory, "com.apple.backboardd.plist", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0 && errno != ENOENT) return errno;
    if (fd >= 0) {
        struct stat status;
        int error = 0;
        if (fstat(fd, &status)) error = errno;
        else if (!S_ISREG(status.st_mode) || status.st_nlink != 1 || status.st_uid != geteuid()) error = EINVAL;
        else if (status.st_size < 0 || status.st_size > 1024 * 1024) error = EFBIG;
        NSMutableData *data = !error ? [NSMutableData dataWithLength:(NSUInteger)status.st_size] : nil;
        size_t offset = 0;
        while (!error && offset < data.length) {
            ssize_t count = read(fd, (unsigned char *)data.mutableBytes + offset, data.length - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { error = count < 0 ? errno : EIO; break; }
            offset += (size_t)count;
        }
        if (!error) {
            unsigned char extra;
            ssize_t count;
            do count = read(fd, &extra, 1); while (count < 0 && errno == EINTR);
            if (count) error = count < 0 ? errno : EAGAIN;
        }
        if (close(fd) && !error) error = errno;
        if (error) return error;
        id existing = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:NULL error:NULL];
        if (![existing isKindOfClass:NSDictionary.class]) return EINVAL;
        preferences = [existing mutableCopy];
        id enabled = preferences[@"BKDigitizerVisualizeTouches"];
        if (enabled) return CFGetTypeID((__bridge CFTypeRef)enabled) == CFBooleanGetTypeID() ? 0 : EINVAL;
    }
    // BKSLocalDefaults uses this domain/key. Only this isolated simulator's file is changed; never use host NSUserDefaults.
    preferences[@"BKDigitizerVisualizeTouches"] = @YES;
    *output = [NSPropertyListSerialization dataWithPropertyList:preferences format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
    return *output ? 0 : EINVAL;
}

static int addEnvironment(LSBSpawnPlan *plan, const char *entry) {
    if (plan->envc == LSB_MAX_VECTOR) return E2BIG;
    int error = LSBPlanAddString(plan, entry, &plan->envp[plan->envc]);
    if (!error) ++plan->envc;
    return error;
}

static BOOL controlledEnvironment(const char *entry, NSDictionary<NSString *, NSString *> *environment) {
    const char *equals = strchr(entry, '=');
    if (!equals) return NO;
    NSString *key = [[NSString alloc] initWithBytes:entry length:(NSUInteger)(equals - entry) encoding:NSUTF8StringEncoding];
    if (!key) return NO;
    return environment[key] != nil || [key hasPrefix:@"XPC_SIMULATOR_"] || [key isEqualToString:@"XPC_USERSPACE_REBOOT_TO_BOOT_MODE"] ||
           [key isEqualToString:@"LSB_FAKE_PTY_STATE"] || [key isEqualToString:@"TERM"] ||
           [key isEqualToString:@"LSB_WORKQUEUE_HARDWARE_BREAKPOINT"] || [key isEqualToString:@"LSB_RETAIN_HOST_CACHE"];
}

static BOOL serviceEnvironmentKey(NSString *key) {
    if (!key.length || [key containsString:@"="]) return NO;
    for (NSString *prefix in @[@"DYLD_", @"LSB_", @"XPC_", @"SIMULATOR_", @"IPHONE_"])
        if ([key hasPrefix:prefix]) return NO;
    return ![@[@"HOME", @"CFFIXED_USER_HOME", @"TMPDIR", @"PATH", @"TERM"] containsObject:key];
}

static BOOL validGuestProgram(NSString *path) {
    if (![path isKindOfClass:NSString.class] || ![path hasPrefix:@"/"] || [path lengthOfBytesUsingEncoding:NSUTF8StringEncoding] >= LSB_PATH_BYTES)
        return NO;
    if (![path canBeConvertedToEncoding:NSUTF8StringEncoding] || [path rangeOfString:@"\0"].location != NSNotFound) return NO;
    for (NSString *part in path.pathComponents) if ([part isEqual:@".."] || [part isEqual:@"."]) return NO;
    return path.length > 1;
}

static int addLaunchAngelPolicy(NSDictionary *stock, NSMutableDictionary *job) {
    // Selected LaunchAngels describe apps activated by RunningBoard, not daemons to force-start during bootstrap.
    // Preserve the supported on-demand policy; do not silently turn an unfamiliar keepalive/transaction policy into another one.
    for (NSString *key in @[@"RunAtLoad", @"KeepAlive", @"EnableTransactions", @"EnablePressuredExit"]) {
        id value = stock[key];
        if (!value) continue;
        if (CFGetTypeID((__bridge CFTypeRef)value) != CFBooleanGetTypeID()) return EINVAL;
        if ([value boolValue]) return ENOTSUP;
        job[key] = value;
    }
    for (NSString *key in @[@"ProcessType", @"POSIXSpawnType"]) {
        id value = stock[key];
        if (!value) continue;
        if (![value isKindOfClass:NSString.class]) return EINVAL;
        if (![value isEqual:@"App"]) return ENOTSUP;
        job[key] = value;
    }
    NSDictionary *properties = stock[@"_AdditionalProperties"];
    if (properties && ![properties isKindOfClass:NSDictionary.class]) return EINVAL;
    NSDictionary *runningBoard = properties[@"RunningBoard"];
    if (runningBoard && ![runningBoard isKindOfClass:NSDictionary.class]) return EINVAL;
    NSMutableDictionary *supported = [NSMutableDictionary dictionary];
    for (NSString *key in @[@"Managed", @"Reported"]) {
        id value = runningBoard[key];
        if (!value) continue;
        if (CFGetTypeID((__bridge CFTypeRef)value) != CFBooleanGetTypeID()) return EINVAL;
        supported[key] = value;
    }
    if (supported.count) job[@"_AdditionalProperties"] = @{@"RunningBoard": supported};
    return 0;
}

static int addServiceJobs(NSString *root, const char *const labels[], NSMutableDictionary<NSString *, NSData *> *jobs,
                         NSMutableArray<NSString *> *logs) {
    if (!labels) return EINVAL;
    NSMutableSet<NSString *> *seen = [NSMutableSet setWithArray:
        @[@"com.apple.backboardd", @"com.apple.cfprefsd.xpc.daemon", @"com.kdt.LiveSimBooter.terminal-fish", @LSB_DISPLAY_SETTINGS_LABEL]];
    for (unsigned index = 0; labels[index]; ++index) {
        if (index == MaximumSelectedServices) return E2BIG;
        const char *labelBytes = labels[index];
        size_t length = strnlen(labelBytes, 129);
        if (!length || length > 128) return EINVAL;
        for (size_t i = 0; i < length; ++i) {
            unsigned char c = labelBytes[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
                return EINVAL;
        }
        NSString *label = [NSString stringWithUTF8String:labelBytes];
        if ([label isEqual:@"."] || [label isEqual:@".."] || [seen containsObject:label]) return EINVAL;
        [seen addObject:label];
        NSString *filename = [label stringByAppendingString:@".plist"];
        NSString *stockPath = [[root stringByAppendingPathComponent:@"System/Library/LaunchDaemons"] stringByAppendingPathComponent:filename];
        struct stat metadata;
        BOOL launchAngel = NO;
        if (lstat(stockPath.fileSystemRepresentation, &metadata)) {
            if (errno != ENOENT) return errno;
            // Search only this explicitly selected label. An existing malformed/symlinked daemon must never fall through.
            stockPath = [[root stringByAppendingPathComponent:@"System/Library/LaunchAngels"] stringByAppendingPathComponent:filename];
            if (lstat(stockPath.fileSystemRepresentation, &metadata)) return errno;
            launchAngel = YES;
        }
        if (!S_ISREG(metadata.st_mode)) return S_ISLNK(metadata.st_mode) ? ELOOP : EINVAL;
        if (metadata.st_size <= 0 || metadata.st_size > 1024 * 1024) return EFBIG;
        char canonical[PATH_MAX];
        if (!realpath(stockPath.fileSystemRepresentation, canonical)) return errno;
        const char *rootBytes = root.fileSystemRepresentation;
        size_t rootLength = strlen(rootBytes);
        if (strncmp(canonical, rootBytes, rootLength) || canonical[rootLength] != '/') return EPERM;
        NSDictionary *stock;
        int readError = readStockPlist(stockPath, EIO, &stock);
        if (readError) return readError;
        if (![stock[@"Label"] isEqual:label]) return EINVAL;
        NSString *program = stock[@"Program"];
        NSArray *arguments = stock[@"ProgramArguments"];
        if (program && !validGuestProgram(program)) return EINVAL;
        if (arguments && (![arguments isKindOfClass:NSArray.class] || !arguments.count || arguments.count > LSB_MAX_VECTOR)) return EINVAL;
        for (id argument in arguments) {
            if (![argument isKindOfClass:NSString.class] || ![argument canBeConvertedToEncoding:NSUTF8StringEncoding] ||
                [argument lengthOfBytesUsingEncoding:NSUTF8StringEncoding] >= LSB_STRING_BYTES || [argument rangeOfString:@"\0"].location != NSNotFound)
                return EINVAL;
        }
        if (!program && (!arguments || !validGuestProgram(arguments[0]))) return EINVAL;
        NSDictionary *services = stock[@"MachServices"];
        if (services && ![services isKindOfClass:NSDictionary.class]) return EINVAL;
        for (id name in services) {
            id value = services[name];
            if (![name isKindOfClass:NSString.class] || ![name length] ||
                (![value isKindOfClass:NSDictionary.class] && ![value isKindOfClass:NSNumber.class])) return EINVAL;
        }
        id publishes = stock[@"PublishesEvents"];
        if (publishes && ![publishes isKindOfClass:NSString.class] && ![publishes isKindOfClass:NSDictionary.class]) return EINVAL;
        NSString *logName = [label stringByAppendingString:@".log"];
        NSString *log = [@"/private/var/mobile/Library/Logs" stringByAppendingPathComponent:logName];
        NSMutableDictionary *job = [@{@"Label": label, @"WorkingDirectory": @"/private/var/mobile/",
            @"StandardOutPath": log, @"StandardErrorPath": log, @"RunAtLoad": launchAngel ? @NO : @YES, @"KeepAlive": @NO,
            @"EnableTransactions": @NO} mutableCopy];
        if (launchAngel) {
            int error = addLaunchAngelPolicy(stock, job);
            if (error) return error;
        }
        if (program) job[@"Program"] = program;
        if (arguments) job[@"ProgramArguments"] = arguments;
        if (services) job[@"MachServices"] = services;
        if (publishes) job[@"PublishesEvents"] = publishes;
        NSDictionary *environment = stock[@"EnvironmentVariables"];
        if (environment && ![environment isKindOfClass:NSDictionary.class]) return EINVAL;
        NSMutableDictionary *safeEnvironment = [NSMutableDictionary dictionary];
        for (id key in environment) {
            id value = environment[key];
            if (![key isKindOfClass:NSString.class] || ![value isKindOfClass:NSString.class] ||
                ![key canBeConvertedToEncoding:NSUTF8StringEncoding] || ![value canBeConvertedToEncoding:NSUTF8StringEncoding] ||
                [key rangeOfString:@"\0"].location != NSNotFound || [value rangeOfString:@"\0"].location != NSNotFound) return EINVAL;
            if (serviceEnvironmentKey(key)) safeEnvironment[key] = value;
        }
        // Cached framework paths need the matching simulator resources. Only this exact host-selected job receives the opt-in.
        NSString *effectiveProgram = program ?: arguments[0];
        if ([label isEqualToString:@"com.apple.SpringBoard"] &&
            [effectiveProgram isEqualToString:@"/System/Library/CoreServices/SpringBoard.app/SpringBoard"]) {
            safeEnvironment[@"LSB_SIMULATE_FRAMEWORK_BUNDLES"] = @"1";
            safeEnvironment[@"LSB_SIMULATE_SPRINGBOARD_ENTITLEMENTS"] = @"1";
            // libxpc PID-domain imports use cache image paths, independently of CFBundle resource redirection.
            safeEnvironment[@"LSB_REGISTER_EXTENSIONKIT"] = @"1";
            safeEnvironment[@"LSB_REGISTER_METAL_COMPILER"] = @"1";
        }
        if ([label isEqualToString:@"com.apple.iconservices.iconservicesagent"] &&
            [effectiveProgram isEqualToString:@"/System/Library/CoreServices/iconservicesagent"]) {
            // Icon rendering needs the matching simulator resources and its own PID-domain shader compiler.
            safeEnvironment[@"LSB_REGISTER_METAL_COMPILER"] = @"1";
            safeEnvironment[@"LSB_SIMULATE_FRAMEWORK_BUNDLES"] = @"1";
        }
        if ([label isEqualToString:@"com.apple.PosterBoard"] &&
            [effectiveProgram isEqualToString:@"/Applications/PosterBoard.app/PosterBoard"]) {
            // Poster provider discovery needs its own PID-domain service, just like SpringBoard's ExtensionKit clients.
            safeEnvironment[@"LSB_REGISTER_EXTENSIONKIT"] = @"1";
        }
        if (safeEnvironment.count) job[@"EnvironmentVariables"] = safeEnvironment;
        NSData *jobData = [NSPropertyListSerialization dataWithPropertyList:job format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
        if (!jobData) return EINVAL;
        jobs[filename] = jobData;
        [logs addObject:logName];
    }
    return 0;
}

static NSString *serviceDirectoryName(const char *const labels[], int *error) {
    *error = EINVAL;
    if (!labels) return nil;
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (unsigned i = 0; labels[i]; ++i) {
        if (i == MaximumSelectedServices) { *error = E2BIG; return nil; }
        if (strnlen(labels[i], 129) > 128) return nil;
        NSString *name = [NSString stringWithUTF8String:labels[i]];
        if (!name) return nil;
        [names addObject:name];
    }
    NSData *selection = [[[names sortedArrayUsingSelector:@selector(compare:)] componentsJoinedByString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding];
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(selection.bytes, (CC_LONG)selection.length, digest);
    NSMutableString *suffix = [NSMutableString string];
    for (unsigned i = 0; i < 16; ++i) [suffix appendFormat:@"%02x", digest[i]];
    // Different selections use distinct directories; narrowing never loads stale jobs or deletes an earlier experiment.
    *error = 0;
    return [@"LiveSimBooter-CoreDaemons-" stringByAppendingString:suffix];
}

static int prepareLaunchdPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[], NSString *probe,
                             const char *const serviceLabels[]) {
    if (!message || !runtimeRoot || runtimeRoot[0] != '/') return EINVAL;
    @autoreleasepool {
        BOOL services = [probe isEqualToString:@"services"], includeJob = probe != nil;
        BOOL backboard = services || [probe isEqualToString:@"backboardd"], lifecycle = [probe isEqualToString:@"lifecycle"];
        char canonical[PATH_MAX];
        if (!realpath(runtimeRoot, canonical)) return errno;
        struct stat status;
        if (stat(canonical, &status)) return errno;
        if (!S_ISDIR(status.st_mode)) return ENOTDIR;
        NSString *root = [NSFileManager.defaultManager stringWithFileSystemRepresentation:canonical length:strlen(canonical)];
        NSString *build = root.stringByDeletingLastPathComponent;
        NSString *dataPath = [build stringByAppendingPathComponent:@LSB_DATA_DIRECTORY];
        NSString *configPath = [dataPath stringByAppendingPathComponent:@"var/run/LiveSimBooter-launchd-bootstrap.plist"];
        NSString *logPath = [dataPath stringByAppendingPathComponent:@"Library/Logs/LiveSimBooter-launchd.log"];
        int selectionError = 0;
        NSString *directoryName = services ? serviceDirectoryName(serviceLabels, &selectionError) : backboard ? @"LiveSimBooter-BackboardDaemons" :
            lifecycle ? @"LiveSimBooter-LifecycleDaemons" : @"LiveSimBooter-LaunchDaemons";
        if (!directoryName) return selectionError ? selectionError : EINVAL;
        NSString *jobDirectory = [[dataPath stringByAppendingPathComponent:@"Library"] stringByAppendingPathComponent:directoryName];
        NSString *label = backboard ? @"com.apple.backboardd" : [@"com.kdt.LiveSimBooter." stringByAppendingString:probe ?: @"df"];
        NSString *jobNameString = [label stringByAppendingString:@".plist"];
        NSString *jobPath = [jobDirectory stringByAppendingPathComponent:jobNameString];
        NSString *logName = [backboard ? @"backboardd" : probe ?: @"df" stringByAppendingString:@".log"];
        NSString *jobLogPath = [[dataPath stringByAppendingPathComponent:@"Library/Logs"] stringByAppendingPathComponent:logName];
        if (strlen(canonical) >= LSB_PATH_BYTES || strlen(dataPath.fileSystemRepresentation) >= LSB_PATH_BYTES ||
            strlen(configPath.fileSystemRepresentation) >= LSB_PATH_BYTES ||
            (includeJob && (strlen(jobPath.fileSystemRepresentation) >= LSB_PATH_BYTES ||
                            strlen(jobLogPath.fileSystemRepresentation) >= LSB_PATH_BYTES))) return ENAMETOOLONG;
        LSBGuestDeviceType *deviceType = configurationDeviceType ?: LSBDefaultGuestDeviceType();
        NSMutableDictionary<NSString *, NSString *> *runtimeEnvironment = [@{
            @"XPC_SIMULATOR_LAUNCHD_NAME": @"com.kdt.LiveSimBooter.launchd",
            @"DYLD_ROOT_PATH": root, @"DYLD_SHARED_CACHE_DIR": [root stringByAppendingPathComponent:@"System/Library/Caches/com.apple.dyld"],
            @"DYLD_FALLBACK_LIBRARY_PATH": [root stringByAppendingPathComponent:@"usr/lib"],
            @"DYLD_FALLBACK_FRAMEWORK_PATH": [root stringByAppendingPathComponent:@"System/Library/Frameworks"],
            @"SIMULATOR_ROOT": root, @"IPHONE_SIMULATOR_ROOT": root, @"SIMULATOR_ARCHS": @"arm64",
            @"SIMULATOR_CAPABILITIES": [root stringByAppendingPathComponent:@"Library/Developer/CoreSimulator/DeviceType/capabilities.plist"],
            // CoreSimulator 1171.7 formats this boot's Unix timestamp as %lu. SimulatorHID persists it as its generation key.
            @"SIMULATOR_BOOT_TIME": [NSString stringWithFormat:@"%lu", (unsigned long)NSDate.date.timeIntervalSince1970],
            // All guests share this simulated boot identity; native kern.bootsessionuuid may be denied inside the app sandbox.
            @"LSB_SIMULATOR_BOOT_UUID": NSUUID.UUID.UUIDString,
            @"SIMULATOR_PRODUCT_CLASS": deviceType.productClass, @"SIMULATOR_MODEL_IDENTIFIER": deviceType.modelIdentifier,
            @"SIMULATOR_HID_SYSTEM_MANAGER": [root stringByAppendingPathComponent:@"Library/Frameworks/SimulatorHID.framework"],
            @"LSB_SIMULATE_SESSIONS": @"1",
            @"LSB_SIMULATE_FILE_LOCK_SUSPENSION": @"1",
            @"SIMULATOR_SHARED_RESOURCES_DIRECTORY": dataPath, @"IPHONE_SHARED_RESOURCES_DIRECTORY": dataPath,
            @"SIMULATOR_LOG_ROOT": [dataPath stringByAppendingPathComponent:@"Library/Logs"],
            @"HOME": dataPath, @"CFFIXED_USER_HOME": dataPath, @"TMPDIR": [dataPath stringByAppendingString:@"/tmp/"],
            @"PATH": [NSString stringWithFormat:@"%@/usr/local/bin:%@/usr/bin:%@/bin:%@/usr/sbin:%@/sbin", root, root, root, root, root]
        } mutableCopy];
        if (services) {
            runtimeEnvironment[@"LSB_SIMULATE_NOTIFICATION_SHM"] = @"1";
            runtimeEnvironment[@"LSB_SIMULATE_CODE_SIGNING"] = @"1";
            runtimeEnvironment[@"LSB_ENABLE_PEER_PROCESS_INSPECTION"] = @"1";
        }
        if (backboard) {
            runtimeEnvironment[@"LSB_SIMULATE_SYSTEM_POWER"] = @"1";
            // The cached client dlopens getenv directly; an absent value resolves its dispatch table back to its own trampolines.
            runtimeEnvironment[@"SIMULATOR_FRAMEBUFFER_FRAMEWORK"] =
                [root stringByAppendingPathComponent:@"Library/PrivateFrameworks/SimFramebuffer.framework/SimFramebuffer"];
            // Match the immutable framebuffer for this session, including iPhone 8 Plus's rendered (not panel) dimensions.
            [runtimeEnvironment addEntriesFromDictionary:@{
                @"SIMULATOR_MAINSCREEN_WIDTH": @(deviceType.width).stringValue, @"SIMULATOR_MAINSCREEN_HEIGHT": @(deviceType.height).stringValue,
                @"SIMULATOR_MAINSCREEN_SCALE": @(deviceType.scale).stringValue, @"SIMULATOR_MAINSCREEN_PITCH": @(deviceType.pitch).stringValue
            }];
        }
        // Explicit diagnostic callers can still override identity when no catalog selection was supplied by the broker.
        for (NSString *key in configurationDeviceType ? @[] : @[@"SIMULATOR_PRODUCT_CLASS", @"SIMULATOR_MODEL_IDENTIFIER"]) {
            const char *name = key.UTF8String;
            size_t length = strlen(name);
            for (const char *const *entry = inheritedEnv; entry && *entry; ++entry) {
                if (strncmp(*entry, name, length) || (*entry)[length] != '=') continue;
                NSString *value = [NSString stringWithUTF8String:*entry + length + 1];
                if (!value.length) return EINVAL;
                runtimeEnvironment[key] = value;
                break; // Match getenv's first occurrence; controlledEnvironment removes all inherited duplicates.
            }
        }
        LSBNativeRuntimePolicy nativePolicy;
        int policyError = LSBResolveNativeRuntimePolicy(LSBHostNativeProductionStatus(), inheritedEnv, &nativePolicy);
        if (policyError) return policyError;
        for (NSString *key in @[@LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV, @LSB_RETAIN_HOST_CACHE_ENV, @"LSB_TRACE_HANDOFF"]) {
            bool defaultValue = [key isEqualToString:@LSB_RETAIN_HOST_CACHE_ENV] ? nativePolicy.retainHostCache :
                [key isEqualToString:@LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV] ? nativePolicy.hardwareWorkqueue : false;
            // launchd replaces inherited environment. Persist the native policy and first exact override in both bootstrap scopes.
            runtimeEnvironment[key] = LSBRuntimeBooleanEnvironment(inheritedEnv, key.UTF8String, defaultValue) ? @"1" : @"0";
        }
        const char *stopChildren = getenv("LSB_DEBUG_STOP_CHILDREN");
        if (stopChildren && !strcmp(stopChildren, "1")) runtimeEnvironment[@"LSB_DEBUG_STOP_BEFORE_HANDOFF"] = @"1";
        NSMutableDictionary<NSString *, NSString *> *launchEnvironment = runtimeEnvironment.mutableCopy;
        // The stock launchd wait compares its return value to this selector; an empty child ledger returns -1/ECHILD.
        // This is a version-specific no-child compatibility sentinel, not an actual tank process PID.
        launchEnvironment[@"XPC_SIMULATOR_HOLDING_TANK_HACK"] = @"-1";
        launchEnvironment[@"XPC_FLAGS"] = @"0x0000000000000000";
        launchEnvironment[@"LSB_SIMULATOR_LAUNCHD"] = @"1";
        NSDictionary *configuration = @{
            @"Bootstrap": @{@"Paths": includeJob ? @[jobDirectory] : @[], @"Self": launchEnvironment, @"Child": runtimeEnvironment, @"ChildDefaults": @{},
                             @"BinaryOrderPreference": @"arm64", @"SimulatorRuntimeRoot": root},
            @"MultipleInstanceLimit": @64, @"LogToConsole": @YES, @"LogToFlatFile": @NO,
            @"LogServiceLifecycle": @YES, @"LogAllBootstrapErrors": @YES, @"RequireDomainBinding": @NO
        };
        NSData *plist = [NSPropertyListSerialization dataWithPropertyList:configuration format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
        if (!plist) return EINVAL;
        NSMutableDictionary<NSString *, NSData *> *jobPlists = [NSMutableDictionary dictionary];
        NSMutableArray<NSString *> *serviceLogs = [NSMutableArray array];
        if (includeJob) {
            // The trailing slash selects launchd's mobile-directory prefix rule before proxy calls chdir on the translated path.
            NSString *guestLog = [@"/private/var/mobile/Library/Logs" stringByAppendingPathComponent:logName];
            NSString *program = backboard ? @"/usr/libexec/backboardd" : lifecycle ? @"/usr/local/bin/lifecycle-probe" : @"/bin/df";
            NSMutableDictionary *job = [@{@"Label": label, @"ProgramArguments": @[program],
                                  @"WorkingDirectory": @"/private/var/mobile/", @"StandardOutPath": guestLog, @"StandardErrorPath": guestLog,
                                  @"RunAtLoad": @YES, @"KeepAlive": @NO} mutableCopy];
            if (backboard) {
                // Import only service declarations. Native scheduler/identity policies in the stock plist are not emulated.
                NSString *stockPath = [root stringByAppendingPathComponent:@"System/Library/LaunchDaemons/com.apple.backboardd.plist"];
                NSDictionary *stock;
                int readError = readStockPlist(stockPath, ENOENT, &stock);
                if (readError) return readError;
                if (![stock[@"MachServices"] isKindOfClass:NSDictionary.class] || ![stock[@"MachServices"] count]) return EINVAL;
                job[@"MachServices"] = stock[@"MachServices"];
                job[@"EnableTransactions"] = @NO;
                // Register the physical runtime compiler bundle in backboardd's PID domain; descendants also face an executable-path gate.
                job[@"EnvironmentVariables"] = @{@"LSB_REGISTER_METAL_COMPILER": @"1", @"LSB_SIMULATE_FRAMEWORK_BUNDLES": @"1"};
            }
            NSData *jobPlist = [NSPropertyListSerialization dataWithPropertyList:job format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
            if (!jobPlist) return EINVAL;
            jobPlists[jobNameString] = jobPlist;
            if (backboard) {
                NSString *preferencesLabel = @"com.apple.cfprefsd.xpc.daemon";
                NSString *preferencesName = [preferencesLabel stringByAppendingString:@".plist"];
                NSString *stockPath = [[root stringByAppendingPathComponent:@"System/Library/LaunchDaemons"]
                    stringByAppendingPathComponent:preferencesName];
                NSDictionary *stock;
                int readError = readStockPlist(stockPath, ENOENT, &stock);
                if (readError) return readError;
                NSArray *arguments = @[@"/usr/sbin/cfprefsd", @"daemon"];
                NSDictionary *services = @{@"com.apple.cfprefsd.daemon": @YES};
                if (![stock[@"Label"] isEqual:preferencesLabel] ||
                    ![stock[@"ProgramArguments"] isEqual:arguments] || ![stock[@"MachServices"] isEqual:services]) return EINVAL;
                // Simulator CoreFoundation's daemon entry selects this listener, even though the runtime also ships a System-session plist.
                // Keep real job/service names and argv. Native scheduler, resource limits and system-session policy are not emulated.
                NSDictionary *preferencesJob = @{
                    @"Label": stock[@"Label"], @"ProgramArguments": stock[@"ProgramArguments"], @"MachServices": stock[@"MachServices"],
                    @"WorkingDirectory": @"/private/var/mobile/", @"StandardOutPath": @"/private/var/mobile/Library/Logs/cfprefsd.log",
                    @"StandardErrorPath": @"/private/var/mobile/Library/Logs/cfprefsd.log", @"RunAtLoad": @YES,
                    @"KeepAlive": @NO, @"EnableTransactions": @NO
                };
                NSData *preferencesPlist = [NSPropertyListSerialization dataWithPropertyList:preferencesJob
                    format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
                if (!preferencesPlist) return EINVAL;
                jobPlists[preferencesName] = preferencesPlist;
                // Launchd owns the job and its stable logical PID across xpcproxy -> helper -> Fish SETEXEC transitions.
                // The broker attaches the terminal only to this exact helper after authenticating the launchd lineage.
                NSDictionary *terminalJob = @{
                    @"Label": @"com.kdt.LiveSimBooter.terminal-fish", @"ProgramArguments": @[@"/usr/libexec/terminal-fish"],
                    @"WorkingDirectory": @"/private/var/mobile/", @"RunAtLoad": @YES, @"KeepAlive": @NO, @"EnableTransactions": @NO
                };
                NSData *terminalPlist = [NSPropertyListSerialization dataWithPropertyList:terminalJob
                    format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
                if (!terminalPlist) return EINVAL;
                jobPlists[@"com.kdt.LiveSimBooter.terminal-fish.plist"] = terminalPlist;
                // A fixed launchd-owned helper applies only the two allowlisted display settings using guest frameworks.
                NSDictionary *settingsJob = @{
                    @"Label": @LSB_DISPLAY_SETTINGS_LABEL, @"ProgramArguments": @[@LSB_DISPLAY_SETTINGS_PROGRAM, @"--serve"],
                    @"WorkingDirectory": @"/private/var/mobile/", @"RunAtLoad": @YES, @"KeepAlive": @NO, @"EnableTransactions": @NO,
                    @"StandardOutPath": @"/private/var/mobile/Library/Logs/display-settings.log",
                    @"StandardErrorPath": @"/private/var/mobile/Library/Logs/display-settings.log"
                };
                NSData *settingsPlist = [NSPropertyListSerialization dataWithPropertyList:settingsJob
                    format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
                if (!settingsPlist) return EINVAL;
                jobPlists[@LSB_DISPLAY_SETTINGS_LABEL ".plist"] = settingsPlist;
                [serviceLogs addObject:@"display-settings.log"];
            }
        }
        if (services) {
            int serviceError = addServiceJobs(root, serviceLabels, jobPlists, serviceLogs);
            if (serviceError) return serviceError;
        }
        LSBSpawnPlan *plan = calloc(1, sizeof(*plan));
        if (!plan) return ENOMEM;
        plan->version = LSB_PLAN_VERSION;
        plan->creationMask = 022;
        int error = LSBPlanAddString(plan, "/sbin/launchd_sim", &plan->path);
        if (!error) error = LSBPlanAddString(plan, dataPath.fileSystemRepresentation, &plan->cwd);
        if (!error) error = LSBPlanAddString(plan, "/sbin/launchd_sim", &plan->argv[plan->argc++]);
        if (!error) error = LSBPlanAddString(plan, configPath.fileSystemRepresentation, &plan->argv[plan->argc++]);
        for (const char *const *entry = inheritedEnv; !error && entry && *entry; ++entry) {
            if (services && (!strncmp(*entry, "LSB_DEBUG_", 10) || !strncmp(*entry, "DYLD_PRINT_", 11))) continue;
            if (!controlledEnvironment(*entry, launchEnvironment)) error = addEnvironment(plan, *entry);
        }
        for (NSString *key in [launchEnvironment.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
            if (error) break;
            error = addEnvironment(plan, [NSString stringWithFormat:@"%@=%@", key, launchEnvironment[key]].UTF8String);
        }
        if (!error) error = LSBValidateSpawnPlan(plan);
        int directories[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1}, fds[3] = {-1, -1, -1};
        NSData *touchPreferences = nil;
        if (!error && (directories[0] = open(build.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) error = errno;
        const char *names[] = {LSB_DATA_DIRECTORY, "tmp", "var", "run", "Library", "Logs"};
        const unsigned parents[] = {0, 1, 1, 3, 1, 5};
        for (unsigned i = 0; !error && i < 6; ++i) {
            directories[i + 1] = openDirectory(directories[parents[i]], names[i]);
            if (directories[i + 1] < 0) error = errno;
        }
        if (!error && backboard) {
            directories[8] = openDirectory(directories[5], "Preferences");
            if (directories[8] < 0) error = errno;
        }
        if (!error && includeJob) {
            directories[7] = openDirectory(directories[5], directoryName.UTF8String);
            if (directories[7] < 0) error = errno;
            else error = checkJobDirectory(directories[7], [NSSet setWithArray:jobPlists.allKeys]);
        }
        if (!error) {
            fds[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (fds[0] < 0) error = errno;
        }
        if (!error) {
            fds[1] = openRegularLog(directories[6], "LiveSimBooter-launchd.log");
            if (fds[1] < 0) error = errno;
        }
        if (!error && (fds[2] = fcntl(fds[1], F_DUPFD_CLOEXEC, 3)) < 0) error = errno;
        if (!error && includeJob) {
            int jobLog = openRegularLog(directories[6], logName.UTF8String);
            if (jobLog < 0) error = errno;
            else close(jobLog);
        }
        if (!error && backboard) {
            int preferencesLog = openRegularLog(directories[6], "cfprefsd.log");
            if (preferencesLog < 0) error = errno;
            else close(preferencesLog);
        }
        for (NSString *name in serviceLogs) {
            if (error) break;
            int serviceLog = openRegularLog(directories[6], name.UTF8String);
            if (serviceLog < 0) error = errno;
            else close(serviceLog);
        }
        if (!error && backboard) error = prepareTouchPreferences(directories[8], &touchPreferences);
        for (NSString *name in [jobPlists.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
            if (error) break;
            error = writeConfiguration(directories[7], name.UTF8String, jobPlists[name]);
        }
        if (!error && touchPreferences) error = writeConfiguration(directories[8], "com.apple.backboardd.plist", touchPreferences);
        if (!error) error = writeConfiguration(directories[4], "LiveSimBooter-launchd-bootstrap.plist", plist);
        if (!error) {
            plan->fdCount = 3;
            for (int i = 0; i < 3; ++i) { plan->fds[i] = (LSBInheritedFD){i, 0}; message->fds[i] = fds[i]; fds[i] = -1; }
            message->plan = *plan;
            fprintf(stderr, "[broker] launchd config=%s log=%s jobs=%s\n", configPath.fileSystemRepresentation,
                    logPath.fileSystemRepresentation, probe ? probe.UTF8String : "empty");
            // Stock launchd still uses /private/var/tmp/<name> for its own state; this helper does not virtualize that path.
        }
        for (unsigned i = 0; i < 9; ++i) if (directories[i] >= 0) close(directories[i]);
        for (unsigned i = 0; i < 3; ++i) if (fds[i] >= 0) close(fds[i]);
        free(plan);
        return error;
    }
}

int LSBPrepareLaunchdPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]) {
    return prepareLaunchdPlan(message, runtimeRoot, inheritedEnv, nil, NULL);
}

int LSBPrepareLaunchdDFPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]) {
    return prepareLaunchdPlan(message, runtimeRoot, inheritedEnv, @"df", NULL);
}

int LSBPrepareLaunchdBackboardPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]) {
    return prepareLaunchdPlan(message, runtimeRoot, inheritedEnv, @"backboardd", NULL);
}

int LSBPrepareLaunchdServicesPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[],
                                 const char *const labels[]) {
    return prepareLaunchdPlan(message, runtimeRoot, inheritedEnv, @"services", labels);
}

int LSBPrepareLaunchdLifecyclePlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]) {
    return prepareLaunchdPlan(message, runtimeRoot, inheritedEnv, @"lifecycle", NULL);
}
