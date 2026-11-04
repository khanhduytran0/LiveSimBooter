#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../ProcessSupport/FakePTY.h"
#include "../../ProcessSupport/RuntimeLayout.h"

extern char **environ;
extern unsigned dyld_get_active_platform(void);

static int prepareEnvironment(void) {
    const char *configuredRoot = getenv("DYLD_ROOT_PATH"), *configuredData = getenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY");
    const char *terminal = getenv(LSB_FAKE_PTY_ENV);
    if (dyld_get_active_platform() != 7 || !configuredRoot || configuredRoot[0] != '/' || !configuredData || configuredData[0] != '/' ||
        !terminal || terminal[0] != '/' || !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || !isatty(STDERR_FILENO)) return ENOTTY;
    char root[PATH_MAX], data[PATH_MAX], expectedData[PATH_MAX];
    if (!realpath(configuredRoot, root) || !realpath(configuredData, data)) return errno;
    const char *leaf = strrchr(root, '/');
    if (!leaf || strcmp(leaf, "/" LSB_RUNTIME_DIRECTORY)) return EINVAL;
    int length = snprintf(expectedData, sizeof(expectedData), "%.*s/" LSB_DATA_DIRECTORY, (int)(leaf - root), root);
    if (length < 0 || (size_t)length >= sizeof(expectedData)) return ENAMETOOLONG;
    if (strcmp(data, expectedData)) return EINVAL;
    static const char *const bootstrapDirs[] = {"usr/bin", "bin", "usr/sbin", "sbin"};
    char path[PATH_MAX * (5 + sizeof(bootstrapDirs) / sizeof(bootstrapDirs[0])) + 80];
    size_t used = 0;
    for (size_t i = 0; i < sizeof(bootstrapDirs) / sizeof(bootstrapDirs[0]); ++i) {
        char directory[PATH_MAX], canonical[PATH_MAX];
        struct stat info;
        length = snprintf(directory, sizeof(directory), "%s/var/jb/%s", data, bootstrapDirs[i]);
        if (length < 0 || (size_t)length >= sizeof(directory) || !realpath(directory, canonical) || strcmp(canonical, directory) ||
            stat(canonical, &info) || !S_ISDIR(info.st_mode)) continue;
        memcpy(path + used, directory, length);
        used += (size_t)length;
        path[used++] = ':';
    }
    length = snprintf(path + used, sizeof(path) - used, "%s/usr/local/bin:%s/bin:%s/usr/bin:%s/usr/sbin:%s/sbin", root, root, root, root, root);
    if (length < 0 || (size_t)length >= sizeof(path) - used) return ENAMETOOLONG;
    if (setenv("TERM", "xterm-256color", 1) || setenv("HOME", data, 1) || setenv("CFFIXED_USER_HOME", data, 1) || setenv("PATH", path, 1))
        return errno;
    return chdir(data) ? errno : 0;
}

int main(void) {
    int error = prepareEnvironment();
    if (error) { fprintf(stderr, "terminal-fish: requires a launchd-attached simulator terminal: %s\n", strerror(error)); return 1; }
    char *arguments[] = {"/bin/fish", "-i", "-N", "-P", "-C",
        "status job-control none; set -g fish_use_posix_spawn 1; set -g fish_greeting; function fish_prompt; printf 'fish> '; end", NULL};
    posix_spawnattr_t attributes;
    error = posix_spawnattr_init(&attributes);
    if (!error) {
        error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETEXEC);
        if (!error) error = posix_spawn(NULL, arguments[0], NULL, &attributes, arguments, environ);
        posix_spawnattr_destroy(&attributes);
    }
    // A successful SETEXEC never returns: the broker keeps this launchd job's logical identity for Fish.
    fprintf(stderr, "terminal-fish: Fish replacement failed: %s\n", strerror(error ?: EIO));
    return 127;
}
