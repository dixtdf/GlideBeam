#!/usr/bin/env python3
"""Validate public UI catalogs without reading user preferences or connecting hosts."""
from __future__ import annotations
import argparse
import collections
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
QUOTED = r'"(?:\\.|[^"\\])*"'
FORMAT = re.compile(r"%(?!%)(?:\d+\$)?[-+# 0']*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcsp@]")


def load_shared() -> list[tuple[str, str]]:
    source = (ROOT / "shared/i18n.cpp").read_text(encoding="utf-8-sig")
    return [(json.loads(a), json.loads(b)) for a, b in re.findall(r"ENTRY\(\s*(" + QUOTED + r")\s*,\s*(" + QUOTED + r")\s*\)", source)]


def load_macos(language: str) -> list[tuple[str, str]]:
    source = (ROOT / f"apps/macos-host/resources/{language}.lproj/Localizable.strings").read_text(encoding="utf-8-sig")
    return [(json.loads(a), json.loads(b)) for a, b in re.findall(r"(" + QUOTED + r")\s*=\s*(" + QUOTED + r")\s*;", source)]


def validate(name: str, entries: list[tuple[str, str]], english: bool, errors: list[str]) -> dict:
    if not entries:
        errors.append(f"{name}: no catalog entries")
    seen = collections.Counter(key for key, value in entries)
    for key, count in seen.items():
        if count > 1:
            errors.append(f"{name}: duplicate source key {key!r}")
    for key, value in entries:
        if not key or not value:
            errors.append(f"{name}: empty source or translation")
        if FORMAT.findall(key) != FORMAT.findall(value):
            errors.append(f"{name}: format arguments differ for {key!r}")
        if english and re.search(r"[\u4e00-\u9fff]", value) and key != "简体中文":
            errors.append(f"{name}: untranslated Chinese in English value for {key!r}")
    return {"entries": len(entries), "unique_source_keys": len(seen)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", type=Path, help="write the validation report")
    arguments = parser.parse_args()
    errors: list[str] = []
    catalogs = {"windows_shared": load_shared(), "macos_en": load_macos("en"), "macos_zh_hans": load_macos("zh-Hans")}
    statistics = {name: validate(name, entries, name != "macos_zh_hans", errors) for name, entries in catalogs.items()}
    windows = dict(catalogs["windows_shared"])
    english, chinese = dict(catalogs["macos_en"]), dict(catalogs["macos_zh_hans"])
    for key in {"配对令牌", "界面语言", "跟随系统", "简体中文", "English", "光梭 · GlideBeam", "重启 Mac", "文件传输", "剪贴板同步", "连接超时，请检查 Mac 地址和网络连接"}:
        if key not in windows:
            errors.append(f"windows_shared: missing required UI key {key!r}")
    if english.keys() != chinese.keys():
        errors.append("macOS locale catalogs have different source keys")
    if not any("GlideBeam" in value for value in english.values()):
        errors.append("macOS English catalog is missing the GlideBeam brand")
    if not any("光梭" in value for value in chinese.values()):
        errors.append("macOS Chinese catalog is missing the Chinese brand")
    # Stable wire strings are displayed independently from the Mac app language.
    for key in english:
        if "画面采集" in key or "画面共享" in key:
            if key not in windows:
                errors.append(f"windows_shared: missing remote capture status {key!r}")
    report = {"passed": not errors, "catalogs": statistics, "errors": errors, "user_settings_read": False, "network_connections": False}
    if arguments.json:
        arguments.json.parent.mkdir(parents=True, exist_ok=True)
        arguments.json.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("Internationalization catalog validation:", "PASS" if not errors else "FAIL")
    for name, counts in statistics.items():
        print(f"  {name}: {counts['entries']} entries")
    for error in errors:
        print("  " + error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    raise SystemExit(main())
