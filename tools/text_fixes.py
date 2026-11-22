#!/usr/bin/env python3
"""Fixes to the speech lines' text, for the subtitles (subtitles.cpp). The bubbles draw their text in capitals, so the
text's own were never seen, and a few slips with them.

  python3 -I tools/text_fixes.py ASSETS > src/conker/game/text_fixes.inc

ASSETS is the game's folder (dvddata/ beside default.xbe). Every change is listed on stderr for review.

What a line should read (fix), on the speech lines (cutscene_* keys and extra_speech):
  - kManual: exact substitutions in a line (typos, names, a missing ¡, the counting rhyme's spelling).
  - English "i" on its own is "I" (not "i.e."), "mr."/"mrs." are "Mr."/"Mrs.".
  - "tediz" is "Tediz", in every language: the race's name.
  - a small letter after a sentence's end is a capital: after a single . (not an ellipsis, an abbreviation or an
    initial), or ! or ? however many, past spaces, piece or page breaks and opening quotes or ¿ ¡. Not after a
    censor's icons (the letter follows them, not the stop).
A fix never adds or takes away a {PMARKER} or {PARAGRAPH}: the cutscene scripts count them. What needs no line's
knowledge (the ellipsis character, "OK", French's spaces before ! ? : ;) is subtitles.cpp's Tidy, not here.

The table holds the differences as patterns, not lines: a mod's text (the community's decensor above all, whose
words are not as long as the censor's icons: "Fan-[5 icons]-tastic" is "fucking") still takes every fix whose
surroundings it kept. A pattern is the text it changes with as much of its page around it, in whole words, as tells
its places from every other page of the language's speech (a page: the codes up to a {PARAGRAPH}, as 0xE1880's hook
reads one), a censor's codes only when nothing else will do, tied to the page's start or end when it reaches them.
subtitles.cpp finds every pattern of the game's language in the page as read, all against that text, and makes the
changes in order of place, one overlapping an earlier one dropped; the tool does the same over every page and stops
unless each comes out as fixed.
"""
import difflib
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
import subtitle_times as st  # noqa: E402

kParagraph, kMarker = 0xBD, 0xFE
# the game's language number ([0x7FA1DC], menus.cpp's GameLanguage)
kLanguageNumber = {"English": 1, "Japanese": 2, "German": 3, "French": 4, "Spanish": 5, "Italian": 6}
kMaxContext = 48

