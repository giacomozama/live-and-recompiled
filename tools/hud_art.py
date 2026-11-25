#!/usr/bin/env python3
"""Rasterizes the redrawn HUD art (src/conker/common/hud_art/svg/<name>.svg, each traced from the game's texture of
that name, e.g. globaleffects/sights/vertexSight) for the game to draw in place of its own (src/conker/common/hud_art.cpp,
settings.toml redrawn_hud).

    tools/hud_art.py [OUT_DIR]

The build runs it (CMakeLists.txt) into generated/hud_art/ of its build folder, which is where OUT_DIR defaults to
(out/hud_art); the rasters are never committed, only the SVGs. Each SVG becomes <OUT_DIR>/<name>.png, at least 512 texels on its long side and twice the texture's
size, and art.inc there embeds them with the texture's name and size, which the game finds it by.

The picture covers what the texture does. An SVG whose canvas no longer matches its texture (widened or moved in
Inkscape) says where the texture's square lies with data-texture="x y w h" on its root, in its own units; without it
the viewBox is the texture. Either way the square's size is the texture's, in texels. Where the canvas reaches past the
square, <name>.full.png is the whole canvas too (at the same texels a unit, the square's edges on whole texels), and
art.inc says how many texels it has past the square on each side: the game draws it where it can draw the texture
larger (hud_art.cpp). An SVG with data-glow on its root gets tools/sight_glow.py's glow under it, its blurs (sigmas in
SVG units) made at the picture's size. Colour is spread into the clear texels from the inked ones nearby, so filtering
toward a clear texel doesn't darken an edge.

Needs resvg on PATH, numpy and Pillow.
"""
import glob
import math
import os
import re
import sys

import numpy as np
from PIL import Image

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import sight_glow  # noqa: E402

SRC = os.path.join(TOOLS, "..", "src", "conker", "common", "hud_art")
OUT = os.path.join(TOOLS, "..", "out", "hud_art")
MIN_LONG = 512  # texels on the picture's long side, at least
MIN_SCALE = 4  # times the texture's size, at least


def rects(text):
    """the texture's square (data-texture, else the viewBox) and the canvas (the viewBox), each x, y, w, h"""
    tag = re.search(r"<svg\b[^>]*>", text, re.S).group(0)
    vb = re.search(r'\bviewBox="([^"]*)"', tag)
    if vb:
        canvas = tuple(float(v) for v in vb.group(1).replace(",", " ").split())
    else:
        canvas = (0.0, 0.0) + tuple(sight_glow.svg_size(text))
    m = re.search(r'\bdata-texture="([^"]*)"', tag)
    return (tuple(float(v) for v in m.group(1).split()) if m else canvas), canvas


def cropped(text, x, y, w, h):
    """the SVG with its canvas the rectangle x, y, w, h (in its units)"""
    root = re.search(r"<svg\b[^>]*>", text, re.S)
    tag = root.group(0)
    new = re.sub(r'\s(width|height|viewBox|data-texture)="[^"]*"', "", tag)
    new = new[:4] + f' width="{w:g}" height="{h:g}" viewBox="{x:g} {y:g} {w:g} {h:g}"' + new[4:]
    return text[:root.start()] + new + text[root.end():]


def picture(text, pw, ph, path):
    if re.search(r"<svg\b[^>]*\bdata-glow=", text, re.S):
        pm = sight_glow.glow_image(text, pw, ph)
    else:
        pm = sight_glow.render(text, pw, ph)
        pm[..., :3] *= pm[..., 3:4]
    bleed(pm).save(path, optimize=True)


