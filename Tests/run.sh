#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_directory=$(mktemp -d /tmp/LiveSimBooter-tests.XXXXXX)
trap 'rm -rf "$test_directory"' EXIT HUP INT TERM
for test_name in HostStartupPolicy UserspaceReboot NativeRuntimePolicy; do
    xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 "Tests/${test_name}Tests.c" -o "$test_directory/$test_name"
    "$test_directory/$test_name"
done
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/NativeProductionStatusTests.c -framework CoreFoundation -o "$test_directory/NativeProductionStatus"
"$test_directory/NativeProductionStatus"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    -DLSB_TEST_STORE_BROKER_DISCOVERY Tests/StoreAppGroupsTests.c ProcessSupport/StoreAppGroups.c \
    ProcessSupport/SpawnTransport.c ProcessSupport/SpawnPlan.c -o "$test_directory/StoreAppGroups"
"$test_directory/StoreAppGroups"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 \
    Tests/RuntimeStorageTests.m Host/RuntimeStorage.m ProcessSupport/StoreAppGroups.c \
    -framework Foundation -o "$test_directory/RuntimeStorage"
"$test_directory/RuntimeStorage"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/SessionControlQueueTests.c Host/SessionControlQueue.c -o "$test_directory/SessionControlQueue"
"$test_directory/SessionControlQueue"
for test_name in DyldSimLoader SharedCacheHandoff MainThreadBootstrap DebuggerBridge LocalMach LocalRuntime; do
    xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -arch arm64 -fno-builtin -fno-stack-protector \
        "Tests/${test_name}Tests.cpp" -o "$test_directory/$test_name"
    "$test_directory/$test_name"
done
xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -arch arm64 -fno-builtin -fno-stack-protector \
    Tests/CompactImageInfoTests.cpp -framework CoreFoundation -o "$test_directory/CompactImageInfo"
"$test_directory/CompactImageInfo"
xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -arch arm64 -fno-builtin -fno-stack-protector \
    Tests/LocalPOSIXTests.cpp LiveSimBooter/LocalPOSIX.cpp -o "$test_directory/LocalPOSIX"
"$test_directory/LocalPOSIX"
xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -arch arm64 -fno-builtin -fno-stack-protector \
    -DLSB_LOCAL_RUNTIME_RAW_TESTING Tests/LocalRuntimeTests.cpp -o "$test_directory/LocalRuntimeRaw"
"$test_directory/LocalRuntimeRaw"
for family in Mach POSIX; do
    xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -arch arm64 -fno-builtin -fno-stack-protector \
        "Tests/Local${family}LiveTests.cpp" "LiveSimBooter/Local${family}.cpp" LiveSimBooter/LocalRuntime.cpp \
        -o "$test_directory/Local${family}Live"
    "$test_directory/Local${family}Live"
done
xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -arch arm64 -fno-builtin -fno-stack-protector \
    Tests/SimSyscallHelpersTests.cpp LiveSimBooter/SimSyscallHelpers.cpp LiveSimBooter/LocalRuntime.cpp \
    LiveSimBooter/LocalPOSIX.cpp LiveSimBooter/LocalMach.cpp LiveSimBooter/CompactImageInfo.cpp -o "$test_directory/SimSyscallHelpers"
"$test_directory/SimSyscallHelpers"
xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -arch arm64 -fno-builtin -fno-stack-protector \
    -DLSB_DEBUGGER_NOTIFICATION_TESTING Tests/DebuggerNotificationTests.cpp LiveSimBooter/SimSyscallHelpers.cpp LiveSimBooter/LocalRuntime.cpp \
    LiveSimBooter/LocalPOSIX.cpp LiveSimBooter/LocalMach.cpp -o "$test_directory/DebuggerNotification"
"$test_directory/DebuggerNotification"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/SpawnPlanTests.c Guest/SpawnAttributes.c Guest/ProcessSession.c Guest/ProcessIdentity.c ProcessSupport/SpawnPlan.c ProcessSupport/ChildStatus.c \
    -DLSB_PROCESS_IDENTITY_TESTING -DLSB_PROCESS_SESSION_TESTING -o "$test_directory/SpawnPlan"
