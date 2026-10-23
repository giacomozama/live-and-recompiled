#!/usr/bin/env python3
"""Find the in-between pictures of an XBE_INTERP_RECORD capture (F4) that differ most from the frames around them:
each one drawn again is compared with the mean of the pictures before and after it, in 16 x 16 blocks, and the worst
block is reported. A phantom or a misplaced draw stands out; motion alone stays low. The pictures the runtime called
suspect (XBE_INTERP_SUSPECT) are listed after, with their error and rank, to tell how well that agrees. Needs numpy and
Pillow.
usage: interp_scan.py cap/interp_HHMMSS [count]"""
import os
import re
import sys

import numpy as np
from PIL import Image


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    folder = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    with open(os.path.join(folder, "notes.txt")) as f:
        notes = f.read()
    own = {int(m.group(1)) for m in re.finditer(r"^(\d{4}): .*the frame's own", notes, re.M)}
    suspects = {int(m.group(1)): m.group(2) for m in re.finditer(r"^(\d{4}): (?:.*\n(?!\d{4}: ))*?  suspects: (.*)", notes, re.M)}
    files = sorted(f for f in os.listdir(folder) if f.endswith(".bmp"))
    pics = [np.asarray(Image.open(os.path.join(folder, f)).convert("RGB"), dtype=np.float32) for f in files]
    found = []
    for i in range(1, len(pics) - 1):
        if i in own:
            continue
        err = np.abs(pics[i] - (pics[i - 1] + pics[i + 1]) / 2).sum(axis=2)
        h, w = err.shape[0] // 16 * 16, err.shape[1] // 16 * 16
        blocks = err[:h, :w].reshape(h // 16, 16, w // 16, 16).mean(axis=(1, 3))
        y, x = np.unravel_index(blocks.argmax(), blocks.shape)
        found.append((blocks.max(), i, x * 16, y * 16))
    ranked = sorted(found, reverse=True)
    for err, i, x, y in ranked[:count]:
        print(f"{i:04d}  block error {err:6.1f} at {x},{y}" + (f"  suspect: {suspects[i]}" if i in suspects else ""))
    if suspects:
        print("suspect pictures:")
        rank = {i: (k, err, x, y) for k, (err, i, x, y) in enumerate(ranked)}
        for i in sorted(suspects):
            if i in rank:
                k, err, x, y = rank[i]
                print(f"{i:04d}  block error {err:6.1f} at {x},{y}, rank {k + 1} of {len(ranked)}: {suspects[i]}")


if __name__ == "__main__":
    main()
