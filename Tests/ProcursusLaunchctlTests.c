#include "../Guest/ProcursusMaintscript.h"
#include <assert.h>
#include <fcntl.h>
#include <removefile.h>
#include <unistd.h>

static LSBGuestRuntimeDirectories directories;
static unsigned lookups;

int LSBGetRuntimeDirectories(const LSBGuestRuntimeDirectories **result) {
    ++lookups;
    *result = &directories;
    return 0;
}

static void makeFile(const char *path, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);
    assert(fd >= 0 && !close(fd));
}

// On passthrough, argv, executable and string storage must be completely untouched.
static void check(const char *program, const char *root, const char *package, const char *op, const char *flag, const char *plist,
                  const char *expected, int error) {
    LSBSpawnPlan plan = {};
    const char *argv[] = {"launchctl", op, flag ? flag : plist, flag ? plist : NULL};
    assert(!LSBPlanAddString(&plan, program, &plan.path));
    for (unsigned i = 0; i < 4 && argv[i]; ++i) assert(!LSBPlanAddString(&plan, argv[i], &plan.argv[plan.argc++]));
    if (root) {
        char env[PATH_MAX + sizeof("DPKG_ROOT=")];
        snprintf(env, sizeof(env), "DPKG_ROOT=%s", root);
        assert(!LSBPlanAddString(&plan, env, &plan.envp[plan.envc++]));
    }
    if (package) assert(!LSBPlanAddString(&plan, package, &plan.envp[plan.envc++]));
    uint32_t used = plan.stringBytes;
    assert(LSBProjectProcursusMaintscript(&plan, strlen(program)) == error);
    assert(!strcmp(plan.strings + plan.path, expected ? "/bin/launchctl" : program));
    for (unsigned i = 0; i < plan.argc; ++i) {
        assert(!strcmp(plan.strings + plan.argv[i], (expected && i == plan.argc - 1) ? expected : argv[i]));
    }
    if (!expected) assert(plan.stringBytes == used);
}

static void checkTrigger(const char *tool, const char *root, const char *package, const char *script, const char *option, const char *extra,
                         bool routed, int error) {
    LSBSpawnPlan plan = {};
    assert(!LSBPlanAddString(&plan, tool, &plan.path));
    const char *argv[] = {"uicache", option, extra, NULL};
    for (unsigned i = 0; argv[i]; ++i) assert(!LSBPlanAddString(&plan, argv[i], &plan.argv[plan.argc++]));
    if (package) assert(!LSBPlanAddString(&plan, package, &plan.envp[plan.envc++]));
    if (script) assert(!LSBPlanAddString(&plan, script, &plan.envp[plan.envc++]));
    if (root) {
        char env[PATH_MAX + sizeof("DPKG_ROOT=")];
        snprintf(env, sizeof(env), "DPKG_ROOT=%s", root);
        assert(!LSBPlanAddString(&plan, env, &plan.envp[plan.envc++]));
    }
    uint32_t used = plan.stringBytes;
    assert(LSBProjectProcursusMaintscript(&plan, strlen(tool)) == error);
    if (routed) {
        assert(plan.argc == 2 && plan.argv[0] == plan.path);
        assert(!strcmp(plan.strings + plan.path, "/usr/libexec/app-registration") && !strcmp(plan.strings + plan.argv[1], "--dpkg-trigger"));
    } else assert(plan.stringBytes == used && !strcmp(plan.strings + plan.path, tool) && !strcmp(plan.strings + plan.argv[1], option));
}