# (language, key, old, new): old ({TOKEN}s as written) found once in the line
kManual = [
    ("English", "cutscene_W2_CANYON_9_2", "it's rodent", "it's Rodent"),
    ("English", "cutscene_W2_HARBOUR_17_1", "It's rodent", "It's Rodent"),
    ("English", "cutscene_W1_DINO_ARENA_26_1", "and jugga", "and Jugga"),
    ("English", "cutscene_W1_TRAINING_2_0", "No, birdy", "No, Birdy"),
    ("English", "cutscene_W1_TRAINING_2_1", "Okay, birdy", "Okay, Birdy"),
    ("English", "cutscene_THE_WINDMILL_21_0", "Scarecrow birdy", "Scarecrow Birdy"),
    ("English", "cutscene_W5_END_ARENA_11_0", "fabled panther king", "fabled Panther King"),
    ("English", "cutscene_W5_END_ARENA_11_0", "fairy panther king", "fairy Panther King"),
    ("English", "cutscene_W1_VOLCANO_RACE_1_1", "OH! hi", "Oh! hi"),
    ("English", "cutscene_W1_COG_LAND_6_2", "the caribbean", "the Caribbean"),
    ("English", "cutscene_W2_BOILERROOM_6_3", "some english person", "some English person"),
    ("English", "cutscene_THE_WINDMILL_13_0", "stick of tnt", "stick of TNT"),
    ("English", "cutscene_W5_END_ARENA_8_0", "Anyway, whoa{PMARKER}", "Anyway, whoa!{PMARKER}"),
    ("English", "cutscene_W5_BANK_ENTRANCE_4_12", "tête-a-tête", "tête-à-tête"),
    ("English", "cutscene_W1_TRAINING_CAVE_2_1", "Eenie, meenie, miney mo.", "Eeny, meeny, miny, moe."),
    ("English", "cutscene_W2_CANYON_9_6", "Eenie, meenie,{PMARKER}minie,", "Eeny, meeny,{PMARKER}miny,"),
    ("English", "cutscene_W2_LAB_2_3", "Eenie, meenie, minie...", "Eeny, meeny, miny..."),
    ("German", "cutscene_W1_TRAINING_2_0", "wie heisst du", "wie heißt du"),
    ("German", "cutscene_W2_BOILERROOM_7_0", "DAs ist", "Das ist"),
    ("German", "cutscene_W5_BANK_ENTRANCE_4_12", "Tête-a-tête", "Tête-à-tête"),
    ("French", "cutscene_FRONTEND_3_1", "Allo...", "Allô..."),
    ("French", "cutscene_FRONTEND_3_1", "Allo\xa0!", "Allô\xa0!"),
    ("French", "cutscene_W5_THRONE_ROOM_14_4", "Euréka\xa0! Euréka", "Eurêka\xa0! Eurêka"),
    ("French", "cutscene_W5_THRONE_ROOM_14_6", "EURÉKA", "EURÊKA"),
    ("French", "cutscene_W1_TRAINING_CAVE_2_1", "colé gram", "colégram"),
    ("Spanish", "cutscene_W1_BARN_INSIDE_5_4", "bam. bín", "bam, bín"),
    ("Spanish", "cutscene_W2_BOILERROOM_2_1", "Eh! No mires", "¡Eh! No mires"),
    ("Spanish", "cutscene_W1_COG_LAND_10_0", "Gracias, {THROB}", "¡Gracias, {THROB}"),
    ("Spanish", "cutscene_W5_END_ARENA_11_0", " A menos que", " ¡A menos que"),
    ("Spanish", "cutscene_W5_END_ARENA_11_0", "Pero es un personaje", "¡Pero es un personaje"),
    ("Spanish", "cutscene_W1_U_BEND_1_1", "Mira, ya te lo he dicho", "¡Mira, ya te lo he dicho"),
    ("Italian", "cutscene_W1_POO_CABIN_1_1", "Chissà perchè", "Chissà perché"),
    ("Italian", "cutscene_W2_CANYON_9_2", "Dì un po'", "Di' un po'"),
    ("Italian", "cutscene_W2_CANYON_9_2", "la tua divisa{PMARKER}È", "la tua divisa.{PMARKER}È"),
    ("Italian", "cutscene_W2_OUTSIDE_MOLEHOLE_9_3", "dirmi cha sta", "dirmi che sta"),
    ("Italian", "cutscene_W5_END_ARENA_19_3", "Iinsomma", "Insomma"),
    ("Italian", "cutscene_W1_ROCK_SOLID_10_0", "incorntrare", "incontrare"),
    ("Italian", "cutscene_THE_WINDMILL_13_0", "una carca di", "una carica di"),
    ("Italian", "cutscene_W5_THRONE_ROOM_17_4", "Ma guada un", "Ma guarda un"),
    ("Italian", "cutscene_W2_CANYON_10_0", "ci vediamo tra po'", "ci vediamo tra un po'"),
    ("Italian", "cutscene_W1_BRONTO_CAVERN_3_0", "una musica po' troppo", "una musica un po' troppo"),
    ("Italian", "cutscene_W1_WINDMILL_EXPLODED_3_1", "Sono rico ricco", "Sono ricco ricco"),
    ("Italian", "cutscene_W5_END_ARENA_8_0", "Comunque, uauh{PMARKER}", "Comunque, uauh!{PMARKER}"),
    ("Italian", "cutscene_W5_BANK_ENTRANCE_4_12", "tête-a-tête", "tête-à-tête"),
    ("Italian", "cutscene_W1_TRAINING_CAVE_2_1", "Ambarabbà", "Ambarabà"),
    ("Italian", "cutscene_W2_CANYON_9_6", "Ambarabbà", "Ambarabà"),
    ("Italian", "cutscene_W2_LAB_2_3", "Ambarabbà", "Ambarabà"),
]

# a word before a full stop that makes it no sentence's end
kAbbreviations = {"mr", "mrs", "ms", "dr", "st", "sr", "sra", "srta", "m", "mme", "mlle", "nr", "ca", "etc", "bzw",
                  "vs", "z.b", "k.o", "u.a", "d.h"}
