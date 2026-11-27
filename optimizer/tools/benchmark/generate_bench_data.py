#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Generate test files for the zero-copy benchmark.

Creates HTML and JPEG files at known sizes. No external dependencies —
uses minimal valid JPEG binary and padded HTML.
"""

import os
import struct

OUTPUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "benchdata")


def ensure_dir(path):
    os.makedirs(path, exist_ok=True)


def generate_html(path, target_size):
    """Generate an HTML file of approximately the target size."""
    header = "<!DOCTYPE html>\n<html><head><title>Bench</title></head>\n<body>\n"
    footer = "\n</body></html>\n"
    overhead = len(header) + len(footer)
    # Fill with paragraph text to reach target size
    fill_size = max(0, target_size - overhead)
    # Use repeating readable text
    line = "<p>" + "The quick brown fox jumps over the lazy dog. " * 5 + "</p>\n"
    repeats = fill_size // len(line)
    remainder = fill_size - repeats * len(line)
    body = line * repeats + line[:remainder]
    content = header + body + footer
    with open(path, "w") as f:
        f.write(content)
    actual = os.path.getsize(path)
    print(f"  {os.path.basename(path)}: {actual} bytes (target {target_size})")


def make_minimal_jpeg():
    """Create a minimal valid 1x1 grayscale JPEG (~158 bytes).

    Returns the raw bytes of a valid JPEG that libjpeg-turbo will accept.
    Includes both DC and AC Huffman tables (required for baseline decoding).
    """
    # SOI
    data = b"\xff\xd8"
    # APP0 JFIF marker
    data += b"\xff\xe0"
    data += struct.pack(">H", 16)  # length
    data += b"JFIF\x00"  # identifier
    data += b"\x01\x01"  # version 1.1
    data += b"\x00"  # aspect ratio units: none
    data += struct.pack(">HH", 1, 1)  # x/y density
    data += b"\x00\x00"  # no thumbnail
    # DQT (quantization table)
    data += b"\xff\xdb"
    data += struct.pack(">H", 67)  # length
    data += b"\x00"  # 8-bit, table 0
    data += bytes([8] * 64)  # flat quantization
    # SOF0 (start of frame, baseline, 1x1, grayscale)
    data += b"\xff\xc0"
    data += struct.pack(">H", 11)  # length
    data += b"\x08"  # 8-bit precision
    data += struct.pack(">HH", 1, 1)  # 1x1 pixels
    data += b"\x01"  # 1 component (grayscale)
    data += b"\x01\x11\x00"  # comp 1: id=1, sampling=1x1, quant table 0
    # DHT — DC Huffman table (class=0, id=0)
    data += b"\xff\xc4"
    dc_bits = bytes([0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])
    dc_vals = bytes([0])
    data += struct.pack(">H", 2 + 1 + 16 + len(dc_vals))
    data += b"\x00"  # DC table 0
    data += dc_bits + dc_vals
    # DHT — AC Huffman table (class=1, id=0) with EOB symbol
    data += b"\xff\xc4"
    ac_bits = bytes([0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])
    ac_vals = bytes([0x00])  # EOB symbol
    data += struct.pack(">H", 2 + 1 + 16 + len(ac_vals))
    data += b"\x10"  # AC table 0 (class=1, id=0)
    data += ac_bits + ac_vals
    # SOS (start of scan)
    data += b"\xff\xda"
    data += struct.pack(">H", 8)  # length
    data += b"\x01"  # 1 component
    data += b"\x01\x00"  # comp 1 uses DC table 0, AC table 0
    data += b"\x00\x3f\x00"  # spectral selection: Ss=0, Se=63, Ah=0|Al=0
    # Entropy-coded segment: DC zero (2 bits) + AC EOB (2 bits) + 4 pad bits
    data += b"\x00"
    # EOI
    data += b"\xff\xd9"
    return data


def generate_jpeg(path, target_size):
    """Generate a JPEG file of approximately the target size.

    Pads a minimal valid JPEG with COM (comment) markers to reach the target.
    Every JPEG decoder ignores COM markers, so the file remains valid at any size.
    COM markers are placed after the APP0/JFIF segment per the JFIF spec.
    """
    base = make_minimal_jpeg()
    if target_size <= len(base):
        with open(path, "wb") as f:
            f.write(base)
    else:
        # Split after SOI + APP0 (JFIF requires APP0 immediately after SOI)
        # SOI = 2 bytes, APP0 = 2 (marker) + 16 (length including itself) = 18 bytes
        app0_end = 2 + 18  # offset 20
        head = base[:app0_end]  # SOI + APP0
        rest = base[app0_end:]  # DQT, SOF0, DHT, SOS, data, EOI

        # Insert COM markers between APP0 and DQT
        # COM marker: FF FE [2-byte length] [data...]
        # Max COM payload: 65533 bytes (length field max 0xFFFF - 2 for itself)
        padding_needed = target_size - len(base)
        comments = b""
        while padding_needed > 0:
            # 4 bytes overhead per COM marker (FF FE + 2-byte length)
            if padding_needed < 4:
                # Can't fit another COM marker; use a minimal one padded to exact size
                # Minimum COM: FF FE 00 02 (empty payload, 4 bytes)
                # We need exactly padding_needed bytes, so just use a COM with
                # (padding_needed - 4) payload — but that's negative. Instead,
                # extend the last COM or accept being slightly over target.
                break
            chunk = min(padding_needed - 4, 65533)
            marker_len = chunk + 2  # length field includes itself
            comments += b"\xff\xfe"
            comments += struct.pack(">H", marker_len)
            comments += b"\x00" * chunk
            padding_needed -= 4 + chunk

        with open(path, "wb") as f:
            f.write(head + comments + rest)

    actual = os.path.getsize(path)
    print(f"  {os.path.basename(path)}: {actual} bytes (target {target_size})")


def main():
    print("Generating benchmark test data...")
    ensure_dir(OUTPUT_DIR)

    # HTML files
    print("\n--- HTML ---")
    html_files = {
        "1k.html": 1024,
        "10k.html": 10 * 1024,
        "100k.html": 100 * 1024,
    }
    for name, size in html_files.items():
        generate_html(os.path.join(OUTPUT_DIR, name), size)

    # JPEG files
    print("\n--- JPEG ---")
    jpeg_files = {
        "10k.jpg": 10 * 1024,
        "100k.jpg": 100 * 1024,
        "1m.jpg": 1024 * 1024,
    }
    for name, size in jpeg_files.items():
        generate_jpeg(os.path.join(OUTPUT_DIR, name), size)

    # Index page
    index = """<!DOCTYPE html>
<html><head><title>Benchmark Origin</title></head>
<body><h1>Benchmark Test Files</h1></body>
</html>"""
    with open(os.path.join(OUTPUT_DIR, "index.html"), "w") as f:
        f.write(index)

    print(f"\nDone! Benchmark data in {OUTPUT_DIR}")


if __name__ == "__main__":
    main()
