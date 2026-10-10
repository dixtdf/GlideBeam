#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
mkdir -p dist/probes
xcrun clang++ -std=c++20 -fobjc-arc -O2 -g -Wall -Wextra -Wno-unused-parameter \
  -mmacosx-version-min=15.2 apps/macos-host/probes/clipboard-exchange.mm \
  apps/macos-host/mac-exchange.mm apps/macos-host/host-ui.mm shared/exchange.cpp shared/diagnostic-log.cpp \
  -framework AppKit -framework ApplicationServices -framework ImageIO \
  -o dist/probes/clipboard-exchange
dist/probes/clipboard-exchange
