#include "DisplaySettingsControl.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uuid/uuid.h>

extern unsigned dyld_get_active_platform(void);

static int finishRequest(int directory, const uint8_t generation[16], bool abandoned) {
    LSBDSettingsRequest request;
    int error = LSBDSettingsReadFile(directory, "active", &request, sizeof(request));
    if (error) return error;
    error = LSBDSettingsValidateRequest(&request, generation, LSBDSettingsNow());
    if (error == EINVAL) return error; // Preserve malformed input for inspection; never derive filenames from it.
    LSBDSettingsResponse response = {.magic = LSBDSettingsMagic, .version = LSBDSettingsVersion, .operation = request.operation,
        .error = abandoned ? ESTALE : error, .hudError = ENODATA, .touchesError = ENODATA, .hudMode = -1, .touches = -1};
    memcpy(response.generation, request.generation, 16);
    memcpy(response.request, request.request, 16);
    char resultName[40];
    LSBDSettingsResultName(request.request, resultName);
    LSBDSettingsResponse prior;
    int previous = LSBDSettingsReadFile(directory, resultName, &prior, sizeof(prior));
    if (!previous) {
        error = LSBDSettingsValidateResponse(&prior, &request);
        return error ?: (unlinkat(directory, "active", 0) ? errno : 0);
    }
    if (previous != ENOENT) return previous;
    if (!response.error) {
        // Bound a stalled preferences RPC. An in-flight operation can still finish after the host times out;
        // a watchdog exit preserves active, and a replacement worker reports ESTALE without replaying it.
        alarm(10);
        LSBDisplaySettingsApply(request.operation, request.value, &response);
        alarm(0);
    }
    error = LSBDSettingsWriteFile(directory, resultName, &response, sizeof(response), false);
    if (!error && unlinkat(directory, "active", 0)) error = errno;
    return error;
}

int LSBDisplaySettingsServe(void) {
    if (dyld_get_active_platform() != 7) return ENOTSUP;
    int directory = -1;
    int error = LSBDSettingsOpenDirectory(getenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY"), true, &directory);
    if (error) return error;
    int lock = openat(directory, "worker.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat metadata;
    if (lock < 0) error = errno;
    else if (fstat(lock, &metadata)) error = errno;
    else if (!S_ISREG(metadata.st_mode) || metadata.st_uid != geteuid() || metadata.st_nlink != 1 || (metadata.st_mode & 0077)) error = EACCES;
    else if (flock(lock, LOCK_EX | LOCK_NB)) error = errno;
    int queue = error ? -1 : kqueue();
    if (!error && queue < 0) error = errno;
    struct kevent event;
    if (!error) {
        EV_SET(&event, directory, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_DELETE | NOTE_RENAME, 0, NULL);
        if (kevent(queue, &event, 1, NULL, 0, NULL)) error = errno;
    }
    LSBDSettingsReady ready = {.magic = LSBDSettingsMagic, .version = LSBDSettingsVersion, .startedNS = LSBDSettingsNow(), .pid = getpid()};
    uuid_generate_random(ready.generation);
    if (!error) {
        int active = LSBDSettingsAbsent(directory, "active");
        if (active == EEXIST) error = finishRequest(directory, ready.generation, true);
        else if (active) error = active;
    }
    if (!error) error = LSBDSettingsWriteFile(directory, "ready", &ready, sizeof(ready), true);
    if (!error) { fprintf(stderr, "[display-settings] ready pid=%d\n", getpid()); fflush(stderr); }
    while (!error) {
        if (!renameatx_np(directory, "request", directory, "active", RENAME_EXCL)) {
            error = finishRequest(directory, ready.generation, false);
            continue;
        }
        if (errno != ENOENT) { error = errno; break; }
        int count = kevent(queue, NULL, 0, &event, 1, NULL);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { error = errno; break; }
        if (count && ((event.flags & EV_ERROR) || (event.fflags & (NOTE_DELETE | NOTE_RENAME)))) error = EPIPE;
    }
    if (queue >= 0) close(queue);
    if (lock >= 0) close(lock);
    close(directory);
    return error;
}
