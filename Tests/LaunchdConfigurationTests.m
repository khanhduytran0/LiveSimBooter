#include "../ProcessSupport/RuntimeLayout.h"
#include "../ProcessSupport/NativeRuntimePolicy.h"
#import <Foundation/Foundation.h>
#include "../Host/LaunchdConfiguration.h"
#include "../Host/NativeProductionStatus.h"
#import "../Host/DeviceType.h"
#include "../ProcessSupport/ChildStatus.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint32_t nativeProductionStatus = LSB_NATIVE_DEVELOPMENT;
uint32_t LSBHostNativeProductionStatus(void) { return nativeProductionStatus; }

static void initialize(LSBReceivedMessage *message) {
    memset(message, 0, sizeof(*message));
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) message->fds[i] = -1;
}

static void dispose(LSBReceivedMessage *message) {
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) if (message->fds[i] >= 0) close(message->fds[i]);
    initialize(message);
}

static const char *environmentValue(const LSBSpawnPlan *plan, const char *key) {
    const char *result = NULL;
    size_t length = strlen(key);
    for (unsigned i = 0; i < plan->envc; ++i) {
        const char *entry = plan->strings + plan->envp[i];
        if (!strncmp(entry, key, length) && entry[length] == '=') { assert(!result); result = entry + length + 1; }
    }
    return result;
}

static void assertUnchanged(const LSBReceivedMessage *message) {
    LSBSpawnPlan empty = {0};
    assert(!memcmp(&message->plan, &empty, sizeof(empty)));
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) assert(message->fds[i] == -1);
}

static NSDictionary *readPlist(NSString *path) {
    return [NSPropertyListSerialization propertyListWithData:[NSData dataWithContentsOfFile:path] options:NSPropertyListImmutable format:NULL error:NULL];
}

static void assertPlistBoolean(id value, BOOL expected) {
    // NSNumber's boolValue/isEqual accept integer 0/1 too, but launchd requires the serialized Boolean type.
    assert(value && CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID());
    assert([value boolValue] == expected);
}

static void testDeviceProfile(LSBReceivedMessage *message, NSString *root, NSString *configPath) {
    const char *overrides[] = {"SIMULATOR_PRODUCT_CLASS=D83", "SIMULATOR_MODEL_IDENTIFIER=iPhone16,1", "SIMULATOR_PRODUCT_CLASS=ignored", NULL};
    assert(!LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, overrides));
    assert(!strcmp(environmentValue(&message->plan, "SIMULATOR_PRODUCT_CLASS"), "D83"));
    assert(!strcmp(environmentValue(&message->plan, "SIMULATOR_MODEL_IDENTIFIER"), "iPhone16,1"));
    NSDictionary *bootstrap = readPlist(configPath)[@"Bootstrap"];
    for (NSString *scope in @[@"Self", @"Child"]) {
        assert([bootstrap[scope][@"SIMULATOR_PRODUCT_CLASS"] isEqualToString:@"D83"]);
        assert([bootstrap[scope][@"SIMULATOR_MODEL_IDENTIFIER"] isEqualToString:@"iPhone16,1"]);
    }
    dispose(message);
    const char *oneOverride[] = {"SIMULATOR_PRODUCT_CLASS=D83", NULL};
    assert(!LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, oneOverride));
    assert(!strcmp(environmentValue(&message->plan, "SIMULATOR_PRODUCT_CLASS"), "D83"));
    assert(!strcmp(environmentValue(&message->plan, "SIMULATOR_MODEL_IDENTIFIER"), "iPhone16,2"));
    dispose(message);
    NSData *previous = [NSData dataWithContentsOfFile:configPath];
    const char *invalid[] = {"SIMULATOR_PRODUCT_CLASS=", "SIMULATOR_MODEL_IDENTIFIER=", "SIMULATOR_PRODUCT_CLASS=\xff",
                             "SIMULATOR_MODEL_IDENTIFIER=\xff"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        const char *environment[] = {invalid[i], NULL};
        assert(LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, environment) == EINVAL);
        assertUnchanged(message);
        assert([[NSData dataWithContentsOfFile:configPath] isEqualToData:previous]);
    }
}

static void testSelectedDeviceProfiles(LSBReceivedMessage *message, NSString *root, NSString *configPath) {
    const char *stale[] = {"SIMULATOR_PRODUCT_CLASS=stale", "SIMULATOR_PRODUCT_CLASS=", "SIMULATOR_MODEL_IDENTIFIER=stale",
        "SIMULATOR_MODEL_IDENTIFIER=", "SIMULATOR_MAINSCREEN_WIDTH=1", "SIMULATOR_MAINSCREEN_HEIGHT=2", "SIMULATOR_MAINSCREEN_SCALE=9",
        "SIMULATOR_MAINSCREEN_PITCH=7", "SIMULATOR_CAPABILITIES=/stale", "SIMULATOR_PRODUCT_CLASS_EXTRA=preserved", NULL};
    NSArray<LSBGuestDeviceType *> *devices = LSBGuestDeviceTypes();
    assert(devices.count == 7);
    for (LSBGuestDeviceType *type in devices) {
        LSBLaunchdConfigurationSetDeviceType(type);
        assert(!LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, stale));
        NSDictionary *expected = @{
            @"SIMULATOR_PRODUCT_CLASS": type.productClass, @"SIMULATOR_MODEL_IDENTIFIER": type.modelIdentifier,
            @"SIMULATOR_MAINSCREEN_WIDTH": @(type.width).stringValue, @"SIMULATOR_MAINSCREEN_HEIGHT": @(type.height).stringValue,
            @"SIMULATOR_MAINSCREEN_SCALE": @(type.scale).stringValue, @"SIMULATOR_MAINSCREEN_PITCH": @(type.pitch).stringValue,
            @"SIMULATOR_CAPABILITIES": [root stringByAppendingPathComponent:@"Library/Developer/CoreSimulator/DeviceType/capabilities.plist"]
        };
        NSDictionary *bootstrap = readPlist(configPath)[@"Bootstrap"];
        for (NSString *key in expected) {
            assert(!strcmp(environmentValue(&message->plan, key.UTF8String), [expected[key] UTF8String]));
            for (NSString *scope in @[@"Self", @"Child"]) assert([bootstrap[scope][key] isEqual:expected[key]]);
        }
        assert(!strcmp(environmentValue(&message->plan, "SIMULATOR_PRODUCT_CLASS_EXTRA"), "preserved"));
        dispose(message);
    }
    LSBLaunchdConfigurationSetDeviceType(nil);
    testDeviceProfile(message, root, configPath); // Explicitly clearing selection restores legacy diagnostic overrides.
}

static void testRuntimeBooleanEnvironment(LSBReceivedMessage *message, NSString *root, NSString *configPath, const char *name,
                                         BOOL enabledByDefault) {
    const struct { const char *first, *second; BOOL enabled; } cases[] = {
        {NULL, NULL, enabledByDefault}, {"0", NULL, NO}, {"1", NULL, YES}, {"", NULL, NO}, {"01", NULL, NO}, {"true", NULL, NO},
        {"1=extra", NULL, NO}, {"1", "0", YES}, {"0", "1", NO}, {"", "1", NO}, {"01", "1", NO}, {"1", "1", YES}
    };
    int (*prepare[])(LSBReceivedMessage *, const char *, const char *const[]) = {LSBPrepareLaunchdPlan, LSBPrepareLaunchdDFPlan};
    for (unsigned mode = 0; mode < sizeof(prepare) / sizeof(prepare[0]); ++mode) {
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            char first[96], second[96];
            char unrelated[96];
            snprintf(unrelated, sizeof(unrelated), "%s_EXTRA=preserved", name);
            const char *inherited[4] = {unrelated, NULL};
            unsigned count = 1;
            if (cases[i].first) {
                snprintf(first, sizeof(first), "%s=%s", name, cases[i].first);
                inherited[count++] = first;
            }
            if (cases[i].second) {
                snprintf(second, sizeof(second), "%s=%s", name, cases[i].second);
                inherited[count++] = second;
            }
            inherited[count] = NULL;
            assert(!prepare[mode](message, root.fileSystemRepresentation, inherited));
            assert(!LSBValidateSpawnPlan(&message->plan));
            const char *value = environmentValue(&message->plan, name);
            assert(value && !strcmp(value, cases[i].enabled ? "1" : "0"));
            unsigned occurrences = 0;
            for (unsigned j = 0; j < message->plan.envc; ++j) {
                const char *entry = message->plan.strings + message->plan.envp[j];
                if (!strncmp(entry, name, strlen(name)) && entry[strlen(name)] == '=') ++occurrences;
            }
            assert(occurrences == 1);
            *strchr(unrelated, '=') = 0;
            assert(!strcmp(environmentValue(&message->plan, unrelated), "preserved"));
            NSDictionary *bootstrap = readPlist(configPath)[@"Bootstrap"];
            for (NSString *scope in @[@"Self", @"Child"]) {
                assert([bootstrap[scope][@(name)] isEqualToString:cases[i].enabled ? @"1" : @"0"]);
            }
            dispose(message);
        }
    }
    // A fresh plan restores the documented default in both bootstrap scopes, without retaining a prior test's opt-in/out.
    assert(!LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, NULL));
    assert(!strcmp(environmentValue(&message->plan, name), enabledByDefault ? "1" : "0"));
    NSDictionary *bootstrap = readPlist(configPath)[@"Bootstrap"];
    for (NSString *scope in @[@"Self", @"Child"]) assert([bootstrap[scope][@(name)] isEqualToString:enabledByDefault ? @"1" : @"0"]);
    dispose(message);
}

static void testLegacyDetachmentMode(LSBReceivedMessage *message, NSString *root, NSString *configPath) {
    const char *inherited[] = {"LSB_RETAIN_HOST_CACHE=0", "LSB_WORKQUEUE_HARDWARE_BREAKPOINT=0",
                              "LSB_RETAIN_HOST_CACHE=1", "LSB_WORKQUEUE_HARDWARE_BREAKPOINT=1", NULL};
    assert(!LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, inherited));
    NSDictionary *bootstrap = readPlist(configPath)[@"Bootstrap"];
    for (NSString *key in @[@"LSB_RETAIN_HOST_CACHE", @"LSB_WORKQUEUE_HARDWARE_BREAKPOINT"]) {
        assert(!strcmp(environmentValue(&message->plan, key.UTF8String), "0"));
        for (NSString *scope in @[@"Self", @"Child"]) assert([bootstrap[scope][key] isEqualToString:@"0"]);
    }
    dispose(message);
}