# a censor's codes (colours, THROB, JIGGLE, the four icons): a decensor mod's words take their place
kCensor = set(range(0xFFA1, 0xFFA8)) | set(range(0xFFB2, 0xFFB6))


def lower(c):
    return ord("a") <= c <= ord("z") or (0xDF <= c <= 0xFF and c not in (0xF7, 0xFE))


def letter(c):
    return lower(c) or ord("A") <= c <= ord("Z") or (0xC0 <= c <= 0xDE and c != 0xD7)


def upper_of(c):
    if c == 0xFF:
        return 0x178
    return c - 0x20 if c != 0xDF else c  # a-z and à-þ are 0x20 below their capitals


def speech(lang, text_dir):
    """(folder, key, string) of the language's speech lines"""
    for folder in sorted(os.listdir(os.path.join(text_dir, lang))):
        p = os.path.join(text_dir, lang, folder, "default.bin")
        if not os.path.exists(p):
            continue
        for key, s in sorted(st.texts(p).items()):
            if key.startswith("cutscene_") or folder == "extra_speech":
                yield folder, key, s


def split(cs, sep):
    out, cur = [], []
    for c in cs:
        if c == sep:
            out.append(cur)
            cur = []
        else:
            cur.append(c)
    out.append(cur)
    return out


def pages(cs):
    """the line's pages as subtitles.cpp's Capture reads them (no line starts with a thought's or electronic
    bubble's mark; the four Spanish ones that start with ¿, 0xBF too, have it put back)"""
    return split(cs, kParagraph)


def fix(lang, key, cs, log=None, s=None):
    """what the line should read: its codes with the fixes made (s, the line as written, for the manual ones)"""
    if s is not None:
        for l, k, old, new in kManual:
            if l == lang and k == key:
                assert s.count(old) == 1, (k, old, s)
                s = s.replace(old, new)
                if log is not None:
                    log.append(f"{lang:8} {key:36} manual {old!r} -> {new!r}")
        cs = st.codes(s)
    cs = list(cs)
    text = lambda: "".join(chr(c) for c in cs)

    def change(i, c, why):
        if cs[i] != c:
            if log is not None:
                t = text()
                log.append(f"{lang:8} {key:36} {why:6} {t[max(0, i - 30):i]!r} {chr(cs[i])}->{chr(c)} "
                           f"{t[i + 1:i + 20]!r}")
            cs[i] = c

    t = text()
    for m in re.finditer(r"\btediz", t):
        change(m.start(), ord("T"), "Tediz")
    if lang == "English":
        for m in re.finditer(r"(?<![\w'])i(?![\w.])", t):
            change(m.start(), ord("I"), "I")
        for m in re.finditer(r"\bmrs?\.", t):
            change(m.start(), ord("M"), "Mr.")

    # a sentence's end before a small letter
    stops = (ord("."), ord("!"), ord("?"))
    gaps = (0x20, 0xA0, kParagraph, kMarker, ord('"'), ord("'"), ord("("), 0xBF, 0xA1)  # 0xBF ¿, 0xA1 ¡
    gaps += tuple(range(0xFFA1, 0xFFA8))  # colours, THROB, JIGGLE (not the icons)
    for i, c in enumerate(cs):
        if not lower(c):
            continue
        j = i - 1
        while j >= 0 and cs[j] in gaps:
            j -= 1
        if j < 0 or j == i - 1 or cs[j] not in stops:
            continue
        e = a = j  # the stop run cs[a..e]
        while a > 0 and cs[a - 1] in stops:
            a -= 1
        run = cs[a:e + 1]
        if run.count(ord(".")) > 1:
            continue  # an ellipsis
        if run == [ord(".")]:
            w = a
            while w > 0 and (letter(cs[w - 1]) or cs[w - 1] == ord(".")):
                w -= 1
            word = "".join(chr(x) for x in cs[w:a]).lower()
            if not word or len(word) == 1 or word in kAbbreviations:
                continue
        change(i, upper_of(c), "stop")
    return cs


def places(page, anchor, needle):
    """where a pattern's text stands in a page"""
    pos = page.find(needle)
    while pos >= 0:
        if (not anchor & 1 or pos == 0) and (not anchor & 2 or pos + len(needle) == len(page)):
            yield pos
        pos = page.find(needle, pos + 1)


