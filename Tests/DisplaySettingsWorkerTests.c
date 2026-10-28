#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include "../Guest/DisplaySettingsControl.h"

static unsigned applies, alarms;
unsigned dyld_get_active_platform(void) { return 7; }
void LSBDisplaySettingsApply(uint32_t operation, int32_t value, LSBDSettingsResponse *response) {
    applies++;
    response->operation = operation;
    response->error = response->hudError = response->touchesError = 0;
    response->hudMode = operation == LSBDSettingsHUD ? value : 2;
    response->touches = operation == LSBDSettingsTouches ? value : 1;
}
static unsigned mockAlarm(unsigned seconds) { assert(seconds == 10 || seconds == 0); alarms++; return 0; }
#define alarm mockAlarm
#include "../Guest/DisplaySettingsWorker.c"
#undef alarm

static LSBDSettingsRequest request(const uint8_t generation[16]) {
    LSBDSettingsRequest result = {.magic = LSBDSettingsMagic, .version = LSBDSettingsVersion, .operation = LSBDSettingsHUD,
        .value = 7, .deadlineNS = LSBDSettingsNow() + 5000000000ull};
    memcpy(result.generation, generation, 16);
    uuid_generate_random(result.request);
    return result;
}
static LSBDSettingsResponse resultFor(int directory, const LSBDSettingsRequest *request) {
    char name[40]; LSBDSettingsResultName(request->request, name);
    LSBDSettingsResponse result;
    assert(!LSBDSettingsReadFile(directory, name, &result, sizeof(result)));
    assert(!LSBDSettingsValidateResponse(&result, request));
    return result;
}
int main(void) {
    char base[] = "/tmp/LiveSimBooter-settings-worker.XXXXXX", data[1024], root[1024];
    assert(mkdtemp(base));
    snprintf(data, sizeof(data), "%s/data", base);
    snprintf(root, sizeof(root), "%s/RuntimeRoot", base);
    assert(!mkdir(data, 0700) && !mkdir(root, 0700));
    int directory;
    assert(!LSBDSettingsOpenDirectory(data, true, &directory));
    uint8_t generation[16]; uuid_generate_random(generation);
    LSBDSettingsRequest active = request(generation);
    assert(!LSBDSettingsWriteFile(directory, "active", &active, sizeof(active), false));
    assert(!finishRequest(directory, generation, false));
    assert(applies == 1 && alarms == 2 && resultFor(directory, &active).hudMode == 7 && !LSBDSettingsAbsent(directory, "active"));
    // A worker interrupted after writing its result must not apply the same change twice.
    assert(!LSBDSettingsWriteFile(directory, "active", &active, sizeof(active), false));
    assert(!finishRequest(directory, generation, false) && applies == 1 && alarms == 2);
    active = request(generation); active.deadlineNS = LSBDSettingsNow() - 1;
    assert(!LSBDSettingsWriteFile(directory, "active", &active, sizeof(active), false));
    assert(!finishRequest(directory, generation, false) && resultFor(directory, &active).error == ETIMEDOUT && applies == 1);
    active = request(generation); active.generation[0] ^= 0xff;
    assert(!LSBDSettingsWriteFile(directory, "active", &active, sizeof(active), false));
    assert(!finishRequest(directory, generation, false) && resultFor(directory, &active).error == ESTALE && applies == 1);
    active = request(generation);
    assert(!LSBDSettingsWriteFile(directory, "active", &active, sizeof(active), false));
    assert(!finishRequest(directory, generation, true) && resultFor(directory, &active).error == ESTALE && applies == 1);
    // Malformed input is never used to derive a path or invoke the settings implementation.
    active = request(generation); active.operation = 99;
    assert(!LSBDSettingsWriteFile(directory, "active", &active, sizeof(active), false));
    assert(finishRequest(directory, generation, false) == EINVAL && LSBDSettingsAbsent(directory, "active") == EEXIST && applies == 1);
    assert(!unlinkat(directory, "active", 0));
    // A result-file collision must preserve evidence and refuse a second operation.
    active = request(generation);
    char name[40]; LSBDSettingsResultName(active.request, name);
    LSBDSettingsResponse wrong = {0};
    assert(!LSBDSettingsWriteFile(directory, name, &wrong, sizeof(wrong), false));
    assert(!LSBDSettingsWriteFile(directory, "active", &active, sizeof(active), false));
    assert(finishRequest(directory, generation, false) == EPROTO && applies == 1 && alarms == 2);
    DIR *entries = fdopendir(dup(directory)); assert(entries);
    struct dirent *entry;
    while ((entry = readdir(entries))) if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) assert(!unlinkat(directory, entry->d_name, 0));
    closedir(entries); close(directory);
    char nested[1100]; snprintf(nested, sizeof(nested), "%s/var/run/display-settings", data); assert(!rmdir(nested));
    snprintf(nested, sizeof(nested), "%s/var/run", data); assert(!rmdir(nested));
    snprintf(nested, sizeof(nested), "%s/var", data); assert(!rmdir(nested));
    assert(!rmdir(data) && !rmdir(root) && !rmdir(base));
    puts("Display-settings worker tests passed");
}