static void testProductionPolicy(LSBReceivedMessage *message, NSString *root, NSString *configPath) {
    const char *detach[] = {LSB_RETAIN_HOST_CACHE_ENV "=0", NULL};
    const char *retain[] = {LSB_RETAIN_HOST_CACHE_ENV "=1", NULL};
    for (uint32_t status = LSB_NATIVE_PRODUCTION_UNKNOWN; status <= LSB_NATIVE_DEVELOPMENT; ++status) {
        nativeProductionStatus = status;
        assert(!LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, NULL));
        NSDictionary *bootstrap = readPlist(configPath)[@"Bootstrap"];
        for (NSString *scope in @[@"Self", @"Child"]) for (NSString *key in @[@LSB_RETAIN_HOST_CACHE_ENV, @LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV])
            assert([bootstrap[scope][key] isEqual:status == LSB_NATIVE_DEVELOPMENT ? @"0" : @"1"]);
        dispose(message);
        NSData *previous = [NSData dataWithContentsOfFile:configPath];
        int error = LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, detach);
        if (status == LSB_NATIVE_DEVELOPMENT) { assert(!error); dispose(message); }
        else {
            assert(error == EPERM);
            assertUnchanged(message);
            assert([[NSData dataWithContentsOfFile:configPath] isEqual:previous]);
        }
        assert(!LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, retain));
        bootstrap = readPlist(configPath)[@"Bootstrap"];
        for (NSString *scope in @[@"Self", @"Child"]) for (NSString *key in @[@LSB_RETAIN_HOST_CACHE_ENV, @LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV])
            assert([bootstrap[scope][key] isEqual:@"1"]);
        dispose(message);
    }
    nativeProductionStatus = LSB_NATIVE_DEVELOPMENT;
}

static void testDFMode(LSBReceivedMessage *message, NSString *root, NSString *dataPath, NSString *configPath, const char *const inherited[]) {
    NSString *jobDirectory = [dataPath stringByAppendingPathComponent:@"Library/LiveSimBooter-LaunchDaemons"];
    NSString *jobPath = [jobDirectory stringByAppendingPathComponent:@"com.kdt.LiveSimBooter.df.plist"];
    NSString *dfLog = [dataPath stringByAppendingPathComponent:@"Library/Logs/df.log"];
    assert(![NSFileManager.defaultManager fileExistsAtPath:jobDirectory]);
    assert(!LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, inherited));
    assert(!LSBValidateSpawnPlan(&message->plan));
    assert(!strcmp(message->plan.strings + message->plan.path, "/sbin/launchd_sim"));
    assert(!strcmp(message->plan.strings + message->plan.argv[1], configPath.fileSystemRepresentation));
    assert(!strcmp(environmentValue(&message->plan, "LSB_DEBUG_STOP_BEFORE_HANDOFF"), "1"));
    NSString *capabilitiesPath = [root stringByAppendingPathComponent:@"Library/Developer/CoreSimulator/DeviceType/capabilities.plist"];
    assert(![NSFileManager.defaultManager fileExistsAtPath:capabilitiesPath]);
    assert(!strcmp(environmentValue(&message->plan, "SIMULATOR_CAPABILITIES"), capabilitiesPath.fileSystemRepresentation));
    assert(!environmentValue(&message->plan, "LSB_SIMULATE_SYSTEM_POWER"));
    NSDictionary *bootstrap = readPlist(configPath)[@"Bootstrap"];
    for (NSString *scope in @[@"Self", @"Child"]) {
        assert(!bootstrap[scope][@"LSB_SIMULATE_SYSTEM_POWER"]);
        assert([bootstrap[scope][@"SIMULATOR_CAPABILITIES"] isEqualToString:capabilitiesPath]);
    }
    assert([bootstrap[@"Paths"] isEqualToArray:@[jobDirectory]]);
    assert(!bootstrap[@"Child"][@"LSB_SIMULATOR_LAUNCHD"]);
    assert([[NSFileManager.defaultManager contentsOfDirectoryAtPath:jobDirectory error:NULL] isEqualToArray:@[@"com.kdt.LiveSimBooter.df.plist"]]);
    NSDictionary *job = readPlist(jobPath);
    NSSet *expectedKeys = [NSSet setWithArray:@[@"Label", @"ProgramArguments", @"WorkingDirectory", @"StandardOutPath", @"StandardErrorPath",
                                              @"RunAtLoad", @"KeepAlive"]];
    assert([[NSSet setWithArray:job.allKeys] isEqualToSet:expectedKeys]);
    assert([job[@"Label"] isEqualToString:@"com.kdt.LiveSimBooter.df"]);
    assert([job[@"ProgramArguments"] isEqualToArray:@[@"/bin/df"]]);
    assert([job[@"WorkingDirectory"] isEqualToString:@"/private/var/mobile/"]);
    assert([job[@"StandardOutPath"] isEqualToString:@"/private/var/mobile/Library/Logs/df.log"] &&
           [job[@"StandardErrorPath"] isEqualToString:job[@"StandardOutPath"]]);
    assertPlistBoolean(job[@"RunAtLoad"], YES);
    assertPlistBoolean(job[@"KeepAlive"], NO);
    struct stat originalJob, originalConfig, status;
    assert(!stat(jobPath.fileSystemRepresentation, &originalJob) && S_ISREG(originalJob.st_mode));
    assert(originalJob.st_uid == geteuid() && (originalJob.st_mode & 0777) == 0600);
    assert(!stat(configPath.fileSystemRepresentation, &originalConfig));
    assert(!stat(dfLog.fileSystemRepresentation, &status) && S_ISREG(status.st_mode) && status.st_nlink == 1);
    assert(status.st_uid == geteuid() && (status.st_mode & 0777) == 0600);
    dispose(message);

    // Regeneration replaces both plists atomically and preserves the existing df log.
    assert([@"df prior output\n" writeToFile:dfLog atomically:NO encoding:NSUTF8StringEncoding error:NULL]);
    assert(!LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL));
    assert(!stat(jobPath.fileSystemRepresentation, &status) && status.st_ino != originalJob.st_ino);
    assert(!stat(configPath.fileSystemRepresentation, &status) && status.st_ino != originalConfig.st_ino);
    assert([readPlist(jobPath) isEqualToDictionary:job]);
    assert([[NSString stringWithContentsOfFile:dfLog encoding:NSUTF8StringEncoding error:NULL] isEqualToString:@"df prior output\n"]);
    dispose(message);
    NSData *priorConfig = [NSData dataWithContentsOfFile:configPath], *priorJob = [NSData dataWithContentsOfFile:jobPath];

    // Unknown jobs, stale temporary files and directories are never loaded or deleted by the probe generator.
    NSString *unexpected = [jobDirectory stringByAppendingPathComponent:@"other.plist"];
    assert([@"unrelated" writeToFile:unexpected atomically:NO encoding:NSUTF8StringEncoding error:NULL]);
    assert(LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL) == EEXIST);
    assertUnchanged(message);
    assert([[NSData dataWithContentsOfFile:configPath] isEqualToData:priorConfig]);
    assert([[NSData dataWithContentsOfFile:jobPath] isEqualToData:priorJob]);
    assert([[NSString stringWithContentsOfFile:unexpected encoding:NSUTF8StringEncoding error:NULL] isEqualToString:@"unrelated"]);
    assert(!unlink(unexpected.fileSystemRepresentation));
    unexpected = [jobDirectory stringByAppendingPathComponent:@".launchd-bootstrap.stale"];
    assert(!mkdir(unexpected.fileSystemRepresentation, 0700));
    assert(LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL) == EEXIST);
    assertUnchanged(message);
    assert(!rmdir(unexpected.fileSystemRepresentation));

    // The probe log cannot be a symlink, FIFO, or multiply linked file, and a job symlink cannot redirect publication.
    assert(!unlink(dfLog.fileSystemRepresentation) && !symlink(configPath.fileSystemRepresentation, dfLog.fileSystemRepresentation));
    assert(LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL) == ELOOP);
    assertUnchanged(message);
    assert(!unlink(dfLog.fileSystemRepresentation) && !mkfifo(dfLog.fileSystemRepresentation, 0600));
    assert(LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL) != 0);
    assertUnchanged(message);
    assert(!unlink(dfLog.fileSystemRepresentation) && !link(configPath.fileSystemRepresentation, dfLog.fileSystemRepresentation));
    assert(LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL) == EINVAL);
    assertUnchanged(message);
    assert(!unlink(dfLog.fileSystemRepresentation));
    assert(!unlink(jobPath.fileSystemRepresentation) && !symlink(configPath.fileSystemRepresentation, jobPath.fileSystemRepresentation));
    assert(LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL) == ELOOP);
    assertUnchanged(message);
    assert([[NSData dataWithContentsOfFile:configPath] isEqualToData:priorConfig]);
    assert(!unlink(jobPath.fileSystemRepresentation));
    assert(!rmdir(jobDirectory.fileSystemRepresentation) && !symlink(root.fileSystemRepresentation, jobDirectory.fileSystemRepresentation));
    assert(LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL) != 0);
    assertUnchanged(message);
    assert(!unlink(jobDirectory.fileSystemRepresentation));
    assert(!LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL));
    dispose(message);

    // The original entry point always publishes an empty bootstrap even after the opt-in probe has been prepared.
    assert(!LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, NULL));
    assert([readPlist(configPath)[@"Bootstrap"][@"Paths"] count] == 0);
    assert([[NSFileManager.defaultManager contentsOfDirectoryAtPath:jobDirectory error:NULL] isEqualToArray:@[@"com.kdt.LiveSimBooter.df.plist"]]);
    dispose(message);
}

