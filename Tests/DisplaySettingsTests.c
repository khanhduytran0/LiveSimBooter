#include "../ProcessSupport/DisplaySettings.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const uint64_t now = UINT64_C(100000000000);
static const uint8_t generation[16] = {1, 2, 3, 4};

static LSBDSettingsRequest requestFor(uint32_t operation, int32_t value) {
    LSBDSettingsRequest request = {.magic = LSBDSettingsMagic, .version = LSBDSettingsVersion,
        .operation = operation, .value = value, .deadlineNS = now + UINT64_C(1000000000)};
    memcpy(request.generation, generation, sizeof(generation));
    for (unsigned i = 0; i < sizeof(request.request); ++i) request.request[i] = (uint8_t)(i + 0xa0);
    return request;
}

static LSBDSettingsResponse responseFor(const LSBDSettingsRequest *request) {
    LSBDSettingsResponse response = {.magic = LSBDSettingsMagic, .version = LSBDSettingsVersion,
        .operation = request->operation, .hudMode = 3, .touches = 1};
    memcpy(response.generation, request->generation, sizeof(response.generation));
    memcpy(response.request, request->request, sizeof(response.request));
    if (request->operation == LSBDSettingsHUD) response.hudMode = request->value;
    if (request->operation == LSBDSettingsTouches) response.touches = request->value;
    return response;
}

static void testRequests(void) {
    LSBDSettingsRequest valid = requestFor(LSBDSettingsRead, 0), request;
    assert(LSBDSettingsValidateRequest(NULL, generation, now) == EINVAL);
    assert(!LSBDSettingsValidateRequest(&valid, generation, now));
    assert(!LSBDSettingsValidateRequest(&valid, NULL, now));
    for (int32_t value = 0; value <= 9; ++value) {
        request = requestFor(LSBDSettingsHUD, value);
        assert(!LSBDSettingsValidateRequest(&request, generation, now));
    }
    for (int32_t value = 0; value <= 1; ++value) {
        request = requestFor(LSBDSettingsTouches, value);
        assert(!LSBDSettingsValidateRequest(&request, generation, now));
    }
    const struct { uint32_t operation; int32_t value; } invalid[] = {
        {0, 0}, {LSBDSettingsTouches + 1, 0}, {UINT32_MAX, 0}, {LSBDSettingsRead, 1}, {LSBDSettingsRead, -1},
        {LSBDSettingsHUD, -1}, {LSBDSettingsHUD, 10}, {LSBDSettingsHUD, INT32_MAX},
        {LSBDSettingsTouches, -1}, {LSBDSettingsTouches, 2}, {LSBDSettingsTouches, INT32_MIN}
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        request = requestFor(invalid[i].operation, invalid[i].value);
        assert(LSBDSettingsValidateRequest(&request, generation, now) == EINVAL);
    }
    request = valid; request.magic ^= 1;
    assert(LSBDSettingsValidateRequest(&request, generation, now) == EINVAL);
    request = valid; ++request.version;
    assert(LSBDSettingsValidateRequest(&request, generation, now) == EINVAL);
    request = valid; memset(request.generation, 0, sizeof(request.generation));
    assert(LSBDSettingsValidateRequest(&request, generation, now) == EINVAL);
    request = valid; memset(request.request, 0, sizeof(request.request));
    assert(LSBDSettingsValidateRequest(&request, generation, now) == EINVAL);
    request = valid; request.generation[15] ^= 1;
    assert(LSBDSettingsValidateRequest(&request, generation, now) == ESTALE);
    request = valid; request.deadlineNS = 0;
    assert(LSBDSettingsValidateRequest(&request, generation, now) == EINVAL);
    request = valid; request.deadlineNS = now;
    assert(LSBDSettingsValidateRequest(&request, generation, now) == ETIMEDOUT);
    request.deadlineNS = now - 1;
    assert(LSBDSettingsValidateRequest(&request, generation, now) == ETIMEDOUT);
    request.deadlineNS = now + 1;
    assert(!LSBDSettingsValidateRequest(&request, generation, now));
    request.deadlineNS = now + UINT64_C(30000000000);
    assert(!LSBDSettingsValidateRequest(&request, generation, now));
    ++request.deadlineNS;
    assert(LSBDSettingsValidateRequest(&request, generation, now) == EINVAL);
    request.deadlineNS = UINT64_MAX;
    assert(LSBDSettingsValidateRequest(&request, generation, now) == EINVAL);
    assert(!LSBDSettingsValidateRequest(&request, generation, UINT64_MAX - 1));
    assert(LSBDSettingsValidateRequest(&valid, generation, 0) == ETIMEDOUT);
    uint64_t first = LSBDSettingsNow(), second = LSBDSettingsNow();
    assert(first && second >= first);
}

