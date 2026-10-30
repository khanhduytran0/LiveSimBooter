#include "../Guest/ExtensionKitRegistration.h"

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

#define LSB_EXTENSION_KIT_REGISTRATION_TESTING
#define xpc_add_bundle recordRegistration
#include "../Guest/ExtensionKitRegistration.c"
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
    assert(LSBRegisterExtensionKitForExecutable(enabled, root, candidate, &requested) == error);
    assert(requested == shouldRequest && registrations == previous + shouldRequest);
    if (requested) assert(registeredFlags == 2);
}

int main(void) {
    char temporary[] = "/tmp/LiveSimBooter-extensionkit.XXXXXX", canonical[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    char root[PATH_MAX], directory[PATH_MAX], executable[PATH_MAX], bundle[PATH_MAX], info[PATH_MAX], service[PATH_MAX];
    join(root, canonical, "RuntimeRoot");
    join(directory, root, "System/Library/CoreServices/SpringBoard.app"); makeDirectory(directory);
    join(executable, directory, "SpringBoard"); makeFile(executable);
    join(bundle, root, "System/Library/Frameworks/ExtensionFoundation.framework/XPCServices/extensionkitservice.xpc"); makeDirectory(bundle);
    join(info, bundle, "Info.plist"); makeFile(info);
    join(service, bundle, "extensionkitservice"); makeFile(service);

    // Opt-in is exact and does not inspect paths when disabled.
    const char *disabled[] = {NULL, "", "0", "true", "01", "1 "};
    for (unsigned i = 0; i < sizeof(disabled) / sizeof(*disabled); ++i) expect(disabled[i], NULL, NULL, 0, false);
    expect("1", NULL, executable, EINVAL, false);
    expect("1", "/", executable, EINVAL, false);
    expect("1", "RuntimeRoot", executable, EINVAL, false);
    expect("1", root, "System/Library/CoreServices/SpringBoard.app/SpringBoard", EINVAL, false);
    assert(LSBRegisterExtensionKitForExecutable("1", root, executable, NULL) == EINVAL && !registrations);

    expect("1", root, executable, 0, true);
    assert(!strcmp(registeredPath, bundle));
    char alias[PATH_MAX], candidate[PATH_MAX];
    join(alias, canonical, "runtime-alias"); assert(!symlink(root, alias));
    join(candidate, alias, "System/Library/CoreServices/SpringBoard.app/../SpringBoard.app/SpringBoard");
    expect("1", alias, candidate, 0, true);
    assert(!strcmp(registeredPath, bundle));

    // PosterBoard discovers system poster providers through the same per-process ExtensionKit service.
    char posterDirectory[PATH_MAX], poster[PATH_MAX];
    join(posterDirectory, root, "Applications/PosterBoard.app"); makeDirectory(posterDirectory);
    join(poster, posterDirectory, "PosterBoard"); makeFile(poster);
    expect("1", root, poster, 0, true);
    join(candidate, alias, "Applications/PosterBoard.app/../PosterBoard.app/PosterBoard");
    expect("1", alias, candidate, 0, true);
    join(candidate, posterDirectory, "PosterBoardOther"); makeFile(candidate);
    expect("1", root, candidate, 0, false);

    // Chronod's widget providers need Launch in its own PID domain; this remains an exact executable opt-in.
    char chronoDirectory[PATH_MAX], chrono[PATH_MAX];
    join(chronoDirectory, root, "System/Library/PrivateFrameworks/ChronoCore.framework/Support"); makeDirectory(chronoDirectory);
    join(chrono, chronoDirectory, "chronod"); makeFile(chrono);
    for (unsigned i = 0; i < sizeof(disabled) / sizeof(*disabled); ++i) expect(disabled[i], root, chrono, 0, false);
    expect("1", root, chrono, 0, true);
    assert(!strcmp(registeredPath, bundle));
    join(candidate, alias, "System/Library/PrivateFrameworks/ChronoCore.framework/Support/../Support/chronod");
    expect("1", alias, candidate, 0, true);
    assert(!strcmp(registeredPath, bundle));
    const char *chronoNearMatches[] = {"chronod-helper", "chrono", "chronod.other"};
    for (unsigned i = 0; i < sizeof(chronoNearMatches) / sizeof(*chronoNearMatches); ++i) {
        join(candidate, chronoDirectory, chronoNearMatches[i]); makeFile(candidate);
        expect("1", root, candidate, 0, false);
    }
    join(candidate, directory, "chronod"); makeFile(candidate);
    expect("1", root, candidate, 0, false);

    // Inherited options never register the service, proxy, launchd, unrelated, or merely prefix-matching executables.
    expect("1", root, service, 0, false);
    const char *otherRoles[] = {"launchd_sim", "xpcproxy_sim", "backboardd", "SpringBoard-other", "df"};
    for (unsigned i = 0; i < sizeof(otherRoles) / sizeof(*otherRoles); ++i) {
        join(candidate, directory, otherRoles[i]); makeFile(candidate);
        expect("1", root, candidate, 0, false);
    }
    char outside[PATH_MAX], outsideFile[PATH_MAX];
    join(outside, canonical, "RuntimeRoot-other"); makeDirectory(outside);
    join(outsideFile, outside, "SpringBoard"); makeFile(outsideFile);
    expect("1", root, outsideFile, 0, false);
    assert(!unlink(chrono) && !symlink(outsideFile, chrono));
    expect("1", root, chrono, 0, false);
    assert(!unlink(chrono)); makeFile(chrono);
    assert(!unlink(poster) && !symlink(outsideFile, poster));
    expect("1", root, poster, 0, false);
    assert(!unlink(poster)); makeFile(poster);
    assert(!unlink(executable) && !symlink(outsideFile, executable));
    expect("1", root, executable, 0, false);
    assert(!unlink(executable)); makeFile(executable);

    // A bundle symlink escaping the canonical root is rejected even if its target contains plausible files.
    char moved[PATH_MAX];
    join(moved, outside, "extensionkitservice.xpc");
    assert(!rename(bundle, moved) && !symlink(moved, bundle));
    expect("1", root, executable, EPERM, false);
    assert(!unlink(bundle));
    expect("1", root, executable, ENOENT, false);
    assert(!rename(moved, bundle));

    // Required files are regular, readable and nonempty; a final symlink or special file is not followed or blocked on.
    assert(!unlink(info)); expect("1", root, executable, ENOENT, false);
    assert(!symlink(outsideFile, info)); expect("1", root, executable, ELOOP, false);
    assert(!unlink(info) && !mkdir(info, 0700)); expect("1", root, executable, EINVAL, false);
    assert(!rmdir(info) && !mkfifo(info, 0600)); expect("1", root, executable, EINVAL, false);
    assert(!unlink(info)); makeFile(info);
    assert(!truncate(service, 0)); expect("1", root, executable, EINVAL, false);
    makeFile(service);
    assert(!unlink(service)); expect("1", root, executable, ENOENT, false);
    assert(!symlink(outsideFile, service)); expect("1", root, executable, ELOOP, false);
    assert(!unlink(service)); makeFile(service);
    expect("1", root, executable, 0, true);
    assert(registrations == 7);
    removeTree(canonical);
    puts("ExtensionKitRegistration: opt-in, canonical executable roles, contained bundle, regular files and exact registration passed");
    return 0;
}