static void testBackboardPreferences(LSBReceivedMessage *message, NSString *root, NSString *dataPath, NSString *configPath) {
    NSString *directory = [dataPath stringByAppendingPathComponent:@"Library/LiveSimBooter-BackboardDaemons"];
    NSString *jobPath = [directory stringByAppendingPathComponent:@"com.apple.cfprefsd.xpc.daemon.plist"];
    NSSet *expectedJobs = [NSSet setWithArray:
        @[@"com.apple.backboardd.plist", @"com.apple.cfprefsd.xpc.daemon.plist",
          @"com.kdt.LiveSimBooter.terminal-fish.plist", @"com.kdt.LiveSimBooter.display-settings.plist"]];
    assert([[NSSet setWithArray:[NSFileManager.defaultManager contentsOfDirectoryAtPath:directory error:NULL]] isEqual:expectedJobs]);
    NSDictionary *job = readPlist(jobPath);
    assert([job[@"Label"] isEqual:@"com.apple.cfprefsd.xpc.daemon"]);
    assert(([job[@"ProgramArguments"] isEqual:@[@"/usr/sbin/cfprefsd", @"daemon"]]));
    assert(([job[@"MachServices"] isEqual:@{@"com.apple.cfprefsd.daemon": @YES}]));
    assert([job[@"RunAtLoad"] boolValue] && ![job[@"KeepAlive"] boolValue] && ![job[@"EnableTransactions"] boolValue]);
    for (NSString *key in @[@"POSIXSpawnType", @"UserName", @"LimitLoadToSessionType", @"HardResourceLimits", @"SoftResourceLimits",
                            @"EnvironmentVariables", @"EnablePressuredExit"]) assert(!job[key]);
    assert([job[@"StandardOutPath"] isEqual:@"/private/var/mobile/Library/Logs/cfprefsd.log"]);
    assert([job[@"StandardErrorPath"] isEqual:job[@"StandardOutPath"]]);
    NSString *preferencesDirectory = [dataPath stringByAppendingPathComponent:@"Library/Preferences"];
    NSString *preferencesPath = [preferencesDirectory stringByAppendingPathComponent:@"com.apple.backboardd.plist"];
    NSString *logPath = [dataPath stringByAppendingPathComponent:@"Library/Logs/cfprefsd.log"];
    struct stat status;
    assert(!stat(preferencesDirectory.fileSystemRepresentation, &status) && S_ISDIR(status.st_mode) && (status.st_mode & 0777) == 0700);
    assert(!stat(preferencesPath.fileSystemRepresentation, &status) && S_ISREG(status.st_mode) && (status.st_mode & 0777) == 0600);
    assert(([readPlist(preferencesPath) isEqual:@{@"BKDigitizerVisualizeTouches": @YES}]));
    assert(!stat(logPath.fileSystemRepresentation, &status) && S_ISREG(status.st_mode) && (status.st_mode & 0777) == 0600);
    NSData *logContent = [@"existing cfprefsd output\n" dataUsingEncoding:NSUTF8StringEncoding];
    assert([logContent writeToFile:logPath atomically:NO]);
    NSDictionary *existing = @{@"BKDigitizerVisualizeTouches": @NO, @"KeepOtherSetting": @[@1, @"value", @{@"nested": @YES}]};
    NSData *binary = [NSPropertyListSerialization dataWithPropertyList:existing format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    assert([binary writeToFile:preferencesPath atomically:NO]);
    assert(!LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, NULL));
    dispose(message);
    assert([readPlist(preferencesPath) isEqual:existing]);
    assert(CFGetTypeID((__bridge CFTypeRef)readPlist(preferencesPath)[@"BKDigitizerVisualizeTouches"]) == CFBooleanGetTypeID());
    assert([[NSData dataWithContentsOfFile:logPath] isEqual:logContent]);
    NSData *preferences = [NSData dataWithContentsOfFile:preferencesPath];
    assert(!stat(preferencesPath.fileSystemRepresentation, &status));
    ino_t inode = status.st_ino;
    assert(!LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, NULL));
    dispose(message);
    assert(!stat(preferencesPath.fileSystemRepresentation, &status) && status.st_ino == inode);
    NSData *configuration = [NSData dataWithContentsOfFile:configPath];
    for (id invalid in @[@[@"not a dictionary"], @"malformed plist", @{@"BKDigitizerVisualizeTouches": @1}]) {
        NSData *data = [invalid isKindOfClass:NSString.class] ? [invalid dataUsingEncoding:NSUTF8StringEncoding] :
            [NSPropertyListSerialization dataWithPropertyList:invalid format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
        assert([data writeToFile:preferencesPath atomically:NO]);
        assert(LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, NULL) == EINVAL);
        assertUnchanged(message);
        assert([[NSData dataWithContentsOfFile:preferencesPath] isEqual:data]);
        assert([[NSData dataWithContentsOfFile:configPath] isEqual:configuration]);
    }
    int fd = open(preferencesPath.fileSystemRepresentation, O_WRONLY);
    assert(fd >= 0 && !ftruncate(fd, 1024 * 1024 + 1) && !close(fd));
    assert(LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, NULL) == EFBIG);
    assertUnchanged(message);
    assert(!unlink(preferencesPath.fileSystemRepresentation));
    assert(!symlink(configPath.fileSystemRepresentation, preferencesPath.fileSystemRepresentation));
    assert(LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, NULL) == ELOOP);
    assertUnchanged(message);
    assert(!unlink(preferencesPath.fileSystemRepresentation));
    assert(!link(configPath.fileSystemRepresentation, preferencesPath.fileSystemRepresentation));
    assert(LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, NULL) == EINVAL);
    assertUnchanged(message);
    assert(!unlink(preferencesPath.fileSystemRepresentation) && !mkfifo(preferencesPath.fileSystemRepresentation, 0600));
    assert(LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, NULL) == EINVAL);
    assertUnchanged(message);
    assert(!unlink(preferencesPath.fileSystemRepresentation));
    assert(!rmdir(preferencesDirectory.fileSystemRepresentation));
    assert(!symlink(root.fileSystemRepresentation, preferencesDirectory.fileSystemRepresentation));
    assert(LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, NULL) != 0);
    assertUnchanged(message);
    assert(!unlink(preferencesDirectory.fileSystemRepresentation) && !mkdir(preferencesDirectory.fileSystemRepresentation, 0700));
    assert([preferences writeToFile:preferencesPath atomically:NO]);
    NSString *unexpectedJob = [directory stringByAppendingPathComponent:@"com.apple.cfprefsd.xpc.daemon.system.plist"];
    assert([@{} writeToFile:unexpectedJob atomically:YES]);
    assert(LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, NULL) == EEXIST);
    assertUnchanged(message);
    assert(!unlink(unexpectedJob.fileSystemRepresentation));
    assert(!unlink(jobPath.fileSystemRepresentation) && !symlink(configPath.fileSystemRepresentation, jobPath.fileSystemRepresentation));
    assert(LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, NULL) == ELOOP);
    assertUnchanged(message);
    assert(!unlink(jobPath.fileSystemRepresentation));
    assert(!unlink(logPath.fileSystemRepresentation) && !symlink(configPath.fileSystemRepresentation, logPath.fileSystemRepresentation));
    assert(LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, NULL) == ELOOP);
    assertUnchanged(message);
    assert(!unlink(logPath.fileSystemRepresentation));
    assert([[NSData dataWithContentsOfFile:configPath] isEqual:configuration]);
    assert(!LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, NULL));
    dispose(message);
    // Other launch modes do not enable or alter this simulator preference.
    assert([binary writeToFile:preferencesPath atomically:NO]);
    assert(!LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, NULL));
    dispose(message);
    assert([[NSData dataWithContentsOfFile:preferencesPath] isEqual:binary]);
    assert(!LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL));
    dispose(message);
    assert([[NSData dataWithContentsOfFile:preferencesPath] isEqual:binary]);
    assert(!LSBPrepareLaunchdLifecyclePlan(message, root.fileSystemRepresentation, NULL));
    dispose(message);
    assert([[NSData dataWithContentsOfFile:preferencesPath] isEqual:binary]);
}

static NSDictionary *snapshotDirectory(NSString *directory) {
    NSMutableDictionary *snapshot = [NSMutableDictionary dictionary];
    NSArray *paths = [NSFileManager.defaultManager subpathsOfDirectoryAtPath:directory error:NULL];
    assert(paths);
    for (NSString *relative in paths) {
        NSString *path = [directory stringByAppendingPathComponent:relative];
        BOOL isDirectory;
        assert([NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&isDirectory]);
        id contents = isDirectory ? @"directory" : [NSData dataWithContentsOfFile:path];
        assert(contents);
        snapshot[relative] = contents;
    }
    return snapshot;
}

static void testServiceCapacity(LSBReceivedMessage *message, NSString *root, NSString *dataPath, NSString *configPath) {
    enum { capacity = 20 };
    assert(capacity + 3 < LSB_CHILD_STATUS_CAPACITY);
    NSString *stockDirectory = [root stringByAppendingPathComponent:@"System/Library/LaunchDaemons"];
    char names[capacity + 1][64];
    const char *selected[capacity + 2] = {0};
    for (unsigned i = 0; i <= capacity; ++i) {
        snprintf(names[i], sizeof(names[i]), "com.kdt.LiveSimBooter.capacity.%u", i);
        selected[i] = names[i];
        NSString *label = [NSString stringWithUTF8String:names[i]];
        NSString *path = [stockDirectory stringByAppendingPathComponent:[label stringByAppendingString:@".plist"]];
        NSDictionary *stock = @{@"Label": label, @"ProgramArguments": @[@"/bin/df"]};
        assert([stock writeToFile:path atomically:YES]);
    }
    NSDictionary *prior = snapshotDirectory(dataPath);
    assert(LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, selected) == E2BIG);
    assertUnchanged(message);
    assert([snapshotDirectory(dataPath) isEqual:prior]); // Includes paths as well as contents: no partial job/log publication.
    selected[capacity] = NULL;
    assert(!LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, selected));
    NSString *directory = [readPlist(configPath)[@"Bootstrap"][@"Paths"] firstObject];
    assert([NSFileManager.defaultManager contentsOfDirectoryAtPath:directory error:NULL].count == capacity + 4);
    dispose(message);
    prior = snapshotDirectory(dataPath);
    selected[capacity] = names[capacity];
    assert(LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, selected) == E2BIG);
    assertUnchanged(message);
    assert([snapshotDirectory(dataPath) isEqual:prior]);
}

