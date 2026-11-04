#include "UserHomeCompatibility.h"
#include "SpawnDiagnostics.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef LSB_TRANSPORT_TRACE
#include <stdatomic.h>
#endif

static char accountHome[PATH_MAX];
#ifdef LSB_TRANSPORT_TRACE
static _Atomic(unsigned) homeTraceCount;
#endif

static void configureUserHome(const char *path) {
    accountHome[0] = '\0';
    if (!path || path[0] != '/') return;
    size_t length = strnlen(path, sizeof(accountHome));
    if (length < 2 || length == sizeof(accountHome) || path[length - 1] == '/' || strchr(path, ':')) return;
    for (const char *component = path + 1; *component;) {
        const char *end = strchr(component, '/');
        size_t size = end ? (size_t)(end - component) : strlen(component);
        if (!size || (size == 1 && component[0] == '.') || (size == 2 && component[0] == '.' && component[1] == '.')) return;
        component += size + (end != NULL);
    }
    memcpy(accountHome, path, length + 1);
}

#ifndef LSB_USER_HOME_TESTING
__attribute__((constructor)) static void initializeUserHome(void) {
    // Snapshot once before guest threads start. HOME can be an application's container, while this is the account's shared data home.
    configureUserHome(getenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY"));
}
#endif

static int translateHome(int error, char *buffer, size_t size, struct passwd **result) {
    if (error || !accountHome[0] || !result || !*result) return error;
    struct passwd *entry = *result;
    if (entry->pw_uid != getuid() || !entry->pw_dir ||
        (strcmp(entry->pw_dir, "/var/mobile") && strcmp(entry->pw_dir, "/private/var/mobile"))) return 0;
    if (!buffer) { *result = NULL; return ERANGE; }

    // The native lookup can arrange strings in any order. Append beyond all of them, preserving aliases and every other field.
    const char *fields[] = {entry->pw_name, entry->pw_passwd, entry->pw_class, entry->pw_gecos, entry->pw_dir, entry->pw_shell};
    uintptr_t base = (uintptr_t)buffer;
    size_t used = 0;
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        uintptr_t address = (uintptr_t)fields[i];
        if (!fields[i] || address < base || address - base >= size) continue;
        size_t offset = (size_t)(address - base);
        const char *end = memchr(fields[i], '\0', size - offset);
        if (!end) { *result = NULL; return ERANGE; }
        size_t after = offset + (size_t)(end - fields[i]) + 1;
        if (after > used) used = after;
    }
    size_t required = strlen(accountHome) + 1;
    if (required > size - used) { *result = NULL; return ERANGE; }
#ifdef LSB_TRANSPORT_TRACE
    if (atomic_fetch_add_explicit(&homeTraceCount, 1, memory_order_relaxed) < 2)
        LSBSpawnTrace("[user-home] uid=%u %s -> %s\n", entry->pw_uid, entry->pw_dir, accountHome);
#endif
    memcpy(buffer + used, accountHome, required);
    entry->pw_dir = buffer + used;
    return 0;
}

int LSBUserHomeGetPWUIDR(uid_t uid, struct passwd *entry, char *buffer, size_t size, struct passwd **result) {
    int error = getpwuid_r(uid, entry, buffer, size, result);
    return translateHome(error, buffer, size, result);
}

int LSBUserHomeGetPWNAMR(const char *name, struct passwd *entry, char *buffer, size_t size, struct passwd **result) {
    int error = getpwnam_r(name, entry, buffer, size, result);
    return translateHome(error, buffer, size, result);
}

#ifndef LSB_USER_HOME_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBUserHomeGetPWUIDR, (const void *)&getpwuid_r}, {(const void *)&LSBUserHomeGetPWNAMR, (const void *)&getpwnam_r}
};
#endif