int main(void) {
    char build[] = "/private/tmp/lsb-launchctl.XXXXXX";
    assert(mkdtemp(build) && !chdir(build));
    const char *folders[] = {"RuntimeRoot", "data", "data/var", "data/var/jb", "data/var/jb/bin", "data/var/jb/usr", "data/var/jb/usr/bin",
        "data/var/jb/Library", "data/var/jb/Library/LaunchDaemons", "data/copy", "data/copy/var", "data/copy/var/jb",
        "data/copy/var/jb/Library", "data/copy/var/jb/Library/LaunchDaemons"};
    for (size_t i = 0; i < sizeof(folders) / sizeof(folders[0]); ++i) assert(!mkdir(folders[i], 0700));
    makeFile("data/var/jb/usr/bin/launchctl", 0700);
    makeFile("data/var/jb/usr/bin/uicache", 0700);
    assert(!symlink("../usr/bin/uicache", "data/var/jb/bin/uicache"));
    assert(!symlink("../usr/bin/launchctl", "data/var/jb/bin/launchctl"));
    makeFile("data/var/jb/Library/LaunchDaemons/test.plist", 0600);
    makeFile("data/copy/var/jb/Library/LaunchDaemons/test.plist", 0600);
    assert(!symlink(build, "data/escape"));
    assert(!symlink("/etc/hosts", "data/copy/var/jb/Library/LaunchDaemons/escape.plist"));
    char data[PATH_MAX], copy[PATH_MAX], actual[PATH_MAX], inCopy[PATH_MAX], physical[PATH_MAX], aliasRoot[PATH_MAX];
    assert(realpath("RuntimeRoot", directories.runtime) && realpath("data/var/jb", directories.bootstrap));
    directories.bootstrapLength = strlen(directories.bootstrap);
    assert(realpath("data", data) && realpath("data/copy", copy) && realpath("data/var/jb/usr/bin/launchctl", physical));
    assert(realpath("data/var/jb/Library/LaunchDaemons/test.plist", actual) && realpath("data/copy/var/jb/Library/LaunchDaemons/test.plist", inCopy));

    const char *pkg = "DPKG_MAINTSCRIPT_PACKAGE=test", *exe = "/var/jb/bin/launchctl", *plist = "/var/jb/Library/LaunchDaemons/test.plist";
    check(exe, data, NULL, "unload", NULL, plist, NULL, 0);
    check(exe, data, "DPKG_MAINTSCRIPT_PACKAGE=", "unload", NULL, plist, NULL, 0);
    check(exe, data, pkg, "print", NULL, plist, NULL, 0);
    check(exe, data, pkg, "unload", "-Z", plist, NULL, 0);
    assert(lookups == 0);

    check(exe, data, pkg, "unload", NULL, plist, actual, 0);
    check("/private/var/jb/usr/bin/launchctl", copy, pkg, "load", "-w", plist, inCopy, 0);
    check(physical, copy, pkg, "unload", NULL, inCopy, inCopy, 0);
    check(physical + strlen("/private"), data, pkg, "load", NULL, "/private/var/jb/Library/LaunchDaemons/test.plist", actual, 0);
    check("/bin/launchctl", data, pkg, "load", NULL, plist, NULL, 0);
    check(exe, data, pkg, "load", NULL, "/var/jb/usr/lib/foo.plist", NULL, 0);
    check(exe, "/private/tmp", pkg, "unload", NULL, plist, NULL, EACCES);
    assert(snprintf(aliasRoot, sizeof(aliasRoot), "%s/escape", data) < (int)sizeof(aliasRoot));
    check(exe, aliasRoot, pkg, "unload", NULL, plist, NULL, EACCES);
    check(exe, copy, pkg, "unload", NULL, "/var/jb/Library/LaunchDaemons/escape.plist", NULL, EACCES);
    check(exe, copy, pkg, "unload", NULL, "/var/jb/Library/LaunchDaemons/../test.plist", NULL, EINVAL);
    check(exe, copy, pkg, "unload", NULL, "/var/jb/Library/LaunchDaemons/deleted.plist", NULL, ENOENT);

    const char *ui = "/var/jb/bin/uicache", *uiPkg = "DPKG_MAINTSCRIPT_PACKAGE=uikittools", *postinst = "DPKG_MAINTSCRIPT_NAME=postinst";
    checkTrigger(ui, copy, uiPkg, postinst, "-a", NULL, true, 0);
    checkTrigger("/private/var/jb/usr/bin/uicache", copy, uiPkg, postinst, "--all", NULL, true, 0);
    assert(realpath("data/var/jb/bin/uicache", physical));
    checkTrigger(physical, data, uiPkg, postinst, "-a", NULL, true, 0);
    checkTrigger(physical + strlen("/private"), data, uiPkg, postinst, "-a", NULL, true, 0);
    char runtimeSpelling[PATH_MAX];
    assert(snprintf(runtimeSpelling, sizeof(runtimeSpelling), "%s/var/jb/bin/uicache", directories.runtime) < (int)sizeof(runtimeSpelling));
    checkTrigger(runtimeSpelling, data, uiPkg, postinst, "-a", NULL, true, 0);
    checkTrigger(ui, copy, uiPkg, postinst, "-p", "/Applications/probe.app", false, 0);
    checkTrigger(ui, copy, uiPkg, postinst, "-a", "-r", false, 0);
    checkTrigger(ui, copy, uiPkg, "DPKG_MAINTSCRIPT_NAME=prerm", "-a", NULL, false, 0);
    checkTrigger(ui, copy, pkg, postinst, "-a", NULL, false, 0);
    checkTrigger(ui, copy, uiPkg, NULL, "-a", NULL, false, 0);
    checkTrigger("/usr/local/bin/uicache", copy, uiPkg, postinst, "-a", NULL, false, 0);
    checkTrigger(ui, NULL, uiPkg, postinst, "-a", NULL, false, EACCES);
    checkTrigger(ui, aliasRoot, uiPkg, postinst, "-a", NULL, false, EACCES);
    checkTrigger(ui, "/private/tmp", uiPkg, postinst, "-a", NULL, false, EACCES);
    checkTrigger(ui, inCopy, uiPkg, postinst, "-a", NULL, false, ENOTDIR);
    assert(!unlink("data/var/jb/bin/uicache") && !symlink("../usr/bin/launchctl", "data/var/jb/bin/uicache"));
    checkTrigger(ui, copy, uiPkg, postinst, "-a", NULL, false, EACCES);
    assert(!unlink("data/var/jb/bin/launchctl") && !symlink("/bin/sh", "data/var/jb/bin/launchctl"));
    check(exe, copy, pkg, "unload", NULL, plist, NULL, EACCES);
    assert(!chdir("/") && !removefile(build, NULL, REMOVEFILE_RECURSIVE));
    puts("dpkg maintainer scripts: rooted launchctl load/unload and scoped uikittools registration trigger passed");
    return 0;
}
