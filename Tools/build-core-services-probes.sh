#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output=${1:-tmp/core-services-probes}
mkdir -p "$output"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios14.0-simulator -std=c11 -Wall -Wextra -Werror \
    Tests/Guest/core-services-probe/CoreServicesProbe.c -o "$output/core-services-probe"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios14.0-simulator -std=c11 -Wall -Wextra -Werror \
    Tests/Guest/distributed-notification-probe/DistributedNotificationProbe.c -framework CoreFoundation -o "$output/distributed-notification-probe"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios26.0-simulator -fobjc-arc -fblocks -Wall -Wextra -Werror \
    Tests/Guest/launchservices-probe/LaunchServicesProbe.m -framework Foundation -o "$output/launchservices-probe"
printf '%s\n' "$output/core-services-probe" "$output/distributed-notification-probe" "$output/launchservices-probe"
