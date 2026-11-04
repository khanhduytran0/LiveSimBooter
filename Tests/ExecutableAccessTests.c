#include "../Guest/libLiveSimSpawn/ExecutableAccess.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <limits.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int nativeResult, nativeError, remainingResult, remainingError;
static unsigned accessCalls, atCalls, openCalls, remainingCalls, getenvCalls, realpathCalls, statCalls;
static int lastMode, lastDirectory, lastFlags;
static uid_t realUser, effectiveUser;
static gid_t realGroup, effectiveGroup, supplementary[NGROUPS_MAX];
static int supplementaryCount;
static bool denyOpen;
static char lastPath[PATH_MAX];

static int nativeCheck(const char *path, int mode) {
    if (path) snprintf(lastPath, sizeof(lastPath), "%s", path);
    else lastPath[0] = 0;
    lastMode = mode;
    if (mode & X_OK) { errno = nativeError; return nativeResult; }
    ++remainingCalls;
    errno = remainingError;
    return remainingResult;
}

static int testAccess(const char *path, int mode) { ++accessCalls; return nativeCheck(path, mode); }
static int testFAccessAt(int directory, const char *path, int mode, int flags) {
    ++atCalls;
    lastDirectory = directory;
    lastFlags = flags;
    return nativeCheck(path, mode);
}
static int testOpen(const char *path, int flags) {
    ++openCalls;
    if (denyOpen) { errno = EACCES; return -1; }
    return open(path, flags);
}
static int testFstat(int descriptor, struct stat *metadata) {
    int result = fstat(descriptor, metadata);
    if (!result) { metadata->st_uid = 501; metadata->st_gid = 20; }
    return result;
}
static uid_t testGetuid(void) { return realUser; }
static uid_t testGeteuid(void) { return effectiveUser; }
static gid_t testGetgid(void) { return realGroup; }
static gid_t testGetegid(void) { return effectiveGroup; }
static int testGetgroups(int capacity, gid_t *groups) {
    if (supplementaryCount < 0) { errno = EIO; return -1; }
    assert(capacity >= supplementaryCount);
    memcpy(groups, supplementary, (size_t)supplementaryCount * sizeof(*groups));
    return supplementaryCount;
}
static char *testGetenv(const char *name) { ++getenvCalls; return getenv(name); }
static char *testRealpath(const char *path, char *resolved) { ++realpathCalls; return realpath(path, resolved); }
static int testStat(const char *path, struct stat *metadata) { ++statCalls; return stat(path, metadata); }

#define LSB_EXECUTABLE_ACCESS_TESTING
#define access testAccess
#define faccessat testFAccessAt
#define open testOpen
#define fstat testFstat
#define getuid testGetuid
#define geteuid testGeteuid
#define getgid testGetgid
#define getegid testGetegid
#define getgroups testGetgroups
#define getenv testGetenv
#define realpath testRealpath
#define stat(path, metadata) testStat(path, metadata)
#include "../Guest/libLiveSimSpawn/RuntimeDirectories.c"
#include "../Guest/libLiveSimSpawn/ExecutableAccess.c"
#undef stat
#undef realpath
#undef getenv
#undef access
#undef faccessat
#undef open
#undef fstat
#undef getuid
#undef geteuid
#undef getgid
#undef getegid
#undef getgroups

static void reset(void) {
    nativeResult = -1; nativeError = EPERM;
    remainingResult = 0; remainingError = ENOTTY;
    accessCalls = atCalls = openCalls = remainingCalls = getenvCalls = realpathCalls = statCalls = 0;
    realUser = effectiveUser = 501;
    realGroup = effectiveGroup = 20;
    supplementaryCount = 0;
    denyOpen = false;
    errno = EDOM;
}

