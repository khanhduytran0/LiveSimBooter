#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
usage() {
    printf '%s\n' 'Usage: Tools/build-metal-iosurface-probe.sh [--clear-only] [output [metallib]]' >&2
}
output=tmp/metal-iosurface/metal-iosurface-probe
metallib=
clear_only=0
positionals=0
options=1
for argument do
    if [ "$options" = 1 ]; then
        case "$argument" in
            --clear-only) clear_only=1; continue ;;
            --help|-h) usage; exit 0 ;;
            --) options=0; continue ;;
            -*) usage; exit 2 ;;
        esac
    fi
    case "$positionals" in
        0) output=${argument:-tmp/metal-iosurface/metal-iosurface-probe} ;;
        1) metallib=$argument ;;
        *) usage; exit 2 ;;
    esac
    positionals=$((positionals + 1))
done
if [ "$clear_only" = 1 ] && [ -n "$metallib" ]; then
    printf '%s\n' '--clear-only cannot be combined with a metallib input.' >&2
    usage
    exit 2
fi
mkdir -p "$(dirname "$output")"
if [ -n "$metallib" ]; then
    header="$output.metallib.h"
    python3 - "$metallib" "$header" <<'PY'
import pathlib, sys
source, header = map(pathlib.Path, sys.argv[1:])
data = source.read_bytes()
if not data.startswith(b'MTLB') or len(data) > 16 * 1024 * 1024:
    raise SystemExit('Expected a Metal library no larger than 16 MiB')
rows = [', '.join(f'0x{value:02x}' for value in data[offset:offset+16]) for offset in range(0, len(data), 16)]
header.write_text('static const unsigned char LSBProbeMetalLibrary[] = {\n    ' + ',\n    '.join(rows) + '\n};\n')
PY
    header="$(cd "$(dirname "$header")" && pwd)/$(basename "$header")"
    set -- "-DLSB_PROBE_METALLIB_HEADER=\"$header\""
else
    set --
fi
if [ "$clear_only" = 1 ]; then
    set -- "$@" -DLSB_PROBE_CLEAR_ONLY=1
fi
xcrun --sdk iphonesimulator clang -arch arm64 -target arm64-apple-ios26.0-simulator -fobjc-arc -fblocks -Wall -Wextra -Werror "$@" \
    Tests/Guest/metal-iosurface-probe/MetalIOSurfaceProbe.m -framework Foundation -framework Metal -framework IOSurface -framework ImageIO \
    -framework CoreGraphics -o "$output"
printf '%s\n' "$output"