"$test_directory/SpawnPlan"
LSB_SIMULATE_SESSIONS=1 LSB_TEST_SESSION_GROUP=1 "$test_directory/SpawnPlan"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/SpawnAttributesTests.c Guest/SpawnAttributes.c -o "$test_directory/SpawnAttributes"
"$test_directory/SpawnAttributes"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/LaunchctlControlTests.c -o "$test_directory/LaunchctlControl"
"$test_directory/LaunchctlControl"
python3 Tests/GuestLaunchctlTests.py
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/ExecutablePathTests.c LiveProcess/ExecutablePath.c -o "$test_directory/ExecutablePath"
"$test_directory/ExecutablePath"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/ProcessDisplayNameTests.c -o "$test_directory/ProcessDisplayName"
"$test_directory/ProcessDisplayName"
python3 Tests/NativeStackReservationTests.py
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/UserHomeCompatibilityTests.c -o "$test_directory/UserHomeCompatibility"
"$test_directory/UserHomeCompatibility"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/ChildMonitorTests.c Host/ChildMonitor.c -o "$test_directory/ChildMonitor"
"$test_directory/ChildMonitor"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/ChildStatusTests.c ProcessSupport/ChildStatus.c -o "$test_directory/ChildStatus"
"$test_directory/ChildStatus"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_CHILD_WAIT_TESTING \
    Tests/ChildWaitTests.c Guest/ChildWait.c ProcessSupport/ChildStatus.c -o "$test_directory/ChildWait"
"$test_directory/ChildWait"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_CHILD_KILL_TESTING \
    Tests/ChildKillTests.c Guest/ChildKill.c Guest/ProcessIdentity.c Guest/ChildWait.c ProcessSupport/ChildStatus.c \
    -DLSB_PROCESS_IDENTITY_TESTING -DLSB_CHILD_WAIT_TESTING -o "$test_directory/ChildKill"
"$test_directory/ChildKill"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_CHILD_KILL_TESTING \
    Tests/ProcessIdentityTests.c Guest/ChildKill.c Guest/ChildWait.c ProcessSupport/ChildStatus.c -DLSB_CHILD_WAIT_TESTING -o "$test_directory/ProcessIdentity"
"$test_directory/ProcessIdentity"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/ProcessInspectionTests.c -o "$test_directory/ProcessInspection"
"$test_directory/ProcessInspection"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_TRANSPORT_TESTING \
    Tests/ProcessInspectionTransportTests.c ProcessSupport/SpawnTransport.c ProcessSupport/SpawnPlan.c -o "$test_directory/ProcessInspectionTransport"
"$test_directory/ProcessInspectionTransport"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_TRANSPORT_TESTING \
    Tests/TaskPortTransportTests.c ProcessSupport/SpawnPlan.c -o "$test_directory/TaskPortTransport"
"$test_directory/TaskPortTransport"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/TaskPortCaptureTests.c ProcessSupport/TaskPortCapture.c -o "$test_directory/TaskPortCapture"
"$test_directory/TaskPortCapture"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/ProcessDebuggerTests.c -o "$test_directory/ProcessDebugger"
"$test_directory/ProcessDebugger"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_TRANSPORT_TESTING \
    Tests/ProcessDebuggerTransportTests.c ProcessSupport/SpawnTransport.c ProcessSupport/SpawnPlan.c -o "$test_directory/ProcessDebuggerTransport"
"$test_directory/ProcessDebuggerTransport"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/PeerDebuggerTests.c ProcessSupport/PeerDebugger.c -o "$test_directory/PeerDebugger"
"$test_directory/PeerDebugger"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -dynamiclib -DLSB_INSPECTION_INTERPOSE_FIXTURE \
    Guest/ProcessInspection.c Tests/ProcessInspectionInterposeTests.c -o "$test_directory/libProcessInspection.dylib"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/ProcessInspectionInterposeTests.c -o "$test_directory/ProcessInspectionInterpose"
