#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { LSBDSettingsMagic = 0x4c534244, LSBDSettingsVersion = 1 };
enum { LSBDSettingsRead = 1, LSBDSettingsHUD = 2, LSBDSettingsTouches = 3 };
#define LSB_DISPLAY_SETTINGS_PROGRAM "/usr/libexec/display-settings"
#define LSB_DISPLAY_SETTINGS_LABEL "com.kdt.LiveSimBooter.display-settings"

typedef struct {
    uint32_t magic, version;
    uint8_t generation[16], request[16];
    uint32_t operation;
    int32_t value;
    uint64_t deadlineNS;
} LSBDSettingsRequest;

typedef struct {
    uint32_t magic, version;
    uint8_t generation[16], request[16];
    uint32_t operation;
    int32_t error, hudError, touchesError, hudMode, touches;
} LSBDSettingsResponse;

typedef struct {
    uint32_t magic, version;
    uint8_t generation[16];
    uint64_t startedNS;
    int32_t pid;
    uint32_t reserved;
} LSBDSettingsReady;

_Static_assert(sizeof(LSBDSettingsRequest) == 56, "settings request wire layout");
_Static_assert(sizeof(LSBDSettingsResponse) == 64, "settings response wire layout");
_Static_assert(sizeof(LSBDSettingsReady) == 40, "settings ready wire layout");

uint64_t LSBDSettingsNow(void);
int LSBDSettingsValidateRequest(const LSBDSettingsRequest *request, const uint8_t generation[16], uint64_t now);
int LSBDSettingsValidateResponse(const LSBDSettingsResponse *response, const LSBDSettingsRequest *request);
int LSBDSettingsValidateReady(const LSBDSettingsReady *ready);
void LSBDSettingsResultName(const uint8_t request[16], char output[40]);
// Opens only an owned, private directory below canonical sibling RuntimeRoot/data. Returns errno, not -1.
int LSBDSettingsOpenDirectory(const char *dataRoot, bool create, int *directory);
// Exact-size, owned, single-link regular files only. Atomic creation never replaces an existing request/result.
int LSBDSettingsReadFile(int directory, const char *name, void *bytes, size_t size);
int LSBDSettingsWriteFile(int directory, const char *name, const void *bytes, size_t size, bool replace);
int LSBDSettingsAbsent(int directory, const char *name);
