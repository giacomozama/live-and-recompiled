#!/usr/bin/env python3
"""Render a sight SVG and put a glow under it, the way the game's locked bazooka sights look: the SVG holds only the
crisp shapes, the glow is made from the raster. The glow is the picture's alpha blurred twice (a tight and a wide
Gaussian), each scaled by its strength, summed, clipped and filled with one colour; the picture is drawn over it.
Its settings come from the SVG root's data-glow attribute, e.g.
    data-glow="color=#5a7cff;sigma1=0.8;strength1=0.6;sigma2=2.6;strength2=0.5"
(sigmas in SVG units), or from the options, which win over it. With --compare the glowing picture is rendered at the
reference's size (4x supersampled) and the mean error per channel (0-255, premultiplied) is printed.
Needs resvg on PATH, numpy and Pillow.
usage: sight_glow.py in.svg out.png [--scale N] [--compare ref.png] [--color #rrggbb] [--sigma1 S] [--strength1 K]
                     [--sigma2 S] [--strength2 K]"""
import os
import re
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

SUPERSAMPLE = 4
DEFAULTS = dict(color='#5a7cff', sigma1=0.8, strength1=0.6, sigma2=2.6, strength2=0.5)


def svg_size(text):
    m = re.search(r'<svg\b[^>]*>', text, re.S)
    w = re.search(r'\bwidth="([\d.]+)', m.group(0))
    h = re.search(r'\bheight="([\d.]+)', m.group(0))
    return float(w.group(1)), float(h.group(1))


def glow_params(text, overrides=None):
    p = dict(DEFAULTS)
    m = re.search(r'<svg\b[^>]*\bdata-glow="([^"]*)"', text, re.S)
    if m:
        for item in m.group(1).split(';'):
            if '=' in item:
                k, v = item.split('=', 1)
                p[k.strip()] = v.strip()
    for k, v in (overrides or {}).items():
        if v is not None:
            p[k] = v
    for k in ('sigma1', 'strength1', 'sigma2', 'strength2'):
        p[k] = float(p[k])
    return p


def render(text, width, height):
    """resvg render -> float RGBA (straight alpha) array."""
    fd, svg = tempfile.mkstemp(suffix='.svg')
    os.close(fd)
    png = svg + '.png'
    try:
        with open(svg, 'w') as f:
            f.write(text)
        subprocess.run(['resvg', '-w', str(width), '-h', str(height), svg, png], check=True)
        return np.asarray(Image.open(png).convert('RGBA')).astype(np.float64) / 255
    finally:
        for p in (svg, png):
            if os.path.exists(p):
                os.remove(p)


def blur(a, sigma):
    """separable Gaussian blur of a 2D array (zero outside)."""
    if sigma <= 0:
        return a.copy()
    r = max(1, int(np.ceil(sigma * 3)))
    x = np.arange(-r, r + 1)
    k = np.exp(-0.5 * (x / sigma) ** 2)
    k /= k.sum()
    out = np.zeros_like(a)
    p = np.pad(a, ((0, 0), (r, r)))
    for i, w in enumerate(k):
        out += w * p[:, i:i + a.shape[1]]
    p = np.pad(out, ((r, r), (0, 0)))
    out = np.zeros_like(a)
    for i, w in enumerate(k):
        out += w * p[i:i + a.shape[0], :]
    return out


def apply_glow(rgba, params, px_per_unit):
    """rgba: straight-alpha float image. Returns premultiplied float RGBA with the glow under the picture."""
    a = rgba[..., 3]
    g = (params['strength1'] * blur(a, params['sigma1'] * px_per_unit)
         + params['strength2'] * blur(a, params['sigma2'] * px_per_unit))
    g = np.clip(g, 0, 1)
    c = params['color'].lstrip('#')
    col = np.array([int(c[i:i + 2], 16) for i in (0, 2, 4)], np.float64) / 255
    src = rgba.copy()
    src[..., :3] *= src[..., 3:4]
    out = np.empty_like(src)
    out[..., :3] = src[..., :3] + col * g[..., None] * (1 - a[..., None])
    out[..., 3] = a + g * (1 - a)
    return out


def glow_image(text, width, height, overrides=None):
    """render text at width x height (no supersampling) with the glow; premultiplied RGBA."""
    w, h = svg_size(text)
    return apply_glow(render(text, width, height), glow_params(text, overrides), width / w)


def compare(text, ref_path, overrides=None):
    ref = np.asarray(Image.open(ref_path).convert('RGBA')).astype(np.float64) / 255
    H, W = ref.shape[:2]
    pm = glow_image(text, W * SUPERSAMPLE, H * SUPERSAMPLE, overrides)
    pm = pm.reshape(H, SUPERSAMPLE, W, SUPERSAMPLE, 4).mean((1, 3))
    ref[..., :3] *= ref[..., 3:4]
    return float(np.abs(pm - ref).mean() * 255), pm


def to_image(pm):
    a = pm[..., 3:4]
    rgb = np.where(a > 0, pm[..., :3] / np.maximum(a, 1e-9), 0)
    return Image.fromarray((np.clip(np.concatenate([rgb, a], -1), 0, 1) * 255 + 0.5).astype(np.uint8), 'RGBA')


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    src, dst = args[0], args[1]
    opts = {}
    i = 2
    while i < len(args):
        k = args[i].lstrip('-')
        if i + 1 >= len(args):
            sys.exit(__doc__)
        opts[k] = args[i + 1]
        i += 2
    text = open(src).read()
    overrides = {k: opts.get(k) for k in DEFAULTS}
    if 'compare' in opts:
        err, pm = compare(text, opts['compare'], overrides)
        print(f'{os.path.basename(src)}: mean error {err:.2f}')
        to_image(pm).save(dst, 'PNG')
        return
    w, h = svg_size(text)
    scale = float(opts.get('scale', 1))
    to_image(glow_image(text, int(round(w * scale)), int(round(h * scale)), overrides)).save(dst, 'PNG')


if __name__ == '__main__':
    main()
