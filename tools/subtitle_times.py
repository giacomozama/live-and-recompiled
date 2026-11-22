#!/usr/bin/env python3
"""Sentence timings for the speech lines that have no {PMARKER}/{PARAGRAPH} to split them (subtitles.cpp).

  python3 -I tools/subtitle_times.py ASSETS TRANSCRIPTS > src/conker/game/subtitle_times.inc

ASSETS is the game's folder (dvddata/ beside default.xbe); TRANSCRIPTS a folder of Whisper word timings, one JSON
per voice clip (a list of segments, each with "words": [[start, end, word], ...]), named <bank>_<entry>.json after
tools/xwb.py's WAVs of dvddata/Audio/cut_*.xwb. Made with faster-whisper large-v3 (word_timestamps, language "en").

A line: a string of a level's (or extra_speech's) text with no marker and over kLong characters. Its English is
found in the clips by its words (the best share of them matched in order); each sentence starts at its first
matched word's time, in seconds from the clip's start. The other languages split at their own sentences: as many as
the English, they take its times; else each starts where its share of the characters falls on the English's.

The table is keyed by the line as the game holds it (tokens turned into codes as 0x1F3110 does, capitals fixed
(tools/text_fixes.py) and typos tidied as subtitles.cpp does, FNV-1a over the 16 bit codes): each entry the codes
where its sentences start and their times in milliseconds, and the voice's end.
"""
import difflib
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
import caff  # noqa: E402
import struct  # noqa: E402

kLong = 90
kLanguages = ["English", "German", "French", "Spanish", "Italian", "Japanese"]
# 0x6B9D98 / 0x6B9E28: the tokens 0x1F3110 turns into codes; any other name gets code 0, ending the string
kTokens = ["NORMAL", "RED", "GREEN", "BLUE", "YELLOW", "A", "B", "X", "Y", "JOYSTICK", "Z", "L", "R", "ARROW_LEFT",
           "ARROW_RIGHT", "ARROW_UP", "ARROW_DOWN", "SKULL", "SWIRL", "STAR", "LIGHTNING", "JIGGLE", "THROB",
           "PARAGRAPH", "THOUGHT", "ELECTRONIC", "TXT_MORE", "TXT_ST", "TXT_RD", "TXT_TH", "TXT_ND", "TXT_CURSOR",
           "TXT_JOYSTICK", "TXT_IGNORE", "PMARKER"]
kCodes = [0xFFA1, 0xFFA2, 0xFFA3, 0xFFA4, 0xFFA5, 0xFFA8, 0xFFA9, 0xFFBC, 0xFFBD, 0xFFBE, 0xFFAE, 0xFFAF, 0xFFB0,
          0xFFAA, 0xFFAB, 0xFFAC, 0xFFAD, 0xFFB2, 0xFFB3, 0xFFB4, 0xFFB5, 0xFFA7, 0xFFA6, 0x00BD, 0x00BE, 0x00BF,
          0xFFBA, 0xFFB6, 0xFFB8, 0xFFB9, 0xFFB7, 0xFFBB, 0xFFB1, 0x03E8, 0x00FE]


def texts(path):
    """key -> string of a text package (the key table names a string id, the LSBL table holds the strings)"""
    pk = caff.Package(path)
    out = {}
    for a in pk.assets.values():
        if a.type != "text":
            continue
        b = pk.data(a)
        u16 = lambda o: b[o] | b[o + 1] << 8

        def wstr(o):
            s = []
            while o + 1 < len(b) and u16(o):
                s.append(chr(u16(o)))
                o += 2
            return "".join(s)

        lsbl, ktab, nkeys = struct.unpack_from("<III", b, 0x20)
        p = lsbl + 0x20
        n = struct.unpack_from("<I", b, p)[0]
        ids = dict(struct.unpack_from("<HI", b, p + 4 + 6 * i) for i in range(n))
        base = p + 4 + 6 * (n + 1)
        for i in range(nkeys):
            off, _hash, sid = struct.unpack_from("<IHH", b, ktab + 8 * i)
            if sid in ids:
                out[wstr(off).lstrip("+")] = wstr(base + 2 * ids[sid])
    return out


def codes(s):
    """the string as the game holds it: each {TOKEN} its code (0x1F3110)"""
    out = []
    i = 0
    while i < len(s):
        if s[i] == "{" and "}" in s[i:]:
            j = s.index("}", i)
            name = s[i + 1:j]
            if name not in kTokens:
                break
            out.append(kCodes[kTokens.index(name)])
            i = j + 1
        else:
            out.append(ord(s[i]))
            i += 1
    return out


