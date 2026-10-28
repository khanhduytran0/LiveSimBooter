#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output=${1:-tmp/haptics/libLiveSimHaptics.dylib}
mkdir -p "$(dirname "$output")"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios26.0-simulator -fobjc-arc -fblocks -Wall -Wextra -Werror \
    -dynamiclib -fvisibility=hidden -install_name /usr/lib/libLiveSimHaptics.dylib -Wl,-not_for_dyld_shared_cache \
    Guest/HapticCompatibility.m -framework Foundation -framework CoreHaptics -o "$output"
printf '%s\n' "$output"