static void writeImage(const char *path, const struct mach_header_64 *header) {
    int descriptor = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0700);
    assert(descriptor >= 0);
    assert(write(descriptor, header, sizeof(*header)) == sizeof(*header));
    struct load_command command = {.cmd = LC_MAIN, .cmdsize = sizeof(command)};
    assert(write(descriptor, &command, sizeof(command)) == sizeof(command));
    assert(!close(descriptor) && !chmod(path, 0755));
}

static void expectDenied(const char *path) {
    errno = ERANGE;
    assert(LSBAccess(path, X_OK) == -1 && errno == EPERM);
}

static void testNativeBehavior(const char *path) {
    reset();
    nativeResult = 0; nativeError = ENOTTY;
    assert(!LSBAccess("/outside/native-success", X_OK) && errno == ENOTTY && accessCalls == 1 && !openCalls);
    assert(!getenvCalls && !realpathCalls && !statCalls);
    const int errors[] = {EACCES, ENOENT, ENOTDIR, EINVAL, EFAULT};
    for (unsigned index = 0; index < sizeof(errors) / sizeof(*errors); ++index) {
        reset(); nativeError = errors[index];
        assert(LSBAccess(path, X_OK) == -1 && errno == errors[index] && accessCalls == 1 && !openCalls);
    }
    reset(); remainingResult = -1; remainingError = EPERM;
    assert(LSBAccess(path, R_OK | W_OK) == -1 && errno == EPERM && accessCalls == 1 && !openCalls);
    reset();
    assert(LSBAccess(path, X_OK | 0x1000) == -1 && errno == EPERM && accessCalls == 1 && !openCalls);
    reset();
    assert(LSBAccess(NULL, X_OK) == -1 && errno == EPERM && accessCalls == 1 && !openCalls);
    reset();
    assert(!LSBAccess(path, X_OK | R_OK | W_OK) && errno == EDOM && accessCalls == 2 && remainingCalls == 1);
    assert(lastMode == (R_OK | W_OK) && !strcmp(lastPath, path));
    reset(); remainingResult = -1; remainingError = EROFS;
    assert(LSBAccess(path, X_OK | W_OK) == -1 && errno == EPERM && accessCalls == 2 && !openCalls);
    reset(); denyOpen = true;
    expectDenied(path);
    assert(openCalls == 1);
}

static void testModes(const char *path) {
    reset(); assert(!chmod(path, 0411)); expectDenied(path); // Owner permissions take precedence over group/other.
    reset(); assert(!chmod(path, 0500)); assert(!LSBAccess(path, X_OK));
    reset(); realUser = 502; assert(!chmod(path, 0410)); assert(!LSBAccess(path, X_OK));
    reset(); realUser = 502; assert(!chmod(path, 0401)); expectDenied(path); // Group selection takes precedence over other.
    reset(); realUser = 502; realGroup = 21; supplementary[0] = 20; supplementaryCount = 1;
    assert(!chmod(path, 0410)); assert(!LSBAccess(path, X_OK));
    reset(); realUser = 502; realGroup = 21; supplementary[0] = 20; supplementaryCount = 1;
    assert(!chmod(path, 0401)); expectDenied(path);
    reset(); realUser = 502; realGroup = 21;
    assert(!chmod(path, 0401)); assert(!LSBAccess(path, X_OK));
    reset(); realUser = 502; realGroup = 21; supplementaryCount = -1; expectDenied(path);
    reset(); realUser = 0; assert(!chmod(path, 0401)); assert(!LSBAccess(path, X_OK));
    assert(!chmod(path, 0400)); expectDenied(path);
    assert(!chmod(path, 0755));
}