def tidy(cs, lang=None):
    """the text as subtitles.cpp's Tidy puts it (the table keyed by the tidied line): a run of stops is the ellipsis
    character; two spaces are one, three or more a line break (a bubble's padding); no space before a comma or a full
    stop (not French's before ! ? : ;); a space after a stop between a small letter and a capital ("a bit.To fool";
    an ellipsis only before ¿ ¡, "Aspetta…degli" stays); "ok" is "OK"; in French a no-break space before ! ? : ;
    (not a time's colon); no spaces or line breaks at the ends"""
    lower = lambda c: ord("a") <= c <= ord("z") or (0xDF <= c <= 0xFF and c not in (0xF7, 0xFE))  # 0xFE the marker
    upper = lambda c: ord("A") <= c <= ord("Z") or (0xC0 <= c <= 0xDE and c != 0xD7)
    digit = lambda c: ord("0") <= c <= ord("9")
    word = lambda c: lower(c) or upper(c) or digit(c)
    stop = lambda c: c in (ord("."), ord("!"), ord("?"), 0x2026)
    dots = []
    for c in cs:
        if c == ord(".") and dots and dots[-1] in (ord("."), 0x2026):
            dots[-1] = 0x2026
        else:
            dots.append(c)
    cs = dots
    out = []
    for i, c in enumerate(cs):
        nxt = cs[i + 1] if i + 1 < len(cs) else 0
        if c == 0x20:
            if i > 0 and cs[i - 1] == 0x20:
                continue
            j = i
            while j < len(cs) and cs[j] == 0x20:
                j += 1
            if j - i >= 3:  # padding that put what follows on a line of its own
                out.append(0x0A)
                continue
            nxt = cs[j] if j < len(cs) else 0
            if nxt in (ord(","), ord(".")):
                continue
        out.append(c)
        if stop(c) and i + 1 < len(cs) and not stop(nxt):
            a = i
            while a > 0 and stop(cs[a - 1]):
                a -= 1
            ellipsis = 0x2026 in cs[a:i + 1]
            if a > 0 and (lower(cs[a - 1]) or upper(cs[a - 1])) and \
                    ((lower(cs[a - 1]) and not ellipsis and upper(nxt)) or nxt in (0xBF, 0xA1)):
                out.append(0x20)
    for i in range(len(out) - 1):
        if out[i] in (ord("o"), ord("O")) and out[i + 1] in (ord("k"), ord("K")) and \
                (i == 0 or not word(out[i - 1])) and (i + 2 == len(out) or not word(out[i + 2])):
            out[i], out[i + 1] = ord("O"), ord("K")
    if lang == "French":
        marks = (ord("!"), ord("?"), ord(":"), ord(";"))
        fr = []
        for i, c in enumerate(out):
            if c in marks and fr and not (c == ord(":") and digit(fr[-1]) and i + 1 < len(out) and digit(out[i + 1])):
                if fr[-1] == 0x20:
                    fr[-1] = 0xA0
                elif fr[-1] not in marks and fr[-1] not in (0xA0, 0x0A):
                    fr.append(0xA0)
            fr.append(c)
        out = fr
    while out and out[-1] in (0x20, 0x0A):
        out.pop()
    while out and out[0] in (0x20, 0x0A):
        out.pop(0)
    return out


def fixed(lang, key, s):
    """the line as subtitles.cpp's Capture holds it: its fixes made (tools/text_fixes.py), tidied"""
    import text_fixes
    return tidy(text_fixes.fix(lang, key, codes(s), s=s), lang)


def fnv(cs):
    h = 0x811C9DC5
    for c in cs:
        h = ((h ^ (c & 0xFF)) * 0x01000193) & 0xFFFFFFFF
        h = ((h ^ (c >> 8)) * 0x01000193) & 0xFFFFFFFF
    return h


def plain(s):
    return re.sub(r"\{[A-Z_]+\}", "#", s)


def sentences(cs):
    """where each sentence starts, in codes: after . ! ? (and ... 。！？) and spaces, or a line break; short ones
    joined to the next, long ones cut at the comma (else the space) nearest their middle"""
    text = "".join(chr(c) if c < 0x10000 else " " for c in cs)
    starts = [0]
    for m in re.finditer(r"([.!?…]+[\"')]*\s+|[。！？]+|\n+)", text):
        if m.end() < len(text):
            starts.append(m.end())
    # join short pieces onto the next
    out = []
    for i, s in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else len(text)
        if out and (end - out[-1] < 28 or s - out[-1] < 28) and end - out[-1] <= kLong:
            continue
        out.append(s)
    # cut long ones
    final = []
    for i, s in enumerate(out):
        end = out[i + 1] if i + 1 < len(out) else len(text)
        final.append(s)
        while end - final[-1] > kLong + 10:
            a = final[-1]
            mid = (a + end) // 2
            cuts = [m.end() for m in re.finditer(r",\s+|、", text[a:end])]
            cuts = [a + c for c in cuts if 15 < c < end - a - 15] or [a + m.end() for m in re.finditer(r"\s+", text[a:end])
                                                                     if 15 < m.end() < end - a - 15]
            if not cuts:
                break
            final.append(min(cuts, key=lambda c: abs(c - mid)))
    return final


def norm(w):
    return re.sub(r"[^a-z0-9]", "", w.lower())


