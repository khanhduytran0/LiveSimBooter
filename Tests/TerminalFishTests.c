#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

static unsigned platform = 7, replacementCalls;
static int terminalFDs = 1;
static char expectedRoot[PATH_MAX], expectedData[PATH_MAX];

unsigned dyld_get_active_platform(void) { return platform; }
static int testIsATTY(int descriptor) { return terminalFDs && descriptor >= 0 && descriptor <= 2; }

static int testSpawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                     const posix_spawnattr_t *attributes, char *const arguments[], char *const environment[]) {
    ++replacementCalls;
    short flags = 0;
    assert(!pid && !actions && attributes && !posix_spawnattr_getflags(attributes, &flags) && flags == POSIX_SPAWN_SETEXEC);
    assert(!strcmp(path, "/bin/fish") && !strcmp(arguments[0], path));
    assert(!strcmp(arguments[1], "-i") && !strcmp(arguments[2], "-N") && !strcmp(arguments[3], "-P") && !strcmp(arguments[4], "-C"));
    assert(strstr(arguments[5], "status job-control none") && strstr(arguments[5], "fish_use_posix_spawn 1") && !arguments[6]);
    assert(environment == environ && !strcmp(getenv("TERM"), "xterm-256color") && !strcmp(getenv("HOME"), expectedData));
    assert(!strcmp(getenv("CFFIXED_USER_HOME"), expectedData));
    char pathExpected[PATH_MAX * 5 + 80], cwd[PATH_MAX];
    assert(snprintf(pathExpected, sizeof(pathExpected), "%s/usr/local/bin:%s/bin:%s/usr/bin:%s/usr/sbin:%s/sbin",
                    expectedRoot, expectedRoot, expectedRoot, expectedRoot, expectedRoot) > 0);
    assert(!strcmp(getenv("PATH"), pathExpected) && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, expectedData));
    return EACCES; // A failed replacement is reported; the helper must not fall back to an ordinary child spawn.
}

#define isatty testIsATTY
#define posix_spawn testSpawn
#define main terminalFishMain
#include "../Guest/TerminalFish.c"
#undef main
#undef posix_spawn
#undef isatty

int main(void) {
    char temporary[] = "/tmp/LiveSimBooter-terminal-fish.XXXXXX", base[PATH_MAX], cwd[PATH_MAX];
    assert(getcwd(cwd, sizeof(cwd)) && mkdtemp(temporary) && realpath(temporary, base));
    assert(snprintf(expectedRoot, sizeof(expectedRoot), "%s/RuntimeRoot", base) > 0);
    assert(snprintf(expectedData, sizeof(expectedData), "%s/data", base) > 0);
    assert(!mkdir(expectedRoot, 0700) && !mkdir(expectedData, 0700));
    assert(!setenv("DYLD_ROOT_PATH", expectedRoot, 1) && !setenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY", expectedData, 1));
    assert(!setenv(LSB_FAKE_PTY_ENV, "/test/attached-terminal", 1));
    platform = 2;
    assert(terminalFishMain() == 1 && !replacementCalls);
    platform = 7;
    terminalFDs = 0;
    assert(terminalFishMain() == 1 && !replacementCalls);
    terminalFDs = 1;
    assert(!unsetenv(LSB_FAKE_PTY_ENV));
    assert(terminalFishMain() == 1 && !replacementCalls);
    assert(!setenv(LSB_FAKE_PTY_ENV, "/test/attached-terminal", 1));
    assert(!setenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY", base, 1));
    assert(terminalFishMain() == 1 && !replacementCalls);
    char previousData[PATH_MAX];
    assert(snprintf(previousData, sizeof(previousData), "%s/SimulatorData", base) > 0 && !mkdir(previousData, 0700));
    assert(!setenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY", previousData, 1));
    assert(terminalFishMain() == 1 && !replacementCalls && !rmdir(previousData));
    assert(!setenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY", expectedData, 1));
    assert(terminalFishMain() == 127 && replacementCalls == 1);
    assert(!chdir(cwd) && !rmdir(expectedData) && !rmdir(expectedRoot) && !rmdir(base));
    puts("TerminalFish: platform/PTY/data-root guards, controlled environment, SETEXEC-only Fish replacement and failure reporting passed");
    return 0;
}