DYLD_INSERT_LIBRARIES="$test_directory/libProcessInspection.dylib" "$test_directory/ProcessInspectionInterpose"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -O1 \
    Tests/PeerProcessCompatibilityTests.c -o "$test_directory/PeerProcessCompatibility"
"$test_directory/PeerProcessCompatibility"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/ProcessSessionTests.c -o "$test_directory/ProcessSession"
"$test_directory/ProcessSession"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -fblocks -DLSB_CHILD_WAIT_TESTING \
    Tests/ProcessEventsTests.c Guest/ChildWait.c ProcessSupport/ChildStatus.c -o "$test_directory/ProcessEvents"
"$test_directory/ProcessEvents"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -fblocks -dynamiclib -DLSB_CHILD_WAIT_TESTING \
    -DLSB_PROCESS_EVENTS_TEST_INSPECTION \
    Guest/ProcessEvents.c Guest/ProcessIdentity.c Guest/ChildWait.c ProcessSupport/ChildStatus.c \
    -install_name "$test_directory/libProcessEvents.dylib" -o "$test_directory/libProcessEvents.dylib"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -fblocks \
    Tests/ProcessEventsInterposeTests.c "$test_directory/libProcessEvents.dylib" -o "$test_directory/ProcessEventsInterpose"
DYLD_INSERT_LIBRARIES="$test_directory/libProcessEvents.dylib" "$test_directory/ProcessEventsInterpose"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/ProcessIdentityInterposeTests.c "$test_directory/libProcessEvents.dylib" -o "$test_directory/ProcessIdentityInterpose"
LSB_GUEST_PID=2147483000 DYLD_INSERT_LIBRARIES="$test_directory/libProcessEvents.dylib" "$test_directory/ProcessIdentityInterpose"
identity_runtime="$(cd "$test_directory" && pwd -P)/IdentityRuntimeRoot"
for identity_name in BaseBoard RunningBoardServices FrontBoard UIKitCore FrontBoardServices BackBoardServices ExtensionFoundation; do
    identity_framework="$identity_runtime/System/Library/PrivateFrameworks/$identity_name.framework"
    if [ "$identity_name" = ExtensionFoundation ]; then identity_framework="$identity_runtime/System/Library/Frameworks/$identity_name.framework"; fi
    identity_unrelated="$identity_runtime/Plugins/$identity_name"
    mkdir -p "$identity_framework" "$identity_runtime/Plugins"
    xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -dynamiclib Tests/ProcessIdentityCallerFixture.c \
        -install_name "$identity_framework/$identity_name" -o "$identity_framework/$identity_name"
    xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -dynamiclib Tests/ProcessIdentityCallerFixture.c \
        -install_name "$identity_unrelated" -o "$identity_unrelated"
    LSB_GUEST_PID=2147483000 DYLD_ROOT_PATH="$identity_runtime" DYLD_INSERT_LIBRARIES="$test_directory/libProcessEvents.dylib" \
        "$test_directory/ProcessIdentityInterpose" "$identity_framework/$identity_name" "$identity_unrelated"
done
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/CreationMaskTests.c -o "$test_directory/CreationMask"
"$test_directory/CreationMask"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/ExecutableAccessTests.c -o "$test_directory/ExecutableAccess"
"$test_directory/ExecutableAccess"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/MetalCompilerRegistrationTests.c -framework CoreFoundation -o "$test_directory/MetalCompilerRegistration"
"$test_directory/MetalCompilerRegistration"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/ExtensionKitRegistrationTests.c -o "$test_directory/ExtensionKitRegistration"
"$test_directory/ExtensionKitRegistration"
xcrun --sdk macosx clang -arch arm64 -fobjc-arc -fblocks -Wall -Wextra -Werror \
    Guest/ApplicationRegistrationPlan.m Tests/ApplicationRegistrationTests.m -framework Foundation -o "$test_directory/ApplicationRegistration"
