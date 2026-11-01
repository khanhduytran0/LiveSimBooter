#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
signing_test_parent=${1:-"$PWD/tmp"}
mkdir -p "$signing_test_parent"
signing_test_directory=$(mktemp -d "$signing_test_parent/signing-tests.XXXXXX")
trap 'rm -rf "$signing_test_directory"' EXIT HUP INT TERM
umask 077

# This throwaway certificate never comes from a user account or keychain. Both tests import it in memory only;
# keychain/defaults writes in SigningIdentityTests are mocks. The trap removes its private key and P12 on exit.
/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj '/CN=LiveSimBooter Signing Test' \
    -keyout "$signing_test_directory/key.pem" -out "$signing_test_directory/certificate.pem" \
    > "$signing_test_directory/openssl.log" 2>&1
/usr/bin/openssl pkcs12 -export -inkey "$signing_test_directory/key.pem" -in "$signing_test_directory/certificate.pem" \
    -out "$signing_test_directory/identity.p12" -passout pass:fixture-only

for signing_test_name in SigningIdentity NativeSignerService; do
    xcrun --sdk macosx clang -fobjc-arc -fblocks -Wall -Wextra -Werror -arch arm64 -mmacosx-version-min=15.0 \
        "Tests/${signing_test_name}Tests.m" -framework Foundation -framework Security -o "$signing_test_directory/$signing_test_name"
    "$signing_test_directory/$signing_test_name" "$signing_test_directory/identity.p12"
done
