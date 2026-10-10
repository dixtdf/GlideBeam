"""Check the public source tree without printing possible credential values."""
from __future__ import annotations

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SENSITIVE_NAMES = {
    "connection.json", "session.json", "pairing-token", "pairing.json",
    "identity-key.der", "identity-cert.der", "auth-policy.json",
    "keychain-password", "identity-sha1", "mac-known-hosts",
}
FORBIDDEN_ROOTS = {"tools", "target", "build", "dist", "work", "outputs", "tmp"}
TEXT_EXTENSIONS = {".cpp", ".h", ".hpp", ".mm", ".py", ".ps1", ".sh", ".rs",
                   ".toml", ".md", ".yml", ".yaml", ".plist", ".strings", ".rc", ".json"}
SECRET_PATTERNS = {
    "GitHub token": re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})\b"),
    "OpenAI key": re.compile(r"\bsk-(?:proj-)?[A-Za-z0-9_-]{40,}\b"),
    "private key material": re.compile(
        r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----[\r\n]+[A-Za-z0-9+/=\r\n]{64,}-----END", re.M),
    "development host credential": re.compile(r"MAC_TASK_PASSWORD\s*=\s*['\"](?:123456|macadmin)['\"]"),
}


def candidates() -> list[Path]:
    result = subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    if result.returncode == 0:
        return [ROOT / name.decode("utf-8") for name in result.stdout.split(b"\0") if name]
    roots = [ROOT / name for name in ("apps", "shared", "crates", "assets", "docs", ".github", ".cargo")]
    files = [file for path in roots if path.exists() for file in path.rglob("*")
             if file.is_file() and not any(part in FORBIDDEN_ROOTS for part in file.relative_to(ROOT).parts)]
    files.extend(ROOT / name for name in ("README.md", "README.zh-CN.md", "LICENSE", "NOTICE", "VERSION",
                                         "Cargo.toml", "Cargo.lock", "rust-toolchain.toml", ".gitignore")
                 if (ROOT / name).is_file())
    files.extend(file for file in (ROOT / "scripts").glob("*")
                 if file.is_file() and not file.name.startswith("mac-")
                 and file.name not in {"package-release.py", "package-mac-source.py", "package-mac-app-source.py"})
    return sorted(set(files))


def main() -> int:
    files = candidates()
    problems: list[str] = []
    for file in files:
        relative = file.relative_to(ROOT)
        if any(part in FORBIDDEN_ROOTS for part in relative.parts[:-1]) or file.name in SENSITIVE_NAMES:
            problems.append(f"{relative.as_posix()}: private/generated file")
            continue
        if file.suffix.lower() in {".pfx", ".p12", ".key", ".pem", ".der", ".keychain-db", ".log"}:
            problems.append(f"{relative.as_posix()}: private/generated extension")
            continue
        if file.suffix.lower() not in TEXT_EXTENSIONS or not file.is_file():
            continue
        source = file.read_text(encoding="utf-8-sig")
        for name, pattern in SECRET_PATTERNS.items():
            if pattern.search(source):
                problems.append(f"{relative.as_posix()}: {name}")
    if problems:
        for problem in problems:
            print(problem)
        print(f"Public source check FAILED ({len(problems)} findings; values suppressed)")
        return 1
    print(f"Public source check PASS ({len(files)} files; no private state or credential patterns found)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
