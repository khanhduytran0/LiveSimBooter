#include "../Guest/MetalCompilerRegistration.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned registrations;
static uint64_t registeredFlags;
static char registeredPath[PATH_MAX];

static void recordRegistration(const char *path, uint64_t flags) {
    ++registrations;
    registeredFlags = flags;
    assert(strlen(path) < sizeof(registeredPath));
    strcpy(registeredPath, path);
}

#define LSB_METAL_COMPILER_REGISTRATION_TESTING
#define xpc_add_bundle recordRegistration
#include "../Guest/MetalCompilerRegistration.c"
#undef xpc_add_bundle

static void join(char output[PATH_MAX], const char *parent, const char *suffix) {
    assert(!appendPath(output, parent, suffix));
}

static void makeDirectory(const char *path) {
    char copy[PATH_MAX];
    assert(strlen(path) < sizeof(copy));
    strcpy(copy, path);
    for (char *separator = copy + 1; *separator; ++separator) {
        if (*separator != '/') continue;
        *separator = 0;
        assert(!mkdir(copy, 0700) || errno == EEXIST);
        *separator = '/';
    }
    assert(!mkdir(copy, 0700) || errno == EEXIST);
}

static void makeFile(const char *path) {
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    assert(descriptor >= 0 && write(descriptor, "fixture", 7) == 7 && !close(descriptor));
}

static void makeAppInfo(const char *path, CFStringRef name, CFStringRef type, CFPropertyListFormat format) {
    const void *keys[] = {CFSTR("CFBundleExecutable"), CFSTR("CFBundlePackageType")};
    const void *values[] = {name, type};
    CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    assert(info);
    CFDataRef data = CFPropertyListCreateData(NULL, info, format, 0, NULL);
    assert(data);
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(descriptor >= 0 && write(descriptor, CFDataGetBytePtr(data), (size_t)CFDataGetLength(data)) == CFDataGetLength(data));
    assert(!close(descriptor));
    CFRelease(data);
    CFRelease(info);
}

static void removeTree(char *path) {
    char *paths[] = {path, NULL};
    FTS *tree = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    assert(tree);
    FTSENT *entry;
    while ((entry = fts_read(tree))) {
        if (entry->fts_info == FTS_D) continue;
        assert(entry->fts_info != FTS_ERR && entry->fts_info != FTS_DNR && entry->fts_info != FTS_NS);
        assert(!(entry->fts_info == FTS_DP ? rmdir(entry->fts_accpath) : unlink(entry->fts_accpath)));
    }
    assert(!fts_close(tree));
}

static void expect(const char *enabled, const char *root, const char *candidate, int error, bool shouldRequest) {
    unsigned previous = registrations;
    bool requested = true;
    assert(LSBRegisterMetalCompilerForExecutable(enabled, root, candidate, &requested) == error);
    assert(requested == shouldRequest && registrations == previous + shouldRequest);
    if (requested) assert(registeredFlags == 2);
}

static void expectRoles(const char *root, const char *backboardd, const char *springboard, const char *iconservices, const char *gpu,
                        int error, bool shouldRequest) {
    expect("1", root, backboardd, error, shouldRequest);
    expect("1", root, springboard, error, shouldRequest);
    expect("1", root, iconservices, error, shouldRequest);
    expect("1", root, gpu, error, shouldRequest);
}

