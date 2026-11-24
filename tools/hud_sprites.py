#!/usr/bin/env python3
"""Dump the game's textures to PNG by their asset names, for redrawing HUD art (crosshairs, frames, bars) as vectors.

    tools/hud_sprites.py OUT [FILTER ...]

Every CAFF package under assets/dvddata/aid is read (caff.py), one per worker process; each package's "package" asset names its files
(aid\\texture\\...\\default.rba) and points at each one's first asset, so a texture asset gets the name of the file it
starts. Textures go to OUT/<name>.png (aid\\texture\\ dropped) (frame 0, level 0; an animated texture's other frames as
<name>.<frame>.png), and OUT/index.tsv lists name, package, format, size, frames. FILTER words (case-insensitive)
keep only names containing one of them; with none, every named texture is written. FILTER "hud" is the HUD's
flat art (sights, the weapon panel and meters, MP radar and icons), without blurs, photos and portraits. A name seen in several
packages is written once.

Texture asset layout (after the 32-byte type header): +0x00 format (XDK D3DFORMAT), +0x0C width, height (u16),
+0x18 levels, +0x20 frames when +0x04 has bit 2; texels from the start of the asset's .gpu part, frames back to back.
"""
import concurrent.futures
import glob
import io
import os
import re
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import caff  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "dvddata", "aid")

# format: (name, bits per texel, swizzled, kind)
FORMATS = {
    0x00: ("L8", 8, True, "l8"), 0x01: ("AL8", 8, True, "l8"), 0x02: ("A1R5G5B5", 16, True, "1555"),
    0x03: ("X1R5G5B5", 16, True, "x555"), 0x04: ("A4R4G4B4", 16, True, "4444"), 0x05: ("R5G6B5", 16, True, "565"),
    0x06: ("A8R8G8B8", 32, True, "8888"), 0x07: ("X8R8G8B8", 32, True, "x888"), 0x0C: ("DXT1", 4, False, "dxt1"),
    0x0E: ("DXT3", 8, False, "dxt3"), 0x0F: ("DXT5", 8, False, "dxt5"), 0x10: ("LIN_A1R5G5B5", 16, False, "1555"),
    0x11: ("LIN_R5G6B5", 16, False, "565"), 0x12: ("LIN_A8R8G8B8", 32, False, "8888"),
    0x13: ("LIN_L8", 8, False, "l8"), 0x19: ("A8", 8, True, "a8"), 0x1A: ("A8L8", 16, True, "a8l8"),
    0x1C: ("LIN_X1R5G5B5", 16, False, "x555"), 0x1D: ("LIN_A4R4G4B4", 16, False, "4444"),
    0x1E: ("LIN_X8R8G8B8", 32, False, "x888"), 0x1F: ("LIN_A8", 8, False, "a8"), 0x20: ("LIN_A8L8", 16, False, "a8l8"),
}


def morton(w, h):
    """Texel index in memory for each (x, y), row-major: x and y bits interleave from the lowest, x first."""
    xs, ys = [0] * w, [0] * h
    bx = by = sb = 0
    while (1 << bx) < w or (1 << by) < h:
        if (1 << bx) < w:
            for x in range(w):
                xs[x] |= ((x >> bx) & 1) << sb
            bx += 1
            sb += 1
        if (1 << by) < h:
            for y in range(h):
                ys[y] |= ((y >> by) & 1) << sb
            by += 1
            sb += 1
    return xs, ys