"$test_directory/ApplicationRegistration"
xcrun --sdk macosx clang -std=c11 -fobjc-arc -Wall -Wextra -Werror -arch arm64 \
    Tests/FrameworkBundleCompatibilityTests.m -framework Foundation -framework QuartzCore -o "$test_directory/FrameworkBundleCompatibility"
"$test_directory/FrameworkBundleCompatibility"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -dynamiclib \
    Guest/FrameworkBundleCompatibility.c -framework CoreFoundation -o "$test_directory/libFrameworkBundleCompatibility.dylib"
xcrun --sdk macosx clang -std=c11 -fobjc-arc -Wall -Wextra -Werror -arch arm64 -DLSB_FRAMEWORK_BUNDLE_INTERPOSE_TESTING \
    Tests/FrameworkBundleCompatibilityTests.m -framework Foundation -framework QuartzCore -o "$test_directory/FrameworkBundleCompatibilityInterpose"
DYLD_INSERT_LIBRARIES="$test_directory/libFrameworkBundleCompatibility.dylib" "$test_directory/FrameworkBundleCompatibilityInterpose"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/SharedCachePathCompatibilityTests.c -o "$test_directory/SharedCachePathCompatibility"
"$test_directory/SharedCachePathCompatibility"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -dynamiclib \
    Guest/SharedCachePathCompatibility.c -o "$test_directory/libSharedCachePathCompatibility.dylib"
mkdir "$test_directory/cache-path-caller"
for cache_path_fixture in Caller Sibling; do
    xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -dynamiclib \
        Tests/SharedCachePathCallerFixture.c -o "$test_directory/cache-path-caller/lib$cache_path_fixture.dylib"
done
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_SHARED_CACHE_PATH_INTERPOSE_TESTING \
    Tests/SharedCachePathCompatibilityTests.c -o "$test_directory/SharedCachePathCompatibilityInterpose"
DYLD_INSERT_LIBRARIES="$test_directory/libSharedCachePathCompatibility.dylib" \
    "$test_directory/SharedCachePathCompatibilityInterpose" "$test_directory/cache-path-caller/libCaller.dylib"
python3 -B Tests/PatchIOSPlatformTests.py
python3 -B Tests/SetupIOS26CacheTests.py
python3 -B Tests/SetupIOS26RuntimeTests.py
python3 -B Tests/LaunchdIdentityPatchTests.py
python3 -B Tests/ContainerMetadataPatchTests.py
python3 -B Tests/KeyboardHapticsPatchTests.py
python3 -B Tests/LaunchdIdentityVeneerTests.py
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/LaunchdIdentityTests.c -o "$test_directory/LaunchdIdentity"
"$test_directory/LaunchdIdentity"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/LaunchdJobIdentityTests.c -o "$test_directory/LaunchdJobIdentity"
"$test_directory/LaunchdJobIdentity"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_FAKE_PTY_TESTING \
    Tests/FakePTYTests.c Guest/FakePTY.c ProcessSupport/FakePTY.c -o "$test_directory/FakePTY"
"$test_directory/FakePTY"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/ThreadBootstrapTests.c -o "$test_directory/ThreadBootstrap"
"$test_directory/ThreadBootstrap"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/WorkqueueBootstrapTests.c -o "$test_directory/WorkqueueBootstrap"
"$test_directory/WorkqueueBootstrap"
xcrun clang -std=c11 -Wall -Wextra -Werror -Wno-unused-function \
    Tests/WorkqueueStartupTests.c -o "$test_directory/WorkqueueStartup"
"$test_directory/WorkqueueStartup"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/WorkqueueMachTests.c ProcessSupport/PeerDebugger.c -o "$test_directory/WorkqueueMach"
"$test_directory/WorkqueueMach"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/LaunchdHandshakeTests.c -o "$test_directory/LaunchdHandshake"
"$test_directory/LaunchdHandshake"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/LaunchdProbeTests.c ProcessSupport/LaunchdProbe.c -o "$test_directory/LaunchdProbe"
"$test_directory/LaunchdProbe"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/LaunchdCompatibilityTests.c -o "$test_directory/LaunchdCompatibility"
"$test_directory/LaunchdCompatibility"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/FileLockCompatibilityTests.c -o "$test_directory/FileLockCompatibility"
"$test_directory/FileLockCompatibility"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -dynamiclib \
    Guest/FileLockCompatibility.c -o "$test_directory/libFileLockCompatibility.dylib"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_FILE_LOCK_INTERPOSE_TESTING \
    Tests/FileLockCompatibilityTests.c -o "$test_directory/FileLockCompatibilityInterpose"
