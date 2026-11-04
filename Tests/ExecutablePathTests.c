#include "../LiveProcess/ExecutablePath.h"
#include "../ProcessSupport/GuestExecutablePath.h"
#include "../ProcessSupport/ForkPolicy.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void join(char output[PATH_MAX], const char *directory, const char *name) {
    int length = snprintf(output, PATH_MAX, "%s/%s", directory, name);
    assert(length > 0 && length < PATH_MAX);
}

static void makeFile(const char *path) {
    int file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(file >= 0 && !close(file));
}

static void resolves(const char *root, const char *path, const char *expected) {
    char result[PATH_MAX];
    assert(!LSBResolveGuestExecutable(root, path, result) && !strcmp(result, expected));
}

static void fails(const char *root, const char *path, int expected) {
    char result[PATH_MAX] = "unchanged";
    int error = LSBResolveGuestExecutable(root, path, result);
    assert((expected ? error == expected : error != 0) && !strcmp(result, "unchanged"));
}

static void makeDirectories(const char *path) {
    char copy[PATH_MAX]; strcpy(copy, path);
    for (char *part = copy + 1; *part; ++part) if (*part == '/') {
        *part = 0; assert(!mkdir(copy, 0700) || errno == EEXIST); *part = '/';
    }
    assert(!mkdir(copy, 0700) || errno == EEXIST);
}

static void testVirtualBootstrapPath(void) {
    static const struct { const char *path; int offset; } cases[] = {
        {"/var/jb", 0}, {"/var/jb/", 0}, {"/var/jb/usr/bin/dpkg", 0},
        {"/private/var/jb", 8}, {"/private/var/jb/", 8}, {"/private/var/jb/usr/bin/dpkg", 8},
        {"/var/jbx", -1}, {"/private/var/jb-other", -1}, {"/private//var/jb", -1}, {"//var/jb", -1},
        {"var/jb", -1}, {"/usr/bin", -1}, {"/var/mobile", -1}, {"/private/var/mobile", -1}
    };
    long pageSize = sysconf(_SC_PAGESIZE);
    assert(pageSize > 0);
    char *pages = mmap(NULL, 2 * (size_t)pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(pages != MAP_FAILED && !mprotect(pages + pageSize, (size_t)pageSize, PROT_NONE));
    assert(LSBGuestVirtualBootstrapPath(NULL) == NULL);

    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        size_t length = strlen(cases[i].path);
        char *path = pages + pageSize - length - 1;
        memcpy(path, cases[i].path, length + 1);
        const char *expected = cases[i].offset < 0 ? NULL : path + cases[i].offset;
        assert(LSBGuestVirtualBootstrapPath(path) == expected);
    }
    // Put the NUL immediately before the guard for every shorter spelling, including the empty string.
    const char *roots[] = {"/var/jb", "/private/var/jb"};
    for (size_t i = 0; i < sizeof(roots) / sizeof(*roots); ++i) {
        for (size_t length = 0; length < strlen(roots[i]); ++length) {
            char *path = pages + pageSize - length - 1;
            memcpy(path, roots[i], length);
            path[length] = 0;
            assert(LSBGuestVirtualBootstrapPath(path) == NULL);
        }
    }
    assert(!munmap(pages, 2 * (size_t)pageSize));
}

