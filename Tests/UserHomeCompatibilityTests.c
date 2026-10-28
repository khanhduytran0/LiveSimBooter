#include "../Guest/UserHomeCompatibility.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char dataHome[] = "/private/var/mobile/Containers/Shared/AppGroup/test/LSB/iOS/23A343/data";
static const char *nativeHome;
static uid_t nativeUID;
static int nativeError;
static bool nativeMissing;
static size_t nativeUsed;

static int nativeLookup(struct passwd *entry, char *buffer, size_t size, struct passwd **result) {
    *result = NULL;
    errno = ENOTTY;
    if (nativeError || nativeMissing) return nativeError;
    const char *values[] = {"mobile", "*", "", "Mobile User", nativeHome, "/bin/sh"};
    // Reverse the field order and leave gaps so the shim must inspect all string endpoints.
    char *fields[6];
    size_t used = 3;
    for (int i = 5; i >= 0; --i) {
        size_t length = strlen(values[i]) + 1;
        if (used > size || length > size - used) return ERANGE;
        fields[i] = buffer + used;
        memcpy(fields[i], values[i], length);
        used += length + 2;
    }
    nativeUsed = used - 2;
    *entry = (struct passwd){.pw_name = fields[0], .pw_passwd = fields[1], .pw_uid = nativeUID, .pw_gid = 20,
                            .pw_change = 17, .pw_class = fields[2], .pw_gecos = fields[3], .pw_dir = fields[4],
                            .pw_shell = fields[5], .pw_expire = 29};
    *result = entry;
    return 0;
}
static int nativeGetPWUIDR(uid_t uid, struct passwd *entry, char *buffer, size_t size, struct passwd **result) {
    (void)uid;
    return nativeLookup(entry, buffer, size, result);
}
static int nativeGetPWNAMR(const char *name, struct passwd *entry, char *buffer, size_t size, struct passwd **result) {
    assert(!strcmp(name, "mobile"));
    return nativeLookup(entry, buffer, size, result);
}

#define LSB_USER_HOME_TESTING
#define getpwuid_r nativeGetPWUIDR
#define getpwnam_r nativeGetPWNAMR
#include "../Guest/UserHomeCompatibility.c"
#undef getpwuid_r
#undef getpwnam_r

static void reset(void) {
    nativeHome = "/var/mobile";
    nativeUID = getuid();
    nativeError = 0;
    nativeMissing = false;
    configureUserHome(dataHome);
}

static void checkOtherFields(const struct passwd *entry) {
    assert(!strcmp(entry->pw_name, "mobile") && !strcmp(entry->pw_passwd, "*") && !strcmp(entry->pw_class, ""));
    assert(!strcmp(entry->pw_gecos, "Mobile User") && !strcmp(entry->pw_shell, "/bin/sh"));
    assert(entry->pw_uid == nativeUID && entry->pw_gid == 20 && entry->pw_change == 17 && entry->pw_expire == 29);
}

static void mappingAndOwnership(void) {
    const char *homes[] = {"/var/mobile", "/private/var/mobile"};
    for (unsigned i = 0; i < 2; ++i) {
        reset();
        nativeHome = homes[i];
        struct passwd entry, *result;
        char buffer[512];
        memset(buffer, 0xa5, sizeof(buffer));
        assert(!LSBUserHomeGetPWUIDR(getuid(), &entry, buffer, sizeof(buffer), &result) && result == &entry && errno == ENOTTY);
        assert(!strcmp(entry.pw_dir, dataHome));
        assert(entry.pw_dir >= buffer && entry.pw_dir + strlen(entry.pw_dir) < buffer + sizeof(buffer));
        checkOtherFields(&entry);
        char other[512];
        struct passwd named, *namedResult;
        assert(!LSBUserHomeGetPWNAMR("mobile", &named, other, sizeof(other), &namedResult) && namedResult == &named);
        assert(!strcmp(named.pw_dir, dataHome) && named.pw_dir != entry.pw_dir);
        checkOtherFields(&named);
        memset(other, 0, sizeof(other));
        assert(!strcmp(entry.pw_dir, dataHome));
    }
}

