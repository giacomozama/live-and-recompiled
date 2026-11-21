#!/usr/bin/env python3
"""Read the game's XACT wave banks (dvddata/Audio/*.xwb, "WBND" version 3).

  xwb.py list BANK               entry, format, channels, rate, seconds
  xwb.py wav BANK OUTDIR [N...]  each entry (or the ones named) as OUTDIR/<bank>_<N>.wav

Header: "WBND", version, then four (offset, length) segments: the bank's data (flags, entry count, name[16], meta
size, name size, alignment), the entries' metadata, the names (empty here) and the waves. An entry is 24 bytes:
flags/duration, the format, the play offset (into the wave segment) and length, the loop start and length. The
format: tag (bits 0-1: 0 PCM, 1 Xbox ADPCM), channels (2-4), samples a second (5-30), 16 bit (31).
Xbox ADPCM is IMA ADPCM in 36 byte blocks a channel (64 samples + the header's), written here as WAV format 0x11.
"""
import os
import struct
import sys


class Entry:
    pass


def read(path):
    d = open(path, "rb").read()
    if d[:4] != b"WBND":
        raise ValueError(f"{path}: not a wave bank")
    segs = [struct.unpack_from("<II", d, 8 + 8 * i) for i in range(4)]
    bank_off = segs[0][0]
    flags, count = struct.unpack_from("<II", d, bank_off)
    name = d[bank_off + 8:bank_off + 24].split(b"\0")[0].decode()
    meta_size = struct.unpack_from("<I", d, bank_off + 24)[0]
    entries = []
    for i in range(count):
        o = segs[1][0] + i * meta_size
        e = Entry()
        e.index = i
        _flags, fmt, e.offset, e.length = struct.unpack_from("<IIII", d, o)
        e.tag, e.channels, e.rate, e.bits16 = fmt & 3, (fmt >> 2) & 7, (fmt >> 5) & 0x3FFFFFF, fmt >> 31
        e.data = d[segs[3][0] + e.offset:segs[3][0] + e.offset + e.length]
        entries.append(e)
    return name, entries


def seconds(e):
    if e.tag == 1:
        return len(e.data) // (36 * e.channels) * 64 / e.rate
    return len(e.data) / (e.channels * (2 if e.bits16 else 1)) / e.rate


def wav(e):
    if e.tag == 1:
        block = 36 * e.channels
        fmt = struct.pack("<HHIIHHHH", 0x11, e.channels, e.rate, e.rate * block // 64, block, 4, 2, 64)
    else:
        bps = 16 if e.bits16 else 8
        fmt = struct.pack("<HHIIHH", 1, e.channels, e.rate, e.rate * e.channels * bps // 8, e.channels * bps // 8, bps)
    body = b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(e.data)) + e.data
    return b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    name, entries = read(sys.argv[2])
    if sys.argv[1] == "list":
        for e in entries:
            print(f"{e.index:4} {['pcm', 'adpcm', 'wma', '?'][e.tag]:5} {e.channels}ch {e.rate:6} {seconds(e):7.2f}s")
    elif sys.argv[1] == "wav":
        os.makedirs(sys.argv[3], exist_ok=True)
        pick = {int(x) for x in sys.argv[4:]}
        for e in entries:
            if not pick or e.index in pick:
                open(os.path.join(sys.argv[3], f"{name}_{e.index}.wav"), "wb").write(wav(e))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
