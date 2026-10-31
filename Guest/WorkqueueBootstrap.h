#ifndef LSB_WORKQUEUE_BOOTSTRAP_H
#define LSB_WORKQUEUE_BOOTSTRAP_H

#include <stdint.h>
#include <mach/mach.h>

// Captured in native code before cache detachment. The feature getter does not initialize the native workqueue.
#define LSB_HOST_WORKQUEUE_START_ENV "LSB_HOST_WORKQUEUE_START"
#define LSB_HOST_WORKQUEUE_FEATURES_ENV "LSB_HOST_WORKQUEUE_FEATURES"
// Retained-cache default: reserve one inherited execution breakpoint until process exit; worker reuse needs it on every entry.
// Exact value "1" enables. With normalized LSB_RETAIN_HOST_CACHE=0, disabling hardware selects the development-only direct trampoline.
#define LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV "LSB_WORKQUEUE_HARDWARE_BREAKPOINT"

// Called only by the validated duplicate libpthread registration adapter; zero disables the adapter.
void LSBWorkqueueRegister(uintptr_t simulatorStart);
int LSBWorkqueueSupported(void);
int LSBWorkqueueOpen(void);
// After guest TLS/constructors only: initialize the shared guest control/exception server, then add a private receive port to its wait set.
// Detached direct-trampoline mode uses this worker only for process control and leaves task exception handlers unchanged.
// Caller retains the receive right and removes it by destroying that right. Never creates a second server or opens the kernel workqueue.
int LSBWorkqueueAddProcessControlPort(mach_port_t port);

#endif
