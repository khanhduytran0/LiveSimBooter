#pragma once

#include "SpawnTransport.h"

#ifdef __OBJC__
@class LSBGuestDeviceType;
// Main-thread host selection; nil restores the legacy default/explicit identity overrides used by standalone probes.
void LSBLaunchdConfigurationSetDeviceType(LSBGuestDeviceType *type);
#endif

// Native host only. Requires a zeroed plan and message->fds initialized to -1; leaves them unchanged on failure.
// Creates only owned data paths beside RuntimeRoot within its build directory, regenerating its bootstrap plist and appending to its regular log file.
// The caller supplies signal masks after success and owns the returned FDs. No terminal is attached.
int LSBPrepareLaunchdPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]);

// Opt-in launchd probe: the bootstrap loads one generated RunAtLoad df job from an otherwise empty owned directory.
// The executable path is absolute inside RuntimeRoot; this does not provide a chroot or load any runtime LaunchDaemons.
int LSBPrepareLaunchdDFPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]);

// Stock backboardd plus the simulator cfprefsd daemon, preserving their MachServices but omitting unsupported native policy attributes.
int LSBPrepareLaunchdBackboardPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]);
// Host-selected stock daemon labels, NULL-terminated, in addition to backboardd/cfprefsd. Never read labels from guest environment.
// Runs each once in a separate owned job directory; preserves service declarations while omitting credentials and native policy.
int LSBPrepareLaunchdServicesPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[],
                                 const char *const labels[]);
// Process-event and orphan-adoption regression job under simulator launchd.
int LSBPrepareLaunchdLifecyclePlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]);
