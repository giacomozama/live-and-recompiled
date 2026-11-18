#!/bin/sh
# Rebuilds src/runtime/app/ui_font_ja.otf: the Japanese characters of the overlay's text (src/conker/game/ui_text.inc,
# ui.cpp and achievements.cpp's descriptions) from Noto Sans CJK JP (SIL OFL 1.1), merged into the overlay's font
# (src/runtime/app/ui_font.cpp). Run it after adding Japanese text. Needs fontTools (pyftsubset) and Noto Sans CJK installed.
set -e
cd "$(dirname "$0")/.."
src=${NOTO_CJK:-/usr/share/fonts/noto-cjk/NotoSansCJK-DemiLight.ttc}
chars=$(mktemp)
trap 'rm -f "$chars"' EXIT
python3 -I - src/conker/game/ui_text.inc src/conker/game/ui.cpp src/conker/game/achievements.cpp > "$chars" <<'PY'
import sys
text = "".join(open(p, encoding="utf-8").read() for p in sys.argv[1:])
print("".join(sorted({c for c in text if ord(c) > 0x2000})))
PY
pyftsubset "$src" --font-number=0 --text-file="$chars" --layout-features= --no-hinting \
    --desubroutinize --output-file=src/runtime/app/ui_font_ja.otf
ls -l src/runtime/app/ui_font_ja.otf
