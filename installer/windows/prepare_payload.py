"""Validate the public portable archive, then stage only its approved files for Inno."""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import importlib.util
import re
import shutil
import stat
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from check_windows_binary import check_runtime

_spec = importlib.util.spec_from_file_location("glidebeam_package_ci", ROOT / "scripts/package-ci.py")
assert _spec is not None and _spec.loader is not None
_package_ci = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_package_ci)


def prepare(package: Path, architecture: str, version: str, output: Path, file_entries: Path) -> None:
    expected_name = f"GlideBeam-{version}-Windows-{architecture}.zip"
    if package.name != expected_name:
        raise ValueError("Expected versioned portable ZIP: " + expected_name)
    if package.is_symlink() or not package.is_file():
        raise ValueError("Package must be a regular file")
    checksum = package.with_suffix(".zip.sha256")
    if checksum.is_symlink() or not checksum.is_file():
        raise ValueError("The portable ZIP's .sha256 sidecar is required")
    expected_hash_line = re.fullmatch(r"([0-9a-fA-F]{64})  " + re.escape(package.name) + r"\n?", checksum.read_text(encoding="ascii"))
    if not expected_hash_line or hashlib.sha256(package.read_bytes()).hexdigest() != expected_hash_line[1].lower():
        raise ValueError("Portable ZIP SHA256 does not match its sidecar")
    if output.is_symlink() or (output.exists() and (not output.is_dir() or any(output.iterdir()))):
        raise ValueError("Payload output must be an empty, regular directory")
    runtime_dll = "libwinpthread-1.dll" if architecture == "x64" else "libunwind.dll"
    allowed = set(_package_ci.PUBLIC_DOCS) | {
        "GlideBeam.exe", "remote_transport.dll", runtime_dll,
        "Start-GlideBeam.cmd", "THIRD-PARTY-LICENSES.txt",
    }
    with zipfile.ZipFile(package) as archive:
        seen = set()
        for entry in archive.infolist():
            name = entry.filename
            # Reject alternate Windows paths, duplicate/case-folded names, directories and links.
            parts = PurePosixPath(name).parts
            if (not name or entry.orig_filename != name or name.startswith("/") or "\\" in name or ":" in name
                    or any(part in ("", ".", "..") for part in name.split("/"))
                    or any(part.endswith((".", " ")) for part in parts)
                    or name.casefold() in seen or name not in allowed):
                raise ValueError("ZIP member is outside the public allowlist: " + name)
            seen.add(name.casefold())
            mode = (entry.external_attr >> 16) & 0xFFFF
            if entry.is_dir() or stat.S_ISLNK(mode) or (stat.S_IFMT(mode) not in (0, stat.S_IFREG)) or entry.flag_bits & 1:
                raise ValueError("ZIP member must be an unencrypted regular file: " + name)
            if entry.file_size <= 0 or entry.file_size > 128 * 1024 * 1024:
                raise ValueError("Empty or oversized ZIP member: " + name)
        if seen != {name.casefold() for name in allowed}:
            raise ValueError("Portable ZIP is missing required public files")
        if sum(entry.file_size for entry in archive.infolist()) > 256 * 1024 * 1024:
            raise ValueError("Portable ZIP expanded payload is too large")
        if archive.testzip() is not None:
            raise ValueError("Portable ZIP CRC verification failed")
        output.mkdir(parents=True, exist_ok=True)
        # Paths are all exact allowlist members; never use extractall on input ZIPs.
        for name in sorted(allowed):
            destination = output.joinpath(*PurePosixPath(name).parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(archive.read(name))
    with tempfile.TemporaryDirectory(prefix="glidebeam-pe-check-") as temporary:
        runtime = Path(temporary)
        shutil.copyfile(output / "GlideBeam.exe", runtime / "RemoteNative.exe")
        for name in ("remote_transport.dll", runtime_dll):
            shutil.copyfile(output / name, runtime / name)
        check_runtime(runtime, architecture, version)
    # Emit one [Files] entry per approved file rather than a recursive wildcard.
    lines = []
    for name in sorted(allowed):
        relative = PurePosixPath(name)
        source = str(output.joinpath(*relative.parts)).replace("{", "{{")
        if '"' in source or "\r" in source or "\n" in source:
            raise ValueError("Unsupported quotation or newline in build path")
        parent = relative.parent.as_posix().replace("/", "\\")
        destination = "{app}" + ("\\" + parent if parent != "." else "")
        lines.append(f'Source: "{source}"; DestDir: "{destination}"; Flags: ignoreversion')
    file_entries.write_text("\n".join(lines) + "\n", encoding="utf-8-sig")
    print(f"Approved installer payload: {len(allowed)} files; {architecture}; {version}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--architecture", choices=("x64", "arm64"), required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--file-entries", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?", args.version):
        raise ValueError("Version must be a semantic version")
    # Keep the input's final symlink visible to the validation above.
    prepare(args.package.absolute(), args.architecture, args.version, args.output.absolute(), args.file_entries.absolute())


if __name__ == "__main__":
    main()
