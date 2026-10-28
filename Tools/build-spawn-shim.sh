#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output=${1:-tmp/multiprocess/libLiveSimSpawn.dylib}
probe=${2:-tmp/multiprocess/spawn-probe}
mkdir -p "$(dirname "$output")" "$(dirname "$probe")"
trace_flags=
if [ "${LSB_TRANSPORT_TRACE:-0}" = 1 ]; then trace_flags=-DLSB_TRANSPORT_TRACE; fi
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios14.0-simulator -std=c11 -Wall -Wextra -Werror \
    -dynamiclib -fblocks -fvisibility=hidden -DLSB_GUEST_RUNTIME -DLSB_XPC_WIRE_COMPATIBILITY $trace_flags \
    -install_name /usr/lib/libLiveSimSpawn.dylib -Wl,-not_for_dyld_shared_cache -Wl,-U,_BSSelfTaskHasEntitlement \
    Guest/SpawnShim.c Guest/SpawnAttributes.c Guest/CreationMask.c Guest/ExecutableAccess.c Guest/ChildWait.c Guest/ChildKill.c \
    Guest/ProcessIdentity.c Guest/ProcessEvents.c Guest/ProcessSession.c Guest/FakePTY.c Guest/FileLockCompatibility.c \
    Guest/ThreadBootstrap.c Guest/MetalCompilerRegistration.c \
    Guest/WorkqueueBootstrap.c Guest/UserspaceReboot.c Guest/LaunchdCompatibility.c Guest/LaunchdIdentity.c Guest/LaunchdJobIdentity.c \
    Guest/XPCWireCompatibility.c Guest/SystemPowerCompatibility.c Guest/LoggingCompatibility.c \
    Guest/FrameworkBundleCompatibility.c Guest/NotificationSharedMemory.c Guest/CodeSigningCompatibility.c Guest/SpringBoardEntitlements.c \
    Guest/PeerProcessCompatibility.c Guest/ProcessInspection.c Guest/ExtensionKitRegistration.c Guest/UserHomeCompatibility.c Guest/SharedCachePathCompatibility.c \
    Guest/ProcessDebugger.c ProcessSupport/PeerDebugger.c ProcessSupport/ChildStatus.c ProcessSupport/FakePTY.c \
    ProcessSupport/SpawnPlan.c ProcessSupport/SpawnTransport.c -framework IOKit -framework CoreFoundation -o "$output"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios14.0-simulator -std=c11 -Wall -Wextra -Werror \
    Guest/SpawnProbe.c -o "$probe"
printf '%s\n' "$output" "$probe"
