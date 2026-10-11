# Building GlideBeam

Build from the repository root. `VERSION` controls release names and UI version metadata. Rust 1.99.0 is pinned in `rust-toolchain.toml`; use `--locked` to preserve `Cargo.lock`. First-party native code requires C++20.

Audio is currently a development-branch feature; released `v0.2.1` binaries do not contain it. The paired audio tests used `0.3.0-dev.audio`: pass `-Version 0.3.0-dev.audio` to the Windows build and use `0.3.0-dev.audio` as the third argument of `build-macos.sh` for matching test metadata. Build both endpoints from the same branch. These overrides do not create a release or change `VERSION`.

## Windows x64

Install Rust/rustup, Python 3, and the MSYS2 UCRT64 GCC toolchain. Use `mingw-w64-ucrt-x86_64-gcc`, with its `ucrt64/bin` directory on PATH. The public repository does not contain a downloaded toolchain.

```powershell
rustup target add x86_64-pc-windows-gnu
./scripts/build-windows.ps1 -Architecture x64 -RustTarget x86_64-pc-windows-gnu -ToolchainDirectory C:\msys64\ucrt64\bin -OutputDirectory dist/windows-x64
./scripts/test-windows-ci.ps1 -RuntimeDirectory dist/windows-x64 -ToolchainDirectory C:\msys64\ucrt64\bin
python scripts/check_windows_binary.py --runtime dist/windows-x64 --architecture x64
python scripts/third-party-notices.py --output build/third-party/THIRD-PARTY-LICENSES.txt --toolchain-root C:\msys64\ucrt64
python scripts/package-ci.py --platform Windows --architecture x64 --runtime dist/windows-x64 --output artifacts
```

The development executable remains `RemoteNative.exe`; public packaging renames it to `GlideBeam.exe` and includes `Start-GlideBeam.cmd`. `-CargoExe`, `-Version`, and `-OutputDirectory` can select explicit build inputs; `-SkipRust` reuses an existing transport DLL. Keep the executable and its required DLLs together.

## Windows ARM64

The Windows x64 host can cross-compile a native ARM64 PE. The setup helper downloads the pinned LLVM-MinGW release and verifies its SHA-256.

```powershell
./scripts/setup-llvm-mingw.ps1 -Destination ./build/llvm-mingw
rustup target add aarch64-pc-windows-gnullvm
./scripts/build-windows.ps1 -Architecture arm64 -RustTarget aarch64-pc-windows-gnullvm -ToolchainDirectory ./build/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64/bin -OutputDirectory dist/windows-arm64
python scripts/check_windows_binary.py --runtime dist/windows-arm64 --architecture arm64
python scripts/third-party-notices.py --output build/third-party/THIRD-PARTY-LICENSES.txt --toolchain-root ./build/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64
python scripts/package-ci.py --platform Windows --architecture arm64 --runtime dist/windows-arm64 --output artifacts
```

ARM64 binaries are verified for architecture, manifest, and imports. They are not executed on an x64 runner, and physical Windows ARM64 operation remains unvalidated. Their runtime includes the applicable ARM64 `libunwind.dll` alongside the transport DLL.

## macOS Intel or Apple Silicon

Install Xcode Command Line Tools, Python 3, and Rust/rustup. On an Intel Mac use `x86_64-apple-darwin` / `x86_64`; on Apple Silicon use `aarch64-apple-darwin` / `arm64`.

```bash
rustup target add x86_64-apple-darwin
cargo test --locked -p remote_transport --target x86_64-apple-darwin
cargo build --locked --release -p remote_transport --target x86_64-apple-darwin
bash apps/macos-host/build-macos.sh target/x86_64-apple-darwin/release/libremote_transport.a 'dist/GlideBeam Host.app' "$(cat VERSION)" x86_64
'dist/GlideBeam Host.app/Contents/MacOS/RemoteHost' --self-test-localization
bash scripts/test-macos-ci.sh
python3 scripts/third-party-notices.py --output build/third-party/THIRD-PARTY-LICENSES.txt
python3 scripts/package-ci.py --platform macOS --architecture x64 --runtime 'dist/GlideBeam Host.app' --output artifacts
```

For Apple Silicon replace the Rust target with `aarch64-apple-darwin`, the clang architecture with `arm64`, and the package architecture with `arm64`. Each ZIP contains one architecture, rather than a universal binary. The minimum deployment version is 15.2.

The build copies English/Simplified Chinese resources and the app icon, signs the app, and verifies the signature. Default CI signing is ad hoc. An explicitly configured local development identity can be reused; see [signing](development-signing.md). `GLIDEBEAM_BUILD_NUMBER` can override the numeric bundle build number.

The notice collector reads resolved registry package sources from `cargo metadata --locked`, preserves their upstream license texts and the pinned Rust standard-library notices, and adds Windows runtime texts when a toolchain root is supplied. GCC static runtime collection includes GPLv3 and the GCC Runtime Library Exception. Missing license coverage fails packaging. Run it for the matching platform/toolchain immediately before packaging; the ZIP includes `THIRD-PARTY-LICENSES.txt`.

## Tests and previews

```powershell
./dist/windows-x64/RemoteNative.exe --self-test
./dist/windows-x64/RemoteNative.exe --selftest-ui
```

Mac supports `--language en` or `--language zh-Hans` with `--preview-ui PATH.png`. A CLI language override is temporary; normal UI language changes are persisted. `--self-test-restart` uses mock authorization and compiles fixed AppleScript without rebooting. Hosted CI does not run hardware capture/codec sessions. Use a paired real desktop to assess hardware acceleration and interactive latency.

The native test scripts also compile the audio packetizer, jitter/playout simulations and Windows sample-format probe. Windows audio links the system `avrt.dll` for MMCSS; no external audio codec is bundled. To inspect an existing paired session from Windows without recording or playing audio, run `python scripts/probe-audio.py --runtime dist/windows-x64 --seconds 18 --output build/audio-probe.json`. It reports packet format, sequence gaps and per-channel RMS while consuming video separately. Play a known sound on the Mac during this check; a silent stream does not establish that system sound was captured. See [audio validation](audio.md) for device and listening checks.

See [release automation](release.md) for the equivalent hosted builds.
