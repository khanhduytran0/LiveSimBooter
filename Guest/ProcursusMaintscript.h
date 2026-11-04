#pragma once

#include "RuntimeDirectories.h"
#include "../ProcessSupport/GuestExecutablePath.h"
#include "../ProcessSupport/SpawnPlan.h"
#include <sys/stat.h>

static inline bool LSBProcursusToolPath(const char *path, const LSBGuestRuntimeDirectories *dirs, const char *tool) {
    const char *guest = LSBGuestVirtualBootstrapPath(path);
    const char *runtime = LSBGuestStorageRootSpelling(dirs->runtime, path);
    if (!guest && LSBGuestPathInsideRoot(path, runtime)) {
        const char *suffix = path + strlen(runtime);
        guest = LSBGuestVirtualBootstrapPath(suffix[1] == '/' ? suffix + 1 : suffix);
    }
    const char *tail;
    if (guest) tail = guest + sizeof("/var/jb") - 1;
    else {
        const char *base = LSBGuestStorageRootSpelling(dirs->bootstrap, path);
        if (!LSBGuestPathInsideRoot(path, base)) return false;
        tail = path + strlen(base);
    }
    if (!strncmp(tail, "/usr/", 5)) tail += 4;
    return !strncmp(tail, "/bin/", 5) && !strcmp(tail + 5, tool);
}

static inline int LSBCheckMaintscriptTarget(const LSBGuestRuntimeDirectories *dirs, const char *root, const char *executable, const char *tool) {
    // The cached bootstrap is <data>/var/jb. Accept the canonical data directory or a transaction root physically beneath it.
    size_t dataLength = dirs->bootstrapLength - (sizeof("/var/jb") - 1);
    if (!root || strncmp(root, dirs->bootstrap, dataLength) || (root[dataLength] && root[dataLength] != '/') || !LSBGuestPathIsCanonical(root)) return EACCES;
    char resolved[PATH_MAX];
    if (!realpath(root, resolved)) return errno;
    if (strcmp(root, resolved)) return EACCES;
    struct stat metadata;
    if (stat(resolved, &metadata)) return errno;
    if (!S_ISDIR(metadata.st_mode)) return ENOTDIR;
    bool handled;
    int error = LSBResolveBootstrapPath(dirs->runtime, executable, resolved, &handled);
    if (error) return error;
    if (!handled || !LSBProcursusToolPath(resolved, dirs, tool)) return EACCES;
    if (stat(resolved, &metadata)) return errno;
    return S_ISREG(metadata.st_mode) && (metadata.st_mode & 0111) ? 0 : EACCES;
}

static inline int LSBUseGuestLaunchctl(LSBSpawnPlan *plan, const char *root, const char *guest) {
    char candidate[PATH_MAX], resolved[PATH_MAX];
    int n = snprintf(candidate, sizeof(candidate), "%s%s", root, guest);
    if (n < 0 || (size_t)n >= sizeof(candidate)) return ENAMETOOLONG;
    if (!realpath(candidate, resolved)) return errno;
    if (!LSBGuestPathInsideRoot(resolved, root)) return EACCES;
    struct stat metadata;
    if (stat(resolved, &metadata)) return errno;
    if (!S_ISREG(metadata.st_mode)) return EINVAL;

    int error = LSBPlanAddString(plan, "/bin/launchctl", &plan->path);
    return error ? error : LSBPlanAddString(plan, resolved, &plan->argv[plan->argc - 1]);
}