static void testSpringBoardResources(LSBReceivedMessage *message, NSString *root, NSString *configPath) {
    NSString *program = @"/System/Library/CoreServices/SpringBoard.app/SpringBoard";
    NSArray<NSDictionary *> *cases = @[
        @{@"Label": @"com.apple.SpringBoard", @"ProgramArguments": @[program], @"expected": @YES},
        @{@"Label": @"com.apple.SpringBoard", @"Program": program, @"expected": @YES},
        @{@"Label": @"com.apple.SpringBoard", @"Program": program, @"ProgramArguments": @[@"different-argv0"], @"expected": @YES},
        @{@"Label": @"com.apple.SpringBoard", @"Program": @"/bin/df", @"ProgramArguments": @[program], @"expected": @NO},
        @{@"Label": @"com.apple.SpringBoard", @"ProgramArguments": @[[program stringByAppendingString:@"Other"]], @"expected": @NO},
        @{@"Label": @"com.apple.springboard", @"ProgramArguments": @[program], @"expected": @NO},
        @{@"Label": @"com.kdt.other", @"ProgramArguments": @[program], @"expected": @NO}
    ];
    for (NSDictionary *test in cases) {
        NSString *label = test[@"Label"], *filename = [label stringByAppendingString:@".plist"];
        NSString *path = [[root stringByAppendingPathComponent:@"System/Library/LaunchDaemons"] stringByAppendingPathComponent:filename];
        NSMutableDictionary *stock = test.mutableCopy;
        [stock removeObjectForKey:@"expected"];
        stock[@"EnvironmentVariables"] = @{@"CA_ASSERT_MAIN_THREAD_TRANSACTIONS": @"1", @"LSB_SIMULATE_FRAMEWORK_BUNDLES": @"untrusted",
                                           @"LSB_SIMULATE_SPRINGBOARD_ENTITLEMENTS": @"untrusted", @"LSB_REGISTER_EXTENSIONKIT": @"untrusted",
                                           @"LSB_REGISTER_METAL_COMPILER": @"1"};
        assert([stock writeToFile:path atomically:YES]);
        const char *selected[] = {label.UTF8String, NULL};
        assert(!LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, selected));
        NSDictionary *bootstrap = readPlist(configPath)[@"Bootstrap"];
        NSString *directory = [bootstrap[@"Paths"] firstObject];
        NSDictionary *environment = readPlist([directory stringByAppendingPathComponent:filename])[@"EnvironmentVariables"];
        assert([environment[@"CA_ASSERT_MAIN_THREAD_TRANSACTIONS"] isEqual:@"1"]);
        for (NSString *key in @[@"LSB_SIMULATE_FRAMEWORK_BUNDLES", @"LSB_SIMULATE_SPRINGBOARD_ENTITLEMENTS",
                                @"LSB_REGISTER_EXTENSIONKIT", @"LSB_REGISTER_METAL_COMPILER"]) {
            if ([test[@"expected"] boolValue]) assert([environment[key] isEqual:@"1"]);
            else assert(!environment[key]);
            assert(!environmentValue(&message->plan, key.UTF8String));
            for (NSString *scope in @[@"Self", @"Child"]) assert(!bootstrap[scope][key]);
        }
        dispose(message);
    }
}

static void testIconServicesGraphics(LSBReceivedMessage *message, NSString *root, NSString *configPath) {
    NSString *label = @"com.apple.iconservices.iconservicesagent", *program = @"/System/Library/CoreServices/iconservicesagent";
    NSArray<NSDictionary *> *cases = @[
        @{@"Label": label, @"ProgramArguments": @[program], @"expected": @YES},
        @{@"Label": label, @"Program": program, @"expected": @YES},
        @{@"Label": label, @"Program": program, @"ProgramArguments": @[@"different-argv0"], @"expected": @YES},
        @{@"Label": label, @"Program": @"/bin/df", @"ProgramArguments": @[program], @"expected": @NO},
        @{@"Label": label, @"ProgramArguments": @[[program stringByAppendingString:@"Other"]], @"expected": @NO},
        @{@"Label": @"com.apple.iconservices.IconServicesAgent", @"ProgramArguments": @[program], @"expected": @NO},
        @{@"Label": @"com.kdt.other-iconservice", @"ProgramArguments": @[program], @"expected": @NO}
    ];
    for (NSDictionary *test in cases) {
        NSString *selectedLabel = test[@"Label"], *filename = [selectedLabel stringByAppendingString:@".plist"];
        NSString *path = [[root stringByAppendingPathComponent:@"System/Library/LaunchDaemons"] stringByAppendingPathComponent:filename];
        NSMutableDictionary *stock = test.mutableCopy;
        [stock removeObjectForKey:@"expected"];
        stock[@"EnvironmentVariables"] = @{@"LSB_REGISTER_METAL_COMPILER": @"untrusted",
                                           @"LSB_SIMULATE_FRAMEWORK_BUNDLES": @"untrusted"};
        assert([stock writeToFile:path atomically:YES]);
        const char *selected[] = {selectedLabel.UTF8String, NULL};
        assert(!LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, selected));
        NSDictionary *bootstrap = readPlist(configPath)[@"Bootstrap"];
        NSString *directory = [bootstrap[@"Paths"] firstObject];
        NSDictionary *environment = readPlist([directory stringByAppendingPathComponent:filename])[@"EnvironmentVariables"];
        BOOL expected = [test[@"expected"] boolValue];
        for (NSString *key in @[@"LSB_REGISTER_METAL_COMPILER", @"LSB_SIMULATE_FRAMEWORK_BUNDLES"]) {
            if (expected) assert([environment[key] isEqual:@"1"]);
            else assert(!environment[key]);
        }
        assert(!environment[@"LSB_SIMULATE_SPRINGBOARD_ENTITLEMENTS"] && !environment[@"LSB_REGISTER_EXTENSIONKIT"]);
        for (NSString *key in @[@"LSB_REGISTER_METAL_COMPILER", @"LSB_SIMULATE_FRAMEWORK_BUNDLES"]) {
            assert(!environmentValue(&message->plan, key.UTF8String));
            for (NSString *scope in @[@"Self", @"Child"]) assert(!bootstrap[scope][key]);
        }
        dispose(message);
    }
}

static void testPosterBoardDiscovery(LSBReceivedMessage *message, NSString *root, NSString *configPath) {
    NSString *program = @"/Applications/PosterBoard.app/PosterBoard";
    NSArray<NSDictionary *> *cases = @[
        @{@"Label": @"com.apple.PosterBoard", @"ProgramArguments": @[program], @"expected": @YES},
        @{@"Label": @"com.apple.PosterBoard", @"Program": program, @"expected": @YES},
        @{@"Label": @"com.apple.PosterBoard", @"Program": program, @"ProgramArguments": @[@"different-argv0"], @"expected": @YES},
        @{@"Label": @"com.apple.PosterBoard", @"Program": @"/bin/df", @"ProgramArguments": @[program], @"expected": @NO},
        @{@"Label": @"com.apple.PosterBoard", @"ProgramArguments": @[[program stringByAppendingString:@"Other"]], @"expected": @NO},
        @{@"Label": @"com.apple.posterboard", @"ProgramArguments": @[program], @"expected": @NO},
        @{@"Label": @"com.kdt.other-poster", @"ProgramArguments": @[program], @"expected": @NO}
    ];
    for (NSDictionary *test in cases) {
        NSString *label = test[@"Label"], *filename = [label stringByAppendingString:@".plist"];
        NSString *path = [[root stringByAppendingPathComponent:@"System/Library/LaunchDaemons"] stringByAppendingPathComponent:filename];
        NSMutableDictionary *stock = test.mutableCopy;
        [stock removeObjectForKey:@"expected"];
        stock[@"EnvironmentVariables"] = @{@"LSB_REGISTER_EXTENSIONKIT": @"untrusted", @"LSB_SIMULATE_SPRINGBOARD_ENTITLEMENTS": @"1"};
        assert([stock writeToFile:path atomically:YES]);
        const char *selected[] = {label.UTF8String, NULL};
        assert(!LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, selected));
        NSDictionary *bootstrap = readPlist(configPath)[@"Bootstrap"];
        NSString *directory = [bootstrap[@"Paths"] firstObject];
        NSDictionary *environment = readPlist([directory stringByAppendingPathComponent:filename])[@"EnvironmentVariables"];
        if ([test[@"expected"] boolValue]) assert([environment[@"LSB_REGISTER_EXTENSIONKIT"] isEqual:@"1"]);
        else assert(!environment[@"LSB_REGISTER_EXTENSIONKIT"]);
        assert(!environment[@"LSB_SIMULATE_SPRINGBOARD_ENTITLEMENTS"]);
        assert(!environmentValue(&message->plan, "LSB_REGISTER_EXTENSIONKIT"));
        for (NSString *scope in @[@"Self", @"Child"]) assert(!bootstrap[scope][@"LSB_REGISTER_EXTENSIONKIT"]);
        dispose(message);
        assert([NSFileManager.defaultManager removeItemAtPath:path error:NULL]);
    }
}