int main(void) {
    char temporary[] = "/tmp/LiveSimBooter-metal-compiler.XXXXXX", canonical[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    char root[PATH_MAX], directory[PATH_MAX], executable[PATH_MAX], bundle[PATH_MAX], info[PATH_MAX], compiler[PATH_MAX];
    join(root, canonical, "RuntimeRoot");
    join(directory, root, "usr/libexec"); makeDirectory(directory);
    join(executable, directory, "backboardd"); makeFile(executable);
    char springboard[PATH_MAX], springboardDirectory[PATH_MAX];
    join(springboardDirectory, root, "System/Library/CoreServices/SpringBoard.app"); makeDirectory(springboardDirectory);
    join(springboard, springboardDirectory, "SpringBoard"); makeFile(springboard);
    char springboardInfo[PATH_MAX];
    join(springboardInfo, springboardDirectory, "Info.plist");
    makeAppInfo(springboardInfo, CFSTR("SpringBoard"), CFSTR("APPL"), kCFPropertyListXMLFormat_v1_0);
    char iconservices[PATH_MAX];
    join(iconservices, root, "System/Library/CoreServices/iconservicesagent"); makeFile(iconservices);
    char gpu[PATH_MAX], gpuDirectory[PATH_MAX];
    join(gpuDirectory, root, "System/Library/ExtensionKit/Extensions/GPUExtension.appex"); makeDirectory(gpuDirectory);
    join(gpu, gpuDirectory, "com.apple.WebKit.GPU"); makeFile(gpu);
    join(bundle, root, "System/Library/Frameworks/Metal.framework/XPCServices/MTLCompilerService.xpc"); makeDirectory(bundle);
    join(info, bundle, "Info.plist"); makeFile(info);
    join(compiler, bundle, "MTLCompilerService"); makeFile(compiler);

    // Opt-in is exact and does not inspect paths when disabled.
    const char *disabled[] = {NULL, "", "0", "true", "01", "1 "};
    for (unsigned i = 0; i < sizeof(disabled) / sizeof(*disabled); ++i) expect(disabled[i], NULL, NULL, 0, false);
    expect("1", NULL, executable, EINVAL, false);
    expect("1", "/", executable, EINVAL, false);
    expect("1", "RuntimeRoot", executable, EINVAL, false);
    expect("1", root, "usr/libexec/backboardd", EINVAL, false);
    assert(LSBRegisterMetalCompilerForExecutable("1", root, executable, NULL) == EINVAL && !registrations);

    expect("1", root, executable, 0, true);
    assert(!strcmp(registeredPath, bundle));
    char alias[PATH_MAX], candidate[PATH_MAX];
    join(alias, canonical, "runtime-alias"); assert(!symlink(root, alias));
    join(candidate, alias, "usr/libexec/../libexec/backboardd");
    expect("1", alias, candidate, 0, true);
    assert(!strcmp(registeredPath, bundle));

    expect("1", root, springboard, 0, true);
    assert(!strcmp(registeredPath, bundle));
    join(candidate, alias, "System/Library/CoreServices/SpringBoard.app/SpringBoard");
    expect("1", alias, candidate, 0, true);
    assert(!strcmp(registeredPath, bundle));
    join(candidate, springboardDirectory, "SpringBoardOther"); makeFile(candidate);
    expect("1", root, candidate, 0, false);

    expect("1", root, iconservices, 0, true);
    assert(!strcmp(registeredPath, bundle));
    join(candidate, alias, "System/Library/CoreServices/iconservicesagent");
    expect("1", alias, candidate, 0, true);
    assert(!strcmp(registeredPath, bundle));
    join(candidate, root, "System/Library/CoreServices/iconservicesagent-other"); makeFile(candidate);
    expect("1", root, candidate, 0, false);

    expect("1", root, gpu, 0, true);
    assert(!strcmp(registeredPath, bundle));
    join(candidate, alias, "System/Library/ExtensionKit/Extensions/GPUExtension.appex/com.apple.WebKit.GPU");
    expect("1", alias, candidate, 0, true);
    assert(!strcmp(registeredPath, bundle));
    join(candidate, gpuDirectory, "com.apple.WebKit.GPU-other"); makeFile(candidate);
    expect("1", root, candidate, 0, false);
    char extension[PATH_MAX];
    const char *otherExtensions[] = {"WebContent.appex", "GPUExtension.appex-other", "Unrelated.appex"};
    for (unsigned i = 0; i < sizeof(otherExtensions) / sizeof(*otherExtensions); ++i) {
        join(extension, root, "System/Library/ExtensionKit/Extensions");
        join(candidate, extension, otherExtensions[i]); makeDirectory(candidate);
        join(extension, candidate, i ? "com.apple.WebKit.GPU" : "com.apple.WebKit.WebContent"); makeFile(extension);
        expect("1", root, extension, 0, false);
    }

    // Inherited options never register compiler, proxy, launchd, unrelated, or merely prefix-matching executables.
    expect("1", root, compiler, 0, false);
    const char *otherRoles[] = {"launchd_sim", "xpcproxy_sim", "backboardd-other", "SpringBoard", "iconservicesagent", "df"};
    for (unsigned i = 0; i < sizeof(otherRoles) / sizeof(*otherRoles); ++i) {
        join(candidate, directory, otherRoles[i]); makeFile(candidate);
        expect("1", root, candidate, 0, false);
    }
    char outside[PATH_MAX], outsideFile[PATH_MAX];
    join(outside, canonical, "RuntimeRoot-other"); makeDirectory(outside);
    join(outsideFile, outside, "backboardd"); makeFile(outsideFile);
    expect("1", root, outsideFile, 0, false);
    assert(!unlink(executable) && !symlink(outsideFile, executable));
    expect("1", root, executable, 0, false);
    assert(!unlink(executable)); makeFile(executable);
    char outsideSpringBoard[PATH_MAX];
    join(outsideSpringBoard, outside, "System/Library/CoreServices/SpringBoard.app"); makeDirectory(outsideSpringBoard);
    join(candidate, outsideSpringBoard, "SpringBoard"); makeFile(candidate);
    expect("1", root, candidate, 0, false);
    assert(!unlink(springboard) && !symlink(candidate, springboard));
    expect("1", root, springboard, 0, false);
    assert(!unlink(springboard)); makeFile(springboard);

    char outsideIconServices[PATH_MAX];
    join(outsideIconServices, outside, "System/Library/CoreServices/iconservicesagent"); makeFile(outsideIconServices);
    expect("1", root, outsideIconServices, 0, false);
    assert(!unlink(iconservices) && !symlink(outsideIconServices, iconservices));
    expect("1", root, iconservices, 0, false);
    assert(!unlink(iconservices)); makeFile(iconservices);

    char outsideGPU[PATH_MAX];
    join(outsideGPU, outside, "System/Library/ExtensionKit/Extensions/GPUExtension.appex"); makeDirectory(outsideGPU);
    join(candidate, outsideGPU, "com.apple.WebKit.GPU"); makeFile(candidate);
    expect("1", root, candidate, 0, false);
    assert(!unlink(gpu) && !symlink(candidate, gpu));
    expect("1", root, gpu, 0, false);
    assert(!unlink(gpu)); makeFile(gpu);
    assert(!unlink(gpu) && !mkdir(gpu, 0700));
    expect("1", root, gpu, EINVAL, false);
    assert(!rmdir(gpu)); makeFile(gpu);

    // A bundle symlink escaping the canonical root is rejected even if its target contains plausible files.
    char moved[PATH_MAX];
    join(moved, outside, "MTLCompilerService.xpc");
    assert(!rename(bundle, moved) && !symlink(moved, bundle));
    expectRoles(root, executable, springboard, iconservices, gpu, EPERM, false);
    assert(!unlink(bundle));
    expectRoles(root, executable, springboard, iconservices, gpu, ENOENT, false);
    assert(!rename(moved, bundle));

    // Required files are regular, readable and nonempty; a final symlink or special file is not followed or blocked on.
    assert(!unlink(info)); expectRoles(root, executable, springboard, iconservices, gpu, ENOENT, false);
    assert(!symlink(outsideFile, info)); expectRoles(root, executable, springboard, iconservices, gpu, ELOOP, false);
    assert(!unlink(info) && !mkdir(info, 0700)); expectRoles(root, executable, springboard, iconservices, gpu, EINVAL, false);
    assert(!rmdir(info) && !mkfifo(info, 0600)); expectRoles(root, executable, springboard, iconservices, gpu, EINVAL, false);
    assert(!unlink(info)); makeFile(info);
    assert(!truncate(compiler, 0)); expectRoles(root, executable, springboard, iconservices, gpu, EINVAL, false);
    makeFile(compiler);
    assert(!unlink(compiler)); expectRoles(root, executable, springboard, iconservices, gpu, ENOENT, false);
    assert(!symlink(outsideFile, compiler)); expectRoles(root, executable, springboard, iconservices, gpu, ELOOP, false);
    assert(!unlink(compiler)); makeFile(compiler);
    expectRoles(root, executable, springboard, iconservices, gpu, 0, true);
    assert(registrations == 12);

    char app[PATH_MAX], appMain[PATH_MAX], appInfo[PATH_MAX];
    join(app, root, "Applications/Contacts.app"); makeDirectory(app);
    join(appMain, app, "Contacts"); makeFile(appMain);
    join(appInfo, app, "Info.plist");
    makeAppInfo(appInfo, CFSTR("Contacts"), CFSTR("APPL"), kCFPropertyListXMLFormat_v1_0);
    expect("1", root, appMain, 0, true);
    assert(!strcmp(registeredPath, bundle));
    join(candidate, alias, "Applications/Contacts.app/Contacts");
    expect("1", alias, candidate, 0, true);
    makeAppInfo(appInfo, CFSTR("Contacts"), CFSTR("APPL"), kCFPropertyListBinaryFormat_v1_0);
    expect("1", root, appMain, 0, true);

    // An app's other binaries and nested app/XPC services cannot inherit its compiler registration.
    join(candidate, app, "Helper"); makeFile(candidate);
    expect("1", root, candidate, 0, false);
    char nested[PATH_MAX];
    join(nested, app, "PlugIns/Widget.app"); makeDirectory(nested);
    join(candidate, nested, "Widget"); makeFile(candidate);
    expect("1", root, candidate, 0, false);
    const char *invalidLocations[] = {"Applications-other/Fake.app", "Applications/Fake.app-other", "Applications/Fake",
                                    "Applications/Folder/Fake.app", "System/Library/CoreServices-other/Fake.app",
                                    "System/Library/PrivateFrameworks/Fake.app", "usr/libexec/Fake.app"};
    for (unsigned i = 0; i < sizeof(invalidLocations) / sizeof(*invalidLocations); ++i) {
        join(nested, root, invalidLocations[i]); makeDirectory(nested);
        join(candidate, nested, "Fake"); makeFile(candidate);
        expect("1", root, candidate, 0, false);
    }

    // Main executables need a valid, bounded APPL Info.plist, including binary plists; a filename convention is insufficient.
    makeAppInfo(appInfo, CFSTR("Helper"), CFSTR("APPL"), kCFPropertyListXMLFormat_v1_0);
    expect("1", root, appMain, 0, false);
    makeAppInfo(appInfo, CFSTR("Contacts"), CFSTR("XPC!"), kCFPropertyListXMLFormat_v1_0);
    expect("1", root, appMain, EINVAL, false);
    makeAppInfo(appInfo, CFSTR("../Contacts"), CFSTR("APPL"), kCFPropertyListXMLFormat_v1_0);
    expect("1", root, appMain, EINVAL, false);
    makeFile(appInfo); expect("1", root, appMain, EINVAL, false);
    assert(!truncate(appInfo, 0)); expect("1", root, appMain, EINVAL, false);
    assert(!truncate(appInfo, 1024 * 1024 + 1)); expect("1", root, appMain, EINVAL, false);
    assert(!unlink(appInfo)); expect("1", root, appMain, ENOENT, false);
    assert(!symlink(info, appInfo)); expect("1", root, appMain, ELOOP, false);
    assert(!unlink(appInfo) && !mkfifo(appInfo, 0600)); expect("1", root, appMain, EINVAL, false);
    assert(!unlink(appInfo) && !mkdir(appInfo, 0700)); expect("1", root, appMain, EINVAL, false);
    assert(!rmdir(appInfo));
    makeAppInfo(appInfo, CFSTR("Contacts"), CFSTR("APPL"), kCFPropertyListXMLFormat_v1_0);
    assert(!unlink(appMain) && !mkdir(appMain, 0700)); expect("1", root, appMain, EINVAL, false);
    assert(!rmdir(appMain)); makeFile(appMain);

    // Canonical app targets cannot escape the runtime or be selected only by a neighboring root's prefix.
    assert(!unlink(appMain) && !symlink(outsideFile, appMain));
    expect("1", root, appMain, 0, false);
    assert(!unlink(appMain)); makeFile(appMain);
    join(nested, outside, "Applications/Contacts.app"); makeDirectory(nested);
    join(candidate, nested, "Contacts"); makeFile(candidate);
    expect("1", root, candidate, 0, false);

    char coreApp[PATH_MAX], coreMain[PATH_MAX], coreInfo[PATH_MAX];
    join(coreApp, root, "System/Library/CoreServices/Example.app"); makeDirectory(coreApp);
    join(coreMain, coreApp, "ActualMain"); makeFile(coreMain);
    join(coreInfo, coreApp, "Info.plist");
    makeAppInfo(coreInfo, CFSTR("ActualMain"), CFSTR("APPL"), kCFPropertyListBinaryFormat_v1_0);
    expect("1", root, coreMain, 0, true);
    join(candidate, coreApp, "Example"); makeFile(candidate);
    expect("1", root, candidate, 0, false);

    join(nested, root, "System/Applications/Image Playground.app"); makeDirectory(nested);
    join(candidate, nested, "Image Playground"); makeFile(candidate);
    char systemAppInfo[PATH_MAX];
    join(systemAppInfo, nested, "Info.plist");
    makeAppInfo(systemAppInfo, CFSTR("Image Playground"), CFSTR("APPL"), kCFPropertyListBinaryFormat_v1_0);
    expect("1", root, candidate, 0, true);
    const UniChar nulName[] = {'C', 'o', 'n', 't', 'a', 'c', 't', 's', 0, 'x'};
    CFStringRef embeddedNUL = CFStringCreateWithCharacters(NULL, nulName, sizeof(nulName) / sizeof(*nulName));
    makeAppInfo(appInfo, embeddedNUL, CFSTR("APPL"), kCFPropertyListBinaryFormat_v1_0);
    CFRelease(embeddedNUL);
    expect("1", root, appMain, 0, false);
    makeAppInfo(appInfo, CFSTR("Contacts"), CFSTR("APPL"), kCFPropertyListXMLFormat_v1_0);

    // The newly eligible app roles retain exactly the same compiler bundle and file validation.
    assert(!rename(bundle, moved) && !symlink(moved, bundle));
    expect("1", root, appMain, EPERM, false);
    expect("1", root, coreMain, EPERM, false);
    assert(!unlink(bundle) && !rename(moved, bundle));
    assert(!unlink(compiler)); expect("1", root, appMain, ENOENT, false);
    makeFile(compiler);
    expect("1", root, appMain, 0, true);
    assert(registrations == 18);
    removeTree(canonical);
    puts("MetalCompilerRegistration: opt-in, canonical roles/app mains, contained bundle, regular files and exact registration passed");
    return 0;
}
