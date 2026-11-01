#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "NativeCacheLayout.h"

#define LSB_RETAIN_HOST_CACHE_ENV "LSB_RETAIN_HOST_CACHE"
#define LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV "LSB_WORKQUEUE_HARDWARE_BREAKPOINT"

enum LSBNativeProductionStatus {
    LSB_NATIVE_PRODUCTION_UNKNOWN = 0,
    LSB_NATIVE_PRODUCTION = 1,
    LSB_NATIVE_DEVELOPMENT = 2
};

typedef struct {
    bool retainHostCache;
    bool hardwareWorkqueue;
} LSBNativeRuntimePolicy;

static inline bool LSBValidNativeProductionStatus(uint32_t status) { return status <= LSB_NATIVE_DEVELOPMENT; }

static inline const char *LSBRuntimeEnvironmentValue(const char *const environment[], const char *key) {
    size_t length = strlen(key);
    for (const char *const *entry = environment; entry && *entry; ++entry)
        if (!strncmp(*entry, key, length) && (*entry)[length] == '=') return *entry + length + 1;
    return NULL;
}

static inline bool LSBRuntimeBooleanEnvironment(const char *const environment[], const char *key, bool defaultValue) {
    const char *value = LSBRuntimeEnvironmentValue(environment, key);
    return value ? !strcmp(value, "1") : defaultValue;
}

// status is native host metadata received through authenticated check-in, never a guest environment or simulated OS query.
// Prefer retention. Automatic detachment additionally requires a confirmed native-header collision; non-production alone does not prove relaxed JIT.
static inline int LSBResolveNativeRuntimePolicy(uint32_t status, LSBNativeCacheLayout layout,
                                               const char *const environment[], LSBNativeRuntimePolicy *policy) {
    if (!policy) return EINVAL;
    *policy = (LSBNativeRuntimePolicy){.retainHostCache = true, .hardwareWorkqueue = true};
    if (!LSBValidNativeProductionStatus(status) || layout < LSB_NATIVE_CACHE_LAYOUT_UNKNOWN || layout > LSB_NATIVE_CACHE_LAYOUT_OVERLAP) return EINVAL;
    bool defaultRetain = status != LSB_NATIVE_DEVELOPMENT || layout != LSB_NATIVE_CACHE_LAYOUT_OVERLAP;
    bool retain = LSBRuntimeBooleanEnvironment(environment, LSB_RETAIN_HOST_CACHE_ENV, defaultRetain);
    if (!retain && status != LSB_NATIVE_DEVELOPMENT) return EPERM;
    policy->retainHostCache = retain;
    policy->hardwareWorkqueue = LSBRuntimeBooleanEnvironment(environment, LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV, retain);
    return 0;
}

// Append only absent flags after successful resolution. Explicit first occurrences retain their existing exact-1 semantics.
static inline int LSBAppendNativeRuntimePolicy(const char *environment[], unsigned *count, unsigned capacity, LSBNativeRuntimePolicy policy) {
    if (!environment || !count || *count >= capacity || environment[*count]) return EINVAL;
    for (unsigned index = 0; index < *count; ++index) if (!environment[index]) return EINVAL;
    bool retainMissing = !LSBRuntimeEnvironmentValue(environment, LSB_RETAIN_HOST_CACHE_ENV);
    bool hardwareMissing = !LSBRuntimeEnvironmentValue(environment, LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV);
    unsigned additional = (unsigned)retainMissing + (unsigned)hardwareMissing;
    if (additional > capacity - *count - 1) return E2BIG;
    if (retainMissing) environment[(*count)++] = policy.retainHostCache ? LSB_RETAIN_HOST_CACHE_ENV "=1" : LSB_RETAIN_HOST_CACHE_ENV "=0";
    if (hardwareMissing)
        environment[(*count)++] = policy.hardwareWorkqueue ? LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV "=1" : LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV "=0";
    environment[*count] = NULL;
    return 0;
}
