"""Read public host counters over real QUIC; optionally test only the owned InputPattern window."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import struct
import time

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--seconds", type=float, default=12)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--owned-input-ready", type=Path)
args = parser.parse_args()
if not 1 <= args.seconds <= 60:
    raise SystemExit("Diagnostic duration must be 1..60 seconds")

class Blob(c.Structure):
    _fields_ = [("length", w.DWORD), ("data", c.POINTER(c.c_ubyte))]

settings = json.loads((Path(os.environ["LOCALAPPDATA"]) / "RemoteNative" / "connection.json").read_text())
encrypted = bytes.fromhex(settings["tokenProtected"])
source_bytes = (c.c_ubyte * len(encrypted)).from_buffer_copy(encrypted)
source = Blob(len(encrypted), source_bytes)
clear = Blob()
crypt = c.WinDLL("crypt32", use_last_error=True)
crypt.CryptUnprotectData.argtypes = [c.POINTER(Blob), c.c_void_p, c.c_void_p, c.c_void_p, c.c_void_p, w.DWORD, c.POINTER(Blob)]
crypt.CryptUnprotectData.restype = w.BOOL
kernel = c.WinDLL("kernel32", use_last_error=True)
kernel.LocalFree.argtypes = [c.c_void_p]
kernel.LocalFree.restype = c.c_void_p
if not crypt.CryptUnprotectData(c.byref(source), None, None, None, None, 1, c.byref(clear)):
    raise SystemExit("Cannot read this user's saved pairing")
try:
    token = c.string_at(clear.data, clear.length)
finally:
    c.memset(clear.data, 0, clear.length)
    kernel.LocalFree(clear.data)

runtime = root / "dist" / "windows"
dll_directory = os.add_dll_directory(str(runtime))
api = c.CDLL(str(runtime / "remote_transport.dll"))
api.remote_client_start.argtypes = [c.c_char_p, c.c_char_p, c.c_char_p]
api.remote_client_start.restype = c.c_void_p
api.remote_status.argtypes = [c.c_void_p]
api.remote_status.restype = c.c_int
api.remote_poll.argtypes = [c.c_void_p, c.POINTER(c.c_uint32), c.c_void_p, c.c_size_t]
api.remote_poll.restype = c.c_int
api.remote_send.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.c_size_t]
api.remote_send.restype = c.c_int
api.remote_close.argtypes = [c.c_void_p]
api.remote_close.restype = None
endpoint = api.remote_client_start(settings["address"].encode(), token, b"")
del token
if not endpoint:
    raise SystemExit("Could not start diagnostic connection")

buffer = (c.c_ubyte * (8 * 1024 * 1024))()
channel = c.c_uint32()
started = time.monotonic()
report = {"schema": "remote.host-status-probe.v1", "status_only_no_decode": True,
          "injects_input": False, "host_status": [], "video_records": 0, "input_events_sent": 0}
sequence = 1
input_sent = False

def send_event(kind, flags=0, x=0, y=0, a=0, b=0, payload=b"", control=False):
    global sequence
    event = struct.pack("<IIffiiQ", kind, flags, x, y, a, b, 0 if control else sequence) + payload
    data = c.create_string_buffer(event)
    if api.remote_send(endpoint, 2, data, len(event)) < 0:
        raise RuntimeError("Input diagnostic send failed")
    if not control:
        sequence += 1
    report["input_events_sent"] += 1

try:
    while time.monotonic() - started < args.seconds:
        state = api.remote_status(endpoint)
        if state < 0:
            report["transport_error"] = True
            break
        if state == 1 and args.owned_input_ready and not input_sent:
            plan = json.loads(args.owned_input_ready.read_text(encoding="utf-8"))
            if (plan.get("schema") != "remote.input-pattern.ready.v1" or not plan.get("ready")
                    or not plan.get("own_app_frontmost") or plan.get("expected_text") != "远控-RemoteNative-testa"
                    or plan.get("target_bundle_id") != "dev.remote-native.input-pattern"
                    or plan.get("target_executable") != "/Users/Shared/RemoteNative/InputPattern.app/Contents/MacOS/InputPattern"
                    or not isinstance(plan.get("pid"), int) or plan["pid"] <= 0
                    or time.time() - args.owned_input_ready.stat().st_mtime > 4):
                raise RuntimeError("Owned input probe is not safely ready")
            x, y = float(plan["normalized_x"]), float(plan["normalized_y"])
            if not 0 <= x <= 1 or not 0 <= y <= 1:
                raise RuntimeError("Owned input coordinates out of range")
            report["injects_input"] = True
            send_event(1, x=x, y=y)
            send_event(2, flags=1, x=x, y=y, a=1)
            send_event(2, x=x, y=y, a=1)
            time.sleep(0.10)
            send_event(6, payload="远控-RemoteNative-test".encode())
            time.sleep(0.10)
            send_event(4, flags=1, a=65, b=30)
            send_event(4, a=65, b=30)
            send_event(3, x=x, y=y, a=1)
            send_event(4, flags=1, a=160, b=42)
            send_event(5, control=True)
            send_event(7, control=True)
            input_sent = True
        for _ in range(64):
            count = api.remote_poll(endpoint, c.byref(channel), buffer, len(buffer))
            if count < 0:
                raise RuntimeError("Diagnostic receive failed")
            if count == 0:
                break
            if channel.value == 1:
                status = json.loads(bytes(buffer[:count]))
                if status.get("type") == "host-status":
                    allowed = ("message", "capture_state", "capture_message", "width", "height", "coded_height", "nvenc_layout_workaround", "captured", "encoded",
                               "dropped", "encoded_bytes", "encode_failures", "accessibility", "screen_recording", "cursor",
                               "restart_supported", "restart_requires_local_authorization", "restart_busy")
                    public = {key: status[key] for key in allowed if key in status}
                    public["at_seconds"] = round(time.monotonic() - started, 3)
                    report["host_status"].append(public)
            elif channel.value == 4:
                report["video_records"] += 1
        time.sleep(0.002)
    report["connected"] = api.remote_status(endpoint) == 1
finally:
    api.remote_close(endpoint)
    report["elapsed_seconds"] = round(time.monotonic() - started, 3)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    dll_directory.close()
print(f"Host status samples: {len(report['host_status'])}; video records: {report['video_records']}; input events: {report['input_events_sent']}")
if not report.get("connected") or report.get("transport_error"):
    raise SystemExit(2)
