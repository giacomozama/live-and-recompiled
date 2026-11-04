#!/usr/bin/env python3
"""Draws the key sprites the button prompts show while a player uses the keyboard (src/conker/common/prompts.cpp):
light key caps with the key's name, and a mouse with its button lit for the mouse buttons. Each goes to
src/conker/common/prompts/<name>.png, named as prompts.cpp looks them up: keyboard::KeyName in lower case, spaces as
underscores. A new file also needs its #embed and table entry in prompts.cpp.

    tools/prompt_sprites.py                  the default bindings' keys
    tools/prompt_sprites.py "Left Ctrl" Q    these keys

Needs Pillow and DejaVu Sans Bold (found with fc-match).
"""
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

OUT = Path(__file__).resolve().parent.parent / "src/conker/common/prompts"
SIZE = 128  # the sprite's square; prompts.cpp centres it on a power of two

EDGE = (40, 40, 44, 255)  # outline and the mouse's lines
BODY = (225, 225, 228, 255)  # the cap's sides
FACE = (250, 250, 250, 255)  # its top
INK = (30, 30, 34, 255)  # the label
LIT = (230, 80, 60, 255)  # the mouse button meant

# the A, B, X and Y bindings (keyboard.cpp kInfo), then the pause screen's fixed keys (menus::PauseKey)
DEFAULTS = ["Space", "E", "Mouse 2", "R", "Tab", "Q", "Escape"]

# what a cap reads where the key's name is too long for it
LABELS = {"Escape": "Esc"}


def font_file():
    return subprocess.check_output(["fc-match", "-f", "%{file}", "DejaVu Sans:bold"]).decode()


def label_size(label):
    # one character large, longer names smaller to fit the cap
    return 64 if len(label) == 1 else 40 if len(label) <= 3 else 28 if len(label) <= 5 else 22


def key_cap(label, font):
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((6, 10, SIZE - 6, SIZE - 6), radius=18, fill=BODY, outline=EDGE, width=6)
    d.rounded_rectangle((16, 18, SIZE - 16, SIZE - 26), radius=12, fill=FACE)
    d.text((SIZE / 2, (18 + SIZE - 26) / 2), label, font=ImageFont.truetype(font, label_size(label)), fill=INK,
           anchor="mm")
    return im


def mouse(button):
    """Mouse 1 left, 2 right, 3 the middle (the wheel)."""
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((30, 8, SIZE - 30, SIZE - 8), radius=34, fill=FACE, outline=EDGE, width=6)
    d.line((64, 10, 64, 56), fill=EDGE, width=5)
    d.line((32, 56, SIZE - 32, 56), fill=EDGE, width=5)
    if button == 1:
        d.rounded_rectangle((36, 14, 60, 52), radius=8, fill=LIT)
    elif button == 2:
        d.rounded_rectangle((68, 14, SIZE - 36, 52), radius=8, fill=LIT)
    else:
        d.rounded_rectangle((58, 20, 70, 46), radius=6, fill=LIT, outline=EDGE, width=3)
    return im


def sprite_name(key):
    return key.lower().replace(" ", "_")


def main():
    keys = sys.argv[1:] or DEFAULTS
    font = font_file()
    OUT.mkdir(parents=True, exist_ok=True)
    for key in keys:
        if key.lower() in ("mouse 1", "mouse 2", "mouse 3"):
            im = mouse(int(key[-1]))
        else:
            im = key_cap(LABELS.get(key, key), font)
        path = OUT / (sprite_name(key) + ".png")
        im.save(path)
        print(path)


if __name__ == "__main__":
    main()
