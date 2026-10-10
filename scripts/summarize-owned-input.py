"""Summarize only fixed input observed in this project's own Mac test window."""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
read = lambda name: json.loads((root / "build" / name).read_text(encoding="utf-8"))
before, after, final = (read(name) for name in ("input-before.json", "input-after.json", "input-final-report.json"))
transport = read("owned-input-transport.json")
same = before.get("session_nonce") == after.get("session_nonce") == final.get("session_nonce")
if not same or not before.get("own_app_frontmost") or not after.get("own_app_frontmost") or not final.get("completed"):
    raise SystemExit("Owned input observation is not a confirmed completed session")
keys = ("mouse_down", "mouse_up", "scroll", "key_down", "key_up", "flags_changed")
deltas = {key: after.get(key, 0) - before.get(key, 0) for key in keys}
checks = {"owned_window_frontmost_confirmed": True, "completed_same_session": True,
          "paired_network_connected": bool(transport.get("connected")),
          "fixed_chinese_and_physical_key_text_matches": bool(after.get("matches_expected_text")),
          "click_press_observed": deltas["mouse_down"] >= 1,
          "scroll_observed": deltas["scroll"] >= 1,
          "key_down_and_up_observed": deltas["key_down"] >= 2 and deltas["key_up"] >= 2,
          "shift_press_and_release_observed": bool(after.get("shift_observed_down")) and not after.get("shift_currently_down") and deltas["flags_changed"] >= 2}
report = {"schema": "remote-native.owned-input-validation/v1", "date": "2026-10-10", "platform": "macOS 15.8.1",
          "verified_checks_passed": all(checks.values()), "checks": checks, "observed_event_deltas": deltas,
          "text_length": after.get("text_length"), "sent_test_events": transport.get("input_events_sent"),
          "elapsed_seconds": final.get("elapsed_seconds"), "captures_other_apps": False, "logs_free_text": False,
          "remaining_manual_checks": ["The text field did not independently report mouse-up; button-pair observation remains inconclusive.",
                                       "Physical desktop drag, IME application compatibility and end-to-end input feedback latency."]}
(root / "build/owned-input-validation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
print(f"Owned input checks passed: {sum(checks.values())}/{len(checks)}; independent mouse-up observation remains inconclusive")
if not all(checks.values()):
    raise SystemExit(1)
