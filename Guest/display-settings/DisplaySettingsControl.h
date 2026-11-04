#pragma once
#include "../../ProcessSupport/DisplaySettings.h"

// Runs in simulator libraries/bootstrap only. Each field reports its own readback error; setters never imply success.
void LSBDisplaySettingsApply(uint32_t operation, int32_t value, LSBDSettingsResponse *response);
int LSBDisplaySettingsServe(void);