static void testAt(const char *root, const char *path) {
    reset(); effectiveUser = 502; effectiveGroup = 21;
    assert(!chmod(path, 0500));
    assert(!LSBFAccessAt(AT_FDCWD, path, X_OK, 0) && errno == EDOM);
    assert(atCalls == 2 && !accessCalls && lastFlags == 0 && lastMode == R_OK);
    reset(); effectiveUser = 502; effectiveGroup = 21;
    assert(LSBFAccessAt(AT_FDCWD, path, X_OK, AT_EACCESS) == -1 && errno == EPERM);
    reset(); realUser = 502; realGroup = 21;
    assert(!LSBFAccessAt(AT_FDCWD, path, X_OK, AT_EACCESS) && errno == EDOM && lastFlags == AT_EACCESS);
    reset();
    assert(LSBFAccessAt(AT_FDCWD, path, X_OK, AT_SYMLINK_NOFOLLOW) == -1 && errno == EPERM && atCalls == 1);
    reset();
    assert(LSBFAccessAt(AT_FDCWD, path, X_OK, 0x40000000) == -1 && errno == EPERM && atCalls == 1);
    reset();
    assert(!LSBFAccessAt(-123, path, X_OK, 0) && errno == EDOM && lastDirectory == -123); // Absolute paths ignore dirfd.
    assert(!chdir(root));
    reset(); assert(!LSBAccess("bin/df", X_OK) && errno == EDOM);
    reset(); assert(!LSBFAccessAt(AT_FDCWD, "bin/df", X_OK, 0) && errno == EDOM);
    reset(); assert(LSBFAccessAt(3, "bin/df", X_OK, 0) == -1 && errno == EPERM && atCalls == 1 && !openCalls);
    reset(); nativeResult = 0; nativeError = ENOTTY;
    assert(!LSBFAccessAt(3, "bin/df", X_OK, AT_SYMLINK_NOFOLLOW) && errno == ENOTTY && atCalls == 1);
    reset(); remainingResult = -1; remainingError = EACCES;
    assert(LSBFAccessAt(AT_FDCWD, "bin/df", X_OK | W_OK, AT_EACCESS) == -1 && errno == EPERM && lastMode == (R_OK | W_OK));
    assert(!chmod(path, 0755));
}

static int configFragment(const struct dirent *entry) { return entry->d_name[0] == '0'; }

static void testDpkgConfig(const char *jb, const char *outside) {
    char etc[PATH_MAX], config[PATH_MAX], parts[PATH_MAX], path[PATH_MAX];
    snprintf(etc, sizeof(etc), "%s/etc", jb);
    snprintf(config, sizeof(config), "%s/dpkg", etc);
    snprintf(parts, sizeof(parts), "%s/dpkg.cfg.d", config);
    assert(!mkdir(etc, 0700) && !mkdir(config, 0700) && !mkdir(parts, 0700));
    const char *fragments[] = {"02-test", "01-test", ".ignored"};
    for (unsigned i = 0; i < 3; ++i) {
        snprintf(path, sizeof(path), "%s/%s", parts, fragments[i]);
        FILE *f = fopen(path, "w");
        assert(f && fputs(fragments[i], f) >= 0 && !fclose(f));
    }
    struct dirent **entries = NULL;
    reset();
    assert(LSBDpkgConfigScandir("/var/jb/etc/dpkg/dpkg.cfg.d", &entries, configFragment, alphasort) == 2);
    assert(!strcmp(entries[0]->d_name, fragments[1]) && !strcmp(entries[1]->d_name, fragments[0]));
    free(entries[0]); free(entries[1]); free(entries);
    assert(!getenvCalls && realpathCalls == 1);

    const char *fragment = "/private/var/jb/etc/dpkg/dpkg.cfg.d/01-test";
    FILE *f = LSBDpkgConfigFopen(fragment, "r");
    char contents[20];
    assert(f && fgets(contents, sizeof(contents), f) && !strcmp(contents, fragments[1]) && !fclose(f));
    assert(!LSBDpkgConfigFopen("/var/jb/etc/dpkg/dpkg.cfg", "r") && errno == ENOENT);
    snprintf(path, sizeof(path), "%s/escape", parts);
    assert(!symlink(outside, path));
    assert(!LSBDpkgConfigFopen("/var/jb/etc/dpkg/dpkg.cfg.d/escape", "r") && errno == EACCES);
    snprintf(path, sizeof(path), "%s/outside.cfg.d", config);
    assert(!symlink(outside, path));
    assert(LSBDpkgConfigScandir("/var/jb/etc/dpkg/outside.cfg.d", &entries, NULL, NULL) == -1 && errno == EACCES);

    reset();
    assert(LSBDpkgConfigScandir("unrelated-missing-dir", &entries, NULL, NULL) == -1 && errno == ENOENT);
    assert(!LSBDpkgConfigFopen("unrelated-missing-file", "r") && errno == ENOENT);
    assert(!LSBDpkgConfigFopen("/var/jb/etc/dpkg-other/no-such-file", "r"));
    assert(!realpathCalls && !getenvCalls);
}

