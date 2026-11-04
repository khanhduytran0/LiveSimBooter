#!/bin/sh
# sh Tests/GenericForkBuild.sh {macosx|iphonesimulator} OUTPUT_DIRECTORY
set -eu
case "${1-}" in
    macosx) target=arm64-apple-macos13 ;;
    iphonesimulator) target=arm64-apple-ios26.0-simulator ;;
    *) echo "usage: $0 {macosx|iphonesimulator} OUTPUT_DIRECTORY" >&2; exit 64 ;;
esac
test "$#" -eq 2
mkdir -p "$2"
tests=$(dirname "$0")
sdk=$1
cc() { xcrun --sdk "$sdk" clang -target "$target" -std=c11 -D_DARWIN_C_SOURCE -Wall -Wextra -Werror "$@"; }
cc -dynamiclib -install_name @loader_path/libfork-fixture.dylib "$tests/Guest/libfork-fixture/GenericForkCaller.c" -o "$2/libfork-fixture.dylib"
cc "$tests/Guest/fork-fixture/GenericFork.c" "$2/libfork-fixture.dylib" -o "$2/fork-fixture"
cc "$tests/Guest/fork-resolver/ForkResolver.c" -o "$2/fork-resolver"
cc -O2 "$tests/Guest/fork-stack-bounds/ForkStackBounds.c" -o "$2/fork-stack-bounds"
