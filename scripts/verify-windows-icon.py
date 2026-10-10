from ctypes import wintypes as W
import ctypes as C
import json
from pathlib import Path
import struct
import hashlib
import argparse

kernel = C.WinDLL("kernel32", use_last_error=True)
user = C.WinDLL("user32", use_last_error=True)
gdi = C.WinDLL("gdi32", use_last_error=True)
kernel.LoadLibraryExW.argtypes = [W.LPCWSTR, W.HANDLE, W.DWORD]
kernel.LoadLibraryExW.restype = W.HMODULE
kernel.FindResourceW.argtypes = [W.HMODULE, C.c_void_p, C.c_void_p]
kernel.FindResourceW.restype = W.HANDLE
kernel.SizeofResource.argtypes = [W.HMODULE, W.HANDLE]
kernel.SizeofResource.restype = W.DWORD
kernel.LoadResource.argtypes = [W.HMODULE, W.HANDLE]
kernel.LoadResource.restype = W.HANDLE
kernel.LockResource.argtypes = [W.HANDLE]
kernel.LockResource.restype = C.c_void_p
kernel.FreeLibrary.argtypes = [W.HMODULE]
user.LoadImageW.argtypes = [W.HINSTANCE, C.c_void_p, W.UINT, C.c_int, C.c_int, W.UINT]
user.LoadImageW.restype = W.HANDLE
user.DestroyIcon.argtypes = [W.HICON]

class IconInfo(C.Structure):
    _fields_ = [("fIcon", W.BOOL), ("xHotspot", W.DWORD), ("yHotspot", W.DWORD),
                ("hbmMask", W.HBITMAP), ("hbmColor", W.HBITMAP)]

class Bitmap(C.Structure):
    _fields_ = [("bmType", W.LONG), ("bmWidth", W.LONG), ("bmHeight", W.LONG),
                ("bmWidthBytes", W.LONG), ("bmPlanes", W.WORD),
                ("bmBitsPixel", W.WORD), ("bmBits", C.c_void_p)]

user.GetIconInfo.argtypes = [W.HICON, C.POINTER(IconInfo)]
gdi.GetObjectW.argtypes = [W.HANDLE, C.c_int, C.c_void_p]
gdi.DeleteObject.argtypes = [W.HANDLE]

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--exe", type=Path, default=root / "dist/windows/RemoteNative.exe")
args = parser.parse_args()
exe = args.exe.resolve()
module = kernel.LoadLibraryExW(str(exe), None, 0x02 | 0x20)
assert module, C.get_last_error()
report = {"executable": exe.name, "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "icon_resource_id": 101, "embedded": False}
try:
    resource = kernel.FindResourceW(module, C.c_void_p(101), C.c_void_p(14))
    assert resource
    resource_size = kernel.SizeofResource(module, resource)
    data = C.string_at(kernel.LockResource(kernel.LoadResource(module, resource)), resource_size)
    reserved, kind, count = struct.unpack_from("<HHH", data)
    assert reserved == 0 and kind == 1 and count == 7
    entries = [struct.unpack_from("<BBBBHHIH", data, 6 + i * 14) for i in range(count)]
    report["embedded_sizes"] = [[e[0] or 256, e[1] or 256] for e in entries]
    assert sorted(e[0] or 256 for e in entries) == [16, 24, 32, 48, 64, 128, 256]
    for entry in entries:
        assert kernel.FindResourceW(module, C.c_void_p(entry[-1]), C.c_void_p(3))
    loaded = []
    for size in (16, 24, 32, 48, 64, 128, 256):
        icon = user.LoadImageW(module, C.c_void_p(101), 1, size, size, 0)
        assert icon, C.get_last_error()
        info = IconInfo()
        try:
            assert user.GetIconInfo(icon, C.byref(info))
            bitmap = Bitmap()
            assert gdi.GetObjectW(info.hbmColor, C.sizeof(bitmap), C.byref(bitmap))
            assert bitmap.bmWidth == size and bitmap.bmHeight == size
            loaded.append({"size": size, "color_bits": bitmap.bmBitsPixel})
        finally:
            if info.hbmMask: gdi.DeleteObject(info.hbmMask)
            if info.hbmColor: gdi.DeleteObject(info.hbmColor)
            user.DestroyIcon(icon)
    report.update(embedded=True, windows_loaded=loaded, passed=True)
finally:
    kernel.FreeLibrary(module)
(root / "build/windows/icon-resource-validation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
print("Release EXE embedded icon: all 7 sizes loaded by Windows")
