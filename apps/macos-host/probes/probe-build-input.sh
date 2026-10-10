#!/bin/bash
set -euo pipefail
probe_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
probe_output="${1:-/Users/Shared/RemoteNative/InputPattern}"
app_dir="${probe_output}.app"
mkdir -p "$app_dir/Contents/MacOS"
xcrun clang++ -std=c++20 -fobjc-arc -O2 -Wall -Wextra -mmacosx-version-min=15.2 \
    -framework AppKit -framework ApplicationServices -framework Foundation \
    "$probe_dir/input-pattern.mm" -o "$app_dir/Contents/MacOS/InputPattern"
cat > "$app_dir/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>dev.remote-native.input-pattern</string>
<key>CFBundleExecutable</key><string>InputPattern</string>
<key>CFBundleName</key><string>Remote Native Input Pattern</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
codesign --force --sign - --identifier dev.remote-native.input-pattern "$app_dir"
# The entry point remains the requested /Users/Shared/RemoteNative/InputPattern path.
cat > "$probe_output" <<'LAUNCHER'
#!/bin/bash
set -euo pipefail
app_dir="${BASH_SOURCE[0]}.app"
exec /usr/bin/open -n "$app_dir" --args "$@"
LAUNCHER
chmod 755 "$probe_output"
printf 'Built own-window input fixture: %s\n' "$probe_output"
