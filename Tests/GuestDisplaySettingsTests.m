#include "../ProcessSupport/RuntimeLayout.h"
#import <Foundation/Foundation.h>
#import "../Host/GuestDisplaySettings.h"
#include "../ProcessSupport/DisplaySettings.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uuid/uuid.h>

static void spinUntil(BOOL (^condition)(void)) {
    NSDate *limit = [NSDate dateWithTimeIntervalSinceNow:3];
    while (!condition() && limit.timeIntervalSinceNow > 0)
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
    assert(condition());
}
static LSBDSettingsRequest takeRequest(int directory) {
    __block LSBDSettingsRequest request;
    spinUntil(^BOOL { return !LSBDSettingsReadFile(directory, "request", &request, sizeof(request)); });
    assert(!renameatx_np(directory, "request", directory, "active", RENAME_EXCL));
    return request;
}
static void reply(int directory, const LSBDSettingsRequest *request, int hud, int touches, BOOL malformed) {
    LSBDSettingsResponse response = {.magic = LSBDSettingsMagic, .version = LSBDSettingsVersion, .operation = request->operation,
        .hudMode = hud, .touches = touches};
    memcpy(response.generation, request->generation, 16);
    memcpy(response.request, request->request, 16);
    if (malformed) response.magic++;
    char name[40];
    LSBDSettingsResultName(request->request, name);
    assert(!LSBDSettingsWriteFile(directory, name, &response, sizeof(response), false));
    assert(!unlinkat(directory, "active", 0));
}
static void finished(LSBGuestDisplaySettings *settings) { spinUntil(^BOOL { return !settings.pending; }); }
int main(void) {
    @autoreleasepool {
        char base[] = "/tmp/LiveSimBooter-settings-model.XXXXXX";
        assert(mkdtemp(base));
        NSString *group = [[NSString stringWithUTF8String:base] stringByResolvingSymlinksInPath];
        NSString *build = [group stringByAppendingPathComponent:@LSB_STORAGE_BUILD_RELATIVE_PATH];
        NSString *data = [build stringByAppendingPathComponent:@LSB_DATA_DIRECTORY];
        assert([NSFileManager.defaultManager createDirectoryAtPath:data withIntermediateDirectories:YES attributes:nil error:NULL]);
        assert(!mkdir([build stringByAppendingPathComponent:@LSB_RUNTIME_DIRECTORY].fileSystemRepresentation, 0700));
        LSBGuestDisplaySettings *model = [[LSBGuestDisplaySettings alloc] initWithDataRoot:[NSURL fileURLWithPath:data] timeout:0.15];
        assert(model.hudMode == -1 && model.touches == -1 && !model.pending && !model.outcomeUnknown);
        [model refresh]; finished(model);
        assert(model.hudMode == -1 && model.touches == -1 && !model.outcomeUnknown);
        int directory = -1;
        assert(!LSBDSettingsOpenDirectory(data.fileSystemRepresentation, true, &directory));
        LSBDSettingsReady ready = {.magic = LSBDSettingsMagic, .version = LSBDSettingsVersion, .startedNS = LSBDSettingsNow(), .pid = getpid()};
        uuid_generate_random(ready.generation);
        assert(!LSBDSettingsWriteFile(directory, "ready", &ready, sizeof(ready), true));
        [model refresh];
        LSBDSettingsRequest request = takeRequest(directory);
        assert(request.operation == LSBDSettingsRead);
        reply(directory, &request, 7, 1, NO); finished(model);
        assert(model.hudMode == 7 && model.touches == 1 && !model.outcomeUnknown);
        [model setHUDMode:3];
        request = takeRequest(directory);
        assert(request.operation == LSBDSettingsHUD && request.value == 3);
        assert(model.pending && model.hudMode == 7); // No optimistic value before readback.
        reply(directory, &request, 3, 1, NO); finished(model);
        assert(model.hudMode == 3 && !model.outcomeUnknown);
        [model setVisualizeTouches:NO];
        request = takeRequest(directory);
        assert(request.operation == LSBDSettingsTouches && request.value == 0);
        finished(model);
        assert(model.outcomeUnknown && model.hudMode == -1 && model.touches == -1);
        [model setHUDMode:8];
        assert(!model.pending && LSBDSettingsAbsent(directory, "request") == 0); // No setter replay or replacement after timeout.
        reply(directory, &request, 3, 0, NO);
        [model refresh]; finished(model);
        assert(!model.outcomeUnknown && model.hudMode == 3 && model.touches == 0 && LSBDSettingsAbsent(directory, "request") == 0);
        [model refresh]; request = takeRequest(directory);
        reply(directory, &request, 100, 500, YES); finished(model);
        assert(model.outcomeUnknown && model.hudMode == -1 && model.touches == -1);
        uuid_generate_random(ready.generation);
        assert(!LSBDSettingsWriteFile(directory, "ready", &ready, sizeof(ready), true));
        [model refresh]; finished(model);
        assert(!model.outcomeUnknown && model.hudMode == -1 && LSBDSettingsAbsent(directory, "request") == 0);
        [model refresh]; request = takeRequest(directory);
        assert(!memcmp(request.generation, ready.generation, 16));
        reply(directory, &request, 0, 0, NO); finished(model);
        assert(model.hudMode == 0 && model.touches == 0 && !model.outcomeUnknown);
        close(directory);
        assert([NSFileManager.defaultManager removeItemAtPath:group error:NULL]);
    }
    puts("Guest display-settings model tests passed");
}
