#!/usr/bin/env python3
"""Run as python3 -I tools/check_ui_text.py. Checks the overlay's text IDs: lists the ones in src/conker/game/ui_text.inc that no source file names, and the
ID-shaped string literals in ui.cpp and presets.cpp (lower case, underscores) that have no line. Exit status 1 on
either. A line used only by code that builds its ID is listed under 'unused': name it in a comment there."""
import re, sys, pathlib

root = pathlib.Path(__file__).resolve().parent.parent / "src/conker/game"
lit = r'"((?:[^"\\]|\\.)*)"'
inc = (root / "ui_text.inc").read_text(encoding="utf-8")
ids = re.findall(r'^\s*\{"([a-z0-9_]+)",', inc, re.M)
dups = sorted({i for i in ids if ids.count(i) > 1})
code = "".join((root / f).read_text(encoding="utf-8") for f in ("ui.cpp", "presets.cpp"))
used = {x.strip() for x in re.findall(lit, code)}  # items like " highest" keep leading spaces
unused = [i for i in ids if i not in used]
known = set(ids)
shaped = re.compile(r'^[a-z][a-z0-9]*(?:_[a-z0-9]+)+$|^[a-z]{3,}$')
widget_ids = {"section_t", "section_h"}  # ImGui hash seeds, not text
missing = sorted({s for s in used if shaped.match(s) and s not in known and "_" in s} - widget_ids)
for title, items in (("duplicate IDs", dups), ("unused lines", unused), ("IDs with no line", missing)):
    if items:
        print(title + ":", *items, sep="\n  ")
sys.exit(1 if dups or unused or missing else 0)
