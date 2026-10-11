# Compatibility

GlideBeam is developed for a logged-in Mac desktop on a LAN, controlled from Windows 11. Compatibility across other hardware, drivers, VMs, and OS versions is not guaranteed.

| Target | Build / packaging | Physical-device validation |
| --- | --- | --- |
| Windows x64 | Native GNU/UCRT build | Windows 11 Pro build 26300, Core i3-10100F, 16 GB, Radeon RX Vega driver 31.0.12027.9001 |
| Windows ARM64 | Native ARM64 PE cross-build using LLVM-MinGW and Rust gnullvm | Not performed |
| Mac Intel x64 | Native x86_64 Mach-O | macOS 15.8.1 build 24H32, Unraid VM, RTX 3060 12 GB passthrough |
| Mac Apple Silicon | Native ARM64 Mach-O build target | Not performed |

The deployment minimum is macOS 15.2. Actual Intel Mac validation used 15.8.1. Windows validation used a 2560 × 1440 local display and a 1920 × 1080 remote stream. The Mac VM has 20 logical CPUs and 16 GiB RAM; it reports Genuine Intel CPU 0000 at 3.30 GHz. The physical host was reported as a Core i9-10900K ES QTB2 / ASRock B460M Pro4S/ac with Above 4G Decoding enabled.

## Capture and codecs

A usable Mac display source, Screen Recording permission, and Accessibility permission are required. The Mac first tries H.264 hardware encoding through VideoToolbox, then Apple's software encoder if session creation, configuration, or preparation fails. The selected mode is reported by both ends. Windows decoding still requires Media Foundation / D3D11 hardware support. H.264 is the protocol codec; HEVC is not implemented.

Software encoding uses CPU resources and is capped at 1080p / 30 FPS, with an 8 Mbps target. These are configuration limits, not a guarantee of delivered frame rate or latency. It uses the original NV12 surface and never applies the NVIDIA-specific layout workaround. Retry capture attempts hardware again.

The tested RTX 3060 environment uses the experimental [nullmoth/nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver). GlideBeam does not include or install that driver and does not require it as a general macOS dependency. The reference VM results do not establish support for other NVIDIA cards, macOS releases, or driver revisions.

A narrow NV12 layout workaround is gated by the SHA-256 of a recognized loaded NVENC module. It compensates for that module's particular buffer layout and a padded 1088-line encoding surface, while presenting the intended 1080-line desktop. It does not modify a driver. An unknown module revision is not automatically treated as equivalent and needs separate validation. See the implementation in `apps/macos-host/nv12-layout.hpp`.

On the reference RTX 3060 VM with NullMoth 1.10.0, hardware encoder creation returned `-12908` and this change selected Apple's software H.264 encoder. A 1920 × 1080, 30-frame codec test passed moving-marker and decoded-pixel checks, including the production single-frame submission path. Windows' production GPU decoder also passed a 30-record fixture test with 29 frames actually presented. A separate 30-second animated desktop session presented 795 frames, used Windows GPU decoding, and reported approximately 29 FPS after startup. The first hardware-decoded frame took 1375 ms in that run. These short tests do not qualify sustained performance or total input latency; an earlier animation run had a long pause that did not recur in the final run.

Dedicated-window input testing received a mouse-down, scroll, fixed Unicode text, and paired key events, with Shift released. Its local monitor did not observe mouse-up; broad application input and subjective pointer feel remain unqualified. The software fallback is a source change and deployment validation, separate from the already published 0.2.0 download.

## Current boundaries

Single display, an already logged-in user desktop, LAN access, text/PNG clipboard, and on-demand two-way file transfer are the current scope. Audio, rich text, directory mirroring, deletion through cross-machine cut, login-before-user control, and public relay/NAT traversal are not implemented. Closing the host window leaves the menu-bar agent running. Start at login starts after that user logs in.

Disconnecting does not actively lock the Mac. Screen saver, power, and password policies are controlled by macOS. A target frame rate of 60 FPS does not guarantee delivered FPS or total input-to-display latency. Static screens can produce very few new capture frames without indicating a transport fault.

Hosted CI checks builds, offline tests, architecture, imports, signatures, and packaging. It cannot establish capture permission, hardware codec behavior, Finder/Explorer drag-and-drop behavior, or remote pointer feel for a particular physical machine.
