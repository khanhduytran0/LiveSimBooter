#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output=${1:-tmp/multiprocess/libLiveSimSpawn.dylib}
probe=${2:-tmp/multiprocess/spawn-probe}
mkdir -p "$(dirname "$output")" "$(dirname "$probe")"
trace_flags=
if [ "${LSB_TRANSPORT_TRACE:-0}" = 1 ]; then trace_flags=-DLSB_TRANSPORT_TRACE; fi
fork_trial=
if [ "${LSB_FORK_SNAPSHOT_TRIAL:-1}" = 1 ]; then fork_trial='-DLSB_FORK_SNAPSHOT_TRIAL Tests/ForkTrial.c'; fi
# Keep BrokerConnection.c first: Mach-O constructor priority does not order separate objects.
# Verify with dyld_info -inits that enableBrokerConnection precedes other inserted-library constructors.
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios14.0-simulator -std=c11 -Wall -Wextra -Werror \
    -dynamiclib -fblocks -fvisibility=hidden -DLSB_GUEST_RUNTIME $trace_flags $fork_trial \
    -install_name /usr/lib/libLiveSimSpawn.dylib -Wl,-not_for_dyld_shared_cache -Wl,-U,_BSSelfTaskHasEntitlement \
    Guest/BrokerConnection.c Guest/SpawnShim.c Guest/SpawnAttributes.c Guest/CreationMask.c Guest/ExecutableAccess.c Guest/RuntimeDirectories.c \
    Guest/ChildWait.c Guest/ChildKill.c Guest/TerminalStopWitness.c \
    Guest/ProcessIdentity.c Guest/ProcessEvents.c Guest/ProcessSession.c Guest/FakePTY.c Guest/FileLockCompatibility.c \
    Guest/ForkHook.c Guest/ForkHook_arm64.S Guest/ForkContext_arm64.S \
    Guest/ThreadBootstrap.c Guest/MetalCompilerRegistration.c \
    Guest/WorkqueueBootstrap.c Guest/WorkqueueTrampoline.c Guest/UserspaceReboot.c Guest/LaunchdCompatibility.c Guest/LaunchdIdentity.c Guest/LaunchdJobIdentity.c \
    Guest/SystemPowerCompatibility.c Guest/LoggingCompatibility.c Guest/LaunchServicesNotifications.c \
    Guest/FrameworkBundleCompatibility.c Guest/NotificationSharedMemory.c Guest/CodeSigningCompatibility.c Guest/SpringBoardEntitlements.c \
    Guest/PeerProcessCompatibility.c Guest/ProcessInspection.c Guest/ExtensionKitRegistration.c Guest/UserHomeCompatibility.c Guest/SharedCachePathCompatibility.c \
    Guest/ProcessDebugger.c ProcessSupport/PeerDebugger.c ProcessSupport/ChildStatus.c ProcessSupport/FakePTY.c \
    ProcessSupport/SpawnPlan.c ProcessSupport/SpawnTransport.c LiveProcess/ExecutablePath.c -framework IOKit -framework CoreFoundation -o "$output"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios14.0-simulator -std=c11 -Wall -Wextra -Werror \
    Guest/SpawnProbe.c -o "$probe"
printf '%s\n' "$output" "$probe"
if [ -n "$fork_trial" ]; then
    # Keep the observer and shim on the same checkpoint protocol.
    observer="$(dirname "$output")/lsb-fork-snapshot-child"
    xcrun --sdk iphonesimulator clang -target arm64-apple-ios26.0-simulator -Wall -Wextra -Werror \
        Tests/ForkSnapshotChild.c Tests/ForkBashResume.c Tests/ForkBashResume_arm64.S Guest/ForkContext_arm64.S "$output" -o "$observer"
    printf '%s\n' "$observer"
fi