def c565(v):
    r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
    return (r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31


def dxt(data, w, h, kind):
    px = bytearray(w * h * 4)
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    bs = 8 if kind == "dxt1" else 16
    for by in range(bh):
        for bx in range(bw):
            o = (by * bw + bx) * bs
            blk = data[o:o + bs]
            if len(blk) < bs:
                continue
            alpha = [255] * 16
            if kind == "dxt3":
                a = int.from_bytes(blk[:8], "little")
                alpha = [(a >> (4 * i) & 15) * 17 for i in range(16)]
                blk = blk[8:]
            elif kind == "dxt5":
                a0, a1 = blk[0], blk[1]
                bits = int.from_bytes(blk[2:8], "little")
                if a0 > a1:
                    pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
                else:
                    pal = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
                alpha = [pal[bits >> (3 * i) & 7] for i in range(16)]
                blk = blk[8:]
            c0, c1, idx = struct.unpack_from("<HHI", blk)
            p0, p1 = c565(c0), c565(c1)
            if c0 > c1 or kind != "dxt1":
                cols = [p0 + (255,), p1 + (255,),
                        tuple((2 * a + b) // 3 for a, b in zip(p0, p1)) + (255,),
                        tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)) + (255,)]
            else:
                cols = [p0 + (255,), p1 + (255,), tuple((a + b) // 2 for a, b in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
            for i in range(16):
                x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                if x < w and y < h:
                    r, g, b, a = cols[idx >> (2 * i) & 3]
                    if kind != "dxt1":
                        a = alpha[i]
                    q = (y * w + x) * 4
                    px[q:q + 4] = bytes((r, g, b, a))
    return px


def texel(v, kind):
    if kind == "8888":
        return v >> 16 & 255, v >> 8 & 255, v & 255, v >> 24
    if kind == "x888":
        return v >> 16 & 255, v >> 8 & 255, v & 255, 255
    if kind == "565":
        return c565(v) + (255,)
    if kind in ("1555", "x555"):
        r, g, b = v >> 10 & 31, v >> 5 & 31, v & 31
        return r * 255 // 31, g * 255 // 31, b * 255 // 31, (255 if v >> 15 or kind == "x555" else 0)
    if kind == "4444":
        return (v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17, (v >> 12) * 17
    if kind == "l8":
        return v, v, v, 255
    if kind == "a8":
        return 255, 255, 255, v
    if kind == "a8l8":
        return v & 255, v & 255, v & 255, v >> 8


def decode(data, fmt, w, h):
    name, bits, swz, kind = FORMATS[fmt]
    if kind.startswith("dxt"):
        return Image.frombytes("RGBA", (w, h), bytes(dxt(data, w, h, kind)))
    bpp = bits // 8
    need = w * h * bpp
    if len(data) < need:
        return None
    vals = [int.from_bytes(data[i * bpp:(i + 1) * bpp], "little") for i in range(w * h)]
    px = bytearray(w * h * 4)
    if swz:
        xs, ys = morton(w, h)
    for y in range(h):
        for x in range(w):
            v = vals[xs[x] | ys[y]] if swz else vals[y * w + x]
            q = (y * w + x) * 4
            px[q:q + 4] = bytes(texel(v, kind))
    return Image.frombytes("RGBA", (w, h), bytes(px))


def level0_size(fmt, w, h):
    _, bits, _, kind = FORMATS[fmt]
    if kind.startswith("dxt"):
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if kind == "dxt1" else 16)
    return w * h * bits // 8


def names(pkg):
    """asset index -> name of the file that asset starts, from the package asset's table"""
    d = pkg.sections[0]
    by_off = {a.data_off: a for a in pkg.assets.values() if a.data_off is not None}
    out = {}
    for a in pkg.assets.values():
        if a.type != "package":
            continue
        b = pkg.data(a)
        for i in range(struct.unpack_from("<I", b, 0x20)[0]):
            no, x = struct.unpack_from("<II", b, 0x24 + 8 * i)
            t = by_off.get(x - 0x20)
            if t is not None:
                out[t.index] = bytes(d[no:d.index(b"\0", no)]).decode("latin1")
    return out


# the "hud" filter: names starting with one of these, minus those containing one of HUD_SKIP
HUD = ["globaleffects/sights/", "globaleffects/smallsight", "globaleffects/sight_", "globaleffects/target_corner",
       "globaleffects/terminator_hud", "userInterface/", "userinterface/", "frontend/healthMarker",
       "frontend/armourMarker", "frontend/boostMarker", "frontend/profileKey", "frontend/pause/",
       "frontend/multiplayer/mpradar", "frontend/multiplayer/icons", "frontend/multiplayer/fe_icons",
       "frontend/multiplayer/multi_numbers", "frontend/multiplayer/multi_grenades",
       "frontend/multiplayer/multi_weapons", "frontend/multiplayer/liveIcons/", "frontend/icons/",
       "frontend/caUpDown", "frontend/controlS", "globaleffects/modeIcons/"]
# the MP objective icons beside the score (each map's flag, crown, barrel...) among the weapon pickup icons
HUD_RE = re.compile(r"globaleffects/icons/(\w+Obj|MP_\w*flag\w*)$")
HUD_SKIP = ["blur", "Money", "Chocolate", "TailHook", "xbox_controller", "red_cross", "BeeScoreBoard", "Bee_icon"]


def short_name(name):
    return name.replace("aid\\texture\\", "").replace("\\default.rba", "").replace("\\", "/")


def wanted(short, filters):
    if not filters:
        return True
    if "hud" in filters and (any(short.startswith(h) for h in HUD) or HUD_RE.match(short)) \
            and not any(k in short for k in HUD_SKIP):
        return True
    return any(f in short.lower() for f in filters if f != "hud")


def package_textures(job):
    """One package's wanted textures: (name, row, [(png path suffix, png bytes)], messages), in name order."""
    path, filters = job
    rel = os.path.relpath(path, ROOT)
    try:
        with open(path, "rb") as f:
            if f.read(4) != b"CAFF":
                return []
        pkg = caff.Package(path)
    except Exception as e:
        return [(None, None, [], [f"{rel}: {e}"])]
    named = names(pkg)
    if not named and rel.startswith("texture"):
        # a lone texture file: its own path is the name
        named = {i: "aid\\" + os.path.dirname(rel).replace("/", "\\") + "\\default.rba"
                 for i, a in pkg.assets.items() if a.type == "texture"}
    out, done = [], set()
    for idx, name in sorted(named.items()):
        a = pkg.assets[idx]
        if a.type != "texture" or name in done:
            continue
        short = short_name(name)
        if not wanted(short, filters):
            continue
        done.add(name)
        hdr = pkg.data(a)[0x20:]
        fmt, flags = struct.unpack_from("<II", hdr, 0)
        w, h = struct.unpack_from("<HH", hdr, 0x0C)
        frames = struct.unpack_from("<I", hdr, 0x20)[0] if flags & 4 and len(hdr) >= 0x24 else 1
        fname = FORMATS.get(fmt, (f"0x{fmt:X}",))[0]
        row = f"{short}\t{rel}\t{fname}\t{w}x{h}\t{frames}"
        if fmt not in FORMATS or not w or not h:
            out.append((name, row, [], [f"{short}: format {fname} {w}x{h} skipped"]))
            continue
        gpu = pkg.gpu(a)
        # frames are spaced by the whole frame (all levels); find it from the .gpu size
        step = len(gpu) // frames if frames > 1 else 0
        pngs, msgs = [], []
        for fr in range(min(frames, 64)):
            img = decode(gpu[fr * step:fr * step + level0_size(fmt, w, h)], fmt, w, h)
            if img is None:
                msgs.append(f"{short}: short data")
                break
            buf = io.BytesIO()
            img.save(buf, "PNG")
            pngs.append((short + (f".{fr}" if fr else "") + ".png", buf.getvalue()))
        out.append((name, row, pngs, msgs))
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out_dir, filters = sys.argv[1], [f.lower() for f in sys.argv[2:]]
    os.makedirs(out_dir, exist_ok=True)
    paths = [p for p in sorted(glob.glob(os.path.join(ROOT, "**", "*"), recursive=True)) if os.path.isfile(p)]
    seen, rows = set(), []
    # packages decode in parallel; results come back in path order, so the first package naming a texture wins
    with concurrent.futures.ProcessPoolExecutor() as pool:
        for result in pool.map(package_textures, [(p, filters) for p in paths], chunksize=4):
            for name, row, pngs, msgs in result:
                for m in msgs:
                    print(m, file=sys.stderr)
                if name is None or name in seen:
                    continue
                seen.add(name)
                rows.append(row)
                for rel_png, data in pngs:
                    dst = os.path.join(out_dir, rel_png)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    with open(dst, "wb") as f:
                        f.write(data)
                if pngs:
                    print(row.replace("\t", " "))
    with open(os.path.join(out_dir, "index.tsv"), "w") as f:
        f.write("name\tpackage\tformat\tsize\tframes\n" + "\n".join(rows) + "\n")


if __name__ == "__main__":
    main()
