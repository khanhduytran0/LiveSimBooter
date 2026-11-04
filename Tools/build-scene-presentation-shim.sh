#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output=${1:-tmp/scene-presentation/libLiveSimScene.dylib}
mkdir -p "$(dirname "$output")"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios26.0-simulator -fobjc-arc -fblocks -Wall -Wextra -Werror \
    -dynamiclib -fvisibility=hidden -Wl,-not_for_dyld_shared_cache Guest/libLiveSimScene/ScenePresentationCompatibility.m \
    -framework Foundation -framework UIKit -framework CoreGraphics -install_name /usr/lib/libLiveSimScene.dylib -o "$output"
printf '%s\n' "$output"
