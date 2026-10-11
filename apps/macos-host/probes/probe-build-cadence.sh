#!/bin/bash
set -euo pipefail
# Optional signing arguments let deployment reuse the host's approved
# development identity/identifier. This script only builds/signs; it never runs
# the probe or changes TCC. Pass the established identity explicitly when needed.
if (( $# > 3 )); then
    printf 'Usage: %s [output] [signing-identity] [signing-identifier]\n' "$0" >&2
    exit 64
fi
probe_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
probe_output="${1:-$probe_dir/capture-cadence}"
probe_identity="${2:--}"
probe_identifier="${3:-dev.remote-native.capture-cadence}"
mkdir -p -- "$(dirname -- "$probe_output")"
xcrun clang++ -std=c++20 -fobjc-arc -O2 -Wall -Wextra -Werror -mmacosx-version-min=13.0 \
    -framework Foundation -framework ScreenCaptureKit -framework CoreGraphics \
    -framework CoreMedia -framework CoreVideo \
    "$probe_dir/capture-cadence.mm" -o "$probe_output"
codesign --force --sign "$probe_identity" --identifier "$probe_identifier" "$probe_output"
codesign --verify --strict "$probe_output"
printf 'Built metadata-only SCK cadence probe: %s\n' "$probe_output"
