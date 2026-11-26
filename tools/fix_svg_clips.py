#!/usr/bin/env python3
"""Inline <use> references inside SVG clip paths.

Inkscape 1.4 saves clip paths as

    <clipPath><use xlink:href="#someGroup"/></clipPath>

Only Inkscape honours that; librsvg, resvg and browsers render nothing (which
is why the logo's eyes disappear).  This script resolves every such <use> and
puts the referenced shapes directly into the <clipPath>.

Usage:
    python3 fix_svg_clips.py [file.svg]     # default: logo.svg next to this script
"""

import sys
import xml.etree.ElementTree as ET
from pathlib import Path

SVG = "http://www.w3.org/2000/svg"
XLINK = "http://www.w3.org/1999/xlink"
SHAPES = {f"{{{SVG}}}{t}" for t in
          ("path", "circle", "ellipse", "rect", "polygon", "polyline")}
CONTAINERS = {f"{{{SVG}}}{t}" for t in ("g", "a", "svg", "switch")}
GEOMETRY_ATTRS = ("d", "cx", "cy", "r", "rx", "ry", "x", "y", "width", "height", "points")
KEEP_ATTRS = ("transform", "fill-rule")


def local(tag):
    return tag.rsplit("}", 1)[-1]


def href(el):
    return el.get(f"{{{XLINK}}}href") or el.get("href")


def collect(el, by_id, out, tx):
    """Append plain shapes for `el` (following <use>) to `out`."""
    tag = el.tag

    if tag == f"{{{SVG}}}use":
        target = by_id.get((href(el) or "").lstrip("#"))
        if target is None:
            print(f"warning: unresolved {href(el)}", file=sys.stderr)
            return
        t = list(tx)
        x, y = el.get("x", "0"), el.get("y", "0")
        if x not in ("0", "0.0") or y not in ("0", "0.0"):
            t.append(f"translate({x} {y})")
        if el.get("transform"):
            t.append(el.get("transform"))
        collect(target, by_id, out, t)

    elif tag in CONTAINERS:
        t = tx + ([el.get("transform")] if el.get("transform") else [])
        for child in el:
            collect(child, by_id, out, t)

    elif tag in SHAPES:
        shape = ET.Element(el.tag)
        for attr in GEOMETRY_ATTRS:
            if el.get(attr) is not None:
                shape.set(attr, el.get(attr))
        for attr in KEEP_ATTRS:
            if el.get(attr) is not None:
                shape.set(attr, el.get(attr))
        t = tx + ([el.get("transform")] if el.get("transform") else [])
        if t:
            shape.set("transform", " ".join(t))
        shape.set("fill", "#000000")  # clip paths must be filled, not stroke-only
        out.append(shape)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    path = Path(args[0]) if args else Path(__file__).with_name("logo.svg")

    # Keep comments, and preserve namespace prefixes on write.
    parser = ET.XMLParser(target=ET.TreeBuilder(insert_comments=True))
    tree = ET.parse(path, parser)
    root = tree.getroot()

    for prefix, uri in (("", SVG), ("xlink", XLINK),
                        ("inkscape", "http://www.inkscape.org/namespaces/inkscape"),
                        ("sodipodi", "http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd")):
        ET.register_namespace(prefix, uri)

    by_id = {el.get("id"): el for el in root.iter() if el.get("id")}

    fixed = 0
    for clip in root.iter(f"{{{SVG}}}clipPath"):
        children = list(clip)
        if not any(c.tag == f"{{{SVG}}}use" for c in children):
            continue
        new = []
        for child in children:
            if child.tag == f"{{{SVG}}}use":
                collect(child, by_id, new, [])
            else:
                new.append(child)
        if not new:
            print(f"warning: {clip.get('id')}: nothing to inline", file=sys.stderr)
            continue
        for child in children:
            clip.remove(child)
        clip.extend(new)
        fixed += 1
        print(f"{clip.get('id')}: inlined {len(new)} shape(s)")

    if not fixed:
        print(f"{path}: no <use>-based clip paths, nothing to do")
        return

    if hasattr(ET, "indent"):
        ET.indent(tree, space="  ")
    tree.write(path, encoding="UTF-8", xml_declaration=True)
    print(f"{path}: fixed {fixed} clip path(s)")


if __name__ == "__main__":
    main()
