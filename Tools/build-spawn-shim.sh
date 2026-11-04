#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output=${1:-tmp/multiprocess/libLiveSimSpawn.dylib}
probe=${2:-tmp/multiprocess/spawn-probe}
mkdir -p "$(dirname "$output")" "$(dirname "$probe")"
trace_flags=
if [ "${LSB_TRANSPORT_TRACE:-0}" = 1 ]; then trace_flags=-DLSB_TRANSPORT_TRACE; fi
spawn=Guest/libLiveSimSpawn
fork_trial=
if [ "${LSB_FORK_SNAPSHOT_TRIAL:-1}" = 1 ]; then fork_trial="-DLSB_FORK_SNAPSHOT_TRIAL $spawn/ForkTrial.c"; fi
# Keep BrokerConnection.c first: Mach-O constructor priority does not order separate objects.
# Verify with dyld_info -inits that enableBrokerConnection precedes other inserted-library constructors.
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios14.0-simulator -std=c11 -Wall -Wextra -Werror \
    -dynamiclib -fblocks -fvisibility=hidden -DLSB_GUEST_RUNTIME $trace_flags $fork_trial \
    -install_name /usr/lib/libLiveSimSpawn.dylib -Wl,-not_for_dyld_shared_cache -Wl,-U,_BSSelfTaskHasEntitlement \
    "$spawn/BrokerConnection.c" "$spawn/SpawnShim.c" "$spawn/SpawnAttributes.c" "$spawn/CreationMask.c" "$spawn/ExecutableAccess.c" \
    "$spawn/RuntimeDirectories.c" "$spawn/ChildWait.c" "$spawn/ChildKill.c" "$spawn/TerminalStopWitness.c" \
    "$spawn/ProcessIdentity.c" "$spawn/ProcessEvents.c" "$spawn/ProcessSession.c" "$spawn/FakePTY.c" "$spawn/FileLockCompatibility.c" \
    "$spawn/ForkHook.c" "$spawn/ForkHook_arm64.S" Guest/Shared/Fork/ForkContext_arm64.S \
    "$spawn/ThreadBootstrap.c" "$spawn/MetalCompilerRegistration.c" \
    "$spawn/WorkqueueBootstrap.c" "$spawn/WorkqueueTrampoline.c" "$spawn/UserspaceReboot.c" \
    "$spawn/LaunchdCompatibility.c" "$spawn/LaunchdIdentity.c" "$spawn/LaunchdJobIdentity.c" \
    "$spawn/SystemPowerCompatibility.c" "$spawn/LoggingCompatibility.c" "$spawn/LaunchServicesNotifications.c" \
    "$spawn/FrameworkBundleCompatibility.c" "$spawn/NotificationSharedMemory.c" "$spawn/CodeSigningCompatibility.c" "$spawn/SpringBoardEntitlements.c" \
    "$spawn/PeerProcessCompatibility.c" "$spawn/ProcessInspection.c" "$spawn/ExtensionKitRegistration.c" "$spawn/UserHomeCompatibility.c" \
    "$spawn/SharedCachePathCompatibility.c" "$spawn/ProcessDebugger.c" ProcessSupport/PeerDebugger.c ProcessSupport/ChildStatus.c ProcessSupport/FakePTY.c \
    ProcessSupport/SpawnPlan.c ProcessSupport/SpawnTransport.c LiveProcess/ExecutablePath.c -framework IOKit -framework CoreFoundation -o "$output"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios14.0-simulator -std=c11 -Wall -Wextra -Werror \
    Tests/Guest/spawn-probe/SpawnProbe.c -o "$probe"
printf '%s\n' "$output" "$probe"
if [ -n "$fork_trial" ]; then
    # Keep the observer and shim on the same checkpoint protocol.
    observer="$(dirname "$output")/lsb-fork-snapshot-child"
    child=Guest/lsb-fork-snapshot-child
    xcrun --sdk iphonesimulator clang -target arm64-apple-ios26.0-simulator -Wall -Wextra -Werror \
        "$child/ForkSnapshotChild.c" "$child/ForkBashResume.c" "$child/ForkBashResume_arm64.S" Guest/Shared/Fork/ForkContext_arm64.S "$output" -o "$observer"
    printf '%s\n' "$observer"
fi
