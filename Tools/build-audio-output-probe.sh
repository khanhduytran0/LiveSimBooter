#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output=${1:-tmp/audio-output/audio-output-probe}
mkdir -p "$(dirname "$output")"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios14.0-simulator -std=c11 -fblocks -Wall -Wextra -Werror \
    Guest/AudioOutputProbe.c -framework AudioToolbox -framework CoreAudio -framework CoreFoundation -o "$output"
printf '%s\n' "$output"
