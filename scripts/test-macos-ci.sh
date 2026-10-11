#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build/ci-tests/macos}"
mkdir -p "$out"
xcrun clang++ -std=c++20 -O2 -Wall -Wextra -Werror \
  shared/probes/audio-playout-test.cpp -o "$out/audio-playout"
"$out/audio-playout" | tee "$out/audio-playout.txt"
xcrun clang++ -std=c++20 -O2 -Wall -Wextra -Werror \
  apps/macos-host/probes/audio-packetizer-test.cpp -o "$out/audio-packetizer"
"$out/audio-packetizer" | tee "$out/audio-packetizer.txt"
# No ScreenCaptureKit capture or VideoToolbox hardware requirement on hosted CI.
# Restart SelfTest injects a mock authorizer and never executes a real reboot.
xcrun clang++ -std=c++20 -O2 -Wall -Wextra -Werror \
  shared/probes/diagnostic-log-test.cpp shared/diagnostic-log.cpp \
  -o "$out/diagnostic-log"
"$out/diagnostic-log" "$out/logger-$(uuidgen)" | tee "$out/diagnostic-log.txt"
xcrun clang++ -std=c++20 -fobjc-arc -O2 -Wall -Wextra -Wno-unused-parameter \
  apps/macos-host/probes/restart-mac.mm apps/macos-host/restart-mac.mm \
  apps/macos-host/localization.mm shared/i18n.cpp shared/diagnostic-log.cpp \
  -framework AppKit -framework Foundation -o "$out/restart-mock"
"$out/restart-mock" | tee "$out/restart-mock.txt"
xcrun clang++ -std=c++20 -O2 -Wall -Wextra -Werror \
  apps/macos-host/probes/nv12-layout-test.cpp -o "$out/nv12-layout"
"$out/nv12-layout" | tee "$out/nv12-layout.txt"
