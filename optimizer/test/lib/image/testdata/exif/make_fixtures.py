#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Regenerates the rotated PNG/WebP EXIF-orientation fixtures in this directory.

Usage: python3 make_fixtures.py [output_dir]   (default: this directory)
Needs Pillow only.

Every fixture is the same 32x16 upright scene (top-left red, top-right green,
bottom-left blue, bottom-right yellow), stored the way a camera stores it for
orientations 3, 6 and 8 (16x32 for 6 and 8) with a one-entry little-endian
EXIF block (26 bytes) carrying the Orientation tag. The PNG is assembled by
hand (IHDR, eXIf, IDAT, IEND); the WebP comes from Pillow's writer (RIFF,
VP8X with the EXIF flag, VP8L or VP8, EXIF). The script asserts that Pillow's
exif_transpose() turns each stored image back into the upright scene.
"""
import os, struct, zlib, io, sys
from PIL import Image, ImageOps
out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
W, H = 32, 16
COL = {'TL': (255,0,0), 'TR': (0,255,0), 'BL': (0,0,255), 'BR': (255,255,0)}
def upright():
    im = Image.new('RGB', (W, H))
    for y in range(H):
        for x in range(W):
            im.putpixel((x, y), COL[('T' if y < H//2 else 'B') + ('L' if x < W//2 else 'R')])
    return im
UP = upright()
# Stored raster = inverse of the display transform for each orientation.
INV = {3: Image.Transpose.ROTATE_180, 6: Image.Transpose.ROTATE_90,   # stored is CCW of display
       8: Image.Transpose.ROTATE_270}
def tiff(o):  # little-endian TIFF, one IFD0 entry: Orientation
    return b'II*\x00\x08\x00\x00\x00' + struct.pack('<HHHIHH', 1, 0x0112, 3, 1, o, 0) + b'\x00\x00\x00\x00'
def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
for o in (3, 6, 8):
    stored = UP.transpose(INV[o])
    # Cross-check against Pillow's own EXIF transpose.
    ex = Image.Exif(); ex[0x0112] = o
    s2 = stored.copy(); s2.info['exif'] = ex.tobytes()
    assert list(ImageOps.exif_transpose(s2).getdata()) == list(UP.getdata()), o
    w, h = stored.size
    raw = b''.join(b'\x00' + stored.crop((0, y, w, y+1)).tobytes() for y in range(h))
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'eXIf', tiff(o)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
    open(f'{out}/exif_orientation_{o}.png', 'wb').write(png)
    b = io.BytesIO(); stored.save(b, 'WEBP', lossless=True, exif=tiff(o)); 
    open(f'{out}/exif_orientation_{o}.webp', 'wb').write(b.getvalue())

# Lossy orientation-6 WebP (VP8 + EXIF), for the lossy re-encode path.
b = io.BytesIO(); UP.transpose(INV[6]).save(b, 'WEBP', quality=100, exif=tiff(6))
open(f'{out}/exif_orientation_6_lossy.webp', 'wb').write(b.getvalue())
