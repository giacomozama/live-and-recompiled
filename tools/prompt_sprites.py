#!/usr/bin/env python3
"""Draws the key sprites the button prompts show while a player uses the keyboard (src/conker/common/prompts.cpp):
light key caps with the key's name, and a mouse with its button lit for the mouse buttons. One for every key a
binding can name (keyboard.cpp Named), since the prompts show whatever key the player bound. Each goes to
src/conker/common/prompts/<file>.png, and sprites.inc there embeds them all with the key names prompts.cpp looks them
up by (keyboard::KeyName).

    tools/prompt_sprites.py

Needs Pillow and DejaVu Sans Bold (found with fc-match).
"""
import subprocess
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

OUT = Path(__file__).resolve().parent.parent / "src/conker/common/prompts"
SIZE = 128  # the sprite's square; prompts.cpp centres it on a power of two

EDGE = (40, 40, 44, 255)  # outline and the mouse's lines
BODY = (225, 225, 228, 255)  # the cap's sides
FACE = (250, 250, 250, 255)  # its top
INK = (30, 30, 34, 255)  # the label
SUB = (110, 110, 118, 255)  # a small label over it ("Num", "Left")
LIT = (230, 80, 60, 255)  # the mouse button meant


def key_names():
    """Every key keyboard.cpp's Named gives a name, in its order."""
    keys = [chr(c) for c in range(ord("A"), ord("Z") + 1)] + [chr(c) for c in range(ord("0"), ord("9") + 1)]
    keys += [f"F{i}" for i in range(1, 13)] + [f"Numpad {i}" for i in range(10)]
    keys += ["Mouse 1", "Mouse 2", "Mouse 3", "Mouse 4", "Mouse 5", "Backspace", "Tab", "Enter", "Caps Lock",
             "Escape", "Space", "Page Up", "Page Down", "End", "Home", "Left", "Up", "Right", "Down", "Insert",
             "Delete", "Numpad *", "Numpad +", "Numpad -", "Numpad .", "Numpad /", "Left Shift", "Right Shift",
             "Left Ctrl", "Right Ctrl", "Left Alt", "Right Alt", ";", "=", ",", "-", ".", "/", "`", "[", "\\", "]",
             "'"]
    return keys


# what a cap reads where the key's name does not fit it: the label, and a small one over it
LABELS = {
    "Escape": ("Esc", None), "Backspace": ("Bksp", None), "Caps Lock": ("Caps", None), "Page Up": ("PgUp", None),
    "Page Down": ("PgDn", None), "Insert": ("Ins", None), "Delete": ("Del", None),
    "Left": ("←", None), "Up": ("↑", None), "Right": ("→", None), "Down": ("↓", None),
    "Left Shift": ("Shift", "Left"), "Right Shift": ("Shift", "Right"), "Left Ctrl": ("Ctrl", "Left"),
    "Right Ctrl": ("Ctrl", "Right"), "Left Alt": ("Alt", "Left"), "Right Alt": ("Alt", "Right"),
}

# a file's name for the characters a path cannot take or reads badly in one
CHARS = {";": "semicolon", "=": "equals", ",": "comma", "-": "minus", ".": "period", "/": "slash", "`": "backquote",
         "[": "left_bracket", "\\": "backslash", "]": "right_bracket", "'": "quote", "*": "star", "+": "plus"}


def label(key):
    if key in LABELS:
        return LABELS[key]
    if key.startswith("Numpad "):
        return key[7:], "Num"
    return key, None


def file_name(key):
    parts = [CHARS.get(w, w.lower()) for w in key.split(" ")]
    return "_".join(parts) + ".png"


def font_file():
    return subprocess.check_output(["fc-match", "-f", "%{file}", "DejaVu Sans:bold"]).decode()


def label_size(text):
    # one character large, longer names smaller to fit the cap
    return 64 if len(text) == 1 else 40 if len(text) <= 3 else 28 if len(text) <= 5 else 22


def key_cap(text, sub, font):
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((6, 10, SIZE - 6, SIZE - 6), radius=18, fill=BODY, outline=EDGE, width=6)
    d.rounded_rectangle((16, 18, SIZE - 16, SIZE - 26), radius=12, fill=FACE)
    middle = (18 + SIZE - 26) / 2
    if sub:
        d.text((SIZE / 2, 34), sub, font=ImageFont.truetype(font, 20), fill=SUB, anchor="mm")
        size = min(label_size(text), 44)
        d.text((SIZE / 2, middle + 14), text, font=ImageFont.truetype(font, size), fill=INK, anchor="mm")
    else:
        d.text((SIZE / 2, middle), text, font=ImageFont.truetype(font, label_size(text)), fill=INK, anchor="mm")
    return im


def mouse(button):
    """Mouse 1 left, 2 right, 3 the middle (the wheel), 4 and 5 the side buttons (back, the lower, and forward)."""
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((30, 8, SIZE - 30, SIZE - 8), radius=34, fill=FACE, outline=EDGE, width=6)
    d.line((64, 10, 64, 56), fill=EDGE, width=5)
    d.line((32, 56, SIZE - 32, 56), fill=EDGE, width=5)
    if button == 1:
        d.rounded_rectangle((36, 14, 60, 52), radius=8, fill=LIT)
    elif button == 2:
        d.rounded_rectangle((68, 14, SIZE - 36, 52), radius=8, fill=LIT)
    elif button == 3:
        d.rounded_rectangle((58, 20, 70, 46), radius=6, fill=LIT, outline=EDGE, width=3)
    else:  # on the left side, below the buttons: back the lower, forward above it
        top = 84 if button == 4 else 62
        d.rounded_rectangle((20, top, 40, top + 18), radius=6, fill=LIT, outline=EDGE, width=4)
    return im


def main():
    font = font_file()
    OUT.mkdir(parents=True, exist_ok=True)
    keys = key_names()
    for key in keys:
        if key.startswith("Mouse "):
            im = mouse(int(key[-1]))
        else:
            im = key_cap(*label(key), font)
        im.save(OUT / file_name(key), optimize=True)
    lines = ["// Generated by tools/prompt_sprites.py: every key sprite, embedded, by its keyboard::KeyName", ""]
    for i, key in enumerate(keys):
        lines += [f"const unsigned char kSprite{i}[] = {{", f'#embed "{file_name(key)}"', "};"]
    lines += ["", "constexpr Sprite kSprites[] = {"]
    for i, key in enumerate(keys):
        name = key.replace("\\", "\\\\")
        lines.append(f'    {{"{name}", kSprite{i}, sizeof kSprite{i}}},')
    lines += ["};", ""]
    (OUT / "sprites.inc").write_text("\n".join(lines))
    print(f"{len(keys)} sprites in {OUT}")


if __name__ == "__main__":
    main()
