#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char image[] = "/System/Library/Frameworks/Security.framework/Security";
static char directory[PATH_MAX], runtimeRoot[PATH_MAX], physical[PATH_MAX];

static void join(char output[PATH_MAX], const char *prefix, const char *suffix) {
    int count = snprintf(output, PATH_MAX, "%s%s", prefix, suffix);
    assert(count > 0 && count < PATH_MAX);
}

static void initialize(void) {
    const char *temporary = getenv("TMPDIR");
    char candidate[PATH_MAX];
    int count = snprintf(candidate, sizeof(candidate), "%s/LiveSimBooter-cache-path.XXXXXX", temporary ?: "/tmp");
    assert(count > 0 && (size_t)count < sizeof(candidate));
    assert(mkdtemp(candidate) && realpath(candidate, directory));
    join(runtimeRoot, directory, "/RuntimeRoot");
    assert(!mkdir(runtimeRoot, 0700));
    join(physical, runtimeRoot, image);
    assert(!setenv("DYLD_ROOT_PATH", runtimeRoot, 1));
}

static void makeImageDirectory(char parent[PATH_MAX]) {
    const char *components[] = {"/System", "/System/Library", "/System/Library/Frameworks", "/System/Library/Frameworks/Security.framework"};
    for (unsigned i = 0; i < sizeof(components) / sizeof(components[0]); ++i) {
        join(parent, runtimeRoot, components[i]);
        assert(!mkdir(parent, 0700));
    }
}

static void removeImageDirectory(void) {
    const char *components[] = {"/System/Library/Frameworks/Security.framework", "/System/Library/Frameworks", "/System/Library", "/System"};
    char path[PATH_MAX];
    for (unsigned i = 0; i < sizeof(components) / sizeof(components[0]); ++i) {
        join(path, runtimeRoot, components[i]);
        assert(!rmdir(path));
    }
}

#ifdef LSB_SHARED_CACHE_PATH_INTERPOSE_TESTING

int main(int argc, char **argv) {
    assert(argc == 2);
    initialize();
    assert(_dyld_shared_cache_contains_path(image));
    void *handle = dlopen(physical, RTLD_NOW | RTLD_LOCAL);
    if (!handle) fprintf(stderr, "%s\n", dlerror());
    assert(handle && dlsym(handle, "SecRandomCopyBytes"));
    void *loaded = dlopen(physical, RTLD_NOLOAD | RTLD_NOW);
    assert(loaded && !dlclose(loaded) && !dlclose(handle));
    char parent[PATH_MAX];
    makeImageDirectory(parent);
    int fd = open(physical, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && !close(fd));
    assert(!dlopen(physical, RTLD_NOW));
    const char *error = dlerror();
    assert(error && strstr(error, physical));
    assert(!unlink(physical));
    removeImageDirectory();

    // The caller fixture resides outside the interposer's directory. A normal wrapper return address would break this lookup.
    void *caller = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    assert(caller);
    void *(*openSibling)(void) = (void *(*)(void))dlsym(caller, "LSBSharedCachePathOpenSibling");
    assert(openSibling);
    void *sibling = openSibling();
    if (!sibling) fprintf(stderr, "%s\n", dlerror());
    assert(sibling && dlsym(sibling, "LSBSharedCachePathOpenSibling") && !dlclose(sibling) && !dlclose(caller));
    assert(!dlopen("/unrelated/LSB-missing-image", RTLD_NOW));
    error = dlerror();
    assert(error && strstr(error, "/unrelated/LSB-missing-image"));
    assert(!rmdir(runtimeRoot) && !rmdir(directory));
    puts("SharedCachePathCompatibility interpose: real cache load, NOLOAD, loose-file error and caller-relative lookup passed");
    return 0;
}

#else

