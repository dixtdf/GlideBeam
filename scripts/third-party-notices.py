"""Collect locked Rust dependency and applicable Windows runtime license texts.

Only upstream LICENSE/COPYING/NOTICE files are read. Generated output includes
package-relative labels, never local registry, user, or toolchain paths.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
LICENSE_NAME = re.compile(r"^(?:licen[cs]e|copying|copyright|notice)(?:[-_.].*)?$", re.I)
SKIP_DIRS = {".git", "target", "node_modules", ".cargo", "__pycache__"}
RUNTIME_FALLBACK = ROOT / "third_party" / "licenses" / "mingw-w64"


def license_files(directory: Path) -> list[Path]:
    files: list[Path] = []
    for base, directories, names in os.walk(directory, followlinks=False):
        directories[:] = sorted(name for name in directories
                                if name not in SKIP_DIRS and not (Path(base) / name).is_symlink())
        for name in sorted(names):
            path = Path(base) / name
            if LICENSE_NAME.fullmatch(name) and path.is_file() and not path.is_symlink():
                files.append(path)
    return sorted(files)


def text_file(path: Path) -> str:
    if path.stat().st_size > 4 * 1024 * 1024:
        raise ValueError("An upstream license file exceeds the 4 MiB limit")
    raw = path.read_bytes()
    if b"\0" in raw:
        raise ValueError("An upstream license file contains binary data")
    return raw.decode("utf-8-sig", errors="replace").replace("\r\n", "\n").strip() + "\n"


def rust_packages(cargo: str, offline: bool) -> list[dict]:
    command = [cargo, "metadata", "--locked", "--format-version", "1"]
    if offline:
        command.append("--offline")
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        # Cargo errors may contain local paths or environment values. Keep the
        # public diagnostic fixed; callers can run metadata locally to inspect.
        raise ValueError("cargo metadata --locked failed; ensure dependencies and the pinned toolchain are available")
    metadata = json.loads(result.stdout)
    resolved = {node["id"] for node in metadata["resolve"]["nodes"]}
    return sorted((package for package in metadata["packages"]
                   if package["id"] in resolved and package.get("source") is not None),
                  key=lambda package: (package["name"], package["version"]))


def rust_runtime_files(cargo: str, rustc: str | None) -> tuple[Path, list[Path]]:
    if rustc is None:
        sibling = Path(cargo).parent / ("rustc.exe" if os.name == "nt" else "rustc")
        rustc = str(sibling) if sibling.is_file() else "rustc"
    result = subprocess.run([rustc, "--print", "sysroot"], cwd=ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise ValueError("rustc sysroot lookup failed; the pinned toolchain is required")
    directory = Path(os.fsdecode(result.stdout).strip()) / "share" / "doc" / "rust"
    copyright_file = directory / "COPYRIGHT-library.html"
    if not copyright_file.is_file():
        raise ValueError("Rust standard-library copyright coverage is missing")
    license_directory = directory / "licenses"
    files = [copyright_file]
    if license_directory.is_dir():
        files.extend(path for path in sorted(license_directory.iterdir())
                     if path.is_file() and not path.is_symlink() and path.suffix == ".txt")
    if not any(path.name == "MIT.txt" for path in files) or not any(path.name == "Apache-2.0.txt" for path in files):
        raise ValueError("Rust standard-library license texts are missing")
    return directory, files


def runtime_files(toolchain: Path) -> list[tuple[str, Path]]:
    toolchain = toolchain.resolve(strict=True)
    if toolchain.name.lower() == "bin":
        toolchain = toolchain.parent
    found: list[tuple[str, Path]] = []
    for name in ("LICENSE.TXT", "LICENSE.txt", "LICENSE", "COPYING", "NOTICE"):
        path = toolchain / name
        if path.is_file():
            found.append(("Windows toolchain/" + name, path))
    for directory in (toolchain / "share" / "licenses", toolchain / "share" / "doc"):
        if directory.is_dir():
            # Only runtime-relevant MSYS2 package notices, never Python or a
            # complete compiler distribution unrelated to shipped DLLs.
            for child in sorted(directory.iterdir()):
                if child.is_dir() and any(word in child.name.lower() for word in ("winpthread", "mingw", "libunwind")):
                    found.extend(("Windows runtime/" + path.relative_to(directory).as_posix(), path)
                                 for path in license_files(child))
    for target in ("x86_64-w64-mingw32", "aarch64-w64-mingw32"):
        directory = toolchain / target / "share" / "mingw32"
        if directory.is_dir():
            found.extend(("MinGW-w64 runtime/" + path.name, path)
                         for path in license_files(directory))
    # Some WinLibs distributions omit runtime notice files. Keep the complete
    # upstream MinGW runtime and winpthreads texts in source as a fallback.
    if RUNTIME_FALLBACK.is_dir():
        found.extend(("MinGW-w64 runtime/" + path.name, path)
                     for path in license_files(RUNTIME_FALLBACK))
    if not any("winpthread" in label.lower() or "mingw" in label.lower() for label, _ in found):
        raise ValueError("MinGW runtime license coverage is missing")
    if (toolchain / "bin" / "aarch64-w64-mingw32-clang++.exe").is_file():
        if not any(path.name.upper() == "LICENSE.TXT" for _, path in found):
            raise ValueError("LLVM runtime license coverage is missing")
    return sorted(found, key=lambda item: item[0])


def collect(output: Path, cargo: str, toolchain: Path | None, offline: bool, rustc: str | None = None) -> dict:
    packages = rust_packages(cargo, offline)
    rust_directory, rust_runtime = rust_runtime_files(cargo, rustc)
    sections = ["GlideBeam third-party license texts\n\n"
                "Generated from the resolved Cargo.lock dependency sources and,\n"
                "when applicable, the Windows runtime/toolchain license files.\n"
                "First-party license: see LICENSE. Dependency licenses follow.\n"]
    missing: list[str] = []
    license_count = 0
    for package in packages:
        directory = Path(package["manifest_path"]).parent.resolve(strict=True)
        files = license_files(directory)
        # r-efi carries its complete license and copyrights in AUTHORS, not a
        # LICENSE file. Include this only when AUTHORS actually has terms.
        authors = directory / "AUTHORS"
        if authors.is_file() and "Permission is hereby granted" in text_file(authors):
            files.append(authors)
        explicit = package.get("license_file")
        if explicit:
            path = (directory / explicit).resolve(strict=True)
            if not path.is_relative_to(directory):
                raise ValueError("A crate license points outside its package directory")
            if path not in files:
                files.append(path)
        if not files:
            # yasna's published crate excludes license files; exact upstream
            # texts for its .cargo_vcs_info.json revision are kept in source.
            fallback = ROOT / "third_party" / "licenses" / (package["name"] + "-" + package["version"])
            files = license_files(fallback) if fallback.is_dir() else []
            if files:
                directory = fallback
            else:
                missing.append(package["name"] + " " + package["version"])
                continue
        sections.append("\n" + "=" * 72 + "\n" + package["name"] + " " + package["version"] +
                        "\nDeclared license: " + (package.get("license") or "See upstream text") + "\n")
        seen: set[str] = set()
        for path in sorted(files):
            content = text_file(path)
            digest = hashlib.sha256(content.encode("utf-8")).hexdigest()
            if digest in seen:
                continue
            seen.add(digest)
            license_count += 1
            sections.append("\n--- " + path.relative_to(directory).as_posix() + " ---\n" + content)
    if missing:
        raise ValueError("Missing upstream license texts for: " + ", ".join(missing))
    for path in rust_runtime:
        sections.append("\n" + "=" * 72 + "\nRust standard library/" +
                        path.relative_to(rust_directory).as_posix() + "\n\n" + text_file(path))
    runtime_count = 0
    if toolchain:
        seen_runtime: set[str] = set()
        for label, path in runtime_files(toolchain):
            content = text_file(path)
            digest = hashlib.sha256(content.encode("utf-8")).hexdigest()
            if digest in seen_runtime:
                continue
            seen_runtime.add(digest)
            runtime_count += 1
            sections.append("\n" + "=" * 72 + "\n" + label + "\n\n" + content)
        root = toolchain.resolve(strict=True)
        if root.name.lower() == "bin":
            root = root.parent
        llvm_toolchain = (root / "bin" / "aarch64-w64-mingw32-clang++.exe").is_file()
        gcc_toolchain = (root / "bin" / "g++.exe").is_file() or (root / "bin" / "g++").is_file()
        if gcc_toolchain and not llvm_toolchain:
            # x64 statically links libstdc++ and libgcc. The standard upstream
            # GPLv3 and GCC Runtime Library Exception texts are also shipped by
            # the pinned Rust toolchain; retain them explicitly for that runtime.
            for name in ("GPL-3.0-or-later.txt", "GCC-exception-3.1.txt"):
                path = rust_directory / "licenses" / name
                if not path.is_file():
                    raise ValueError("GCC static runtime license coverage is missing")
                sections.append("\n" + "=" * 72 + "\nGCC libstdc++ / libgcc runtime/" + name + "\n\n" + text_file(path))
                runtime_count += 1
    data = "".join(sections)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="\n") as destination:
        destination.write(data)
    return {"rust_packages": len(packages), "rust_license_texts": license_count,
            "rust_runtime_license_texts": len(rust_runtime),
            "runtime_license_texts": runtime_count, "bytes": len(data.encode("utf-8"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--toolchain-root", type=Path)
    parser.add_argument("--cargo", default="cargo")
    parser.add_argument("--rustc")
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args()
    try:
        report = collect(args.output, args.cargo, args.toolchain_root, args.offline, args.rustc)
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as error:
        if isinstance(error, ValueError):
            print("License collection FAILED: " + str(error), file=sys.stderr)
        else:
            print("License collection FAILED: dependency metadata or upstream text unavailable", file=sys.stderr)
        return 1
    print("License collection PASS: " + json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
