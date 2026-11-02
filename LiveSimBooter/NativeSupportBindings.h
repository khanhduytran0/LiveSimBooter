#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <mach-o/loader.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uintptr_t (*LSBNativeSupportOverrideCallback)(const char *library, const char *symbol, uintptr_t nativeAddress);
typedef struct {
    void *(*map)(void *address, size_t size, int protection, int flags, int fd, off_t offset);
    int (*protect)(void *address, size_t size, int protection);
} LSBNativeSupportMemory;

// Native-only preparation: inspect the exact experimental cache and snapshot native dlsym results before guest TLS/constructors.
int LSBPrepareNativeSupportBindings(const char *cachePath, uint64_t actualBase, LSBNativeSupportOverrideCallback override, char *error, size_t errorSize);
// After all six cache mappings exist, before dyld reads cached exports. Calls only supplied app-local memory operations.
int LSBApplyNativeSupportBindings(const LSBNativeSupportMemory *memory);
// Replay resolved __interpose tuples before guest initializers: stock dyld ignores cache interposing for an absolute/native replacee.
int LSBApplyNativeSupportInterposing(const struct mach_header *image, const LSBNativeSupportMemory *memory);
int LSBNativeSupportBindingsPrepared(void);

#ifdef __cplusplus
}
#endif