mkdir -p "$test_directory/file-lock-interpose/data" "$test_directory/file-lock-interpose/RuntimeRoot"
for file_lock_enabled in 1 0; do
    LSB_SIMULATE_FILE_LOCK_SUSPENSION="$file_lock_enabled" SIMULATOR_SHARED_RESOURCES_DIRECTORY="$test_directory/file-lock-interpose/data" \
        DYLD_INSERT_LIBRARIES="$test_directory/libFileLockCompatibility.dylib" \
        "$test_directory/FileLockCompatibilityInterpose" "$test_directory/file-lock-interpose/data"
done
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/NotificationSharedMemoryTests.c -o "$test_directory/NotificationSharedMemory"
"$test_directory/NotificationSharedMemory"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -dynamiclib \
    Guest/NotificationSharedMemory.c -o "$test_directory/libNotificationSharedMemory.dylib"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_NOTIFICATION_SHM_INTERPOSE_TESTING \
    Tests/NotificationSharedMemoryTests.c -o "$test_directory/NotificationSharedMemoryInterpose"
DYLD_INSERT_LIBRARIES="$test_directory/libNotificationSharedMemory.dylib" "$test_directory/NotificationSharedMemoryInterpose"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/CodeSigningCompatibilityTests.c -framework CoreFoundation -o "$test_directory/CodeSigningCompatibility"
"$test_directory/CodeSigningCompatibility"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_GUEST_RUNTIME \
    Tests/LoggingCompatibilityTests.c -o "$test_directory/LoggingCompatibility"
"$test_directory/LoggingCompatibility"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/LoggingCompatibilityTests.c -o "$test_directory/LoggingCompatibilityNative"
"$test_directory/LoggingCompatibilityNative"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_GUEST_RUNTIME -dynamiclib \
    Guest/LoggingCompatibility.c -o "$test_directory/libLoggingCompatibility.dylib"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_LOGGING_INTERPOSE_TESTING \
    Tests/LoggingCompatibilityTests.c -o "$test_directory/LoggingCompatibilityInterpose"
DYLD_INSERT_LIBRARIES="$test_directory/libLoggingCompatibility.dylib" "$test_directory/LoggingCompatibilityInterpose"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/GuestEntitlementsTests.c ProcessSupport/GuestEntitlements.c -o "$test_directory/GuestEntitlements"
"$test_directory/GuestEntitlements"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/GuestCodeSignatureTests.c ProcessSupport/GuestCodeSignature.c -o "$test_directory/GuestCodeSignature"
"$test_directory/GuestCodeSignature"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 \
    Tests/GuestEntitlementsConversionTests.m Host/GuestEntitlementsConversion.m ProcessSupport/GuestEntitlements.c \
    -framework Foundation -o "$test_directory/GuestEntitlementsConversion"
"$test_directory/GuestEntitlementsConversion"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/SpringBoardEntitlementsTests.c -framework CoreFoundation -o "$test_directory/SpringBoardEntitlements"
"$test_directory/SpringBoardEntitlements"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -dynamiclib \
    -DBSSelfTaskHasEntitlement=LSBSpringBoardOriginalFixture -Wl,-U,_LSBSpringBoardOriginalFixture \
    Guest/SpringBoardEntitlements.c -framework CoreFoundation -o "$test_directory/libSpringBoardEntitlements.dylib"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -dynamiclib -DLSB_SPRINGBOARD_ENTITLEMENTS_PROVIDER \
    Tests/SpringBoardEntitlementsInterposeTests.c -framework CoreFoundation \
    -install_name "$test_directory/libSpringBoardEntitlementsProvider.dylib" -o "$test_directory/libSpringBoardEntitlementsProvider.dylib"
