#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output=${1:-tmp/display-settings}
mkdir -p "$output"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios14.0-simulator -fobjc-arc -Wall -Wextra -Werror -O2 \
    Guest/display-settings/DisplaySettingsControl.m Guest/display-settings/DisplaySettingsRenderServer.c Guest/display-settings/DisplaySettingsWorker.c \
    ProcessSupport/DisplaySettings.c \
    -framework Foundation -o "$output/display-settings"
printf '%s\n' "$output/display-settings"