static void testDataExecutables(const char *base, const char *root) {
    char data[PATH_MAX], app[PATH_MAX], image[PATH_MAX], path[PATH_MAX], output[PATH_MAX];
    join(data, base, "data");
    join(app, data, "Containers/Bundle/Application/11111111-1111-1111-1111-111111111111/Example.app");
    makeDirectories(app);
    join(image, app, "Example"); makeFile(image);
    resolves(root, image, image);
    const char *nested[] = {"Frameworks/Example.framework/Example", "PlugIns/Widget.appex/Widget", "XPCServices/Worker.xpc/Worker"};
    for (unsigned i = 0; i < sizeof(nested) / sizeof(*nested); ++i) {
        join(path, app, nested[i]);
        char parent[PATH_MAX]; strcpy(parent, path); *strrchr(parent, '/') = 0; makeDirectories(parent); makeFile(path);
        resolves(root, path, path);
    }
    char alias[PATH_MAX]; join(alias, data, "alias"); assert(!symlink(app, alias));
    join(path, alias, "Example"); resolves(root, path, image);
    int cwd = open(".", O_RDONLY | O_DIRECTORY);
    assert(cwd >= 0 && !chdir(app)); resolves(root, "Example", image); assert(!fchdir(cwd) && !close(cwd));
    join(path, app, "../Example.app/Example"); fails(root, path, EINVAL);
    join(path, app, "//Example"); fails(root, path, EINVAL);
    join(path, app, "missing"); fails(root, path, ENOENT);
    fails(root, app, EACCES);
    join(path, app, "Frameworks"); fails(root, path, EINVAL);
    const char *invalid[] = {"Containers/Data/Application/other/Bad.app/Bad", "Containers/Bundle/Application-other/id/Bad.app/Bad",
        "Containers/Bundle/Application/id/Bad.app-other/Bad", "Containers/Bundle/Application/id/Bad", "staging/Bad.app/Bad"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        join(path, data, invalid[i]);
        char parent[PATH_MAX]; strcpy(parent, path); *strrchr(parent, '/') = 0; makeDirectories(parent); makeFile(path);
        fails(root, path, EACCES);
    }
    join(path, app, "escape"); assert(!symlink("/usr/bin/false", path)); fails(root, path, EACCES);
    assert(LSBGuestInstalledApplicationPath(root, image) && !LSBGuestInstalledApplicationPath(root, "/x"));
    assert(!LSBGuestExecutablePhysicalPath(root, image, output, sizeof(output)) && !strcmp(output, image));
    assert(LSBGuestExecutablePhysicalPath(root, image, output, strlen(image)) == ENAMETOOLONG);
    join(path, data, "staging/Bad.app/Bad");
    assert(LSBGuestExecutablePhysicalPath(root, path, output, sizeof(output)) == EACCES);
    join(path, root, "bin/df");
    assert(!LSBGuestExecutablePhysicalPath(root, "/bin/df", output, sizeof(output)) && !strcmp(output, path));

    // Bootstrap executables keep their physical identity for ps and never acquire a simulator service's virtual path through a symlink.
    char jb[PATH_MAX], tool[PATH_MAX], runtimeFile[PATH_MAX];
    join(jb, data, "var/jb"); join(path, jb, "usr/bin"); makeDirectories(path);
    join(tool, path, "example"); makeFile(tool);
    resolves(root, tool, tool);
    resolves(root, "/var/jb/usr/bin/example", tool);
    resolves(root, "/private/var/jb/usr/bin/example", tool);
    join(path, root, "/var/jb/usr/bin/example"); resolves(root, path, tool); // launchd prefixes RuntimeRoot + "/"
    join(path, jb, "internal"); assert(!symlink("usr/bin/example", path)); resolves(root, "/var/jb/internal", tool);
    char alternatives[PATH_MAX], awk[PATH_MAX], choice[PATH_MAX];
    join(alternatives, jb, "etc/alternatives"); makeDirectories(alternatives);
    join(choice, alternatives, "awk"); assert(!symlink("/private/var/jb/usr/bin/example", choice));
    join(awk, jb, "usr/bin/awk"); assert(!symlink("/var/jb/etc/alternatives/awk", awk));
    resolves(root, awk, tool); resolves(root, "/var/jb/usr/bin/awk", tool);
    join(path, jb, "usr-alias"); assert(!symlink("/var/jb/usr", path)); resolves(root, "/private/var/jb/usr-alias/bin/awk", tool);
    assert(!unlink(choice) && !symlink("../../usr/bin/example", choice)); resolves(root, awk, tool);
    assert(!unlink(choice) && !symlink("../../../outside", choice)); fails(root, awk, EACCES);
    assert(!unlink(choice) && !symlink("/bin/df", choice)); fails(root, awk, EACCES);
    assert(!unlink(choice) && !symlink("/var/jb/usr/bin/awk", choice)); fails(root, awk, ELOOP);
    assert(!unlink(choice)); fails(root, awk, ENOENT);
    join(path, jb, "bad:name"); makeFile(path); fails(root, "/var/jb/bad:name", EACCES);
    join(runtimeFile, root, "bin/df");
    join(path, jb, "system"); assert(!symlink(runtimeFile, path)); fails(root, path, EACCES);
    fails(root, "/var/jb/system", EACCES);
    join(path, jb, "installed"); assert(!symlink(image, path)); fails(root, "/private/var/jb/installed", EACCES);
    join(path, jb, "native"); assert(!symlink("/usr/bin/false", path)); fails(root, path, EACCES);
    join(path, app, "bootstrap"); assert(!symlink(tool, path)); fails(root, path, EACCES);
    join(path, data, "var/jb-other/file"); join(alias, data, "var/jb-other"); makeDirectories(alias); makeFile(path);
    fails(root, path, EACCES);
    fails(root, "/var/jb/missing", ENOENT); fails(root, "/var/jb/../jb/usr/bin/example", EINVAL);
    fails(root, "/var/jb", EINVAL);
    assert(!LSBGuestExecutablePhysicalPath(root, tool, output, sizeof(output)) && !strcmp(output, tool));
    cwd = open(".", O_RDONLY | O_DIRECTORY); assert(cwd >= 0 && !chdir(jb));
    resolves(root, "internal", tool); fails(root, "system", EACCES); assert(!fchdir(cwd) && !close(cwd));

    char *paths[] = {data, NULL}; FTS *tree = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL); assert(tree);
    FTSENT *entry;
    while ((entry = fts_read(tree))) {
        if (entry->fts_info == FTS_D) continue;
        assert(!(entry->fts_info == FTS_DP ? rmdir(entry->fts_accpath) : unlink(entry->fts_accpath)));
    }
    assert(!fts_close(tree));
}

