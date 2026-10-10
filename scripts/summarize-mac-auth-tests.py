"""Record the executed Mac cargo tests without importing any pairing material."""
import json
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
log = (root / "build/mac-auth-tests.log").read_text(encoding="utf-8")
tests = [{"name": name, "result": "pass" if result == "ok" else "fail"}
         for name, result in re.findall(r"^test (\S+) \.\.\. (ok|FAILED)$", log, re.MULTILINE)]
summary = re.search(r"test result: ok\. (\d+) passed; (\d+) failed;.*?finished in ([\d.]+)s", log)
if not summary or len(tests) != int(summary[1]) or int(summary[2]) or any(t["result"] != "pass" for t in tests):
    raise SystemExit("Mac authentication test log is incomplete or failed")
report = {"schema": "remote-native.auth-transport-validation/v1", "date": "2026-10-10", "platform": "macOS 15.8.1",
          "architecture": "x86_64", "test_count": len(tests), "passed": len(tests), "failed": 0,
          "test_duration_seconds": float(summary[3]), "tests": tests}
(root / "build/mac-auth-transport-validation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
print(f"Mac authentication report: {len(tests)} passed")
