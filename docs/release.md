# Release automation

The workflow is `.github/workflows/build.yml`. It runs on pushes to `main`/`master`, pull requests, manual `workflow_dispatch` runs, and `v*` tags. Manual runs have no version input; the version is read from `VERSION`.

## Architecture packages

| Package architecture | Runner / toolchain |
| --- | --- |
| Windows x64 | `windows-latest`, MSYS2 UCRT64 GCC, Rust `x86_64-pc-windows-gnu` |
| Windows ARM64 | `windows-latest`, cross LLVM-MinGW 20261006, Rust `aarch64-pc-windows-gnullvm` |
| macOS x64 | `macos-15-intel`, clang, Rust `x86_64-apple-darwin` |
| macOS ARM64 | `macos-15`, clang, Rust `aarch64-apple-darwin` |

Rust is pinned to 1.99.0 and dependency resolution uses `Cargo.lock`. Windows x64 executes native/offline tests and an isolated silent Setup install/same-version repair/uninstall check. Windows ARM64 checks PE architecture, manifest, and imports without running the application on x64. Both Windows jobs compile Setup with pinned Inno Setup 6.7.3. Mac runs Rust, logging, layout, localization, and mock restart checks. Mock restart does not reboot the runner. Neither platform runs an actual paired capture/codec session in hosted CI.

Successful jobs upload `GlideBeam-Windows-x64`, `GlideBeam-Windows-arm64`, `GlideBeam-macOS-x64`, and `GlideBeam-macOS-arm64` artifacts. ZIPs use the pattern `GlideBeam-<version>-<platform>-<architecture>.zip`, each with a `.zip.sha256` file. Each Windows artifact also contains `GlideBeam-<version>-Windows-<architecture>-Setup.exe` and its `.exe.sha256` file.

Windows ZIPs contain `GlideBeam.exe`, the Rust transport DLL, applicable runtime DLLs (`libwinpthread-1.dll` for x64, `libunwind.dll` for ARM64), and `Start-GlideBeam.cmd`. Mac ZIPs contain `GlideBeam Host.app`; the internal executable `RemoteHost` and bundle identifier remain unchanged. Packaging uses a public-file allowlist and excludes logs, local reports, pairing settings, private keys, and development keychains.

Each job collects complete upstream license/copyright texts from the locked Rust dependency sources and applicable Windows runtime files. Every package includes `THIRD-PARTY-LICENSES.txt`; missing dependency or runtime license coverage fails the notice step, and packaging requires the generated file.

Setup is built from the verified matching Windows ZIP. Its payload has the same public-file allowlist, with no credentials or local configuration. It installs into `%LOCALAPPDATA%\Programs\GlideBeam` for the current user, creates shortcuts and an uninstall entry, and preserves `%LOCALAPPDATA%\RemoteNative` settings during upgrade and uninstall. Installation does not require administrator privileges. Silent installation does not launch the client.

## Prepare a release

1. Update `VERSION` and the application metadata as needed, then commit the intended release source.
2. Run the branch workflow and review its results and architecture artifacts.
3. Create and push a tag exactly matching `v` plus `VERSION`, for example `v1.0.0`.
4. After every architecture job succeeds, the workflow verifies the four ZIPs and two Setup EXEs and creates a **draft GitHub Release** with those six binaries and a combined `SHA256SUMS.txt` (seven assets total). Sidecar checksums remain in Actions artifacts; the release uses the combined manifest.
5. Review the release notes and download the artifacts. Verify all six files against `SHA256SUMS.txt`, inspect archive contents and embedded versions/architectures, and check the x64 Setup lifecycle results before publishing the draft. Keep physical ARM64 and paired-session limits explicit.

A prerelease version marks the draft as a prerelease. A rerun may update an existing draft; it refuses to overwrite an already published release. Only the tag release job receives repository content-write permission; builds use read permission.

The asset verifier can also check a downloaded release locally:

```powershell
python scripts/verify-release-assets.py --directory ./artifacts
```

Use `--write-manifest` only when preparing the combined manifest from verified Actions sidecars. It is not a substitute for comparing downloaded files against the published manifest.

## Signing and validation scope

No signing secrets are required by the configured workflow. Windows release binaries and Setup installers are not Authenticode-signed. Mac CI builds are ad-hoc-signed, without Developer ID signing or notarization. Local signing identities and pairing credentials are never exported to GitHub. Ad-hoc Mac builds may require permission approval again after an update; see [development signing](development-signing.md).

Workflow configuration and local tests are distinct from a successful hosted run. Check the repository's Actions history for the actual run status. Windows ARM64 and Apple Silicon physical-device compatibility remain unvalidated even when their builds pass; see [compatibility](compatibility.md).
