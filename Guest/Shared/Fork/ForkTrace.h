#pragma once

#include "../../../ProcessSupport/FakePTY.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum { ForkTraceUnset, ForkTraceOff, ForkTraceDefault, ForkTraceVerbose };
static _Atomic unsigned char forkTraceLevel;

// Snapshot diagnostics before fork preparation. A running shell can change them for its next attempt.
static inline unsigned char forkTrialCaptureTrace(void) {
    const char *value = getenv("LSB_FORK_SNAPSHOT_TRACE");
    unsigned char level = ForkTraceOff;
    if (value) {
        if (!strcmp(value, "1")) level = ForkTraceVerbose;
    } else if (!getenv(LSB_FAKE_PTY_ENV)) level = ForkTraceDefault;
    atomic_store_explicit(&forkTraceLevel, level, memory_order_relaxed);
    return level;
}

static inline unsigned char forkTrialTraceSetting(void) {
    unsigned char level = atomic_load_explicit(&forkTraceLevel, memory_order_relaxed);
    return level == ForkTraceUnset ? forkTrialCaptureTrace() : level;
}

static inline bool forkTrialTraceEnabled(void) {
    return forkTrialTraceSetting() >= ForkTraceDefault;
}
