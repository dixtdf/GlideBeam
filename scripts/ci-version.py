"""Validate VERSION and tag consistency; emit safe GitHub Actions job outputs."""
from pathlib import Path
import argparse
import os
import re

ROOT = Path(__file__).resolve().parents[1]


def release_version() -> str:
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?", version):
        raise ValueError("VERSION must contain one semantic version")
    if any(int(part) > 65535 for part in version.split("-")[0].split(".")):
        raise ValueError("VERSION numeric components must fit Windows VERSIONINFO")
    return version


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-tag", default=os.environ.get("GITHUB_REF", ""))
    args = parser.parse_args()
    version = release_version()
    if args.check_tag.startswith("refs/tags/") and args.check_tag != "refs/tags/v" + version:
        raise SystemExit("The release tag must be v" + version + " and match VERSION")
    output = "version=" + version + "\n"
    output += "prerelease=" + ("true" if "-" in version else "false") + "\n"
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as file:
            file.write(output)
    print(output, end="")


if __name__ == "__main__":
    main()