static void testResponses(void) {
    LSBDSettingsRequest request = requestFor(LSBDSettingsRead, 0);
    LSBDSettingsResponse valid = responseFor(&request), response;
    assert(!LSBDSettingsValidateResponse(&valid, &request));
    assert(LSBDSettingsValidateResponse(NULL, &request) == EPROTO);
    assert(LSBDSettingsValidateResponse(&valid, NULL) == EPROTO);
    response = valid; response.magic ^= 1;
    assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);
    response = valid; ++response.version;
    assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);
    response = valid; response.generation[15] ^= 1;
    assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);
    response = valid; response.request[15] ^= 1;
    assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);
    response = valid; response.operation = LSBDSettingsHUD;
    assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);
    const int32_t errors[] = {-1, ELAST + 1, INT32_MAX};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        response = valid; response.error = errors[i];
        assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);
        response = valid; response.hudError = errors[i]; response.hudMode = -1;
        assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);
        response = valid; response.touchesError = errors[i]; response.touches = -1;
        assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);
    }
    const int32_t invalidHUD[] = {-1, 10}, invalidTouches[] = {-1, 2};
    for (unsigned i = 0; i < 2; ++i) {
        response = valid; response.hudMode = invalidHUD[i];
        assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);
        response = valid; response.touches = invalidTouches[i];
        assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);
    }
    response = valid; response.error = EIO; response.hudError = EIO;
    assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);
    response.hudMode = -1;
    assert(!LSBDSettingsValidateResponse(&response, &request));
    response.error = 0; // A partial read must not look like a successful refresh.
    assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);
    response = valid; response.error = EIO; response.touchesError = EIO;
    assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);
    response.touches = -1;
    assert(!LSBDSettingsValidateResponse(&response, &request));
    response.error = 0;
    assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);

    request = requestFor(LSBDSettingsHUD, 9); response = responseFor(&request);
    assert(!LSBDSettingsValidateResponse(&response, &request));
    response.hudMode = 8;
    assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);
    response.hudMode = -1; response.hudError = EIO;
    assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);
    response.error = EIO;
    assert(!LSBDSettingsValidateResponse(&response, &request));
    response = responseFor(&request); response.touchesError = ENOTSUP; response.touches = -1;
    assert(!LSBDSettingsValidateResponse(&response, &request)); // Unrelated query failure does not invalidate the verified HUD setter.
    request = requestFor(LSBDSettingsTouches, 0); response = responseFor(&request);
    assert(!LSBDSettingsValidateResponse(&response, &request));
    response.touches = 1;
    assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);
    response.touches = -1; response.touchesError = EIO;
    assert(LSBDSettingsValidateResponse(&response, &request) == EPROTO);
    response.error = EIO;
    assert(!LSBDSettingsValidateResponse(&response, &request));
    response = responseFor(&request); response.hudError = ENOTSUP; response.hudMode = -1;
    assert(!LSBDSettingsValidateResponse(&response, &request));
}

static void testReadyAndNames(void) {
    LSBDSettingsReady valid = {.magic = LSBDSettingsMagic, .version = LSBDSettingsVersion, .startedNS = now, .pid = getpid()}, ready;
    memcpy(valid.generation, generation, sizeof(generation));
    assert(!LSBDSettingsValidateReady(&valid));
    assert(LSBDSettingsValidateReady(NULL) == EPROTO);
    ready = valid; ready.magic ^= 1;
    assert(LSBDSettingsValidateReady(&ready) == EPROTO);
    ready = valid; ++ready.version;
    assert(LSBDSettingsValidateReady(&ready) == EPROTO);
    ready = valid; memset(ready.generation, 0, sizeof(ready.generation));
    assert(LSBDSettingsValidateReady(&ready) == EPROTO);
    ready = valid; ready.startedNS = 0;
    assert(LSBDSettingsValidateReady(&ready) == EPROTO);
    ready = valid; ready.pid = 0;
    assert(LSBDSettingsValidateReady(&ready) == EPROTO);
    ready.pid = -1;
    assert(LSBDSettingsValidateReady(&ready) == EPROTO);
    ready = valid; ready.reserved = 1;
    assert(LSBDSettingsValidateReady(&ready) == EPROTO);
    struct { char name[40]; unsigned char guard; } output = {.guard = 0xa5};
    LSBDSettingsRequest request = requestFor(LSBDSettingsRead, 0);
    LSBDSettingsResultName(request.request, output.name);
    assert(!strcmp(output.name, "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf.result") && output.guard == 0xa5);
}

