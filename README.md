# GlideBeam / 光梭

<img src="assets/app-icon-preview.png" alt="GlideBeam icon" width="128">

**Native Windows-to-Mac remote desktop, built for responsive LAN work.**

[简体中文](README.zh-CN.md) · [Downloads](https://github.com/dixtdf/GlideBeam/releases) · [Build instructions](docs/building.md) · [Compatibility](docs/compatibility.md)

GlideBeam combines a local Windows cursor, independent input transport, Mac H.264 encoding, and Windows GPU decoding. The Mac prefers hardware encoding and falls back to Apple's software encoder when unavailable. Its native, flat interface supports **English, Simplified Chinese, and Follow system**, with a saved language preference on each computer.

## Features

- Native Windows 11 client and macOS host; a full-screen desktop with an automatically hidden top toolbar and movable, window-bounded diagnostics overlay.
- ScreenCaptureKit capture and VideoToolbox H.264 encoding on Mac, with hardware preference and a software fallback capped at 30 FPS; Media Foundation/D3D11 hardware decoding and GPU presentation on Windows.
- Mac system audio on Windows (0.3.0; requires updated endpoints): 5 ms PCM packets, an independent WASAPI worker, adaptive buffering and clock-drift correction, with mute and output-device recovery. The diagnostics overlay reports audio queues, engine period, underruns and loss; see [audio](docs/audio.md).
- Pair using an address and one customizable, persistent token. Ten consecutive invalid token proofs block the source IP; the Mac UI can unblock it.
- Two-way Unicode text and image clipboard sharing; files and folders through copy/paste, drag/drop, or the transfer panel. Progress, pause/resume, cancellation, and SHA-256 verification.
- Rotating logs on both computers, Mac start at login, and an authenticated Mac restart command with native administrator authorization.

The initial scope is an already logged-in, single-display Mac desktop on a LAN. Microphone forwarding, rich text, directory mirroring, unattended login-screen control, and an Internet relay are not implemented. H.264 is the current video codec; HEVC is not promised.

Audio is included in [v0.3.0](https://github.com/dixtdf/GlideBeam/releases/tag/v0.3.0). Update both endpoints to use it; older `v0.2.1` packages do not include audio. See the [0.3.0 release notes](docs/releases/0.3.0.md) for changes and validation scope.

## Get started

1. Download the package for each computer's architecture. Windows packages contain `GlideBeam.exe` and `Start-GlideBeam.cmd`; Mac packages contain `GlideBeam Host.app`.
2. Open the Mac host. Grant **Screen Recording** and **Accessibility** in System Settings, and use **Screen and input permissions** / **Retry capture** if needed. A working Mac desktop and display source are required.
3. Copy the address and full pairing token shown by the Mac host. Enter both in the Windows client and connect. The default port is **UDP 47990**; allow it on your LAN. An existing VNC password is unrelated to GlideBeam pairing.
4. Move the pointer to the top center of the remote window to reveal the toolbar. File transfer, statistics, logs, and restart controls are available there. The Mac host can continue in the menu bar when its window closes.

Choose **Follow system**, **English**, or **简体中文** separately on each computer. Windows provides **Preferences → Interface language**, plus **More → Language** while connected. Mac provides a language selector at the top of the host window and in its menu. Enable **Start at login** on Mac to run after that user logs in; it does not provide control before login.

Disconnecting GlideBeam releases remote keyboard/mouse state and does **not** actively lock the Mac. macOS screen saver, sleep, and password settings remain in effect.

The public brand is GlideBeam / 光梭. Existing `RemoteNative` configuration paths and the Mac bundle identifier remain compatible with earlier development builds. Windows saves pairing data under the current user's DPAPI; Mac settings and identity stay in `~/Library/Application Support/RemoteNative`. Keep those private directories outside the source repository.

## Tested configuration and compatibility

**Compatibility with other hardware, drivers, virtual machines, or OS versions is not guaranteed.** Hardware acceleration depends on functioning OS capture/codec APIs and drivers. Building an architecture package does not establish that remote capture, encoding, decoding, or input works on a physical device.

| Component | Windows client actually tested | Mac host actually tested |
| --- | --- | --- |
| OS | Windows 11 Pro, build 26300 | macOS 15.8.1, build 24H32 |
| CPU | Intel Core i3-10100F, 3.60 GHz | Unraid x64 VM, 20 logical CPUs; guest reports Genuine Intel CPU 0000, 3.30 GHz |
| Memory | 16 GB | 16 GiB allocated to the VM |
| Graphics | Radeon RX Vega, driver 31.0.12027.9001 | RTX 3060 12 GB passed through to the VM |
| Display / video | Local display 2560 × 1440 | Remote capture 1920 × 1080, H.264 NVENC |
| Additional context | Native x64 client | Experimental [nullmoth/nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver) |

The physical VM host was reported as an Intel Core i9-10900K ES QTB2 on an ASRock B460M Pro4S/ac, with Above 4G Decoding enabled. This is distinct from the CPU identity exposed to the guest.

Windows x64 and Intel Mac were exercised on the setup above. **Windows ARM64 and Apple Silicon packages are build targets without physical-device validation.** The Mac deployment minimum is 15.2; the tested system is 15.8.1. The experimental NVIDIA driver is **not bundled**, and its behavior is specific to that setup. See [compatibility details](docs/compatibility.md), including the narrowly scoped NVENC layout workaround.

A target of 60 FPS is a pacing goal, not a guarantee of delivered frame rate or input latency.

## Build and release

The [build guide](docs/building.md) covers Windows x64 / ARM64 and Mac Intel / Apple Silicon. Core code is C++20 / Objective-C++ with a Rust QUIC transport. Versions come from `VERSION`; Rust dependencies are locked by `Cargo.lock` and the compiler by `rust-toolchain.toml`.

[GitHub Actions](.github/workflows/build.yml) is configured to build four architecture packages on pushes, pull requests, and manual runs. A matching `v<VERSION>` tag prepares a **draft** release containing four ZIPs and `SHA256SUMS.txt` after all builds pass. Hosted CI uses offline/native API tests and architecture checks; it does not perform a real desktop or physical ARM64 session. See the [release guide](docs/release.md) for exact behavior.

## Documentation

- [Clipboard and file transfer](docs/clipboard-files.md)
- [Logs, Mac restart, and disconnect behavior](docs/diagnostics-restart.md)
- [Builds](docs/building.md) and [release automation](docs/release.md)
- [Compatibility](docs/compatibility.md) and [Mac signing](docs/development-signing.md)
- [Transport protocol](docs/transport.md)
- [System audio, device setup and validation](docs/audio.md)
- [Latency, graphics APIs and browser FPS assessment](docs/performance-assessment-2026-10-11.md)
- [Latency scheduling changes and measurement guide](docs/latency-optimization-2026-10-11.md)
- [FPS stability experiments and capture diagnostics](docs/fps-stability-2026-10-12.md)

## License

GlideBeam source and first-party assets are released under the [MIT License](LICENSE). Dependencies retain their upstream licenses; see [NOTICE](NOTICE). No NVIDIA driver, operating-system image, pairing credential, or development signing key is included.
