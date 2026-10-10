# Cursor orientation regression

The cursor wire format uses rows from top to bottom, straight-alpha RGBA, and a hotspot measured from the top-left corner in pixels. AppKit supplies the hotspot in points with the same top-left origin. See [Apple's NSCursor hotspot documentation](https://developer.apple.com/documentation/appkit/nscursor/hotspot).

The old Mac bitmap conversion applied `Translate(0, height)` and `Scale(1, -1)` before drawing the cursor CGImage. In a fresh bitmap context that inverted transmitted rows while leaving the hotspot upright. `cursor-image.h` now preserves the CGImage row order. `cursor-native.h` consumes that order through a [negative DIB height](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapv5header).

The Windows V5 color bitmap retains straight BGRA channels before `CreateIconIndirect`. On the tested Windows 11 native cursor path, an additional manual RGB pre-multiplication darkened translucent pixels twice. The native rendering test checks actual `DrawIconEx` source-over output against the expected colors on an opaque gray background, including translucent arrow edges.

## Mac

From the project root:

```bash
xcrun clang++ -std=c++20 -fobjc-arc -O2 -Wall -Wextra -Werror \
  -mmacosx-version-min=15.2 apps/macos-host/probes/cursor-pattern.mm \
  -framework AppKit -framework ApplicationServices -o build/CursorPattern
build/CursorPattern build/cursor-validation
```

The probe creates an asymmetric 7 × 5 image with a red top row, blue bottom row, green bottom-right corner and translucent center. Its logical size is half its pixel size, so hotspot `(1, 0.5)` must become `(2, 1)`. It also converts the system arrow and compares its alpha coverage to an independent `NSBitmapImageRep` reader. A vertically mirrored arrow must differ from that reference. It exports only cursor images and cursor packets; it does not capture the desktop or read credentials.

## Windows

Copy the probe's `cursor-asymmetric.bin` and `cursor-arrow.bin` to `build/`, then run:

```powershell
tools/mingw64/bin/g++.exe -std=c++20 -O2 -Wall -Wextra -Werror `
  -DUNICODE -D_UNICODE -static-libgcc -static-libstdc++ `
  apps/windows-client/probes/cursor-render.cpp -o build/CursorRender.exe -lgdi32 -luser32
build/CursorRender.exe build/cursor-asymmetric.bin build/cursor-arrow.bin build/cursor-windows-validation.json
```

The test uses the production cursor creation function. `GetIconInfo` verifies the native hotspot; `DrawIconEx` checks the rendered rows, channel order and transparency against the independently defined fixture and the Mac arrow's wire pixels. It also rejects an out-of-bounds hotspot. It draws into its own memory bitmap and does not move the desktop mouse or inject input.

Validated on the target Mac on 2026-10-10: 11 checks passed; native arrow 17 × 23, hotspot `(4, 4)`. The Windows native rendering test also passed all 11 checks.
