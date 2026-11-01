#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output=${1:?Usage: build-native-signer.sh output.dylib}
mkdir -p "$(dirname "$output")"
xcrun --sdk iphoneos clang -arch arm64 -target arm64-apple-ios26.0 -dynamiclib -fobjc-arc -fblocks -O2 -Wall -Wextra -Werror \
    NativeSigner/LazyCodeSigner.m NativeSigner/Service.m -framework Foundation -framework Security \
    -Wl,-install_name,@rpath/NativeSigner.dylib -o "$output"
if [ "${CODE_SIGNING_ALLOWED:-NO}" = YES ] && [ -n "${EXPANDED_CODE_SIGN_IDENTITY:-}" ]; then
    /usr/bin/codesign --force --sign "$EXPANDED_CODE_SIGN_IDENTITY" --timestamp=none "$output"
fi
