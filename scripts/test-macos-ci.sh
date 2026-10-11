#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build/ci-tests/macos}"
transport_lib="${2:-target/release/libremote_transport.a}"
mkdir -p "$out"
# Compile diagnostics without starting desktop capture or opening test windows.
bash apps/macos-host/probes/probe-build-pattern.sh "$out/capture-pattern" > "$out/capture-pattern-build.txt"
bash apps/macos-host/probes/probe-build-cadence.sh "$out/capture-cadence" > "$out/capture-cadence-build.txt"
"$out/capture-cadence" --help > "$out/capture-cadence-help.txt"
expect_argument_failure() {
  local executable="$1" expected="$2" result
  shift 2
  if "$executable" "$@" > /dev/null 2>&1; then
    printf 'Diagnostic accepted invalid arguments\n' >&2; exit 1
  else
    result=$?
    if [[ "$result" != "$expected" ]]; then
      printf 'Diagnostic returned unexpected argument error: %s\n' "$result" >&2; exit 1
    fi
  fi
}
expect_argument_failure "$out/capture-cadence" 64 --seconds 0
expect_argument_failure "$out/capture-cadence" 64 --seconds 61
expect_argument_failure "$out/capture-cadence" 64 --queue-depth 2
expect_argument_failure "$out/capture-cadence" 64 --fps 61
expect_argument_failure "$out/capture-cadence" 64 --max-height 721
expect_argument_failure "$out/capture-cadence" 64 --seconds
expect_argument_failure "$out/capture-pattern" 2 --pacing unknown
expect_argument_failure "$out/capture-pattern" 2 --fullscreen false
printf 'PASS capture diagnostic build and argument validation (no desktop capture)\n'
xcrun clang++ -std=c++20 -O2 -Wall -Wextra -Werror \
  shared/probes/latency-stats-test.cpp -o "$out/latency-stats"
"$out/latency-stats" | tee "$out/latency-stats.txt"
xcrun clang++ -std=c++20 -O2 -Wall -Wextra -Werror \
  apps/macos-host/probes/encoder-wake-test.cpp -o "$out/encoder-wake"
"$out/encoder-wake" | tee "$out/encoder-wake.txt"
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
# Owned synthetic samples exercise production epoch / recovery / wake logic;
# this probe does not start ScreenCaptureKit or perform a real restart.
xcrun clang++ -std=c++20 -fobjc-arc -O2 -Wall -Wextra -Wno-unused-parameter \
  -mmacosx-version-min=15.2 apps/macos-host/probes/capture-recovery-probe.mm \
  apps/macos-host/host-ui.mm apps/macos-host/mac-exchange.mm apps/macos-host/restart-mac.mm \
  apps/macos-host/localization.mm shared/i18n.cpp shared/exchange.cpp shared/diagnostic-log.cpp "$transport_lib" \
  -framework AppKit -framework ApplicationServices -framework ScreenCaptureKit \
  -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework Security \
  -framework Carbon -framework SystemConfiguration -framework Network -framework Foundation -framework ImageIO \
  -o "$out/capture-recovery"
"$out/capture-recovery" "$out/capture-recovery.json" > "$out/capture-recovery.txt"
printf 'PASS production capture lifecycle / recovery / wake regression\n'
