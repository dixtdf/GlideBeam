"""Package public binaries from an explicit allowlist, never local test reports."""
from pathlib import Path
import argparse
import hashlib
import plistlib
import stat
import struct
import zipfile

from ci_version import release_version

ROOT = Path(__file__).resolve().parents[1]
PUBLIC_DOCS = (
    "README.md", "README.zh-CN.md", "LICENSE", "NOTICE",
    "docs/building.md", "docs/release.md", "docs/compatibility.md",
    "docs/clipboard-files.md", "docs/diagnostics-restart.md",
    "docs/development-signing.md", "docs/transport.md", "assets/app-icon-preview.png",
)
MAC_FILES = {
    "Contents/MacOS/RemoteHost", "Contents/Info.plist",
    "Contents/Resources/RemoteNative.icns", "Contents/_CodeSignature/CodeResources",
    "Contents/Resources/en.lproj/Localizable.strings",
    "Contents/Resources/en.lproj/InfoPlist.strings",
    "Contents/Resources/zh-Hans.lproj/Localizable.strings",
    "Contents/Resources/zh-Hans.lproj/InfoPlist.strings",
}


def add_file(archive: zipfile.ZipFile, name: str, contents: bytes, executable: bool = False) -> None:
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.create_system = 3
    info.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
    info.compress_type = zipfile.ZIP_DEFLATED
    archive.writestr(info, contents)


def package(platform: str, architecture: str, runtime: Path, output: Path, version: str) -> Path:
    files: dict[str, tuple[bytes, bool]] = {}
    runtime = runtime.resolve(strict=True)
    if platform == "Windows":
        entries = {"RemoteNative.exe": "GlideBeam.exe", "remote_transport.dll": "remote_transport.dll"}
        if architecture == "x64":
            entries["libwinpthread-1.dll"] = "libwinpthread-1.dll"
        else:
            entries["libunwind.dll"] = "libunwind.dll"
        from check_windows_binary import check_runtime
        check_runtime(runtime, architecture, version)
        for source_name, target_name in entries.items():
            source = runtime / source_name
            if source.is_symlink() or not source.is_file():
                raise ValueError("Missing regular runtime file: " + source_name)
            files[target_name] = (source.read_bytes(), False)
        launch = '@echo off\r\ncd /d "%~dp0"\r\nstart "" "%~dp0GlideBeam.exe" --connect\r\n'
        files["Start-GlideBeam.cmd"] = (launch.encode("ascii"), False)
    else:
        info = plistlib.loads((runtime / "Contents/Info.plist").read_bytes())
        if info.get("CFBundleShortVersionString") != version.split("-")[0]:
            raise ValueError("Mac bundle version does not match VERSION")
        binary = (runtime / "Contents/MacOS/RemoteHost").read_bytes()
        if len(binary) < 8 or struct.unpack_from("<II", binary) != (0xFEEDFACF, {"x64": 0x01000007, "arm64": 0x0100000C}[architecture]):
            raise ValueError("Mac executable architecture does not match the package")
        for source in sorted(runtime.rglob("*")):
            if source.is_symlink():
                raise ValueError("Unexpected app symlink: " + str(source.relative_to(runtime)))
            if source.is_dir():
                continue
            name = source.relative_to(runtime).as_posix()
            if name not in MAC_FILES:
                raise ValueError("App file is outside the public allowlist: " + name)
            files["GlideBeam Host.app/" + name] = (source.read_bytes(), name == "Contents/MacOS/RemoteHost")
        for required in ("Contents/MacOS/RemoteHost", "Contents/Info.plist", "Contents/Resources/RemoteNative.icns", "Contents/_CodeSignature/CodeResources"):
            if "GlideBeam Host.app/" + required not in files:
                raise ValueError("Missing signed Mac app file: " + required)
    for document in PUBLIC_DOCS:
        source = ROOT / document
        if source.is_file() and not source.is_symlink():
            files[document] = (source.read_bytes(), False)
    notices = ROOT / "build/third-party/THIRD-PARTY-LICENSES.txt"
    if notices.is_symlink() or not notices.is_file():
        raise ValueError("Generate THIRD-PARTY-LICENSES.txt before packaging")
    files["THIRD-PARTY-LICENSES.txt"] = (notices.read_bytes(), False)
    output.mkdir(parents=True, exist_ok=True)
    destination = output / f"GlideBeam-{version}-{platform}-{architecture}.zip"
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted(files):
            add_file(archive, name, *files[name])
    with zipfile.ZipFile(destination) as archive:
        if archive.testzip() is not None:
            raise ValueError("Archive verification failed")
    digest = hashlib.sha256(destination.read_bytes()).hexdigest()
    destination.with_suffix(".zip.sha256").write_text(digest + "  " + destination.name + "\n", encoding="ascii")
    print(f"Packaged {destination.name}: {len(files)} files, SHA256 {digest}")
    return destination


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("Windows", "macOS"), required=True)
    parser.add_argument("--architecture", choices=("x64", "arm64"), required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    package(args.platform, args.architecture, args.runtime, args.output, release_version())


if __name__ == "__main__":
    main()