// These stock tools have device-specific assumptions. Scope replacements to dpkg's child environment and the exact supported invocation.
static inline int LSBProjectProcursusMaintscript(LSBSpawnPlan *plan, size_t pathLength) {
    const char *path = plan->strings + plan->path;
    static const char uiSuffix[] = "/uicache", ctlSuffix[] = "/launchctl";
    bool ui = pathLength >= sizeof(uiSuffix) - 1 && !strcmp(path + pathLength - (sizeof(uiSuffix) - 1), uiSuffix);
    if (!ui && !(pathLength >= sizeof(ctlSuffix) - 1 && !strcmp(path + pathLength - (sizeof(ctlSuffix) - 1), ctlSuffix))) return 0;
    const char *tool = ui ? uiSuffix + 1 : ctlSuffix + 1;
    if (plan->argc < 2) return 0;
    const char *operation = plan->strings + plan->argv[1];
    if (ui) {
        if (plan->argc != 2 || (strcmp(operation, "-a") && strcmp(operation, "--all"))) return 0;
    } else {
        if (plan->argc != 3 && !(plan->argc == 4 && !strcmp(plan->strings + plan->argv[2], "-w"))) return 0;
        if (strcmp(operation, "load") && strcmp(operation, "unload")) return 0;
    }
    static const char packageKey[] = "DPKG_MAINTSCRIPT_PACKAGE=", scriptKey[] = "DPKG_MAINTSCRIPT_NAME=", rootKey[] = "DPKG_ROOT=";
    const char *root = NULL, *package = NULL, *script = NULL;
    for (unsigned i = 0; i < plan->envc; ++i) {
        const char *entry = plan->strings + plan->envp[i];
        if (!package && !strncmp(entry, packageKey, sizeof(packageKey) - 1)) package = entry + sizeof(packageKey) - 1;
        if (!root && !strncmp(entry, rootKey, sizeof(rootKey) - 1)) root = entry + sizeof(rootKey) - 1;
        if (!script && !strncmp(entry, scriptKey, sizeof(scriptKey) - 1)) script = entry + sizeof(scriptKey) - 1;
    }
    if (!package || !*package) return 0;
    if (ui && (strcmp(package, "uikittools") || !script || strcmp(script, "postinst"))) return 0;
    const LSBGuestRuntimeDirectories *dirs;
    int error = LSBGetRuntimeDirectories(&dirs);
    if (error || !dirs->bootstrapLength) return ui ? (error ? error : EACCES) : 0;
    if (!LSBProcursusToolPath(path, dirs, tool)) return 0;
    if (ui) {
        // uikittools 2.1.7 postinst runs uicache -a for its /var/jb/Applications trigger. Native paths would enumerate the phone's apps.
        error = LSBCheckMaintscriptTarget(dirs, root, path, tool);
        if (!error) error = LSBPlanAddString(plan, "/usr/libexec/app-registration", &plan->path);
        if (!error) {
            plan->argv[0] = plan->path;
            error = LSBPlanAddString(plan, "--dpkg-trigger", &plan->argv[1]);
        }
        return error;
    }

    // Procursus launchctl 1.1.1 imports _launch_active_user_switch, absent in 23A343 libSystem.
    // The simulator's launchctl sends the plist path to guest launchd; it must name the file inside dpkg's selected install root.
    if (!root || !*root) return 0;
    unsigned last = plan->argc - 1;
    const char *operand = plan->strings + plan->argv[last];
    const char *guest = LSBGuestVirtualBootstrapPath(operand);
    if (!guest) {
        const char *spelling = LSBGuestStorageRootSpelling(root, operand);
        if (!LSBGuestPathInsideRoot(operand, spelling)) return 0;
        guest = operand + strlen(spelling);
    }
    static const char prefix[] = "/var/jb/Library/LaunchDaemons/";
    if (strncmp(guest, prefix, sizeof(prefix) - 1)) return 0;
    if (!LSBGuestPathIsCanonical(guest)) return EINVAL;
    const char *name = guest + sizeof(prefix) - 1;
    size_t length = strlen(name);
    if (length <= sizeof(".plist") - 1 || strcmp(name + length - (sizeof(".plist") - 1), ".plist") || strchr(name, '/')) return 0;
    error = LSBCheckMaintscriptTarget(dirs, root, path, tool);
    if (error) return error;
    return LSBUseGuestLaunchctl(plan, root, guest);
}