static void testLaunchAngelJobs(LSBReceivedMessage *message, NSString *root, NSString *dataPath, NSString *configPath) {
    NSString *directory = [root stringByAppendingPathComponent:@"System/Library/LaunchAngels"];
    assert([NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:NULL]);
    NSString *filename = @"com.apple.PosterBoard.plist", *path = [directory stringByAppendingPathComponent:filename];
    NSDictionary *runningBoard = @{@"Managed": @YES, @"Reported": @YES};
    NSDictionary *stock = @{
        @"Label": @"com.apple.PosterBoard", @"ProgramArguments": @[@"/Applications/PosterBoard.app/PosterBoard"],
        @"RunAtLoad": @NO, @"KeepAlive": @NO, @"EnableTransactions": @NO, @"EnablePressuredExit": @NO,
        @"ProcessType": @"App", @"POSIXSpawnType": @"App", @"UserName": @"mobile",
        @"MachServices": @{@"com.apple.posterboardservices.services": @{@"_ExperimentalNonLaunching": @YES}},
        @"_AdditionalProperties": @{@"RunningBoard": runningBoard, @"UnrelatedPolicy": @YES},
        @"EnvironmentVariables": @{@"MallocNanoZone": @"0", @"HOME": @"/wrong", @"LSB_FAKE_PTY_STATE": @"/wrong"}
    };
    assert([stock writeToFile:path atomically:YES]);
    // Merely being in LaunchAngels never selects an application.
    assert(([@{@"Label": @"com.kdt.unselected", @"ProgramArguments": @[@"/bin/df"]}
        writeToFile:[directory stringByAppendingPathComponent:@"com.kdt.unselected.plist"] atomically:YES]));
    NSString *workerFilename = @"com.kdt.startup-worker.plist";
    NSString *workerPath = [[root stringByAppendingPathComponent:@"System/Library/LaunchDaemons"] stringByAppendingPathComponent:workerFilename];
    assert(([@{@"Label": @"com.kdt.startup-worker", @"ProgramArguments": @[@"/usr/libexec/launchctl-control-worker"]}
        writeToFile:workerPath atomically:YES]));
    const char *selected[] = {"com.apple.PosterBoard", "com.apple.notifyd", "com.kdt.startup-worker", NULL};
    assert(!LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, selected));
    NSString *generated = [readPlist(configPath)[@"Bootstrap"][@"Paths"] firstObject];
    NSDictionary *job = readPlist([generated stringByAppendingPathComponent:filename]);
    for (NSString *key in @[@"ProgramArguments", @"MachServices", @"RunAtLoad", @"KeepAlive", @"EnableTransactions",
                            @"EnablePressuredExit", @"ProcessType", @"POSIXSpawnType"]) assert([job[key] isEqual:stock[key]]);
    assertPlistBoolean(job[@"RunAtLoad"], NO);
    assert(([job[@"_AdditionalProperties"] isEqual:@{@"RunningBoard": runningBoard}]));
    assert(!job[@"UserName"] && !job[@"GroupName"]);
    assert(([job[@"EnvironmentVariables"] isEqual:@{@"MallocNanoZone": @"0", @"LSB_REGISTER_EXTENSIONKIT": @"1"}]));
    assert([job[@"StandardOutPath"] isEqual:@"/private/var/mobile/Library/Logs/com.apple.PosterBoard.log"]);
    assert([job[@"StandardErrorPath"] isEqual:job[@"StandardOutPath"]]);
    assert([job[@"WorkingDirectory"] isEqual:@"/private/var/mobile/"]);
    assert([NSFileManager.defaultManager contentsOfDirectoryAtPath:generated error:NULL].count == 7);
    assert(![NSFileManager.defaultManager fileExistsAtPath:[generated stringByAppendingPathComponent:@"com.kdt.unselected.plist"]]);
    NSDictionary *daemon = readPlist([generated stringByAppendingPathComponent:@"com.apple.notifyd.plist"]);
    assert([daemon[@"RunAtLoad"] boolValue] && ![daemon[@"KeepAlive"] boolValue] && !daemon[@"_AdditionalProperties"]);
    assertPlistBoolean(daemon[@"RunAtLoad"], YES);
    NSDictionary *worker = readPlist([generated stringByAppendingPathComponent:workerFilename]);
    assert(!worker[@"MachServices"] && [worker[@"ProgramArguments"] isEqual:@[@"/usr/libexec/launchctl-control-worker"]]);
    assertPlistBoolean(worker[@"RunAtLoad"], YES); // Without a demand service, a numeric 1 silently leaves this worker unstarted.
    dispose(message);

    // Reject unsupported on-demand policies before publishing any output, while preserving the prior working selection.
    NSDictionary *prior = snapshotDirectory(dataPath);
    NSArray *invalid = @[
        @{@"RunAtLoad": @YES}, @{@"KeepAlive": @YES}, @{@"EnableTransactions": @YES}, @{@"EnablePressuredExit": @YES},
        @{@"ProcessType": @"Interactive"}, @{@"POSIXSpawnType": @"Interactive"},
        @{@"RunAtLoad": @"false"}, @{@"KeepAlive": @{}}, @{@"EnablePressuredExit": @0}, @{@"ProcessType": @YES},
        @{@"_AdditionalProperties": @[]}, @{@"_AdditionalProperties": @{@"RunningBoard": @YES}},
        @{@"_AdditionalProperties": @{@"RunningBoard": @{@"Managed": @"true"}}}
    ];
    for (NSUInteger i = 0; i < invalid.count; ++i) {
        NSMutableDictionary *malformed = stock.mutableCopy;
        [malformed addEntriesFromDictionary:invalid[i]];
        assert([malformed writeToFile:path atomically:YES]);
        assert(LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, selected) == (i < 6 ? ENOTSUP : EINVAL));
        assertUnchanged(message);
        assert([snapshotDirectory(dataPath) isEqual:prior]);
    }
    assert([stock writeToFile:path atomically:YES]);
    NSString *daemonPath = [[root stringByAppendingPathComponent:@"System/Library/LaunchDaemons"] stringByAppendingPathComponent:filename];
    assert(!symlink(path.fileSystemRepresentation, daemonPath.fileSystemRepresentation));
    assert(LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, selected) == ELOOP);
    assertUnchanged(message);
    assert(!unlink(daemonPath.fileSystemRepresentation));
    assert(!unlink(path.fileSystemRepresentation));
    assert(!symlink(configPath.fileSystemRepresentation, path.fileSystemRepresentation));
    assert(LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, selected) == ELOOP);
    assertUnchanged(message);
    assert([snapshotDirectory(dataPath) isEqual:prior]);
    assert(!unlink(path.fileSystemRepresentation));
    assert([stock writeToFile:path atomically:YES]);
}

