#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// The loader supplies these Boolean settings for the lifetime of this guest. Keep only the value, never a pointer into environ.
typedef struct {
    const char *name;
    _Atomic(unsigned char) state;
} LSBLaunchFlag;

enum { LSBLaunchFlagUninitialized, LSBLaunchFlagOff, LSBLaunchFlagOn };

static inline bool LSBReadLaunchFlag(const LSBLaunchFlag *flag) {
    const char *value = getenv(flag->name);
    return value && !strcmp(value, "1");
}

static inline void LSBCaptureLaunchFlag(LSBLaunchFlag *flag) {
    atomic_store_explicit(&flag->state, LSBReadLaunchFlag(flag) ? LSBLaunchFlagOn : LSBLaunchFlagOff, memory_order_relaxed);
}

static inline bool LSBLaunchFlagEnabled(const LSBLaunchFlag *flag) {
    unsigned char state = atomic_load_explicit(&flag->state, memory_order_relaxed);
    // An interposed call from an earlier initializer sees the same environment it would have read before caching.
    return state == LSBLaunchFlagUninitialized ? LSBReadLaunchFlag(flag) : state == LSBLaunchFlagOn;
}
