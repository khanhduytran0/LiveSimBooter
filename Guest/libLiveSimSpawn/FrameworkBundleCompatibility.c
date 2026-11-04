#include "FrameworkBundleCompatibility.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static _Thread_local bool resolvingBundle;

static bool systemFrameworkPath(const char *path) {
    static const char *const prefixes[] = {"/System/Library/Frameworks/", "/System/Library/PrivateFrameworks/"};
    bool prefix = false;
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i)
        if (!strncmp(path, prefixes[i], strlen(prefixes[i]))) prefix = true;
    size_t length = strlen(path);
    if (!prefix || length < sizeof(".framework") || strcmp(path + length - strlen(".framework"), ".framework")) return false;
    // Reject traversal and ambiguous empty components before concatenating the guest namespace with its physical root.
    for (const char *component = path + 1; *component;) {
        const char *end = strchr(component, '/');
        size_t count = end ? (size_t)(end - component) : strlen(component);
        if (!count || (count == 1 && component[0] == '.') || (count == 2 && component[0] == '.' && component[1] == '.')) return false;
        if (!end) break;
        component = end + 1;
    }
    return true;
}

static bool contained(const char *path, const char *directory) {
    size_t length = strlen(directory);
    return !strncmp(path, directory, length) && path[length] == '/';
}

static bool readableBundleInfo(const char *bundlePath) {
    static const char *const layouts[] = {"Info.plist", "Resources/Info.plist"};
    for (unsigned i = 0; i < sizeof(layouts) / sizeof(layouts[0]); ++i) {
        char path[PATH_MAX], canonical[PATH_MAX];
        int count = snprintf(path, sizeof(path), "%s/%s", bundlePath, layouts[i]);
        struct stat status;
        if (count > 0 && (size_t)count < sizeof(path) && realpath(path, canonical) && contained(canonical, bundlePath) &&
            !stat(canonical, &status) && S_ISREG(status.st_mode) && !access(canonical, R_OK)) return true;
    }
    return false;
}

static CFURLRef copyRuntimeFrameworkURL(CFURLRef url) {
    if (!url) return NULL;
    CFStringRef scheme = CFURLCopyScheme(url);
    bool fileURL = scheme && CFEqual(scheme, CFSTR("file"));
    if (scheme) CFRelease(scheme);
    if (!fileURL) return NULL;
    char path[PATH_MAX];
    if (!CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, sizeof(path))) return NULL;
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/') path[--length] = '\0';
    if (!systemFrameworkPath(path)) return NULL;

    const char *root = getenv(LSB_FRAMEWORK_BUNDLE_ROOT_ENV);
    char canonicalRoot[PATH_MAX], candidate[PATH_MAX], canonicalBundle[PATH_MAX];
    struct stat status;
    if (!root || root[0] != '/' || !realpath(root, canonicalRoot) || !canonicalRoot[1] ||
        stat(canonicalRoot, &status) || !S_ISDIR(status.st_mode)) return NULL;
    int count = snprintf(candidate, sizeof(candidate), "%s%s", canonicalRoot, path);
    if (count <= 0 || (size_t)count >= sizeof(candidate) || !realpath(candidate, canonicalBundle) ||
        !contained(canonicalBundle, canonicalRoot) || !systemFrameworkPath(canonicalBundle + strlen(canonicalRoot)) ||
        stat(canonicalBundle, &status) || !S_ISDIR(status.st_mode) || !readableBundleInfo(canonicalBundle)) return NULL;
    return CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)canonicalBundle, strlen(canonicalBundle), true);
}

CFBundleRef LSBFrameworkBundleCreate(CFAllocatorRef allocator, CFURLRef url) {
    if (resolvingBundle) return CFBundleCreate(allocator, url);
    const char *enabled = getenv(LSB_FRAMEWORK_BUNDLE_ENABLE_ENV);
    if (!enabled || strcmp(enabled, "1")) return CFBundleCreate(allocator, url);
    // CF URL/bundle initialization can reenter Foundation. Each thread suppresses only its own recursive translations.
    resolvingBundle = true;
    CFURLRef redirected = copyRuntimeFrameworkURL(url);
    CFBundleRef bundle = CFBundleCreate(allocator, redirected ? redirected : url);
    if (redirected) CFRelease(redirected);
    resolvingBundle = false;
    return bundle;
}

#ifndef LSB_FRAMEWORK_BUNDLE_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interposition = {
    (const void *)&LSBFrameworkBundleCreate, (const void *)&CFBundleCreate
};
#endif