static void testServiceJobs(LSBReceivedMessage *message, NSString *root, NSString *dataPath, NSString *configPath) {
    NSString *stockDirectory = [root stringByAppendingPathComponent:@"System/Library/LaunchDaemons"];
    NSString *notifyPath = [stockDirectory stringByAppendingPathComponent:@"com.apple.notifyd.plist"];
    NSDictionary *notify = @{
        @"Label": @"com.apple.notifyd", @"ProgramArguments": @[@"/usr/sbin/notifyd", @"--test"],
        @"MachServices": @{@"com.apple.system.notification_center": @{@"ResetAtClose": @YES}},
        @"PublishesEvents": @"com.apple.notifyd.matching", @"UserName": @"root", @"GroupName": @"wheel",
        @"POSIXSpawnType": @"Interactive", @"KeepAlive": @YES, @"EnableTransactions": @YES, @"EnablePressuredExit": @YES,
        @"LimitLoadToSessionType": @"System", @"LaunchEvents": @{@"other": @{}}, @"HardResourceLimits": @{@"NumberOfFiles": @512},
        @"EnvironmentVariables": @{@"ASL_DISABLE": @"1", @"DEBUGSCOPE": @"normal", @"DYLD_INSERT_LIBRARIES": @"/bad",
            @"LSB_DEBUG_STOP_BEFORE_HANDOFF": @"1", @"SIMULATOR_ROOT": @"/bad", @"XPC_FLAGS": @"2", @"HOME": @"/bad"}
    };
    assert([notify writeToFile:notifyPath atomically:YES]);
    NSString *runningPath = [stockDirectory stringByAppendingPathComponent:@"com.apple.runningboardd.plist"];
    NSDictionary *running = @{@"Label": @"com.apple.runningboardd", @"Program": @"/usr/libexec/runningboardd",
        @"MachServices": @{@"com.apple.runningboard": @YES}, @"PublishesEvents": @{@"test.event": @{@"RequireTrusted": @YES}}};
    assert([running writeToFile:runningPath atomically:YES]);
    NSString *oldDirectory = [dataPath stringByAppendingPathComponent:@"Library/LiveSimBooter-BackboardDaemons"];
    NSString *oldJobPath = [oldDirectory stringByAppendingPathComponent:@"com.apple.backboardd.plist"];
    NSData *oldJob = [NSData dataWithContentsOfFile:oldJobPath];
    const char *selected[] = {"com.apple.notifyd", NULL}, *both[] = {"com.apple.notifyd", "com.apple.runningboardd", NULL};
    const char *inherited[] = {"LSB_DEBUG_STOP_BEFORE_HANDOFF=1", "LSB_DEBUG_OTHER=1", "DYLD_PRINT_LIBRARIES=1", "KEEP=ok", NULL};
    assert(!getenv("LSB_DEBUG_STOP_CHILDREN"));
    assert(!LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, inherited, selected));
    assert(!environmentValue(&message->plan, "LSB_DEBUG_STOP_BEFORE_HANDOFF") && !environmentValue(&message->plan, "LSB_DEBUG_OTHER"));
    assert(!environmentValue(&message->plan, "DYLD_PRINT_LIBRARIES") && !strcmp(environmentValue(&message->plan, "KEEP"), "ok"));
    NSDictionary *bootstrap = readPlist(configPath)[@"Bootstrap"];
    NSString *directory = [bootstrap[@"Paths"] firstObject], *selectedDirectory = directory;
    assert([directory hasPrefix:[dataPath stringByAppendingPathComponent:@"Library/LiveSimBooter-CoreDaemons-"]]);
    assert([bootstrap[@"Paths"] isEqual:@[directory]]);
    for (NSString *scope in @[@"Self", @"Child"]) {
        assert(!bootstrap[scope][@"LSB_DEBUG_STOP_BEFORE_HANDOFF"]);
        assert([bootstrap[scope][@"LSB_SIMULATE_SYSTEM_POWER"] isEqual:@"1"]);
        assert([bootstrap[scope][@"LSB_SIMULATE_NOTIFICATION_SHM"] isEqual:@"1"]);
        assert([bootstrap[scope][@"LSB_SIMULATE_CODE_SIGNING"] isEqual:@"1"]);
        assert([bootstrap[scope][@"LSB_ENABLE_PEER_PROCESS_INSPECTION"] isEqual:@"1"]);
        assert([bootstrap[scope][@"SIMULATOR_MAINSCREEN_WIDTH"] isEqual:@"1290"]);
    }
    NSString *jobPath = [directory stringByAppendingPathComponent:@"com.apple.notifyd.plist"];
    NSDictionary *job = readPlist(jobPath);
    assert([job[@"ProgramArguments"] isEqual:notify[@"ProgramArguments"]] && [job[@"MachServices"] isEqual:notify[@"MachServices"]]);
    assert([job[@"PublishesEvents"] isEqual:notify[@"PublishesEvents"]]);
    assert(([job[@"EnvironmentVariables"] isEqual:@{@"ASL_DISABLE": @"1", @"DEBUGSCOPE": @"normal"}]));
    assert([job[@"RunAtLoad"] boolValue] && ![job[@"KeepAlive"] boolValue] && ![job[@"EnableTransactions"] boolValue]);
    assertPlistBoolean(job[@"RunAtLoad"], YES);
    for (NSString *key in @[@"UserName", @"GroupName", @"POSIXSpawnType", @"EnablePressuredExit", @"LimitLoadToSessionType",
                            @"LaunchEvents", @"HardResourceLimits"]) assert(!job[key]);
    assert([job[@"StandardOutPath"] isEqual:@"/private/var/mobile/Library/Logs/com.apple.notifyd.log"]);
    assert([job[@"StandardErrorPath"] isEqual:job[@"StandardOutPath"]]);
    NSString *log = [dataPath stringByAppendingPathComponent:@"Library/Logs/com.apple.notifyd.log"];
    struct stat metadata;
    assert(!stat(log.fileSystemRepresentation, &metadata) && S_ISREG(metadata.st_mode) && (metadata.st_mode & 0777) == 0600);
    assert([[NSData dataWithContentsOfFile:oldJobPath] isEqual:oldJob]);
    dispose(message);
    assert(!LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, both));
    directory = [readPlist(configPath)[@"Bootstrap"][@"Paths"] firstObject];
    assert(![directory isEqual:selectedDirectory]);
    jobPath = [directory stringByAppendingPathComponent:@"com.apple.notifyd.plist"];
    job = readPlist([directory stringByAppendingPathComponent:@"com.apple.runningboardd.plist"]);
    assert([job[@"Program"] isEqual:running[@"Program"]] && !job[@"ProgramArguments"]);
    assert([job[@"PublishesEvents"] isEqual:running[@"PublishesEvents"]]);
    assert([NSFileManager.defaultManager contentsOfDirectoryAtPath:directory error:NULL].count == 6);
    dispose(message);
    NSData *priorJob = [NSData dataWithContentsOfFile:jobPath];
    const char *duplicate[] = {"com.apple.notifyd", "com.apple.notifyd", NULL};
    assert(LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, duplicate) == EINVAL);
    assert(LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, NULL) == EINVAL);
    const char *invalid[] = {"", "..", "../com.apple.notifyd", "com.apple.notifyd/plist", "com.apple.backboardd",
        "com.apple.cfprefsd.xpc.daemon", "com.kdt.LiveSimBooter.terminal-fish", "com.kdt.LiveSimBooter.display-settings"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        const char *labels[] = {invalid[i], NULL};
        assert(LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, labels) == EINVAL);
    }
    for (NSString *key in @[@"Label", @"ProgramArguments", @"MachServices", @"PublishesEvents", @"EnvironmentVariables"]) {
        NSMutableDictionary *malformed = notify.mutableCopy;
        malformed[key] = @7;
        assert([malformed writeToFile:notifyPath atomically:YES]);
        assert(LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, both) == EINVAL);
        assertUnchanged(message);
    }
    for (id value in @[@[@"relative"], @[@"/usr/../sbin/notifyd"], @[@"/usr/sbin/notifyd", @7], @[]]) {
        NSMutableDictionary *malformed = notify.mutableCopy;
        malformed[@"ProgramArguments"] = value;
        assert([malformed writeToFile:notifyPath atomically:YES]);
        assert(LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, both) == EINVAL);
        assertUnchanged(message);
    }
    assert([notify writeToFile:notifyPath atomically:YES]);
    assert(!LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, selected));
    assert([readPlist(configPath)[@"Bootstrap"][@"Paths"] isEqual:@[selectedDirectory]]);
    assert([[NSData dataWithContentsOfFile:jobPath] isEqual:priorJob]); // Narrowing does not load or delete the broader experiment.
    dispose(message);
    const char *reordered[] = {"com.apple.runningboardd", "com.apple.notifyd", NULL};
    assert(!LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, reordered));
    assert([readPlist(configPath)[@"Bootstrap"][@"Paths"] isEqual:@[directory]]);
    dispose(message);
    assert(!unlink(log.fileSystemRepresentation) && !symlink(configPath.fileSystemRepresentation, log.fileSystemRepresentation));
    assert(LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, both) == ELOOP);
    assertUnchanged(message);
    assert(!unlink(log.fileSystemRepresentation));
    assert(!LSBPrepareLaunchdServicesPlan(message, root.fileSystemRepresentation, NULL, both));
    dispose(message);
    assert(!LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, NULL));
    assert([readPlist(configPath)[@"Bootstrap"][@"Paths"] isEqual:@[oldDirectory]]);
    assert([[NSData dataWithContentsOfFile:oldJobPath] isEqual:oldJob]);
    dispose(message);
    testServiceCapacity(message, root, dataPath, configPath);
    testSpringBoardResources(message, root, configPath);
    testIconServicesGraphics(message, root, configPath);
    testPosterBoardDiscovery(message, root, configPath);
    testLaunchAngelJobs(message, root, dataPath, configPath);
}

