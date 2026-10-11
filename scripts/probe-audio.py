"""Read negotiated system-audio metadata without saving audio or injecting input.

Windows only; uses this user's existing DPAPI-protected pairing. A known stereo
test tone may be played separately on the Mac to check channel energy. This
probe does not play sound or measure capture-to-speaker latency.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import math
import os
from pathlib import Path
import struct
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--address", help="Override the saved server address, e.g. for an isolated test port")
    parser.add_argument("--seconds", type=float, default=12)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 60:
        raise SystemExit("Duration must be 1..60 seconds")

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
    if not crypt.CryptUnprotectData(c.byref(source), None, None, None, None, 1, c.byref(clear)):
        raise SystemExit("Cannot read this user's saved pairing")
    try:
        token = c.string_at(clear.data, clear.length)
    finally:
        c.memset(clear.data, 0, clear.length)
        kernel.LocalFree(clear.data)

    runtime = args.runtime.resolve(strict=True)
    with os.add_dll_directory(str(runtime)):
        api = c.CDLL(str(runtime / "remote_transport.dll"))
    api.remote_client_start.argtypes = [c.c_char_p, c.c_char_p, c.c_char_p]
    api.remote_client_start.restype = c.c_void_p
    for name in ("remote_status", "remote_close"):
        getattr(api, name).argtypes = [c.c_void_p]
    api.remote_close.restype = None
    api.remote_poll.argtypes = [c.c_void_p, c.POINTER(c.c_uint32), c.c_void_p, c.c_size_t]
    api.remote_send.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.c_size_t]
    api.remote_audio_poll.argtypes = [c.c_void_p, c.c_void_p, c.c_size_t]
    endpoint = api.remote_client_start((args.address or settings["address"]).encode(), token, b"")
    del token
    if not endpoint:
        raise SystemExit("Diagnostic connection could not start")

    audio = (c.c_ubyte * 992)()
    video = (c.c_ubyte * (8 * 1024 * 1024))()
    channel = c.c_uint32()
    report = {"schema": "glidebeam.audio-probe.v1", "test_scope": "subscription-and-packet-format", "injects_input": False,
              "plays_audio": False, "saves_audio": False, "measures_end_to_end_latency": False, "listening_validated": False,
              "audio_records": 0, "video_records": 0, "invalid_records": 0,
              "missing_sequences": 0, "nonzero_frames": 0, "discontinuities": 0,
              "host_audio_state": "", "sample_rate": 48000, "channels": 2, "packet_ms": 5}
    start = time.monotonic()
    sent = False
    previous = None
    squares = [0.0, 0.0]
    frames = 0
    try:
        while time.monotonic() - start < args.seconds:
            status = api.remote_status(endpoint)
            if status < 0:
                report["transport_error"] = True
                break
            if status == 1 and not sent:
                request = b'{"type":"audio-start","version":1}'
                if api.remote_send(endpoint, 1, request, len(request)) != 1:
                    raise RuntimeError("Audio request was not accepted")
                sent = True
            for _ in range(32):
                count = api.remote_poll(endpoint, c.byref(channel), video, len(video))
                if count <= 0:
                    break
                if channel.value == 4:
                    report["video_records"] += 1
                elif channel.value == 1:
                    try:
                        message = json.loads(bytes(video[:count]))
                    except (ValueError, UnicodeError):
                        continue
                    if message.get("type") == "audio-status" and message.get("state") in ("running", "unavailable", "stopped"):
                        report["host_audio_state"] = message["state"]
                    if message.get("type") == "host-status":
                        report["host_counters"] = {key: message[key] for key in
                            ("captured", "encoded", "audio_packets", "audio_dropped", "audio_max_callback_frames")
                            if isinstance(message.get(key), int) and not isinstance(message[key], bool)}
            for _ in range(16):
                count = api.remote_audio_poll(endpoint, audio, len(audio))
                if count <= 0:
                    break
                payload = bytes(audio[:count])
                magic, rate, channels, samples, flags, sequence, _ = struct.unpack_from("<IIHHIQQ", payload)
                if count != 992 or (magic, rate, channels, samples) != (0x31414652, 48000, 2, 240) or flags & ~1:
                    report["invalid_records"] += 1
                    continue
                report["audio_records"] += 1
                if previous is not None and sequence > previous + 1:
                    report["missing_sequences"] += sequence - previous - 1
                previous = max(sequence, previous) if previous is not None else sequence
                report["discontinuities"] += bool(flags & 1)
                for left, right in struct.iter_unpack("<hh", payload[32:]):
                    squares[0] += (left / 32768.0) ** 2
                    squares[1] += (right / 32768.0) ** 2
                    report["nonzero_frames"] += bool(left or right)
                    frames += 1
            time.sleep(.001)
        report["duration_seconds"] = round(time.monotonic() - start, 3)
        report["rms_left"] = math.sqrt(squares[0] / frames) if frames else 0
        report["rms_right"] = math.sqrt(squares[1] / frames) if frames else 0
        report["passed"] = report["host_audio_state"] == "running" and report["audio_records"] > 100 and not report["invalid_records"] and not report.get("transport_error")
    finally:
        before_close = time.monotonic()
        api.remote_close(endpoint)
        report["close_ms"] = round((time.monotonic() - before_close) * 1000, 3)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))
    return 0 if report.get("passed") else 1


if __name__ == "__main__":
    raise SystemExit(main())