static unsigned nativeCalls, cacheCalls, environmentCalls;
static bool cacheContains = true, nativeFailure;
static bool networkCacheContains = true, networkAliasCacheContains;
static const char networkAlias[] = "/usr/lib/libnetwork.dylib";
static const char networkImage[] = "/System/Library/Frameworks/Network.framework/Network";
static char *seenPath;
static int seenMode;
static void *nativeDlopen(const char *path, int mode) {
    assert(errno == EDOM);
    ++nativeCalls;
    free(seenPath);
    seenPath = path ? strdup(path) : NULL;
    seenMode = mode;
    errno = ENOTTY;
    return nativeFailure ? NULL : (void *)0x1234;
}
static bool cacheContainsPath(const char *path) {
    ++cacheCalls;
    errno = ENOSPC;
    return (cacheContains && !strcmp(path, image)) || (networkCacheContains && !strcmp(path, networkImage)) ||
        (networkAliasCacheContains && !strcmp(path, networkAlias));
}
static char *testGetenv(const char *name) { ++environmentCalls; return getenv(name); }
#define LSB_SHARED_CACHE_PATH_TESTING
#define dlopen nativeDlopen
#define _dyld_shared_cache_contains_path cacheContainsPath
#define getenv testGetenv
#include "../Guest/SharedCachePathCompatibility.c"
#undef getenv
#undef _dyld_shared_cache_contains_path
#undef dlopen

static void check(const char *path, const char *expected, int mode) {
    unsigned calls = nativeCalls;
    errno = EDOM;
    assert(LSBSharedCacheDlopen(path, mode) == (nativeFailure ? NULL : (void *)0x1234));
    assert(nativeCalls == calls + 1 && seenMode == mode && errno == ENOTTY);
    assert(expected ? seenPath && !strcmp(expected, seenPath) : !seenPath);
}

static void checkNetworkAlias(const char *physicalAlias, bool mapped) {
    check(networkAlias, mapped ? networkImage : networkAlias, RTLD_LAZY | RTLD_LOCAL);
    check(physicalAlias, mapped ? networkImage : physicalAlias, RTLD_LAZY | RTLD_LOCAL);
}

static void testMissingNetworkAlias(void) {
    char alias[PATH_MAX], target[PATH_MAX], parent[PATH_MAX];
    join(alias, runtimeRoot, networkAlias);
    join(target, runtimeRoot, networkImage);
    checkNetworkAlias(alias, true);
    const int modes[] = {RTLD_NOW, RTLD_NOLOAD | RTLD_NOW, RTLD_NOW | RTLD_FIRST, 0x7fffffff};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        check(networkAlias, networkImage, modes[i]);
        check(alias, networkImage, modes[i]);
    }
    nativeFailure = true;
    checkNetworkAlias(alias, true);
    nativeFailure = false;
    // A future cache may already know the alias; do not replace its own lookup semantics.
    networkAliasCacheContains = true;
    check(networkAlias, networkAlias, RTLD_NOW);
    check(alias, networkAlias, RTLD_NOW);
    networkAliasCacheContains = false;
    networkCacheContains = false;
    checkNetworkAlias(alias, false);
    networkCacheContains = true;
    if (!strncmp(runtimeRoot, "/private/var/", 13)) check(alias + 8, networkImage, RTLD_NOW);
    const char *unrelated[] = {"/usr/lib/libnetwork.dylib-other", "/usr/lib/./libnetwork.dylib", "/usr//lib/libnetwork.dylib", networkImage};
    for (unsigned i = 0; i < sizeof(unrelated) / sizeof(unrelated[0]); ++i) check(unrelated[i], unrelated[i], RTLD_NOW);
    assert(!unsetenv("DYLD_ROOT_PATH"));
    checkNetworkAlias(alias, false);
    assert(!setenv("DYLD_ROOT_PATH", "/", 1));
    checkNetworkAlias(alias, false);
    assert(!setenv("DYLD_ROOT_PATH", runtimeRoot, 1));

    const char *components[] = {"/usr", "/usr/lib", "/System", "/System/Library", "/System/Library/Frameworks",
        "/System/Library/Frameworks/Network.framework"};
    for (unsigned i = 0; i < sizeof(components) / sizeof(components[0]); ++i) {
        join(parent, runtimeRoot, components[i]);
        assert(!mkdir(parent, 0700));
    }
    checkNetworkAlias(alias, true);
    // Neither an alias override nor a canonical override may be bypassed, even when it is a dangling symlink.
    const char *overrides[] = {alias, target};
    for (unsigned i = 0; i < sizeof(overrides) / sizeof(overrides[0]); ++i) {
        int fd = open(overrides[i], O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0 && !close(fd));
        checkNetworkAlias(alias, false);
        assert(!unlink(overrides[i]) && !symlink("missing-image", overrides[i]));
        checkNetworkAlias(alias, false);
        assert(!unlink(overrides[i]));
    }
    const char *parents[] = {"/usr/lib", "/System/Library/Frameworks/Network.framework"};
    for (unsigned i = 0; i < sizeof(parents) / sizeof(parents[0]); ++i) {
        join(parent, runtimeRoot, parents[i]);
        assert(!rmdir(parent) && !symlink(directory, parent));
        checkNetworkAlias(alias, false);
        assert(!unlink(parent) && !mkdir(parent, 0700));
    }
    checkNetworkAlias(alias, true);
    for (unsigned i = sizeof(components) / sizeof(components[0]); i > 0; --i) {
        join(parent, runtimeRoot, components[i - 1]);
        assert(!rmdir(parent));
    }
}

