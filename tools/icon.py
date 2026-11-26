#!/usr/bin/env python3
"""Makes the executables' icon from tools/icon.svg, into src/runtime/app/icon/:

    icon.ico    Windows: every size as PNG, linked into Game.exe and setup-windows.exe by icon.rc (Explorer, the
                taskbar, SDL's window class)
    icon.rgba   256x256 straight RGBA, the window icon where an executable carries none (Linux: the game's SDL window,
                Setup's FLTK one), embedded by app/icon.h

    tools/icon.py

Needs rsvg-convert and Pillow. The picture is centred on a transparent square.
"""
import io
import struct
import subprocess
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SVG = ROOT / "tools/icon.svg"
OUT = ROOT / "src/runtime/app/icon"
ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
RGBA_SIZE = 256


def render(size):
    """the SVG drawn at this size on its longer side, then centred on a size x size square"""
    png = subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), "--keep-aspect-ratio", str(SVG)],
                         check=True, capture_output=True).stdout
    pic = Image.open(io.BytesIO(png)).convert("RGBA")
    square = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    square.paste(pic, ((size - pic.width) // 2, (size - pic.height) // 2))
    return square


def png_bytes(img):
    b = io.BytesIO()
    img.save(b, "PNG", optimize=True)
    return b.getvalue()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    images = [png_bytes(render(s)) for s in ICO_SIZES]
    # ICONDIR, then an ICONDIRENTRY per image (0 means 256), then the PNGs
    ico = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for s, data in zip(ICO_SIZES, images):
        ico += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    ico += b"".join(images)
    (OUT / "icon.ico").write_bytes(ico)
    (OUT / "icon.rgba").write_bytes(render(RGBA_SIZE).tobytes())


if __name__ == "__main__":
    main()