def bleed(pm):
    """premultiplied RGBA -> straight RGBA, the clear texels taking the colour of the inked ones nearby"""
    # push-pull: the premultiplied picture halved down to a texel, then each clear texel takes the colour of the
    # coarser level's texel over it (the alpha-weighted mean of the inked texels under that one)
    alpha = pm[..., 3]
    levels = [pm]
    while min(levels[-1].shape[:2]) > 1 and levels[-1].shape[0] % 2 == 0 and levels[-1].shape[1] % 2 == 0:
        p = levels[-1]
        levels.append(p.reshape(p.shape[0] // 2, 2, p.shape[1] // 2, 2, 4).mean((1, 3)))
    rgb = None
    for p in reversed(levels):
        a = p[..., 3:4]
        own = np.where(a > 0, p[..., :3] / np.maximum(a, 1e-12), 0)
        if rgb is not None:
            own = np.where(a > 0, own, rgb.repeat(2, 0).repeat(2, 1))
        rgb = own
    out = np.concatenate([rgb, alpha[..., None]], -1)
    return Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA")


def main():
    global OUT
    if len(sys.argv) > 1:
        OUT = sys.argv[1]
    rows, embeds = [], []
    svg_root = os.path.join(SRC, "svg")
    os.makedirs(OUT, exist_ok=True)
    for old in glob.glob(os.path.join(OUT, "**", "*.png"), recursive=True):
        os.remove(old)  # pictures of art since removed go too
    for i, path in enumerate(sorted(glob.glob(os.path.join(svg_root, "**", "*.svg"), recursive=True))):
        name = os.path.splitext(os.path.relpath(path, svg_root))[0].replace(os.sep, "/")
        os.makedirs(os.path.dirname(os.path.join(OUT, name)), exist_ok=True)
        text = open(path).read()
        (tx, ty, uw, uh), (cx, cy, cw, ch) = rects(text)
        tw, th = round(uw), round(uh)
        if abs(uw - tw) > 0.01 or abs(uh - th) > 0.01 or tw & (tw - 1) or th & (th - 1):
            print(f"{name}: its texture square is {uw:g}x{uh:g}, not a texture's size: give it data-texture",
                  file=sys.stderr)
            continue
        scale = max(MIN_SCALE, -(-MIN_LONG // max(tw, th)))
        pw, ph = tw * scale, th * scale
        png = f"{name}.png"
        picture(cropped(text, tx, ty, uw, uh), pw, ph, os.path.join(OUT, png))
        embeds.append(f"const unsigned char kArt{i}[] = {{\n#embed \"{png}\"\n}};")
        # the canvas past the square, in whole texels of the picture: a larger picture of the whole canvas
        k = pw / uw  # texels a unit
        pad = [max(0, math.ceil(round(v * k, 3))) for v in (tx - cx, ty - cy, cx + cw - tx - uw, cy + ch - ty - uh)]
        full = "nullptr, 0"
        if any(pad):
            fw, fh = pw + pad[0] + pad[2], ph + pad[1] + pad[3]
            fpng = f"{name}.full.png"
            picture(cropped(text, tx - pad[0] / k, ty - pad[1] / k, fw / k, fh / k), fw, fh, os.path.join(OUT, fpng))
            embeds.append(f"const unsigned char kArtFull{i}[] = {{\n#embed \"{fpng}\"\n}};")
            full = f"kArtFull{i}, sizeof(kArtFull{i})"
            print(f"{name}: {tw}x{th} -> {pw}x{ph}, the whole canvas {fw}x{fh} (texels past the square: left "
                  f"{pad[0]}, top {pad[1]}, right {pad[2]}, bottom {pad[3]})")
        else:
            print(f"{name}: {tw}x{th} -> {pw}x{ph}")
        game_name = name.replace("/", "\\\\")  # the game's paths' separators, escaped for C
        rows.append(f'    {{"{game_name}", {tw}, {th}, {pw}, {ph}, kArt{i}, sizeof(kArt{i}), {full}, '
                    f'{{{pad[0]}, {pad[1]}, {pad[2]}, {pad[3]}}}}},')
    with open(os.path.join(OUT, "art.inc"), "w") as f:
        f.write("// Generated by tools/hud_art.py: each redrawn picture, embedded, by the name and size of its texture\n\n")
        f.write("\n".join(embeds) + "\n\nconst Art kArt[] = {\n" + "\n".join(rows) + "\n};\n")


if __name__ == "__main__":
    main()
