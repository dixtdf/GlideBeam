"""Verify the six public packages and their SHA256 manifest without running apps."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import plistlib
import re
import stat
import struct
import tempfile
import zipfile
from pathlib import Path

from ci_version import release_version
from check_windows_binary import PE, check_runtime

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("package_ci", ROOT / "scripts/package-ci.py")
assert SPEC is not None and SPEC.loader is not None
PACKAGING = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGING)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def checksum(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def checksum_entries(text: str, expected: set[str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in text.splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]{64}) [ *]([^\r\n]+)", line)
        require(match is not None, "Malformed checksum entry")
        assert match is not None
        digest, name = match.groups()
        require(name in expected and name not in result, f"Unexpected or duplicate checksum: {name}")
        result[name] = digest.lower()
    require(set(result) == expected, "Checksum entries do not match expected assets")
    return result


def verify_zip(path: Path, platform: str, architecture: str, version: str) -> None:
    runtime = {"GlideBeam.exe", "remote_transport.dll", "Start-GlideBeam.cmd",
               "libwinpthread-1.dll" if architecture == "x64" else "libunwind.dll"}
    allowed = set(PACKAGING.PUBLIC_DOCS) | {"THIRD-PARTY-LICENSES.txt"}
    allowed |= runtime if platform == "Windows" else {"GlideBeam Host.app/" + name for name in PACKAGING.MAC_FILES}
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        names = {entry.filename for entry in entries}
        require(names == allowed and len(entries) == len(allowed), f"Incorrect package allowlist: {path.name}")
        for entry in entries:
            name = entry.filename
            require(entry.orig_filename == name and not entry.is_dir(), "Invalid ZIP filename")
            require(not name.startswith("/") and "\\" not in name and ":" not in name
                    and all(part not in ("", ".", "..") for part in name.split("/")), "Unsafe ZIP path")
            require(stat.S_IFMT(entry.external_attr >> 16) in (0, stat.S_IFREG)
                    and not entry.flag_bits & 1, "Non-regular or encrypted ZIP entry")
            require(0 < entry.file_size <= 128 * 1024 * 1024, "Empty or oversized ZIP entry")
        require(sum(entry.file_size for entry in entries) <= 512 * 1024 * 1024, "Oversized package")
        require(archive.testzip() is None, "ZIP CRC mismatch")
        require(bool(archive.read("THIRD-PARTY-LICENSES.txt").strip()), "Missing license notices")
        if platform == "Windows":
            # Only validated, explicitly named files are written to a fresh temporary directory.
            with tempfile.TemporaryDirectory(prefix="glidebeam-release-") as temporary:
                target = Path(temporary)
                for name in runtime - {"Start-GlideBeam.cmd"}:
                    destination = "RemoteNative.exe" if name == "GlideBeam.exe" else name
                    (target / destination).write_bytes(archive.read(name))
                check_runtime(target, architecture, version)
        else:
            prefix = "GlideBeam Host.app/Contents/"
            info = plistlib.loads(archive.read(prefix + "Info.plist"))
            require(info.get("CFBundleShortVersionString") == version.split("-")[0]
                    and info.get("CFBundleIdentifier") == "dev.remote-native.host"
                    and info.get("CFBundleExecutable") == "RemoteHost", "Incorrect Mac bundle metadata")
            binary = archive.read(prefix + "MacOS/RemoteHost")
            require(len(binary) >= 32 and struct.unpack_from("<II", binary)
                    == (0xFEEDFACF, {"x64": 0x01000007, "arm64": 0x0100000C}[architecture]), "Incorrect Mac architecture")


def verify(directory: Path, write_manifest: bool = False) -> None:
    version = release_version()
    zips = {f"GlideBeam-{version}-{platform}-{architecture}.zip": (platform, architecture)
            for platform in ("Windows", "macOS") for architecture in ("x64", "arm64")}
    setups = {f"GlideBeam-{version}-Windows-{architecture}-Setup.exe" for architecture in ("x64", "arm64")}
    expected = set(zips) | setups
    allowed = expected | {name + ".sha256" for name in expected} | {"SHA256SUMS.txt"}
    require(directory.is_dir(), "Missing asset directory")
    require(all(path.is_file() and not path.is_symlink() and path.name in allowed
                for path in directory.iterdir()), "Unexpected release file or directory")
    actual: dict[str, str] = {}
    for name in sorted(expected):
        path = directory / name
        require(path.is_file() and not path.is_symlink(), f"Missing asset: {name}")
        actual[name] = checksum(path)
        sidecar = directory / (name + ".sha256")
        if write_manifest or sidecar.exists():
            require(sidecar.is_file() and sidecar.stat().st_size < 1024, f"Missing checksum sidecar: {name}")
            require(checksum_entries(sidecar.read_text(encoding="ascii"), {name}) == {name: actual[name]}, f"SHA256 mismatch: {name}")
        if name in zips:
            verify_zip(path, *zips[name], version)
        else:
            # Inno's bootstrap architecture is independent of the installed app architecture.
            require(PE(path, allow_pe32=True).file_version() == tuple(int(part) for part in version.split("-")[0].split(".")) + (0,), "Incorrect Setup version")
        print(f"PASS {name}")
    manifest = directory / "SHA256SUMS.txt"
    if write_manifest:
        manifest.write_text("".join(f"{actual[name]}  {name}\n" for name in sorted(actual)), encoding="ascii")
    require(manifest.is_file() and manifest.stat().st_size < 4096, "Missing release checksum manifest")
    require(checksum_entries(manifest.read_text(encoding="ascii"), expected) == actual, "Release SHA256 mismatch")
    print("PASS six packages and SHA256SUMS.txt; no packaged application executed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--write-manifest", action="store_true", help="Combine six verified CI checksum sidecars")
    arguments = parser.parse_args()
    verify(arguments.directory, arguments.write_manifest)