springboard_test_root="$test_directory/SpringBoardRuntimeRoot"
springboard_test_directory="$springboard_test_root/System/Library/CoreServices/SpringBoard.app"
mkdir -p "$springboard_test_directory"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/SpringBoardEntitlementsInterposeTests.c "$test_directory/libSpringBoardEntitlementsProvider.dylib" -framework CoreFoundation \
    -o "$springboard_test_directory/SpringBoard"
DYLD_INSERT_LIBRARIES="$test_directory/libSpringBoardEntitlements.dylib" DYLD_ROOT_PATH="$springboard_test_root" \
    LSB_SIMULATE_SPRINGBOARD_ENTITLEMENTS=1 "$springboard_test_directory/SpringBoard" enabled
DYLD_INSERT_LIBRARIES="$test_directory/libSpringBoardEntitlements.dylib" DYLD_ROOT_PATH="$springboard_test_root" \
    LSB_SIMULATE_SPRINGBOARD_ENTITLEMENTS=0 "$springboard_test_directory/SpringBoard" disabled
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/SpringBoardEntitlementsInterposeTests.c -framework CoreFoundation -Wl,-U,_LSBSpringBoardOriginalFixture \
    -o "$test_directory/SpringBoardEntitlementsAbsent"
DYLD_INSERT_LIBRARIES="$test_directory/libSpringBoardEntitlements.dylib" DYLD_ROOT_PATH="$springboard_test_root" \
    LSB_SIMULATE_SPRINGBOARD_ENTITLEMENTS=1 "$test_directory/SpringBoardEntitlementsAbsent" absent
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/SystemPowerCompatibilityTests.c -framework IOKit -framework CoreFoundation -o "$test_directory/SystemPowerCompatibility"
"$test_directory/SystemPowerCompatibility"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -dynamiclib \
    Guest/SystemPowerCompatibility.c -framework IOKit -install_name "$test_directory/libSystemPowerCompatibility.dylib" \
    -o "$test_directory/libSystemPowerCompatibility.dylib"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_SYSTEM_POWER_INTERPOSE_TESTING \
    Tests/SystemPowerCompatibilityTests.c -framework IOKit -framework CoreFoundation -o "$test_directory/SystemPowerCompatibilityInterpose"
DYLD_INSERT_LIBRARIES="$test_directory/libSystemPowerCompatibility.dylib" "$test_directory/SystemPowerCompatibilityInterpose"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/BootstrapRoleTests.c ProcessSupport/SpawnPlan.c ProcessSupport/StoreAppGroups.c -o "$test_directory/BootstrapRole"
"$test_directory/BootstrapRole"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/NativeServiceTransportTests.c ProcessSupport/SpawnPlan.c ProcessSupport/StoreAppGroups.c -o "$test_directory/NativeServiceTransport"
"$test_directory/NativeServiceTransport"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/NativeMetalControlTests.c ProcessSupport/NativeMetalControl.c -o "$test_directory/NativeMetalControl"
"$test_directory/NativeMetalControl"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -dynamiclib \
    Tests/NativeProbeFixture.c -o "$test_directory/NativeProbeFixture.dylib"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -dynamiclib -DLSB_NATIVE_PROBE_MISSING_ENTRY \
    Tests/NativeProbeFixture.c -o "$test_directory/NativeProbeMissingEntry.dylib"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/NativeProbeLoaderTests.c LiveProcess/NativeProbeLoader.c -o "$test_directory/NativeProbeLoader"
"$test_directory/NativeProbeLoader" "$test_directory/NativeProbeFixture.dylib" "$test_directory/NativeProbeMissingEntry.dylib"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_TRANSPORT_TESTING \
    Tests/SpawnTransportTests.c ProcessSupport/SpawnTransport.c ProcessSupport/SpawnPlan.c -o "$test_directory/SpawnTransport"
