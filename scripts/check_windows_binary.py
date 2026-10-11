"""Read PE headers/resources/imports without executing cross-compiled binaries."""
from pathlib import Path
import argparse
import struct

MACHINES = {"x64": 0x8664, "arm64": 0xAA64}
SYSTEM_DLLS = {
    "advapi32.dll", "bcrypt.dll", "bcryptprimitives.dll", "comctl32.dll",
    "crypt32.dll", "d2d1.dll", "d3d11.dll", "dwmapi.dll", "dwrite.dll",
    "gdi32.dll", "imm32.dll", "kernel32.dll", "mf.dll", "mfplat.dll",
    "ntdll.dll", "ole32.dll", "oleaut32.dll", "shell32.dll", "shlwapi.dll",
    "user32.dll", "userenv.dll", "ucrtbase.dll", "version.dll",
    "windowscodecs.dll", "winmm.dll", "ws2_32.dll", "msvcrt.dll", "avrt.dll",
}


class PE:
    def __init__(self, path: Path):
        self.data = path.read_bytes()
        if self.data[:2] != b"MZ":
            raise ValueError("Not a PE file: " + path.name)
        offset = self.u32(0x3C)
        if self.data[offset:offset + 4] != b"PE\0\0":
            raise ValueError("Invalid PE header: " + path.name)
        self.machine, section_count = struct.unpack_from("<HH", self.data, offset + 4)
        optional = offset + 24
        if self.u16(optional) != 0x20B:
            raise ValueError("Expected a 64 bit PE file")
        self.directories = optional + 112
        section_offset = optional + self.u16(offset + 20)
        self.sections = []
        for index in range(section_count):
            self.sections.append(struct.unpack_from("<IIII", self.data, section_offset + index * 40 + 8))

    def u16(self, offset: int) -> int:
        return struct.unpack_from("<H", self.data, offset)[0]

    def u32(self, offset: int) -> int:
        return struct.unpack_from("<I", self.data, offset)[0]

    def at(self, rva: int) -> int:
        for virtual_size, address, size, pointer in self.sections:
            if address <= rva < address + max(virtual_size, size):
                return pointer + rva - address
        raise ValueError("PE RVA is outside the image")

    def imports(self) -> set[str]:
        rva = self.u32(self.directories + 8)
        if not rva:
            return set()
        position = self.at(rva)
        names = set()
        for _ in range(256):
            descriptor = struct.unpack_from("<IIIII", self.data, position)
            if not any(descriptor):
                return names
            start = self.at(descriptor[3])
            end = self.data.index(b"\0", start)
            names.add(self.data[start:end].decode("ascii").lower())
            position += 20
        raise ValueError("Excessive import descriptors")

    def resources(self, resource_kind: int) -> list[bytes]:
        base = self.at(self.u32(self.directories + 16))
        results = []

        def entries(offset: int):
            count = self.u16(base + offset + 12) + self.u16(base + offset + 14)
            return [struct.unpack_from("<II", self.data, base + offset + 16 + index * 8) for index in range(count)]

        for kind, type_directory in entries(0):
            if kind != resource_kind:
                continue
            for resource_id, language_directory in entries(type_directory & 0x7FFFFFFF):
                if resource_id != 1:
                    raise ValueError("Unexpected application resource ID")
                for _, data_entry in entries(language_directory & 0x7FFFFFFF):
                    entry = base + data_entry
                    position, size = self.at(self.u32(entry)), self.u32(entry + 4)
                    results.append(self.data[position:position + size])
        return results

    def manifests(self) -> list[bytes]:
        return self.resources(24)

    def file_version(self) -> tuple[int, int, int, int]:
        versions = self.resources(16)
        if len(versions) != 1:
            raise ValueError("Expected one VERSIONINFO resource")
        data = versions[0]
        position = 6
        while data[position:position + 2] != b"\0\0":
            position += 2
            if position > len(data) - 2:
                raise ValueError("Malformed VERSIONINFO resource")
        position = (position + 2 + 3) & ~3
        fields = struct.unpack_from("<6I", data, position)
        if fields[0] != 0xFEEF04BD:
            raise ValueError("Missing VS_FIXEDFILEINFO signature")
        if fields[2:4] != fields[4:6]:
            raise ValueError("File and product versions differ")
        return fields[2] >> 16, fields[2] & 0xFFFF, fields[3] >> 16, fields[3] & 0xFFFF


def check_runtime(runtime: Path, architecture: str, version: str | None = None) -> None:
    names = ["RemoteNative.exe", "remote_transport.dll"]
    if architecture == "x64":
        names.append("libwinpthread-1.dll")
    else:
        names.append("libunwind.dll")
    for name in names:
        image = PE(runtime / name)
        if image.machine != MACHINES[architecture]:
            raise ValueError("Wrong architecture in " + name)
        bundled = {item.lower() for item in names}
        unknown = {item for item in image.imports() if item not in SYSTEM_DLLS and item not in bundled and not item.startswith(("api-ms-win-", "ext-ms-win-"))}
        if unknown:
            raise ValueError("Missing runtime dependencies in " + name + ": " + ", ".join(sorted(unknown)))
    executable = PE(runtime / "RemoteNative.exe")
    manifests = executable.manifests()
    if len(manifests) != 1 or b"PerMonitorV2" not in manifests[0] or b"asInvoker" not in manifests[0]:
        raise ValueError("Expected one application manifest with PerMonitorV2 and asInvoker")
    if version and executable.file_version() != tuple(int(part) for part in version.split("-")[0].split(".")) + (0,):
        raise ValueError("Windows executable version does not match VERSION")
    print(f"PE architecture, imports, version and single manifest verified: {architecture}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--architecture", choices=tuple(MACHINES), required=True)
    parser.add_argument("--version")
    arguments = parser.parse_args()
    from ci_version import release_version
    check_runtime(arguments.runtime, arguments.architecture, arguments.version or release_version())