int main(void) {
    initialize();
    // Before constructor readiness, even an otherwise matching path forwards without environment/cache work.
    check(physical, physical, RTLD_NOW);
    check(networkAlias, networkAlias, RTLD_LAZY | RTLD_LOCAL);
    assert(!environmentCalls && !cacheCalls);
    initializeSharedCachePath();
    const int modes[] = {RTLD_NOW, RTLD_LAZY | RTLD_LOCAL, RTLD_NOLOAD | RTLD_NOW, RTLD_NOW | RTLD_FIRST, 0x7fffffff};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) check(physical, image, modes[i]);
    nativeFailure = true;
    check(physical, image, RTLD_NOW);
    nativeFailure = false;
    cacheContains = false;
    check(physical, physical, RTLD_NOW);
    cacheContains = true;
    const char *unrelated[] = {NULL, "", "relative.dylib", "@rpath/Security", image, "/unrelated/image"};
    for (unsigned i = 0; i < sizeof(unrelated) / sizeof(unrelated[0]); ++i) check(unrelated[i], unrelated[i], RTLD_LAZY);
    char path[PATH_MAX];
    const char *invalid[] = {"/../RuntimeRoot/System/Library/Frameworks/Security.framework/Security", "/./image", "//image", "/image/"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        join(path, runtimeRoot, invalid[i]);
        check(path, path, RTLD_NOW);
    }
    join(path, runtimeRoot, "-other/System/Library/Frameworks/Security.framework/Security");
    check(path, path, RTLD_NOW);
    if (!strncmp(runtimeRoot, "/private/var/", 13)) {
        check(physical + 8, image, RTLD_NOW);
        assert(!setenv("DYLD_ROOT_PATH", runtimeRoot + 8, 1));
        check(physical, image, RTLD_NOW);
    }
    assert(!unsetenv("DYLD_ROOT_PATH"));
    check(physical, physical, RTLD_NOW);
    assert(!setenv("DYLD_ROOT_PATH", "/", 1));
    check(physical, physical, RTLD_NOW);
    join(path, directory, "/alias");
    assert(!symlink(runtimeRoot, path) && !setenv("DYLD_ROOT_PATH", path, 1));
    check(physical, physical, RTLD_NOW);
    assert(!unlink(path) && !setenv("DYLD_ROOT_PATH", runtimeRoot, 1));

    testMissingNetworkAlias();

    char parent[PATH_MAX];
    makeImageDirectory(parent);
    check(physical, image, RTLD_NOW);
    int fd = open(physical, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && !close(fd));
    check(physical, physical, RTLD_NOW);
    assert(!unlink(physical) && !symlink("missing-image", physical));
    check(physical, physical, RTLD_NOW);
    assert(!unlink(physical) && !rmdir(parent) && !symlink(directory, parent));
    check(physical, physical, RTLD_NOW);
    assert(!unlink(parent) && !mkdir(parent, 0700));
    removeImageDirectory();
    assert(!rmdir(runtimeRoot) && !rmdir(directory));
    free(seenPath);
    puts("SharedCachePathCompatibility: canonical/var paths, missing Network alias, cache membership, overrides, modes, errno and early readiness passed");
    return 0;
}
#endif