static void removeTree(int directory) {
    int copy = dup(directory);
    assert(copy >= 0);
    DIR *stream = fdopendir(copy);
    assert(stream);
    struct dirent *entry;
    while ((entry = readdir(stream))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat metadata;
        assert(!fstatat(directory, entry->d_name, &metadata, AT_SYMLINK_NOFOLLOW));
        if (S_ISDIR(metadata.st_mode)) {
            int child = openat(directory, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            assert(child >= 0);
            removeTree(child);
            assert(!close(child));
            assert(!unlinkat(directory, entry->d_name, AT_REMOVEDIR));
        } else assert(!unlinkat(directory, entry->d_name, 0));
    }
    assert(!closedir(stream));
}

static void expectRejectedRead(int directory, const char *name, size_t size) {
    unsigned char bytes[sizeof(LSBDSettingsResponse)], original[sizeof(bytes)];
    memset(bytes, 0xa5, sizeof(bytes));
    memcpy(original, bytes, sizeof(bytes));
    assert(LSBDSettingsReadFile(directory, name, bytes, size));
    assert(!memcmp(bytes, original, sizeof(bytes)));
}

static void testFiles(int directory) {
    LSBDSettingsRequest first = requestFor(LSBDSettingsHUD, 1), second = requestFor(LSBDSettingsHUD, 2), readback;
    assert(!LSBDSettingsAbsent(directory, "request"));
    assert(!LSBDSettingsWriteFile(directory, "request", &first, sizeof(first), false));
    assert(LSBDSettingsAbsent(directory, "request") == EEXIST);
    struct stat metadata;
    assert(!fstatat(directory, "request", &metadata, AT_SYMLINK_NOFOLLOW));
    assert(S_ISREG(metadata.st_mode) && (metadata.st_mode & 0777) == 0600 && metadata.st_nlink == 1 && metadata.st_uid == geteuid());
    assert(LSBDSettingsWriteFile(directory, "request", &second, sizeof(second), false) == EEXIST);
    assert(LSBDSettingsWriteFile(directory, "request", &second, sizeof(second), true) == EINVAL);
    assert(!LSBDSettingsReadFile(directory, "request", &readback, sizeof(readback)) && !memcmp(&readback, &first, sizeof(first)));
    expectRejectedRead(directory, "request", sizeof(first) - 1);
    expectRejectedRead(directory, "request", sizeof(first) + 1);
    assert(!fchmodat(directory, "request", 0640, 0));
    expectRejectedRead(directory, "request", sizeof(first));
    assert(!fchmodat(directory, "request", 0600, 0));
    assert(!linkat(directory, "request", directory, "hardlink", 0));
    expectRejectedRead(directory, "request", sizeof(first));
    expectRejectedRead(directory, "hardlink", sizeof(first));
    assert(LSBDSettingsWriteFile(directory, "hardlink", &second, sizeof(second), false) == EEXIST);
    assert(!unlinkat(directory, "hardlink", 0));
    assert(!symlinkat("request", directory, "link"));
    expectRejectedRead(directory, "link", sizeof(first));
    assert(LSBDSettingsAbsent(directory, "link") == EEXIST);
    assert(LSBDSettingsWriteFile(directory, "link", &second, sizeof(second), false) == EEXIST);
    assert(!fstatat(directory, "link", &metadata, AT_SYMLINK_NOFOLLOW) && S_ISLNK(metadata.st_mode));
    assert(!LSBDSettingsReadFile(directory, "request", &readback, sizeof(readback)) && !memcmp(&readback, &first, sizeof(first)));
    assert(!symlinkat("absent-target", directory, "dangling"));
    assert(LSBDSettingsAbsent(directory, "dangling") == EEXIST);
    assert(LSBDSettingsWriteFile(directory, "dangling", &second, sizeof(second), false) == EEXIST);
    expectRejectedRead(directory, "dangling", sizeof(first));
    assert(!mkfifoat(directory, "fifo", 0600));
    expectRejectedRead(directory, "fifo", sizeof(first)); // Must reject without blocking on a FIFO writer.
    assert(!mkdirat(directory, "directory", 0700));
    expectRejectedRead(directory, "directory", sizeof(first));

    const char *invalid[] = {NULL, "", ".", "..", "../escape", "sub/file", "/absolute", "UPPER", "with space", "back\\slash"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(LSBDSettingsAbsent(directory, invalid[i]) == EINVAL);
        assert(LSBDSettingsReadFile(directory, invalid[i], &readback, sizeof(readback)) == EINVAL);
        assert(LSBDSettingsWriteFile(directory, invalid[i], &first, sizeof(first), false) == EINVAL);
    }
    char tooLong[65]; memset(tooLong, 'a', sizeof(tooLong) - 1); tooLong[sizeof(tooLong) - 1] = 0;
    assert(LSBDSettingsWriteFile(directory, tooLong, &first, sizeof(first), false) == EINVAL);
    assert(LSBDSettingsReadFile(directory, "request", NULL, sizeof(first)) == EINVAL);
    assert(LSBDSettingsReadFile(directory, "request", &readback, 0) == EINVAL);
    assert(LSBDSettingsReadFile(directory, "request", &readback, sizeof(LSBDSettingsResponse) + 1) == EINVAL);
    assert(LSBDSettingsWriteFile(directory, "invalid", NULL, sizeof(first), false) == EINVAL);
    assert(LSBDSettingsWriteFile(directory, "invalid", &first, 0, false) == EINVAL);
    assert(LSBDSettingsWriteFile(directory, "invalid", &first, sizeof(LSBDSettingsResponse) + 1, false) == EINVAL);

    LSBDSettingsReady old = {.magic = LSBDSettingsMagic, .version = LSBDSettingsVersion, .startedNS = now, .pid = getpid()}, newer, ready;
    memcpy(old.generation, generation, sizeof(generation));
    newer = old; newer.generation[15] = 1; ++newer.startedNS;
    assert(!LSBDSettingsWriteFile(directory, "ready", &old, sizeof(old), true));
    assert(!LSBDSettingsWriteFile(directory, "ready", &newer, sizeof(newer), true));
    assert(!LSBDSettingsReadFile(directory, "ready", &ready, sizeof(ready)) && !memcmp(&ready, &newer, sizeof(ready)));
    assert(!linkat(directory, "ready", directory, "ready-link", 0));
    assert(LSBDSettingsWriteFile(directory, "ready", &old, sizeof(old), true) == EINVAL);
    assert(!unlinkat(directory, "ready-link", 0));
    assert(!LSBDSettingsReadFile(directory, "ready", &ready, sizeof(ready)) && !memcmp(&ready, &newer, sizeof(ready)));
    assert(!unlinkat(directory, "ready", 0));
    assert(!symlinkat("request", directory, "ready"));
    assert(LSBDSettingsWriteFile(directory, "ready", &old, sizeof(old), true) == EINVAL);
    assert(!fstatat(directory, "ready", &metadata, AT_SYMLINK_NOFOLLOW) && S_ISLNK(metadata.st_mode));
    assert(!unlinkat(directory, "ready", 0));
}

static void testExclusiveWriters(int directory) {
    int gate[2];
    assert(!pipe(gate));
    pid_t writers[2];
    LSBDSettingsRequest requests[] = {requestFor(LSBDSettingsHUD, 1), requestFor(LSBDSettingsHUD, 9)}, actual;
    for (unsigned i = 0; i < 2; ++i) {
        writers[i] = fork();
        assert(writers[i] >= 0);
        if (!writers[i]) {
            close(gate[1]);
            char byte;
            if (read(gate[0], &byte, 1) != 1) _exit(20);
            int error = LSBDSettingsWriteFile(directory, "concurrent", &requests[i], sizeof(requests[i]), false);
            _exit(!error ? 0 : error == EEXIST ? 10 : 21);
        }
    }
    assert(!close(gate[0]));
    assert(write(gate[1], "go", 2) == 2);
    assert(!close(gate[1]));
    unsigned winners = 0, winner = 0;
    for (unsigned i = 0; i < 2; ++i) {
        int status;
        assert(waitpid(writers[i], &status, 0) == writers[i] && WIFEXITED(status));
        assert(WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 10);
        if (!WEXITSTATUS(status)) { ++winners; winner = i; }
    }
    assert(winners == 1);
    assert(!LSBDSettingsReadFile(directory, "concurrent", &actual, sizeof(actual)) && !memcmp(&actual, &requests[winner], sizeof(actual)));
    int copy = dup(directory);
    assert(copy >= 0);
    DIR *stream = fdopendir(copy);
    assert(stream);
    struct dirent *entry;
    while ((entry = readdir(stream))) assert(!strstr(entry->d_name, ".new"));
    assert(!closedir(stream));
}

static void testDirectoriesAndFiles(void) {
    char fixture[] = "/tmp/lsb-display-settings-XXXXXX", dataRoot[PATH_MAX];
    assert(mkdtemp(fixture));
    int root = open(fixture, O_RDONLY | O_DIRECTORY | O_CLOEXEC), directory = 123;
    assert(root >= 0);
    assert(!mkdirat(root, "data", 0700));
    assert(snprintf(dataRoot, sizeof(dataRoot), "%s/data", fixture) < (int)sizeof(dataRoot));
    assert(LSBDSettingsOpenDirectory(dataRoot, true, &directory) == ENOENT && directory == -1);
    assert(!mkdirat(root, "RuntimeRoot", 0755));
    assert(LSBDSettingsOpenDirectory(dataRoot, false, &directory) == ENOENT && directory == -1);
    assert(!LSBDSettingsOpenDirectory(dataRoot, true, &directory));
    assert(fcntl(directory, F_GETFD) & FD_CLOEXEC);
    struct stat metadata;
    assert(!fstat(directory, &metadata) && (metadata.st_mode & 0777) == 0700 && metadata.st_uid == geteuid());
    int reopened = -1;
    assert(!LSBDSettingsOpenDirectory(dataRoot, false, &reopened));
    struct stat other;
    assert(!fstat(reopened, &other) && metadata.st_dev == other.st_dev && metadata.st_ino == other.st_ino);
    assert(!close(reopened));
    testFiles(directory);
    testExclusiveWriters(directory);
    assert(!close(directory));

    const char *components[] = {"RuntimeRoot", "data", "data/var", "data/var/run", "data/var/run/display-settings"};
    for (unsigned i = 0; i < sizeof(components) / sizeof(components[0]); ++i) {
        assert(!fchmodat(root, components[i], i == 4 ? 0750 : 0775, 0));
        assert(LSBDSettingsOpenDirectory(dataRoot, false, &directory) == EACCES && directory == -1);
        assert(!fchmodat(root, components[i], i == 4 ? 0700 : 0755, 0));
    }
    const char *links[] = {"RuntimeRoot", "data/var", "data/var/run", "data/var/run/display-settings"};
    for (unsigned i = 0; i < sizeof(links) / sizeof(links[0]); ++i) {
        char target[PATH_MAX];
        assert(snprintf(target, sizeof(target), "%s/%s-saved", fixture, links[i]) < (int)sizeof(target));
        char saved[PATH_MAX];
        assert(snprintf(saved, sizeof(saved), "%s-saved", links[i]) < (int)sizeof(saved));
        assert(!renameat(root, links[i], root, saved));
        assert(!symlinkat(target, root, links[i]));
        assert(LSBDSettingsOpenDirectory(dataRoot, false, &directory) && directory == -1);
        assert(!unlinkat(root, links[i], 0));
        assert(!renameat(root, saved, root, links[i]));
    }
    assert(LSBDSettingsOpenDirectory(NULL, false, &directory) == EINVAL && directory == -1);
    assert(LSBDSettingsOpenDirectory("data", false, &directory) == EINVAL && directory == -1);
    assert(LSBDSettingsOpenDirectory(fixture, false, &directory) == EINVAL && directory == -1);
    assert(LSBDSettingsOpenDirectory(dataRoot, false, NULL) == EINVAL);
    assert(!mkdirat(root, "SimulatorData", 0700));
    char previousData[PATH_MAX];
    assert(snprintf(previousData, sizeof(previousData), "%s/SimulatorData", fixture) < (int)sizeof(previousData));
    assert(LSBDSettingsOpenDirectory(previousData, false, &directory) == EINVAL && directory == -1);
    removeTree(root);
    assert(!close(root));
    assert(!rmdir(fixture));
}

int main(void) {
    mode_t oldMask = umask(0077);
    testRequests();
    testResponses();
    testReadyAndNames();
    testDirectoriesAndFiles();
    umask(oldMask);
    puts("Display settings tests passed");
    return 0;
}
