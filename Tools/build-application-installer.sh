#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output=${1:-tmp/application-installer}
mkdir -p "$output"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios26.0-simulator -fobjc-arc -fblocks -Wall -Wextra -Werror \
    Guest/lsb-app-install/ApplicationInstaller.m -F Guest/lsb-app-install/Frameworks -framework Foundation -framework CoreServices \
    -framework InstallCoordination -framework MobileInstallation -o "$output/lsb-app-install"
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios26.0-simulator -fobjc-arc -fblocks -Wall -Wextra -Werror \
    -dynamiclib Guest/libLiveSimInstall/InstallationCompatibility.m -framework Foundation -install_name /usr/lib/libLiveSimInstall.dylib \
    -o "$output/libLiveSimInstall.dylib"
printf '%s\n' "$output/lsb-app-install" "$output/libLiveSimInstall.dylib"