static void testBootstrap(const char *base, const char *data, const struct mach_header_64 *header) {
    char var[PATH_MAX], jb[PATH_MAX], bin[PATH_MAX], image[PATH_MAX], link[PATH_MAX], systemImage[PATH_MAX], expected[PATH_MAX];
    snprintf(var, sizeof(var), "%s/var", data);
    snprintf(jb, sizeof(jb), "%s/jb", var);
    snprintf(bin, sizeof(bin), "%s/bin", jb);
    snprintf(image, sizeof(image), "%s/tool", bin);
    snprintf(link, sizeof(link), "%s/link", bin);
    snprintf(systemImage, sizeof(systemImage), "%s/RuntimeRoot/bin/other", base);
    // Installing the optional bootstrap while the guest is running must make it immediately visible.
    reset(); assert(LSBAccess("/var/jb/bin/tool", F_OK) == -1 && errno == ENOENT && !accessCalls);
    assert(!mkdir(var, 0700) && !mkdir(jb, 0700) && !mkdir(bin, 0700));
    writeImage(image, header); writeImage(systemImage, header);
    assert(realpath(image, expected) && !symlink("tool", link));
    reset(); assert(!LSBAccess("/var/jb/bin/link", X_OK) && errno == EDOM && !strcmp(lastPath, expected));
    assert(!getenvCalls && realpathCalls <= 1 && !statCalls);
    reset(); assert(!LSBFAccessAt(-123, "/private/var/jb/bin/tool", X_OK, AT_EACCESS) && errno == EDOM && !strcmp(lastPath, expected));
    reset(); assert(!LSBAccess(image, X_OK));
    reset(); assert(!LSBFAccessAt(AT_FDCWD, image, X_OK, AT_EACCESS));

    reset(); nativeResult = 0;
    assert(LSBAccess("/var/jb/bin/missing", X_OK) == -1 && errno == ENOENT && !accessCalls);
    assert(LSBAccess("/var/jb/bin/../bin/tool", X_OK) == -1 && errno == EINVAL && !accessCalls);
    assert(!LSBAccess("/var/jb-other/tool", X_OK) && !strcmp(lastPath, "/var/jb-other/tool"));
    assert(!unlink(link) && !symlink(systemImage, link));
    reset(); assert(LSBAccess("/var/jb/bin/link", R_OK) == -1 && errno == EACCES && !accessCalls);
    reset(); expectDenied(link); assert(!openCalls);
    // Checking the link itself keeps the real no-follow target, even when its referent is elsewhere or absent.
    reset(); assert(!LSBFAccessAt(AT_FDCWD, "/var/jb/bin/link", F_OK, AT_SYMLINK_NOFOLLOW) && lastFlags == AT_SYMLINK_NOFOLLOW);
    assert(realpath(bin, expected) && strlcat(expected, "/link", sizeof(expected)) < sizeof(expected) && !strcmp(expected, lastPath));
    assert(!unlink(link) && !symlink("../../../..", link)); // The link now leads to the directory containing RuntimeRoot and data.
    reset(); assert(LSBFAccessAt(AT_FDCWD, "/var/jb/bin/link/RuntimeRoot", F_OK, AT_SYMLINK_NOFOLLOW) == -1 && errno == EACCES && !atCalls);
    char sibling[PATH_MAX];
    snprintf(sibling, sizeof(sibling), "%s-extra", jb);
    assert(!mkdir(sibling, 0700) && !unlink(link) && !symlink(sibling, link));
    reset(); assert(LSBAccess("/private/var/jb/bin/link", F_OK) == -1 && errno == EACCES && !accessCalls);

    struct mach_header_64 wrong = *header;
    wrong.cpusubtype = CPU_SUBTYPE_ARM64E;
    writeImage(image, &wrong); reset(); expectDenied("/var/jb/bin/tool");
    atomic_store_explicit(&directoriesReady, false, memory_order_release);
    reset(); assert(LSBAccess("/var/jb/bin/tool", X_OK) == -1 && errno == EACCES && !accessCalls);
    assert(!getenvCalls && !realpathCalls && !statCalls);
    atomic_store_explicit(&directoriesReady, true, memory_order_release);
    testDpkgConfig(jb, systemImage);
    assert(!unlink(systemImage));
}

