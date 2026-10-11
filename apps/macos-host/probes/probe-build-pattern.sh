#!/bin/bash
set -euo pipefail
probe_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
probe_output="${1:-$probe_dir/capture-pattern}"
# NSView's macOS 14+ CADisplayLink is provided by AppKit/QuartzCore. The fixture
# guards its new display/animation modes at runtime; it does not use the legacy
# CoreVideo CVDisplayLink API and needs no additional CoreVideo framework link.
xcrun clang++ -std=c++20 -fobjc-arc -O2 -Wall -Wextra \
    -framework AppKit -framework Foundation -framework QuartzCore \
    "$probe_dir/capture-pattern.mm" -o "$probe_output"
printf 'Built own-window capture fixture: %s\n' "$probe_output"
