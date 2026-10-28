#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output=${1:-tmp/haptics/lsb-haptic-relay-probe}
mkdir -p "$(dirname "$output")"
# Build only. The runtime stager signs this executable for the destination app; the guest helper is separately inserted by its broker plan.
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios26.0-simulator -fobjc-arc -fblocks -Wall -Wextra -Werror -O2 \
    Guest/HapticProbe.m -framework Foundation -framework CoreHaptics -framework AVFAudio -o "$output"
printf '%s\n' "$output"