static void testInstalledApplications(const char *base, const struct mach_header_64 *header) {
    char data[PATH_MAX], app[PATH_MAX], path[PATH_MAX], parent[PATH_MAX];
    assert(snprintf(data, sizeof(data), "%s/data", base) > 0);
    const char *locations[] = {"Containers/Bundle/Application/id/Example.app/Example",
        "Containers/Bundle/Application/id/Example.app/PlugIns/Widget.appex/Widget", "Containers/Data/Application/id/Example.app/Example",
        "Containers/Bundle/Application-other/id/Example.app/Example", "Containers/Bundle/Application/id/Example.app-other/Example", "staging/Example.app/Example"};
    for (unsigned i = 0; i < sizeof(locations) / sizeof(*locations); ++i) {
        assert(snprintf(path, sizeof(path), "%s/%s", data, locations[i]) > 0);
        strcpy(parent, path); *strrchr(parent, '/') = 0;
        for (char *part = parent + 1; *part; ++part) if (*part == '/') {
            *part = 0; assert(!mkdir(parent, 0700) || errno == EEXIST); *part = '/';
        }
        assert(!mkdir(parent, 0700) || errno == EEXIST);
        writeImage(path, header); reset();
        if (i < 2) {
            assert(!LSBAccess(path, X_OK) && errno == EDOM);
            reset(); assert(!LSBFAccessAt(AT_FDCWD, path, X_OK, AT_EACCESS) && errno == EDOM);
        } else { expectDenied(path); assert(!openCalls); }
    }
    assert(snprintf(app, sizeof(app), "%s/Containers/Bundle/Application/id/Example.app", data) > 0);
    assert(snprintf(path, sizeof(path), "%s/Example", app) > 0);
    testNativeBehavior(path); testModes(path);
    assert(!unlink(path) && !symlink("/usr/bin/false", path)); reset(); expectDenied(path); assert(!openCalls);
    assert(!unlink(path)); writeImage(path, header);
    struct mach_header_64 invalid = *header; invalid.filetype = MH_DYLIB;
    writeImage(path, &invalid); reset(); expectDenied(path);
    testBootstrap(base, data, header);
    char *paths[] = {data, NULL}; FTS *tree = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL); assert(tree);
    FTSENT *entry;
    while ((entry = fts_read(tree))) {
        if (entry->fts_info == FTS_D) continue;
        assert(!(entry->fts_info == FTS_DP ? rmdir(entry->fts_accpath) : unlink(entry->fts_accpath)));
    }
    assert(!fts_close(tree));
}

