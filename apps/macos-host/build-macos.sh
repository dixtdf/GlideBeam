#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
transport_lib="${1:-target/release/libremote_transport.a}"
app_dir="${2:-dist/RemoteHost.app}"
version="${3:-${GLIDEBEAM_VERSION:-$(cat VERSION)}}"
architecture="${4:-$(uname -m)}"
if [[ ! "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$ ]]; then
  printf 'Invalid semantic version: %s\n' "$version" >&2
  exit 2
fi
case "$architecture" in x86_64|arm64) ;; *) printf 'Unsupported architecture: %s\n' "$architecture" >&2; exit 2;; esac
lipo "$transport_lib" -verify_arch "$architecture"
mkdir -p "$app_dir/Contents/MacOS" "$app_dir/Contents/Resources"
xcrun clang++ -std=c++20 -fobjc-arc -O2 -g -Wall -Wextra -Wno-unused-parameter \
  -arch "$architecture" -mmacosx-version-min=15.2 apps/macos-host/main.mm apps/macos-host/host-ui.mm \
  apps/macos-host/mac-exchange.mm apps/macos-host/restart-mac.mm \
  apps/macos-host/localization.mm shared/i18n.cpp \
  shared/exchange.cpp shared/diagnostic-log.cpp "$transport_lib" \
  -framework AppKit -framework ApplicationServices -framework ScreenCaptureKit \
  -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework Security \
  -framework Carbon -framework SystemConfiguration -framework Network -framework Foundation -framework ImageIO \
  -o "$app_dir/Contents/MacOS/RemoteHost"
cp apps/macos-host/Info.plist "$app_dir/Contents/Info.plist"
cp assets/app-icon.icns "$app_dir/Contents/Resources/RemoteNative.icns"
if [ -d apps/macos-host/resources ]; then
  cp -R apps/macos-host/resources/. "$app_dir/Contents/Resources/"
fi
plutil -replace CFBundleShortVersionString -string "${version%%-*}" "$app_dir/Contents/Info.plist"
if [ -n "${GLIDEBEAM_BUILD_NUMBER:-}" ]; then
  [[ "$GLIDEBEAM_BUILD_NUMBER" =~ ^[0-9]+$ ]]
  plutil -replace CFBundleVersion -string "$GLIDEBEAM_BUILD_NUMBER" "$app_dir/Contents/Info.plist"
fi
plutil -lint "$app_dir/Contents/Info.plist"
project_signing="$HOME/Library/Application Support/RemoteNative/signing"
if [ "${CI:-false}" != true ] && [ "${REMOTE_USE_LOCAL_SIGNING:-1}" != 0 ] && [ -z "${REMOTE_SIGNING_IDENTITY:-}" ] && [ -f "$project_signing/identity-sha1" ]; then
  REMOTE_SIGNING_IDENTITY=$(cat "$project_signing/identity-sha1")
  REMOTE_SIGNING_KEYCHAIN="$project_signing/development.keychain-db"
  security unlock-keychain -p "$(cat "$project_signing/keychain-password")" "$REMOTE_SIGNING_KEYCHAIN"
fi
if [ -n "${REMOTE_SIGNING_IDENTITY:-}" ]; then
  : "${REMOTE_SIGNING_KEYCHAIN:?Dedicated development keychain is required}"
  [ "${#REMOTE_SIGNING_IDENTITY}" -eq 40 ]
  case "$REMOTE_SIGNING_IDENTITY" in *[!0-9a-fA-F]*) exit 1;; esac
  /usr/bin/python3 apps/macos-host/sign-project.py --identity "$REMOTE_SIGNING_IDENTITY" \
    --keychain "$REMOTE_SIGNING_KEYCHAIN" --app "$app_dir"
else
  codesign --force --sign - --identifier dev.remote-native.host "$app_dir"
fi
codesign --verify --strict "$app_dir"
printf 'Built %s (%s, %s)\n' "$app_dir" "$version" "$architecture"
