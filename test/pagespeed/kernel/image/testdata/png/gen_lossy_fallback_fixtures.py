#!/usr/bin/env python3
# Generates the two PNGs the PNG lossy fallback tests use. Deterministic (seeded)
# and built from nothing but arithmetic and the font Pillow bundles, so the
# repository carries no third-party photograph. That font is Aileron Regular
# (Sora Sagano, dotcolon.net), which Pillow's ImageFont.load_default(size)
# loads; it is dedicated to the public domain (CC0 1.0; the font's own
# copyright field reads "No Rights Reserved"), not licensed under the SIL OFL.
#
#   python3 gen_lossy_fallback_fixtures.py OUTDIR
#
# smooth_noisy_blobs.png (400x300): twelve soft colour blobs on a mid-tone base
#   plus Gaussian noise (sigma 2). A smooth, photo-like image that the photo
#   detector classifies as graphics (PhotoMetric 5), large and costly to store
#   losslessly (about 9.4 bits per pixel), tiny and visually clean as lossy
#   WebP. The fallback must convert it.
#
# chroma_text_over_noise.png (400x300): a dark version of the same backdrop
#   with six lines of 12 px saturated red, green and magenta text. It also
#   classifies as graphics (PhotoMetric 5) and passes the size and ratio gates,
#   but 4:2:0 chroma subsampling smears the coloured glyph edges (worst 32x32
#   tile 20.2 dB at quality 80). The fallback must refuse it.
#
# The checked-in PNGs are pixel-identical to this script's output under
# numpy 2.0 and Pillow 10.4. The tests assert wide margins, not exact bytes, so
# a different numpy or font build does not require regenerating them.
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

W, H = 400, 300
SEED = 20261116


def blobs(rng, base, colour_lo, colour_hi):
    y, x = np.mgrid[0:H, 0:W].astype(np.float64)
    img = np.zeros((H, W, 3)) + np.array(base)
    for _ in range(12):
        cx, cy = rng.uniform(0, W), rng.uniform(0, H)
        r = rng.uniform(40, 140)
        col = rng.uniform(colour_lo, colour_hi, 3)
        img += col * np.exp(-((x - cx) ** 2 + (y - cy) ** 2) / (2 * r * r))[..., None]
    return img


def to_image(img):
    return Image.fromarray(np.clip(np.rint(img), 0, 255).astype(np.uint8), "RGB")


def positive(path, sigma=2.0):
    rng = np.random.default_rng(SEED)
    img = blobs(rng, [110.0, 120.0, 100.0], -70, 70)
    img += rng.normal(0, sigma, (H, W, 3))
    to_image(img).save(path)


def negative(path, sigma=2.0, font_size=12, lines=6):
    rng = np.random.default_rng(SEED)
    img = blobs(rng, [20.0, 22.0, 40.0], 0, 25)
    img += rng.normal(0, sigma, (H, W, 3))
    im = to_image(img)
    draw = ImageDraw.Draw(im)
    font = ImageFont.load_default(size=font_size)
    colours = [(255, 40, 40), (40, 255, 60), (255, 60, 255)]
    for i in range(lines):
        draw.text((8, 8 + i * (font_size + 5)), "$ make check  PASS 42/42  FAIL 0",
                  fill=colours[i % 3], font=font)
    im.save(path)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    positive(out + "/smooth_noisy_blobs.png")
    negative(out + "/chroma_text_over_noise.png")