static void testAdditionalJobs(LSBReceivedMessage *message, NSString *root, NSString *dataPath, NSString *configPath) {
    NSData *previous = [NSData dataWithContentsOfFile:configPath];
    assert(LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, NULL) == ENOENT);
    assertUnchanged(message);
    assert([[NSData dataWithContentsOfFile:configPath] isEqualToData:previous]);
    NSString *stockDirectory = [root stringByAppendingPathComponent:@"System/Library/LaunchDaemons"];
    assert([NSFileManager.defaultManager createDirectoryAtPath:stockDirectory withIntermediateDirectories:YES attributes:nil error:NULL]);
    NSString *stockPath = [stockDirectory stringByAppendingPathComponent:@"com.apple.backboardd.plist"];
    NSDictionary *services = @{@"com.apple.CARenderServer": @{@"ResetAtClose": @YES}, @"com.apple.backboardd.virtualframebuffer": @YES};
    NSDictionary *stock = @{@"MachServices": services, @"KeepAlive": @YES, @"POSIXSpawnType": @"Interactive", @"UserName": @"root"};
    assert([stock writeToFile:stockPath atomically:YES]);
    assert(LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, NULL) == ENOENT);
    assertUnchanged(message);
    assert([[NSData dataWithContentsOfFile:configPath] isEqualToData:previous]);
    NSString *preferencesStockPath = [stockDirectory stringByAppendingPathComponent:@"com.apple.cfprefsd.xpc.daemon.plist"];
    NSDictionary *preferencesStock = @{
        @"Label": @"com.apple.cfprefsd.xpc.daemon", @"ProgramArguments": @[@"/usr/sbin/cfprefsd", @"daemon"],
        @"MachServices": @{@"com.apple.cfprefsd.daemon": @YES}, @"POSIXSpawnType": @"Adaptive", @"EnableTransactions": @YES,
        @"EnablePressuredExit": @NO, @"HardResourceLimits": @{@"NumberOfFiles": @512}, @"SoftResourceLimits": @{@"NumberOfFiles": @512}
    };
    for (NSString *key in @[@"Label", @"ProgramArguments", @"MachServices"]) {
        NSMutableDictionary *invalid = preferencesStock.mutableCopy;
        invalid[key] = @"unexpected";
        assert([invalid writeToFile:preferencesStockPath atomically:YES]);
        assert(LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, NULL) == EINVAL);
        assertUnchanged(message);
        assert([[NSData dataWithContentsOfFile:configPath] isEqualToData:previous]);
    }
    assert([preferencesStock writeToFile:preferencesStockPath atomically:YES]);
    const char *inherited[] = {
        "SIMULATOR_FRAMEBUFFER_FRAMEWORK=/System/Library/PrivateFrameworks/SimFramebufferClient.framework/SimFramebufferClient",
        "SIMULATOR_FRAMEBUFFER_FRAMEWORK=", "SIMULATOR_FRAMEBUFFER_FRAMEWORK_EXTRA=preserved",
        "SIMULATOR_MAINSCREEN_WIDTH=1", "SIMULATOR_MAINSCREEN_WIDTH=", "SIMULATOR_MAINSCREEN_HEIGHT=2", "SIMULATOR_MAINSCREEN_HEIGHT=3",
        "SIMULATOR_MAINSCREEN_SCALE=99", "SIMULATOR_MAINSCREEN_SCALE=", "SIMULATOR_MAINSCREEN_PITCH=0", "SIMULATOR_MAINSCREEN_PITCH=7",
        "SIMULATOR_MAINSCREEN_WIDTH_EXTRA=preserved", "LSB_SIMULATE_SYSTEM_POWER=0", "LSB_SIMULATE_SYSTEM_POWER=",
        "LSB_SIMULATE_SYSTEM_POWER_EXTRA=preserved", NULL
    };
    NSDictionary<NSString *, NSString *> *screen = @{
        @"SIMULATOR_MAINSCREEN_WIDTH": @"1290", @"SIMULATOR_MAINSCREEN_HEIGHT": @"2796",
        @"SIMULATOR_MAINSCREEN_SCALE": @"3", @"SIMULATOR_MAINSCREEN_PITCH": @"460"
    };
    NSString *uncanonicalRoot = [root stringByAppendingString:@"/."];
    assert(!LSBPrepareLaunchdBackboardPlan(message, uncanonicalRoot.fileSystemRepresentation, inherited));
    NSString *provider = [root stringByAppendingPathComponent:@"Library/PrivateFrameworks/SimFramebuffer.framework/SimFramebuffer"];
    assert(!strcmp(environmentValue(&message->plan, "SIMULATOR_FRAMEBUFFER_FRAMEWORK"), provider.fileSystemRepresentation));
    assert(!strcmp(environmentValue(&message->plan, "SIMULATOR_FRAMEBUFFER_FRAMEWORK_EXTRA"), "preserved"));
    assert(!strcmp(environmentValue(&message->plan, "SIMULATOR_MAINSCREEN_WIDTH_EXTRA"), "preserved"));
    assert(!strcmp(environmentValue(&message->plan, "LSB_SIMULATE_SYSTEM_POWER"), "1"));
    assert(!strcmp(environmentValue(&message->plan, "LSB_SIMULATE_SYSTEM_POWER_EXTRA"), "preserved"));
    NSDictionary *bootstrap = readPlist(configPath)[@"Bootstrap"];
    for (NSString *key in screen)
        assert(!strcmp(environmentValue(&message->plan, key.UTF8String), screen[key].UTF8String));
    for (NSString *scope in @[@"Self", @"Child"]) {
        assert([bootstrap[scope][@"SIMULATOR_FRAMEBUFFER_FRAMEWORK"] isEqualToString:provider]);
        assert([bootstrap[scope][@"LSB_SIMULATE_SYSTEM_POWER"] isEqualToString:@"1"]);
        for (NSString *key in screen) assert([bootstrap[scope][key] isEqualToString:screen[key]]);
    }
    NSString *directory = [dataPath stringByAppendingPathComponent:@"Library/LiveSimBooter-BackboardDaemons"];
    NSDictionary *terminal = readPlist([directory stringByAppendingPathComponent:@"com.kdt.LiveSimBooter.terminal-fish.plist"]);
    assert(([terminal isEqual:@{@"Label": @"com.kdt.LiveSimBooter.terminal-fish", @"ProgramArguments": @[@"/usr/libexec/terminal-fish"],
        @"WorkingDirectory": @"/private/var/mobile/", @"RunAtLoad": @YES, @"KeepAlive": @NO, @"EnableTransactions": @NO}]));
    NSDictionary *settings = readPlist([directory stringByAppendingPathComponent:@"com.kdt.LiveSimBooter.display-settings.plist"]);
    assert(([settings[@"ProgramArguments"] isEqual:@[@"/usr/libexec/display-settings", @"--serve"]]));
    for (NSString *key in @[@"RunAtLoad", @"KeepAlive", @"EnableTransactions"])
        assert(CFGetTypeID((__bridge CFTypeRef)settings[key]) == CFBooleanGetTypeID());
    assert([settings[@"RunAtLoad"] boolValue] && ![settings[@"KeepAlive"] boolValue] && ![settings[@"EnableTransactions"] boolValue]);
    assert(!settings[@"MachServices"] && !settings[@"EnvironmentVariables"]);
    assert([settings[@"StandardOutPath"] isEqual:@"/private/var/mobile/Library/Logs/display-settings.log"]);
    assert([settings[@"StandardErrorPath"] isEqual:settings[@"StandardOutPath"]]);
    NSDictionary *job = readPlist([directory stringByAppendingPathComponent:@"com.apple.backboardd.plist"]);
    assert([readPlist(configPath)[@"Bootstrap"][@"Paths"] isEqualToArray:@[directory]]);
    assert([job[@"Label"] isEqualToString:@"com.apple.backboardd"] && [job[@"MachServices"] isEqualToDictionary:services]);
    assert([job[@"ProgramArguments"] isEqualToArray:@[@"/usr/libexec/backboardd"]]);
    assert(([job[@"EnvironmentVariables"] isEqualToDictionary:
        @{@"LSB_REGISTER_METAL_COMPILER": @"1", @"LSB_SIMULATE_FRAMEWORK_BUNDLES": @"1"}]));
    assert(!bootstrap[@"Self"][@"LSB_REGISTER_METAL_COMPILER"] && !bootstrap[@"Child"][@"LSB_REGISTER_METAL_COMPILER"]);
    assert(!bootstrap[@"Self"][@"LSB_SIMULATE_FRAMEWORK_BUNDLES"] && !bootstrap[@"Child"][@"LSB_SIMULATE_FRAMEWORK_BUNDLES"]);
    assert(!environmentValue(&message->plan, "LSB_REGISTER_METAL_COMPILER"));
    assert(![job[@"KeepAlive"] boolValue] && ![job[@"EnableTransactions"] boolValue]);
    assert(!job[@"POSIXSpawnType"] && !job[@"UserName"]);
    assert([job[@"StandardOutPath"] isEqualToString:@"/private/var/mobile/Library/Logs/backboardd.log"]);
    dispose(message);
    testBackboardPreferences(message, root, dataPath, configPath);
    assert(!LSBPrepareLaunchdLifecyclePlan(message, root.fileSystemRepresentation, NULL));
    assert(!environmentValue(&message->plan, "SIMULATOR_FRAMEBUFFER_FRAMEWORK"));
    assert(!environmentValue(&message->plan, "LSB_SIMULATE_SYSTEM_POWER"));
    for (NSString *key in screen) assert(!environmentValue(&message->plan, key.UTF8String));
    for (NSString *scope in @[@"Self", @"Child"]) {
        assert(!readPlist(configPath)[@"Bootstrap"][scope][@"SIMULATOR_FRAMEBUFFER_FRAMEWORK"]);
        assert(!readPlist(configPath)[@"Bootstrap"][scope][@"LSB_SIMULATE_SYSTEM_POWER"]);
        for (NSString *key in screen) assert(!readPlist(configPath)[@"Bootstrap"][scope][key]);
    }
    directory = [dataPath stringByAppendingPathComponent:@"Library/LiveSimBooter-LifecycleDaemons"];
    job = readPlist([directory stringByAppendingPathComponent:@"com.kdt.LiveSimBooter.lifecycle.plist"]);
    assert([readPlist(configPath)[@"Bootstrap"][@"Paths"] isEqualToArray:@[directory]]);
    assert([job[@"ProgramArguments"] isEqualToArray:@[@"/bin/lifecycle-probe"]]);
    assert(!job[@"EnvironmentVariables"][@"LSB_REGISTER_METAL_COMPILER"]);
    assert(!job[@"MachServices"] && ![job[@"KeepAlive"] boolValue]);
    dispose(message);
    assert(!LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, NULL));
    assert(!environmentValue(&message->plan, "LSB_SIMULATE_SYSTEM_POWER"));
    for (NSString *key in screen) assert(!environmentValue(&message->plan, key.UTF8String));
    for (NSString *scope in @[@"Self", @"Child"]) {
        assert(!readPlist(configPath)[@"Bootstrap"][scope][@"LSB_SIMULATE_SYSTEM_POWER"]);
        for (NSString *key in screen) assert(!readPlist(configPath)[@"Bootstrap"][scope][key]);
    }
    dispose(message);
    testServiceJobs(message, root, dataPath, configPath);
    testSelectedDeviceProfiles(message, root, configPath);
    assert([NSFileManager.defaultManager removeItemAtPath:[root stringByAppendingPathComponent:@"System"] error:NULL]);
}

