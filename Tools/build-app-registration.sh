#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output=${1:-tmp/app-registration}
mkdir -p "$output"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios26.0-simulator -fobjc-arc -fblocks -Wall -Wextra -Werror \
    Guest/app-registration/ApplicationRegistration.m Guest/app-registration/ApplicationRegistrationPlan.m -framework Foundation -framework CoreServices \
    -o "$output/app-registration"
printf '%s\n' "$output/app-registration"
