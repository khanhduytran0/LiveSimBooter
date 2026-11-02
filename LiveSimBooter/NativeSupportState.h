#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Experimental native-support mode only. Preparation is fallible and leaves native state unchanged.
int LSBPrepareNativeSupportState(uintptr_t stackTop, size_t stackSize, char *error, size_t errorSize);
uintptr_t LSBOverrideNativeSupportSymbol(const char *library, const char *symbol, uintptr_t nativeAddress);
// One-way transition, after native XPC/logging/allocations have finished. The caller must use app-local exit on failure.
int LSBActivateNativeSupportState(void);

#ifdef __cplusplus
}
#endif
