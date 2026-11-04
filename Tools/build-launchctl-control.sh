#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output=${1:-tmp/launchctl-control}
mkdir -p "$output"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios14.0-simulator -std=c11 -Wall -Wextra -Werror -O2 \
    Guest/launchctl-control/LaunchctlControl.c -o "$output/launchctl-control"
printf '%s\n' "$output/launchctl-control"