def apply(page, patterns):
    """subtitles.cpp's FixPage: every pattern's places in the page, the changes in order of place"""
    edits = []
    for anchor, left, old, right, new in patterns:
        edits += [(pos + len(left), len(old), new) for pos in places(page, anchor, left + old + right)]
    edits.sort(key=lambda e: e[0])  # stable, as std::stable_sort
    out, cur = [], 0
    for at, n, new in edits:
        if at < cur:
            continue
        out += [page[cur:at], new]
        cur = at + n
    out.append(page[cur:])
    return "".join(out)


def pattern(before, wanted, n, i1, length, new, censor=False):
    """the change's pattern: the least context, fewest characters first, that finds only changes it makes; a censor's
    codes in it only when nothing else tells its places apart (a decensored line goes without that fix)"""
    page = before[n]
    i2 = i1 + length
    for total in range(kMaxContext + 1):
        for l in range(total + 1):
            r = total - l
            if l > i1 or r > len(page) - i2:
                continue
            left, right = page[i1 - l:i1], page[i2:i2 + r]
            if not censor and any(ord(c) in kCensor for c in left + right):
                continue
            anchor = (1 if l == i1 else 0) | (2 if r == len(page) - i2 else 0)
            # whole words: the context starts and ends past a word's letters, or at the page's edge
            if not (anchor & 1 or (left and not left[0].isalnum())) or \
                    not (anchor & 2 or (right and not right[-1].isalnum())):
                continue
            needle = left + page[i1:i2] + right
            if all(wanted.get((m, pos + l)) == (length, new)
                   for m, other in enumerate(before) for pos in places(other, anchor, needle)):
                return (anchor, left, page[i1:i2], right, new)
    if not censor:
        print(f"// censor in the pattern for {page[max(0, i1 - 20):i2 + 20]!r}", file=sys.stderr)
        return pattern(before, wanted, n, i1, length, new, True)
    sys.exit(f"no pattern for {page[max(0, i1 - 20):i2 + 20]!r}")


def main():
    assets = sys.argv[1]
    text_dir = os.path.join(assets, "dvddata", "aid", "text")
    log, entries = [], []
    for lang in st.kLanguages:
        before, after = [], []  # every page of the language's speech
        for folder, key, s in speech(lang, text_dir):
            cs = st.codes(s)
            po, pf = pages(cs), pages(fix(lang, key, cs, log, s))
            assert [len(split(p, kMarker)) for p in po] == [len(split(p, kMarker)) for p in pf], (lang, key)
            before += ["".join(map(chr, p)) for p in po]
            after += ["".join(map(chr, p)) for p in pf]

        wanted = {}  # (page, place) -> (length, new)
        for n, (a, b) in enumerate(zip(before, after)):
            if a != b:
                for op, i1, i2, j1, j2 in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes():
                    if op != "equal":
                        wanted[(n, i1)] = (i2 - i1, b[j1:j2])
        patterns = []
        for (n, i1), (length, new) in sorted(wanted.items()):
            p = pattern(before, wanted, n, i1, length, new)
            if p not in patterns:
                patterns.append(p)
        for a, b in zip(before, after):
            if apply(a, patterns) != b:
                sys.exit(f"{lang}: {a!r} comes out {apply(a, patterns)!r}, not {b!r}")
        entries += [(kLanguageNumber[lang], p) for p in patterns]
        print(f"// {lang}: {len(wanted)} changes, {len(patterns)} patterns", file=sys.stderr)
    print("\n".join(log), file=sys.stderr)

    def lit(s):
        return 'u"' + "".join(c if 0x20 <= ord(c) < 0x7F and c not in '"\\?' else f"\\u{ord(c):04X}" for c in s) + '"'

    print("// Generated by tools/text_fixes.py: the speech lines' fixes, as patterns in a page")
    print("// {language ([0x7FA1DC]), anchor (1 the page's start, 2 its end), left, old, right, new}")
    for lang, (anchor, left, old, right, new) in entries:
        print(f"{{{lang}, {anchor}, {lit(left)}, {lit(old)}, {lit(right)}, {lit(new)}}},")


if __name__ == "__main__":
    main()
