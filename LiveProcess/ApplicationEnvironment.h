#pragma once

#include "../ProcessSupport/GuestExecutablePath.h"
#include "../ProcessSupport/NativeRuntimePolicy.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

typedef struct {
    bool applies;
    char fixedHome[sizeof("CFFIXED_USER_HOME=") + PATH_MAX];
    char temporaryDirectory[sizeof("TMPDIR=") + PATH_MAX];
} LSBApplicationEnvironment;

static inline bool LSBApplicationContainerUUID(const char *identifier) {
    if (strlen(identifier) != 36) return false;
    for (unsigned index = 0; index < 36; ++index) {
        unsigned char value = (unsigned char)identifier[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) { if (value != '-') return false; }
        else if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F'))) return false;
    }
    return true;
}

// RunningBoard supplies the real MCM home. Do not infer a data UUID from the bundle UUID, create a container, or rewrite shared roots.
// This adjusts directory semantics only for a top-level installed .app executable; native service roles never call it.
// 23A343 CoreFoundation prefers CFFIXED_USER_HOME over HOME; replace launchd's inherited shared value before Foundation caches it.
static inline int LSBPrepareApplicationEnvironment(const char *runtimeRoot, const char *executable, const char *const environment[],
                                                   LSBApplicationEnvironment *output) {
    if (!output) return EINVAL;
    *output = (LSBApplicationEnvironment){0};
    if (!runtimeRoot || !executable) return EINVAL;
    if (!LSBGuestPathIsCanonical(runtimeRoot) || !LSBGuestPathIsCanonical(executable)) return 0;
    char root[PATH_MAX], image[PATH_MAX], home[PATH_MAX];
    struct stat metadata;
    if (!realpath(runtimeRoot, root) || strcmp(strrchr(root, '/'), "/RuntimeRoot") || stat(root, &metadata) || !S_ISDIR(metadata.st_mode) ||
        !realpath(executable, image) || stat(image, &metadata) || !S_ISREG(metadata.st_mode)) return 0;
    const char *app = LSBGuestInstalledApplicationPath(root, image);
    const char *leaf = app ? strchr(app, '/') + 1 : NULL;
    if (!leaf || strchr(leaf, '/')) return 0;
    const char *requestedHome = LSBRuntimeEnvironmentValue(environment, "HOME");
    if (!LSBGuestPathIsCanonical(requestedHome) || !realpath(requestedHome, home) || stat(home, &metadata) || !S_ISDIR(metadata.st_mode)) return 0;
    size_t parentLength = (size_t)(strrchr(root, '/') - root), homeLength = strlen(home);
    static const char location[] = "/data/Containers/Data/Application/";
    size_t prefixLength = parentLength + sizeof(location) - 1;
    if (homeLength <= prefixLength || strncmp(home, root, parentLength) ||
        strncmp(home + parentLength, location, sizeof(location) - 1) || !LSBApplicationContainerUUID(home + prefixLength)) return 0;
    if (homeLength + sizeof("/tmp/") > PATH_MAX) return ENAMETOOLONG;
    int length = snprintf(output->fixedHome, sizeof(output->fixedHome), "CFFIXED_USER_HOME=%s", home);
    if (length < 0 || (size_t)length >= sizeof(output->fixedHome)) return ENAMETOOLONG;
    length = snprintf(output->temporaryDirectory, sizeof(output->temporaryDirectory), "TMPDIR=%s/tmp/", home);
    if (length < 0 || (size_t)length >= sizeof(output->temporaryDirectory)) return ENAMETOOLONG;
    output->applies = true;
    return 0;
}

static inline bool LSBApplicationEnvironmentReplacedEntry(const char *entry) {
    return !strncmp(entry, "CFFIXED_USER_HOME=", sizeof("CFFIXED_USER_HOME=") - 1) || !strncmp(entry, "TMPDIR=", sizeof("TMPDIR=") - 1);
}

// Validate capacity before changing the vector. The caller owns output storage until the dyld_sim handoff completes.
static inline int LSBApplyApplicationEnvironment(const LSBApplicationEnvironment *prepared, const char *environment[],
                                                 unsigned *count, unsigned capacity) {
    if (!prepared || !environment || !count || *count >= capacity || environment[*count]) return EINVAL;
    unsigned retained = 0;
    for (unsigned index = 0; index < *count; ++index) {
        if (!environment[index]) return EINVAL;
        if (!LSBApplicationEnvironmentReplacedEntry(environment[index])) ++retained;
    }
    if (!prepared->applies) return 0;
    if (capacity - retained <= 2) return E2BIG;
    unsigned position = 0;
    for (unsigned index = 0; index < *count; ++index)
        if (!LSBApplicationEnvironmentReplacedEntry(environment[index])) environment[position++] = environment[index];
    environment[position++] = prepared->fixedHome;
    environment[position++] = prepared->temporaryDirectory;
    environment[position] = NULL;
    *count = position;
    return 0;
}