int main(void) {
    testVirtualBootstrapPath();
    const char *runtime = "/private/var/group/LSB/iOS/23A343/RuntimeRoot";
    const char *package = "/var/group/LSB/iOS/23A343/data/var/jb/usr/bin/example";
    assert(LSBUseForkDonorLayout(runtime, package, NULL));
    assert(!LSBUseForkDonorLayout(runtime, package, "0"));
    assert(!LSBUseForkDonorLayout(runtime, package, "early"));
    assert(!LSBUseForkDonorLayout(runtime, "/var/jb/usr/bin/example", NULL));
    assert(!LSBUseForkDonorLayout(runtime, "/private/var/group/LSB/iOS/23A343/data/var/jb-other/example", NULL));
    assert(!LSBUseForkDonorLayout(runtime, "/private/var/group/LSB/iOS/23A343/RuntimeRoot/bin/example", NULL));
    assert(LSBUseForkDonorLayout(runtime, "/private/var/group/LSB/iOS/23A343/RuntimeRoot/bin/example", "resume"));
    char temporary[] = "/tmp/LiveSimBooter-executable-path.XXXXXX", base[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, base));
    char root[PATH_MAX], bin[PATH_MAX], executable[PATH_MAX], alias[PATH_MAX], aliasedExecutable[PATH_MAX];
    join(root, base, "RuntimeRoot");
    join(bin, root, "bin");
    join(executable, bin, "df");
    join(alias, base, "root-alias");
    assert(!mkdir(root, 0700) && !mkdir(bin, 0700) && !symlink(root, alias));
    makeFile(executable);
    resolves(root, "/bin/df", executable);
    resolves(root, executable, executable);
    join(aliasedExecutable, alias, "bin/df");
    resolves(root, aliasedExecutable, executable);
    resolves(alias, aliasedExecutable, executable);
    // macOS /tmp -> /private/tmp reproduces iOS's /var -> /private/var physical prefix mismatch.
    join(aliasedExecutable, temporary, "RuntimeRoot/bin/df");
    resolves(root, aliasedExecutable, executable);

    char outside[PATH_MAX], escape[PATH_MAX], internal[PATH_MAX];
    join(outside, base, "outside");
    join(escape, bin, "escape");
    join(internal, bin, "internal");
    makeFile(outside);
    assert(!symlink(outside, escape) && !symlink(executable, internal));
    resolves(root, "/bin/internal", executable);
    fails(root, "/bin/escape", EACCES);
    fails(root, escape, EACCES);
    join(aliasedExecutable, alias, "bin/escape");
    fails(root, aliasedExecutable, 0);
    fails(root, outside, 0);
    fails(root, root, EACCES);
    fails(root, alias, EACCES);

    int originalCWD = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(originalCWD >= 0 && !chdir(bin));
    resolves(root, "df", executable);
    resolves(root, "../bin/df", executable);
    assert(!chdir(base));
    fails(root, "outside", EACCES);
    assert(!fchdir(originalCWD) && !close(originalCWD));

    // An existing native executable must still resolve to the separate guest file, never to host /usr/bin/false.
    char usr[PATH_MAX], usrBin[PATH_MAX], guestFalse[PATH_MAX];
    join(usr, root, "usr");
    join(usrBin, usr, "bin");
    join(guestFalse, usrBin, "false");
    assert(!mkdir(usr, 0700) && !mkdir(usrBin, 0700));
    makeFile(guestFalse);
    resolves(root, "/usr/bin/false", guestFalse);
    char missing[PATH_MAX];
    join(missing, root, "bin/missing");
    fails(root, missing, ENOENT);
    fails(root, "/bin/missing", ENOENT);
    fails(NULL, "/bin/df", EINVAL);
    fails("relative", "/bin/df", EINVAL);
    fails(root, NULL, EINVAL);
    fails(root, "", EINVAL);
    fails("/", "/bin/df", EINVAL);
    fails(executable, "/bin/df", ENOTDIR);
    assert(LSBResolveGuestExecutable(root, "/bin/df", NULL) == EINVAL);
    char oversized[PATH_MAX + 1];
    memset(oversized, 'x', sizeof(oversized));
    oversized[0] = '/'; oversized[PATH_MAX] = 0;
    fails(root, oversized, ENAMETOOLONG);

    testDataExecutables(base, root);
    oversized[PATH_MAX - 1] = 0;
    fails(root, oversized, ENAMETOOLONG);

    assert(!unlink(guestFalse) && !rmdir(usrBin) && !rmdir(usr));
    assert(!unlink(internal) && !unlink(escape) && !unlink(outside) && !unlink(executable));
    assert(!unlink(alias) && !rmdir(bin) && !rmdir(root) && !rmdir(base));
    puts("ExecutablePath tests passed");
    return 0;
}