int main(void) {
    @autoreleasepool {
        char temporary[] = "/tmp/LiveSimBooter-launchd-config.XXXXXX", canonical[PATH_MAX];
        assert(mkdtemp(temporary) && realpath(temporary, canonical));
        NSString *group = [NSString stringWithUTF8String:canonical];
        NSString *build = [group stringByAppendingPathComponent:@LSB_STORAGE_BUILD_RELATIVE_PATH];
        NSString *root = [build stringByAppendingPathComponent:@LSB_RUNTIME_DIRECTORY];
        NSString *dataPath = [build stringByAppendingPathComponent:@LSB_DATA_DIRECTORY];
        NSString *logPath = [dataPath stringByAppendingPathComponent:@"Library/Logs/LiveSimBooter-launchd.log"];
        NSString *capabilitiesPath = [root stringByAppendingPathComponent:@"Library/Developer/CoreSimulator/DeviceType/capabilities.plist"];
        assert([NSFileManager.defaultManager createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:NULL]);
        assert(![NSFileManager.defaultManager fileExistsAtPath:capabilitiesPath]);
        const char *inherited[] = {"LSB_DEBUG_STOP_BEFORE_HANDOFF=1", "KEEP=value=with=equals", "HOME=/wrong", "PATH=/wrong",
            "XPC_SIMULATOR_LAUNCHD_NAME=wrong", "XPC_SIMULATOR_HOLDING_TANK_HACK=999", "XPC_SIMULATOR_HOLDING_TANK_FD_HACK=7",
            "XPC_USERSPACE_REBOOT_TO_BOOT_MODE=wrong", "XPC_FLAGS=0x2", "TERM=xterm", "LSB_FAKE_PTY_STATE=/wrong", "LSB_SIMULATOR_LAUNCHD=0",
            "SIMULATOR_BOOT_TIME=stale", "SIMULATOR_BOOT_TIME=0", "SIMULATOR_BOOT_TIME_EXTRA=preserved",
            "LSB_SIMULATOR_BOOT_UUID=stale", "LSB_SIMULATOR_BOOT_UUID=00000000-0000-0000-0000-000000000000",
            "LSB_SIMULATOR_BOOT_UUID_EXTRA=preserved",
            "SIMULATOR_CAPABILITIES=/wrong", "SIMULATOR_CAPABILITIES=", "SIMULATOR_CAPABILITIES_EXTRA=preserved", NULL};
        LSBReceivedMessage *message = calloc(1, sizeof(*message));
        assert(message);
        initialize(message);
        time_t beforeBoot = time(NULL);
        assert(!LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, inherited));
        LSBSpawnPlan *plan = &message->plan;
        assert(!LSBValidateSpawnPlan(plan) && plan->argc == 2 && plan->fdCount == 3 && plan->creationMask == 022);
        assert(!strcmp(plan->strings + plan->path, "/sbin/launchd_sim"));
        assert(!strcmp(plan->strings + plan->argv[0], "/sbin/launchd_sim"));
        assert(!strcmp(plan->strings + plan->cwd, dataPath.fileSystemRepresentation));
        assert(!strcmp(environmentValue(plan, "LSB_DEBUG_STOP_BEFORE_HANDOFF"), "1"));
        assert(!strcmp(environmentValue(plan, "KEEP"), "value=with=equals"));
        assert(!strcmp(environmentValue(plan, "XPC_SIMULATOR_HOLDING_TANK_HACK"), "-1"));
        assert(!strcmp(environmentValue(plan, "XPC_FLAGS"), "0x0000000000000000"));
        assert(!strcmp(environmentValue(plan, "LSB_SIMULATOR_LAUNCHD"), "1"));
        assert(!strcmp(environmentValue(plan, "SIMULATOR_PRODUCT_CLASS"), "D84"));
        assert(!strcmp(environmentValue(plan, "SIMULATOR_MODEL_IDENTIFIER"), "iPhone16,2"));
        assert(!strcmp(environmentValue(plan, "SIMULATOR_CAPABILITIES"), capabilitiesPath.fileSystemRepresentation));
        assert(!strcmp(environmentValue(plan, "SIMULATOR_CAPABILITIES_EXTRA"), "preserved"));
        const char *bootTime = environmentValue(plan, "SIMULATOR_BOOT_TIME");
        assert(bootTime && *bootTime && strspn(bootTime, "0123456789") == strlen(bootTime));
        char *end;
        errno = 0;
        unsigned long bootSeconds = strtoul(bootTime, &end, 10);
        assert(!errno && !*end && bootSeconds >= (unsigned long)beforeBoot && bootSeconds <= (unsigned long)time(NULL));
        assert(!strcmp(environmentValue(plan, "SIMULATOR_BOOT_TIME_EXTRA"), "preserved"));
        NSString *bootGeneration = [NSString stringWithUTF8String:bootTime];
        NSString *bootUUID = [NSString stringWithUTF8String:environmentValue(plan, "LSB_SIMULATOR_BOOT_UUID")];
        assert(bootUUID.length == 36 && [[NSUUID alloc] initWithUUIDString:bootUUID]);
        assert(![bootUUID isEqualToString:@"00000000-0000-0000-0000-000000000000"]);
        assert(!strcmp(environmentValue(plan, "LSB_SIMULATOR_BOOT_UUID_EXTRA"), "preserved"));
        assert(!environmentValue(plan, "SIMULATOR_FRAMEBUFFER_FRAMEWORK"));
        assert(!environmentValue(plan, "LSB_SIMULATE_SYSTEM_POWER"));
        assert(!strcmp(environmentValue(plan, "HOME"), dataPath.fileSystemRepresentation));
        assert(!environmentValue(plan, "TERM") && !environmentValue(plan, "LSB_FAKE_PTY_STATE"));
        assert(!environmentValue(plan, "XPC_USERSPACE_REBOOT_TO_BOOT_MODE") && !environmentValue(plan, "XPC_SIMULATOR_HOLDING_TANK_FD_HACK"));
        NSString *configPath = [NSString stringWithUTF8String:plan->strings + plan->argv[1]];
        NSDictionary *config = [NSPropertyListSerialization propertyListWithData:[NSData dataWithContentsOfFile:configPath]
            options:NSPropertyListImmutable format:NULL error:NULL];
        assert(config && [config[@"Bootstrap"][@"Paths"] count] == 0);
        assert([config[@"Bootstrap"][@"BinaryOrderPreference"] isEqualToString:@"arm64"]);
        assert([config[@"Bootstrap"][@"SimulatorRuntimeRoot"] isEqualToString:root]);
        assert([config[@"Bootstrap"][@"Self"][@"HOME"] isEqualToString:dataPath]);
        assert([config[@"Bootstrap"][@"Child"][@"HOME"] isEqualToString:dataPath]);
        assert([config[@"Bootstrap"][@"Child"][@"SIMULATOR_HID_SYSTEM_MANAGER"]
            isEqualToString:[root stringByAppendingPathComponent:@"Library/Frameworks/SimulatorHID.framework"]]);
        for (NSString *scope in @[@"Self", @"Child"]) {
            assert([config[@"Bootstrap"][scope][@"SIMULATOR_BOOT_TIME"] isEqualToString:bootGeneration]);
            assert([config[@"Bootstrap"][scope][@"LSB_SIMULATOR_BOOT_UUID"] isEqualToString:bootUUID]);
            assert([config[@"Bootstrap"][scope][@"SIMULATOR_PRODUCT_CLASS"] isEqualToString:@"D84"]);
            assert([config[@"Bootstrap"][scope][@"SIMULATOR_MODEL_IDENTIFIER"] isEqualToString:@"iPhone16,2"]);
            assert([config[@"Bootstrap"][scope][@"SIMULATOR_CAPABILITIES"] isEqualToString:capabilitiesPath]);
            assert(!config[@"Bootstrap"][scope][@"LSB_SIMULATE_SYSTEM_POWER"]);
        }
        assert(!config[@"Bootstrap"][@"Child"][@"XPC_SIMULATOR_HOLDING_TANK_HACK"]);
        assert(!config[@"Bootstrap"][@"Child"][@"LSB_DEBUG_STOP_BEFORE_HANDOFF"]);
        assert([config[@"Bootstrap"][@"Self"][@"LSB_SIMULATOR_LAUNCHD"] isEqualToString:@"1"]);
        assert(!config[@"Bootstrap"][@"Child"][@"LSB_SIMULATOR_LAUNCHD"]);
        assert([config[@"MultipleInstanceLimit"] intValue] == 64);
        struct stat output, errorOutput;
        assert(!fstat(message->fds[1], &output) && !fstat(message->fds[2], &errorOutput));
        assert(S_ISREG(output.st_mode) && output.st_ino == errorOutput.st_ino && output.st_dev == errorOutput.st_dev);
        for (int i = 0; i < 3; ++i) {
            assert(!isatty(message->fds[i]) && plan->fds[i].number == i && !plan->fds[i].flags);
            assert(fcntl(message->fds[i], F_GETFD) & FD_CLOEXEC);
        }
        char input;
        assert(read(message->fds[0], &input, 1) == 0);
        assert(write(message->fds[1], "stdout\n", 7) == 7 && write(message->fds[2], "stderr\n", 7) == 7);
        dispose(message);
        assert(!LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, NULL));
        NSString *nextBootUUID = [NSString stringWithUTF8String:environmentValue(&message->plan, "LSB_SIMULATOR_BOOT_UUID")];
        assert([[NSUUID alloc] initWithUUIDString:nextBootUUID] && ![nextBootUUID isEqualToString:bootUUID]);
        assert(write(message->fds[1], "again\n", 6) == 6);
        dispose(message);
        assert([[NSString stringWithContentsOfFile:logPath encoding:NSUTF8StringEncoding error:NULL] isEqualToString:@"stdout\nstderr\nagain\n"]);
        testDeviceProfile(message, root, configPath);
        testDFMode(message, root, dataPath, configPath, inherited);
        LSBNativeRuntimePolicy policy;
        assert(!LSBResolveNativeRuntimePolicy(nativeProductionStatus, NULL, &policy));
        testRuntimeBooleanEnvironment(message, root, configPath, LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV, policy.hardwareWorkqueue);
        testRuntimeBooleanEnvironment(message, root, configPath, LSB_RETAIN_HOST_CACHE_ENV, policy.retainHostCache);
        testRuntimeBooleanEnvironment(message, root, configPath, "LSB_TRACE_HANDOFF", NO);
        testLegacyDetachmentMode(message, root, configPath);
        testProductionPolicy(message, root, configPath);
        testAdditionalJobs(message, root, dataPath, configPath);

        // Invalid/oversized plans never publish partial strings or descriptors and do not replace the existing configuration.
        NSData *priorConfig = [NSData dataWithContentsOfFile:configPath];
        const char *tooMany[LSB_MAX_VECTOR + 2];
        for (unsigned i = 0; i < LSB_MAX_VECTOR + 1; ++i) tooMany[i] = "KEEP=1";
        tooMany[LSB_MAX_VECTOR + 1] = NULL;
        assert(LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, tooMany) == E2BIG);
        assertUnchanged(message);
        assert([[NSData dataWithContentsOfFile:configPath] isEqualToData:priorConfig]);
        assert(LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, tooMany) == E2BIG);
        assertUnchanged(message);
        assert([[NSData dataWithContentsOfFile:configPath] isEqualToData:priorConfig]);
        assert(LSBPrepareLaunchdPlan(message, "relative", NULL) == EINVAL);
        assertUnchanged(message);

        // Existing symlinks and non-regular logs must never redirect writes or block on a FIFO.
        assert(!unlink(logPath.fileSystemRepresentation));
        assert(!symlink(configPath.fileSystemRepresentation, logPath.fileSystemRepresentation));
        assert(LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, NULL) == ELOOP);
        assertUnchanged(message);
        assert(!unlink(logPath.fileSystemRepresentation) && !mkfifo(logPath.fileSystemRepresentation, 0600));
        assert(LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, NULL) != 0);
        assertUnchanged(message);
        assert(!unlink(logPath.fileSystemRepresentation));
        assert(!unlink(configPath.fileSystemRepresentation));
        assert(!symlink("/dev/null", configPath.fileSystemRepresentation));
        assert(LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, NULL) == ELOOP);
        assertUnchanged(message);
        assert([NSFileManager.defaultManager removeItemAtPath:dataPath error:NULL]);
        assert(!symlink(root.fileSystemRepresentation, dataPath.fileSystemRepresentation));
        assert(LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, NULL) != 0);
        assertUnchanged(message);
        assert([NSFileManager.defaultManager contentsOfDirectoryAtPath:root error:NULL].count == 0);
        assert([NSFileManager.defaultManager removeItemAtPath:group error:NULL]);
        free(message);
        puts("LaunchdConfiguration: isolated jobs, device profile defaults/overrides, regular logs, atomic regeneration and failure cleanup passed");
    }
    return 0;
}