def main():
    assets, transcripts = sys.argv[1], sys.argv[2]
    clips = {}
    for f in sorted(glob.glob(os.path.join(transcripts, "*.json"))):
        words = [(norm(w[2]), w[0], w[1]) for seg in json.load(open(f)) for w in seg["words"]]
        clips[os.path.basename(f)[:-5]] = [w for w in words if w[0]]
    text_dir = os.path.join(assets, "dvddata", "aid", "text")
    folders = sorted(os.listdir(os.path.join(text_dir, "English")))
    entries = []
    for folder in folders:
        packs = {}
        for lang in kLanguages:
            p = os.path.join(text_dir, lang, folder, "default.bin")
            if os.path.exists(p):
                packs[lang] = texts(p)
        eng = packs.get("English", {})
        for key, s in sorted(eng.items()):
            if not (key.startswith("cutscene_") or folder == "extra_speech"):
                continue
            if "{PMARKER}" in s or "{PARAGRAPH}" in s or len(plain(s)) <= kLong or " " not in s:
                continue
            ecs = fixed("English", key, s)
            estarts = sentences(ecs)
            # the clip: the most of the line's words matched in order
            lw = [(norm(m.group(0)), m.start()) for m in re.finditer(r"\S+", "".join(map(chr, ecs)))]
            lw = [w for w in lw if w[0]]
            best = None
            for name, cw in clips.items():
                sm = difflib.SequenceMatcher(None, [w[0] for w in lw], [w[0] for w in cw], autojunk=False)
                blocks = sm.get_matching_blocks()
                got = sum(b.size for b in blocks)
                if not best or got > best[0] or (got == best[0] and len(cw) < len(best[3])):  # ties: the shortest clip
                    best = (got, name, blocks, cw)
            got, clip, blocks, cw = best
            if got < 0.5 * len(lw):
                print(f"// {folder} {key}: no clip ({got}/{len(lw)} words)", file=sys.stderr)
                continue
            at = {}
            for b in blocks:
                for k in range(b.size):
                    at[b.a + k] = cw[b.b + k]
            matched = sorted(at)
            end_t = at[matched[-1]][2]

            # each of the line's words on the clip's: a matched one where it matched, the others spread evenly over
            # the clip's words between their matched neighbours (Whisper's "Konka" for "Conker" still lands)
            to_clip = {}
            for b in blocks:
                for k in range(b.size):
                    to_clip[b.a + k] = b.b + k
            pairs = sorted(to_clip.items())
            first_a, first_b = pairs[0]
            if first_a > 0:  # unmatched words first: as many of the clip's words before the first match
                pairs.insert(0, (0, max(0, first_b - first_a)))

            def clip_index(i):
                for (a0, b0), (a1, b1) in zip(pairs, pairs[1:]):
                    if a0 <= i <= a1:
                        return round(b0 + (b1 - b0) * (i - a0) / (a1 - a0)) if a1 > a0 else b0
                return pairs[-1][1] if i > pairs[-1][0] else pairs[0][1]

            def time_of_char(pos):
                for i, (_w, p) in enumerate(lw):
                    if p >= pos:
                        return cw[min(clip_index(i), len(cw) - 1)][1]
                return end_t

            etimes = [time_of_char(p) for p in estarts]
            for i in range(1, len(etimes)):
                etimes[i] = max(etimes[i], etimes[i - 1])
            print(f"// {folder} {key}: {clip} {got}/{len(lw)} words, {len(estarts)} sentences, "
                  f"{etimes[0]:.2f}-{end_t:.2f}s", file=sys.stderr)

            def warp(frac):
                # a share of the English's characters to its time, through its sentences' starts
                pts = [(p / len(ecs), t) for p, t in zip(estarts, etimes)] + [(1.0, end_t)]
                for (f0, t0), (f1, t1) in zip(pts, pts[1:]):
                    if frac <= f1:
                        return t0 + (t1 - t0) * ((frac - f0) / (f1 - f0) if f1 > f0 else 0.0)
                return end_t

            for lang, pack in packs.items():
                if key not in pack:
                    continue
                cs = fixed(lang, key, pack[key])
                if not cs:
                    continue
                starts = sentences(cs)
                if len(starts) == len(estarts):
                    times = etimes
                else:
                    times = [warp(p / len(cs)) for p in starts]
                entries.append((fnv(cs), lang, folder, key, starts, times, end_t, len(cs)))

    print("// Generated by tools/subtitle_times.py: sentence timings of the speech lines with no markers")
    print("// {hash of the line's codes, sentences, {start code, ms from the voice's start}..., voice end ms}")
    for h, lang, folder, key, starts, times, end_t, n in entries:
        pairs = ", ".join(f"{{{s}, {int(t * 1000)}}}" for s, t in zip(starts, times))
        print(f"{{0x{h:08X}u, {n}, {int(end_t * 1000)}, {{{pairs}}}}},  // {lang} {folder} {key}")


if __name__ == "__main__":
    main()
