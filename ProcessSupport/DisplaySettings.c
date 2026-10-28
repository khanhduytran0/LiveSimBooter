#include "DisplaySettings.h"
#include "RuntimeLayout.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <uuid/uuid.h>

static bool nonzero(const uint8_t bytes[16]) {
    unsigned value = 0;
    for (unsigned i = 0; i < 16; ++i) value |= bytes[i];
    return value != 0;
}

uint64_t LSBDSettingsNow(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

int LSBDSettingsValidateRequest(const LSBDSettingsRequest *request, const uint8_t generation[16], uint64_t now) {
    if (!request || request->magic != LSBDSettingsMagic || request->version != LSBDSettingsVersion ||
        !nonzero(request->generation) || !nonzero(request->request) || !request->deadlineNS) return EINVAL;
    if (request->operation < LSBDSettingsRead || request->operation > LSBDSettingsTouches ||
        (request->operation == LSBDSettingsRead && request->value) ||
        (request->operation == LSBDSettingsHUD && (request->value < 0 || request->value > 9)) ||
        (request->operation == LSBDSettingsTouches && (request->value < 0 || request->value > 1))) return EINVAL;
    if (generation && memcmp(request->generation, generation, 16)) return ESTALE;
    if (!now || request->deadlineNS <= now) return ETIMEDOUT;
    if (request->deadlineNS - now > UINT64_C(30000000000)) return EINVAL;
    return 0;
}

int LSBDSettingsValidateResponse(const LSBDSettingsResponse *response, const LSBDSettingsRequest *request) {
    if (!response || !request || response->magic != LSBDSettingsMagic || response->version != LSBDSettingsVersion ||
        memcmp(response->generation, request->generation, 16) || memcmp(response->request, request->request, 16) ||
        response->operation != request->operation || response->error < 0 || response->hudError < 0 || response->touchesError < 0 ||
        response->hudError > ELAST || response->touchesError > ELAST || response->error > ELAST) return EPROTO;
    if ((!response->hudError && (response->hudMode < 0 || response->hudMode > 9)) ||
        (response->hudError && response->hudMode != -1) ||
        (!response->touchesError && (response->touches < 0 || response->touches > 1)) ||
        (response->touchesError && response->touches != -1)) return EPROTO;
    if (!response->error && ((request->operation == LSBDSettingsHUD && (response->hudError || response->hudMode != request->value)) ||
        (request->operation == LSBDSettingsTouches && (response->touchesError || response->touches != request->value)) ||
        (request->operation == LSBDSettingsRead && (response->hudError || response->touchesError)))) return EPROTO;
    return 0;
}

int LSBDSettingsValidateReady(const LSBDSettingsReady *ready) {
    return !ready || ready->magic != LSBDSettingsMagic || ready->version != LSBDSettingsVersion || !nonzero(ready->generation) ||
        !ready->startedNS || ready->pid <= 0 || ready->reserved ? EPROTO : 0;
}

void LSBDSettingsResultName(const uint8_t request[16], char output[40]) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) { output[i * 2] = hex[request[i] >> 4]; output[i * 2 + 1] = hex[request[i] & 15]; }
    memcpy(output + 32, ".result", 8);
}

static bool validName(const char *name) {
    if (!name || !*name || strlen(name) > 63 || !strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.')) return false;
    return true;
}

static int safeDirectory(int descriptor, bool private) {
    struct stat metadata;
    if (fstat(descriptor, &metadata)) return errno;
    return !S_ISDIR(metadata.st_mode) || metadata.st_uid != geteuid() || (metadata.st_mode & (private ? 0077 : 0022)) ? EACCES : 0;
}

int LSBDSettingsOpenDirectory(const char *dataRoot, bool create, int *directory) {
    if (!directory) return EINVAL;
    *directory = -1;
    char canonical[PATH_MAX], sibling[PATH_MAX];
    if (!dataRoot || dataRoot[0] != '/') return EINVAL;
    if (!realpath(dataRoot, canonical)) return errno;
    char *leaf = strrchr(canonical, '/');
    if (!leaf || strcmp(leaf, "/" LSB_DATA_DIRECTORY)) return EINVAL;
    int length = snprintf(sibling, sizeof(sibling), "%.*s/" LSB_RUNTIME_DIRECTORY, (int)(leaf - canonical), canonical);
    if (length <= 0 || (size_t)length >= sizeof(sibling)) return ENAMETOOLONG;
    int fd = open(sibling, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno;
    int error = safeDirectory(fd, false);
    close(fd);
    if (error) return error;
    fd = open(canonical, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno;
    error = safeDirectory(fd, false);
    const char *components[] = {"var", "run", "display-settings"};
    for (unsigned i = 0; !error && i < 3; ++i) {
        if (create && mkdirat(fd, components[i], i == 2 ? 0700 : 0755) && errno != EEXIST) { error = errno; break; }
        int next = openat(fd, components[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) { error = errno; break; }
        close(fd);
        fd = next;
        error = safeDirectory(fd, i == 2);
    }
    if (error) close(fd);
    else *directory = fd;
    return error;
}

static int safeFile(const struct stat *metadata, size_t size) {
    return !S_ISREG(metadata->st_mode) || metadata->st_uid != geteuid() || metadata->st_nlink != 1 ||
        (metadata->st_mode & 0077) || metadata->st_size != (off_t)size ? EINVAL : 0;
}

int LSBDSettingsReadFile(int directory, const char *name, void *bytes, size_t size) {
    if (!validName(name) || !bytes || !size || size > sizeof(LSBDSettingsResponse)) return EINVAL;
    int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno;
    struct stat metadata;
    int error = fstat(fd, &metadata) ? errno : safeFile(&metadata, size);
    size_t offset = 0;
    while (!error && offset < size) {
        ssize_t count = read(fd, (char *)bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) error = count ? errno : EIO;
        else offset += (size_t)count;
    }
    if (!error) {
        char extra;
        ssize_t count;
        do count = read(fd, &extra, 1); while (count < 0 && errno == EINTR);
        if (count) error = count < 0 ? errno : EAGAIN;
    }
    close(fd);
    return error;
}

int LSBDSettingsAbsent(int directory, const char *name) {
    if (!validName(name)) return EINVAL;
    struct stat metadata;
    if (!fstatat(directory, name, &metadata, AT_SYMLINK_NOFOLLOW)) return EEXIST;
    return errno == ENOENT ? 0 : errno;
}

int LSBDSettingsWriteFile(int directory, const char *name, const void *bytes, size_t size, bool replace) {
    if (!validName(name) || !bytes || !size || size > sizeof(LSBDSettingsResponse) || (replace && strcmp(name, "ready"))) return EINVAL;
    if (replace) {
        struct stat metadata;
        if (!fstatat(directory, name, &metadata, AT_SYMLINK_NOFOLLOW)) {
            int error = safeFile(&metadata, size);
            if (error) return error;
        } else if (errno != ENOENT) return errno;
    }
    uuid_t identifier;
    uuid_generate_random(identifier);
    char temporary[40];
    LSBDSettingsResultName(identifier, temporary);
    memcpy(temporary + 32, ".new", 5);
    int fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return errno;
    int error = 0;
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = write(fd, (const char *)bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = count ? errno : EIO; break; }
        offset += (size_t)count;
    }
    if (close(fd) && !error) error = errno;
    if (!error && renameatx_np(directory, temporary, directory, name, replace ? 0 : RENAME_EXCL)) error = errno;
    if (error) unlinkat(directory, temporary, 0);
    return error;
}
