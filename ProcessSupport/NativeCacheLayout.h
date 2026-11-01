#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LSB_NATIVE_CACHE_LAYOUT_UNKNOWN = 0,
    LSB_NATIVE_CACHE_LAYOUT_DISJOINT,
    LSB_NATIVE_CACHE_LAYOUT_OVERLAP
} LSBNativeCacheLayout;

// Native pre-handoff only. Reads the runtime header and this process's actual loaded native cache header; preserves errno.
// Unknown/malformed inputs never authorize automatic detachment. Final loader collision checks remain authoritative.
LSBNativeCacheLayout LSBReadNativeCacheLayout(const char *runtimeRoot);

#ifdef __cplusplus
}
#endif