static void passthrough(void) {
    reset();
    struct passwd entry, *result;
    char buffer[512];
    const char *homes[] = {"/var/mobile2", "/var/mobile/Library", "/private/var/root", "/Users/duy", dataHome};
    for (unsigned i = 0; i < sizeof(homes) / sizeof(homes[0]); ++i) {
        nativeHome = homes[i];
        assert(!LSBUserHomeGetPWUIDR(getuid(), &entry, buffer, sizeof(buffer), &result));
        assert(result == &entry && !strcmp(entry.pw_dir, homes[i]) && errno == ENOTTY);
        checkOtherFields(&entry);
    }
    nativeHome = "/var/mobile";
    ++nativeUID;
    assert(!LSBUserHomeGetPWUIDR(nativeUID, &entry, buffer, sizeof(buffer), &result) && !strcmp(entry.pw_dir, nativeHome));
    assert(!LSBUserHomeGetPWNAMR("mobile", &entry, buffer, sizeof(buffer), &result) && !strcmp(entry.pw_dir, nativeHome));
    nativeUID = getuid();
    nativeMissing = true;
    assert(!LSBUserHomeGetPWUIDR(getuid(), &entry, buffer, sizeof(buffer), &result) && !result && errno == ENOTTY);
    nativeMissing = false;
    nativeError = EIO;
    assert(LSBUserHomeGetPWNAMR("mobile", &entry, buffer, sizeof(buffer), &result) == EIO && !result && errno == ENOTTY);
}

static void bufferBoundaries(void) {
    reset();
    struct passwd entry, *result;
    char buffer[512];
    assert(!LSBUserHomeGetPWUIDR(getuid(), &entry, buffer, sizeof(buffer), &result));
    size_t exactSize = nativeUsed + sizeof(dataHome);
    assert(exactSize < sizeof(buffer));
    memset(buffer, 0xa5, sizeof(buffer));
    assert(LSBUserHomeGetPWUIDR(getuid(), &entry, buffer, exactSize - 1, &result) == ERANGE && !result && errno == ENOTTY);
    for (size_t i = nativeUsed; i < sizeof(buffer); ++i) assert((unsigned char)buffer[i] == 0xa5);
    assert(!LSBUserHomeGetPWUIDR(getuid(), &entry, buffer, exactSize, &result) && result == &entry && !strcmp(entry.pw_dir, dataHome));
    for (size_t i = exactSize; i < sizeof(buffer); ++i) assert((unsigned char)buffer[i] == 0xa5);
    checkOtherFields(&entry);
    assert(LSBUserHomeGetPWNAMR("mobile", &entry, buffer, 4, &result) == ERANGE && !result);
}

static void configuration(void) {
    reset();
    const char *invalid[] = {NULL, "", "/", "relative", "/private//data", "/private/./data", "/private/../data", "/data/", "/data:/other"};
    struct passwd entry, *result;
    char buffer[512];
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        configureUserHome(invalid[i]);
        assert(!LSBUserHomeGetPWUIDR(getuid(), &entry, buffer, sizeof(buffer), &result) && !strcmp(entry.pw_dir, nativeHome));
    }
    char oversized[PATH_MAX + 1];
    memset(oversized, 'a', sizeof(oversized)); oversized[0] = '/'; oversized[PATH_MAX] = '\0';
    configureUserHome(oversized);
    assert(!LSBUserHomeGetPWUIDR(getuid(), &entry, buffer, sizeof(buffer), &result) && !strcmp(entry.pw_dir, nativeHome));
}

int main(void) {
    mappingAndOwnership();
    passthrough();
    bufferBoundaries();
    configuration();
    puts("UserHomeCompatibility: current-user homes, retained metadata, caller buffers, ERANGE, lookup failures and path validation passed");
    return 0;
}