"$test_directory/SpawnTransport"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_TRANSPORT_TESTING \
    Tests/PeerProcessTransportTests.c ProcessSupport/SpawnTransport.c ProcessSupport/SpawnPlan.c -o "$test_directory/PeerProcessTransport"
"$test_directory/PeerProcessTransport"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_GUEST_RUNTIME \
    Tests/SpawnTransportTests.c ProcessSupport/SpawnTransport.c ProcessSupport/SpawnPlan.c -o "$test_directory/GuestSpawnTransport"
XPC_FLAGS=0x2 "$test_directory/GuestSpawnTransport"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_XPC_WIRE_TESTING -dynamiclib \
    Guest/XPCWireCompatibility.c -install_name "$test_directory/libXPCWireCompatibility.dylib" -o "$test_directory/libXPCWireCompatibility.dylib"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    -DLSB_GUEST_RUNTIME -DLSB_XPC_WIRE_COMPATIBILITY -DLSB_XPC_WIRE_TESTING \
    Tests/SpawnTransportTests.c ProcessSupport/SpawnTransport.c ProcessSupport/SpawnPlan.c \
    "$test_directory/libXPCWireCompatibility.dylib" -o "$test_directory/WireSpawnTransport"
DYLD_INSERT_LIBRARIES="$test_directory/libXPCWireCompatibility.dylib" XPC_FLAGS=0x2 "$test_directory/WireSpawnTransport"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 -DLSB_XPC_WIRE_TESTING \
    Tests/XPCWireCompatibilityTests.c "$test_directory/libXPCWireCompatibility.dylib" -o "$test_directory/XPCWireCompatibility"
DYLD_INSERT_LIBRARIES="$test_directory/libXPCWireCompatibility.dylib" XPC_FLAGS=0x2 "$test_directory/XPCWireCompatibility"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 -IProcessSupport -IHost \
    Tests/BrokerTests.m Host/SessionControlQueue.c Host/TerminalSession.m Host/LaunchdConfiguration.m ProcessSupport/LaunchdHandshake.c ProcessSupport/LaunchdProbe.c \
    Host/IndigoHIDServer.m Host/FramebufferServer.m Host/IOSurfaceServer.m ProcessSupport/LaunchdEndpoint.c \
    Host/GuestEntitlementsConversion.m Host/DeviceType.m Host/DeviceConfiguration.m Host/RuntimeStorage.m ProcessSupport/StoreAppGroups.c \
    ProcessSupport/GuestEntitlements.c \
    ProcessSupport/FakePTY.c ProcessSupport/SpawnPlan.c ProcessSupport/ChildStatus.c ProcessSupport/NativeMetalControl.c \
    -framework Foundation -framework IOSurface -o "$test_directory/Broker"
"$test_directory/Broker"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 \
    Tests/IndigoHIDServerTests.m Host/IndigoHIDServer.m -framework Foundation -o "$test_directory/IndigoHIDServer"
"$test_directory/IndigoHIDServer"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 \
    Tests/IndigoKeyboardTests.m Host/IndigoHIDServer.m -framework Foundation -o "$test_directory/IndigoKeyboard"
"$test_directory/IndigoKeyboard"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 \
    Tests/IndigoHomeButtonTests.m Host/IndigoHIDServer.m -framework Foundation -o "$test_directory/IndigoHomeButton"
"$test_directory/IndigoHomeButton"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/SimulatorKeyboardInputTests.c -o "$test_directory/SimulatorKeyboardInput"
"$test_directory/SimulatorKeyboardInput"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 \
    Tests/FramebufferServerTests.m Host/FramebufferServer.m -framework Foundation -framework IOSurface -framework CoreGraphics \
    -o "$test_directory/FramebufferServer"
"$test_directory/FramebufferServer"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 \
    Tests/IOSurfaceServerTests.m -framework Foundation -framework IOSurface -o "$test_directory/IOSurfaceServer"
