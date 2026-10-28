#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output=${1:-tmp/audio-output/libLiveSimAudio.dylib}
mkdir -p "$(dirname "$output")"
# Separate optional image: ordinary guests keep the C spawn shim's dependency graph when audio is disabled.
# Broker calls resolve to that shim, retaining its one synchronous XPC wire context and mach_msg interposer.
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios14.0-simulator -std=c11 -fblocks -Wall -Wextra -Werror \
    -dynamiclib -fvisibility=hidden -install_name /usr/lib/libLiveSimAudio.dylib -Wl,-not_for_dyld_shared_cache \
    -Wl,-U,_LSBTransportOpenAudioOutput -Wl,-U,_LSBTransportCloseAudioOutput \
    Guest/AudioOutputCompatibility.c Guest/AudioOutputTransport.c ProcessSupport/AudioOutput.c \
    -framework CoreAudio -framework CoreFoundation -o "$output"
printf '%s\n' "$output"
