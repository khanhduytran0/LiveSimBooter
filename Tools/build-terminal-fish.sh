#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output=${1:-tmp/terminal-fish}
mkdir -p "$output"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios14.0-simulator -std=c11 -Wall -Wextra -Werror -O2 \
    Guest/terminal-fish/TerminalFish.c -o "$output/terminal-fish"
printf '%s\n' "$output/terminal-fish"