int main(void) {
    char previousDirectory[PATH_MAX];
    assert(getcwd(previousDirectory, sizeof(previousDirectory)));
    char directory[] = "/tmp/LiveSimBooter-access.XXXXXX";
    assert(mkdtemp(directory));
    char root[PATH_MAX], bin[PATH_MAX], path[PATH_MAX], outside[PATH_MAX], escape[PATH_MAX], rootLink[PATH_MAX], insideLink[PATH_MAX];
    snprintf(root, sizeof(root), "%s/RuntimeRoot", directory);
    snprintf(bin, sizeof(bin), "%s/bin", root);
    snprintf(path, sizeof(path), "%s/df", bin);
    snprintf(outside, sizeof(outside), "%s/RuntimeRoot-other", directory);
    snprintf(escape, sizeof(escape), "%s/escape", root);
    snprintf(rootLink, sizeof(rootLink), "%s/root-link", directory);
    snprintf(insideLink, sizeof(insideLink), "%s/inside-link", root);
    assert(!mkdir(root, 0700) && !mkdir(bin, 0700));
    struct mach_header_64 header = {.magic = MH_MAGIC_64, .cputype = CPU_TYPE_ARM64, .cpusubtype = CPU_SUBTYPE_ARM64_ALL,
        .filetype = MH_EXECUTE, .ncmds = 1, .sizeofcmds = sizeof(struct load_command), .flags = MH_PIE};
    writeImage(path, &header); writeImage(outside, &header);
    assert(!symlink(outside, escape) && !symlink(root, rootLink) && !symlink(path, insideLink));
    assert(!setenv("DYLD_ROOT_PATH", root, 1));
    LSBResetExecutableAccessForTesting();
    // The guest captures its namespace before its first lookup, even when its application later changes environ.
    assert(!unsetenv("DYLD_ROOT_PATH"));
    reset(); assert(!LSBAccess(path, X_OK) && errno == EDOM);
    assert(!getenvCalls);
    reset(); expectDenied(outside); expectDenied(root); expectDenied(escape);
    assert(accessCalls == 3 && !remainingCalls && !openCalls);
    reset(); assert(!LSBAccess(insideLink, X_OK));
    assert(!getenvCalls && realpathCalls <= 1 && !statCalls);
    assert(!setenv("DYLD_ROOT_PATH", rootLink, 1));
    LSBResetExecutableAccessForTesting();
    reset(); assert(!LSBAccess(path, X_OK));
    assert(!unsetenv("DYLD_ROOT_PATH"));
    LSBResetExecutableAccessForTesting();
    reset(); expectDenied(path);
    assert(!setenv("DYLD_ROOT_PATH", "/", 1));
    LSBResetExecutableAccessForTesting();
    reset(); expectDenied(path);
    assert(!setenv("DYLD_ROOT_PATH", root, 1));
    LSBResetExecutableAccessForTesting();
    testNativeBehavior(path); testModes(path); testAt(root, path);
    for (unsigned variant = 0; variant < 8; ++variant) {
        struct mach_header_64 bad = header;
        switch (variant) {
            case 0: bad.magic = MH_CIGAM_64; break;
            case 1: bad.cputype = CPU_TYPE_X86_64; break;
            case 2: bad.cpusubtype = CPU_SUBTYPE_ARM64E; break;
            case 3: bad.filetype = MH_DYLIB; break;
            case 4: bad.flags = 0; break;
            case 5: bad.ncmds = 0; break;
            case 6: bad.ncmds = 2; break;
            case 7: bad.sizeofcmds = UINT32_MAX; break;
        }
        writeImage(path, &bad); reset(); expectDenied(path);
    }
    assert(!truncate(path, 4)); reset(); expectDenied(path);
    assert(!chdir(previousDirectory));
    testInstalledApplications(directory, &header);
    assert(!unlink(insideLink) && !unlink(rootLink) && !unlink(escape) && !unlink(outside) && !unlink(path));
    assert(!rmdir(bin) && !rmdir(root) && !rmdir(directory));
    puts("ExecutableAccess: native forwarding, EPERM confinement, readable Mach-O roles, identities/groups, faccessat modes, and errno passed");
    return 0;
}
