"""Package the GlideBeam artwork as reproducible ICO and ICNS assets.

Requires Pillow. The committed RGBA master is the source of every icon size.
This script only resamples and packages it; it does not alter the design.
"""
from __future__ import annotations

import io
from pathlib import Path
import struct

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
MASTER = ASSETS / "app-icon-master.png"


def render(size: int) -> Image.Image:
    with Image.open(MASTER) as source:
        if source.width != source.height or source.mode != "RGBA":
            raise ValueError("The icon master must be a square RGBA image")
        return source.resize((size, size), Image.Resampling.LANCZOS)


def png(image: Image.Image) -> bytes:
    output = io.BytesIO()
    image.save(output, "PNG", optimize=True)
    return output.getvalue()


def main() -> None:
    sizes = (16, 24, 32, 48, 64, 128, 256, 512, 1024)
    images = {size: render(size) for size in sizes}
    images[512].save(ASSETS / "app-icon-preview.png", "PNG", optimize=True)
    images[256].save(ASSETS / "app-icon.ico", "ICO", sizes=[(s, s) for s in sizes if s <= 256],
                     append_images=[images[s] for s in sizes if s < 256])

    iconset = ASSETS / "app-icon.iconset"
    iconset.mkdir(exist_ok=True)
    for size in (16, 32, 128, 256, 512):
        images[size].save(iconset / f"icon_{size}x{size}.png", "PNG", optimize=True)
        images[size * 2].save(iconset / f"icon_{size}x{size}@2x.png", "PNG", optimize=True)

    # Modern ICNS accepts PNG chunks. Include standard and Retina variants,
    # including explicit 16 px and 32 px representations for small UI icons.
    types = {b"icp4": 16, b"icp5": 32, b"icp6": 64, b"ic07": 128,
             b"ic08": 256, b"ic09": 512, b"ic10": 1024,
             b"ic11": 32, b"ic12": 64, b"ic13": 256, b"ic14": 512}
    chunks = [kind + struct.pack(">I", len(data) + 8) + data
              for kind, size in types.items() for data in (png(images[size]),)]
    toc = b"".join(chunk[:8] for chunk in chunks)
    toc_chunk = b"TOC " + struct.pack(">I", len(toc) + 8) + toc
    body = toc_chunk + b"".join(chunks)
    (ASSETS / "app-icon.icns").write_bytes(b"icns" + struct.pack(">I", len(body) + 8) + body)
    print("Generated app-icon.ico (16–256 px), app-icon.icns (16–1024 px), and PNG preview.")


if __name__ == "__main__":
    main()
