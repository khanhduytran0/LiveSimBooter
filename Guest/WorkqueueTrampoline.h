#ifndef LSB_WORKQUEUE_TRAMPOLINE_H
#define LSB_WORKQUEUE_TRAMPOLINE_H

#include <stdint.h>

// After guest TLS initialization and native-cache detachment only. Success owns the RX mapping until process exit.
// Recreates the kernel's registered entry with a direct guest branch; only an empty range or verified detached DYLD placeholder is accepted.
int LSBWorkqueueTrampolineInstall(uintptr_t nativeEntry, uintptr_t guestEntry);

#endif
