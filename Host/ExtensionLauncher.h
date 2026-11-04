#pragma once

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Events run on the main thread. A positive PID comes from pkd's launch reply; an error is terminal for this request.
// No child NSXPC extension-context handshake is required. Broker check-in establishes guest readiness separately.
typedef void (*LSBExtensionEvent)(uint64_t requestID, pid_t pid, int error, void *context);

// Call on the main thread. Returns an errno value for an immediate failure, with no callback for that failure.
// A zero return only means the request was submitted. Keep context alive until LSBReleaseExtension returns.
int LSBLaunchExtension(const char *identifier, uint64_t requestID, LSBExtensionEvent event, void *context);

enum { LSB_EXTENSION_LAUNCH_GPU = 1u << 0 };
// Host-selected native Metal roles may request GPU access. This remains subject to the host's foreground assertion lifetime.
// Unknown options fail with EINVAL. The default entry above uses no options.
int LSBLaunchExtensionWithOptions(const char *identifier, uint64_t requestID, uint32_t options, LSBExtensionEvent event, void *context);

// Main-thread pool operations for ordinary simulator launches. Claim after both pkd's PID reply and the broker's authenticated check-in.
int LSBLaunchWarmExtension(const char *identifier, uint64_t poolID, LSBExtensionEvent event, void *context);
// On success the existing assertion, service instance and future events belong to requestID. The launch event is not replayed.
int LSBClaimWarmExtension(uint64_t poolID, uint64_t requestID, pid_t expectedPID, LSBExtensionEvent event, void *context);
// Suppress callbacks and release an unclaimed launch. Send SIGKILL to its exact instance once pkd has confirmed a PID.
int LSBCancelWarmExtension(uint64_t poolID);

// Call on the main thread. Returns the PID supplied by pkd after its launch reply, or 0 if unavailable.
pid_t LSBExtensionProcessIdentifier(uint64_t requestID);

// Call on the main thread for an active launched session. Signals its exact service instance through launchd.
// After a GPU session's assertion invalidates, only SIGKILL remains available until the session is released.
// Zero means submitted: the underlying private API returns no delivery result.
int LSBSignalExtension(uint64_t requestID, int signal);

// Call on the main thread. Stops callbacks and releases the retained launch and process assertion; does not send a termination request.
void LSBReleaseExtension(uint64_t requestID);

#ifdef __cplusplus
}
#endif