"$test_directory/IOSurfaceServer"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/LaunchdEndpointTests.c ProcessSupport/LaunchdEndpoint.c -o "$test_directory/LaunchdEndpoint"
"$test_directory/LaunchdEndpoint"
xcrun --sdk macosx clang -fobjc-arc -Wall -Wextra -Werror -arch arm64 Tests/SimulatorDisplayTests.m \
    -framework Foundation -framework CoreGraphics -framework IOSurface -framework ImageIO -o "$test_directory/SimulatorDisplay"
"$test_directory/SimulatorDisplay"
xcrun --sdk macosx clang -Wall -Wextra -Werror -arch arm64 Tests/SimulatorDisplayLayoutTests.m \
    -framework CoreGraphics -o "$test_directory/SimulatorDisplayLayout"
"$test_directory/SimulatorDisplayLayout"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 \
    Tests/MetalDisplayRendererTests.m Host/MetalDisplayRenderer.m \
    -framework Foundation -framework QuartzCore -framework CoreGraphics -framework IOSurface -framework Metal \
    -o "$test_directory/MetalDisplayRenderer"
"$test_directory/MetalDisplayRenderer"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 -IProcessSupport -IHost \
    Tests/LaunchdConfigurationTests.m Host/LaunchdConfiguration.m Host/DeviceType.m ProcessSupport/SpawnPlan.c \
    -framework Foundation -o "$test_directory/LaunchdConfiguration"
"$test_directory/LaunchdConfiguration"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 \
    Tests/DeviceTypeTests.m Host/DeviceType.m -framework Foundation -o "$test_directory/DeviceType"
"$test_directory/DeviceType"
python3 Tests/SetupIOS26DeviceTypesTests.py
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 \
    Tests/DeviceConfigurationTests.m Host/DeviceConfiguration.m Host/DeviceType.m -framework Foundation -o "$test_directory/DeviceConfiguration"
"$test_directory/DeviceConfiguration"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 -IHost -IProcessSupport \
    Tests/GuestDeviceSettingsTests.m Host/GuestDeviceSettings.m Host/DeviceType.m -framework Foundation -o "$test_directory/GuestDeviceSettings"
"$test_directory/GuestDeviceSettings"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 -IProcessSupport -IHost \
    Tests/TerminalSessionTests.m Host/TerminalSession.m ProcessSupport/FakePTY.c ProcessSupport/SpawnPlan.c \
    -framework Foundation -o "$test_directory/TerminalSession"
"$test_directory/TerminalSession"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/TerminalFishTests.c -o "$test_directory/TerminalFish"
"$test_directory/TerminalFish"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 \
    Tests/TerminalScreenTests.m -framework Foundation -o "$test_directory/TerminalScreen"
"$test_directory/TerminalScreen"
xcrun --sdk macosx clang -fobjc-arc -Wall -Wextra -Werror -arch arm64 \
    Tests/BackgroundAudioTests.m Host/BackgroundAudio.m -framework Foundation -o "$test_directory/BackgroundAudio"
"$test_directory/BackgroundAudio"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/DisplaySettingsTests.c ProcessSupport/DisplaySettings.c -o "$test_directory/DisplaySettings"
"$test_directory/DisplaySettings"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/DisplaySettingsRenderServerTests.c -o "$test_directory/DisplaySettingsRenderServer"
"$test_directory/DisplaySettingsRenderServer"
xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
    Tests/DisplaySettingsWorkerTests.c ProcessSupport/DisplaySettings.c -o "$test_directory/DisplaySettingsWorker"
"$test_directory/DisplaySettingsWorker"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 \
    Tests/GuestDisplaySettingsTests.m Host/GuestDisplaySettings.m ProcessSupport/DisplaySettings.c \
    Host/RuntimeStorage.m ProcessSupport/StoreAppGroups.c -framework Foundation -o "$test_directory/GuestDisplaySettings"
"$test_directory/GuestDisplaySettings"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 \
    Tests/ExtensionLauncherTests.m -framework Foundation -o "$test_directory/ExtensionLauncher"
"$test_directory/ExtensionLauncher"
xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 \
    Tests/RunningBoardMonitorTests.m -framework Foundation -o "$test_directory/RunningBoardMonitor"
"$test_directory/RunningBoardMonitor"
