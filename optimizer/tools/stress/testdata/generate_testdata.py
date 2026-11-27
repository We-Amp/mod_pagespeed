#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Generate varied test assets for mod_pagespeed stress tests.

Creates images, CSS, JS, and HTML files at various sizes for testing
resource limits, cache pressure, and content-type handling.

Requires: pip install Pillow
"""

import os
import random
import string

OUTPUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "generated")


def ensure_dir(path):
    os.makedirs(path, exist_ok=True)


def random_text(size_bytes):
    """Generate random text content of approximately the given size."""
    words = [
        "".join(random.choices(string.ascii_lowercase, k=random.randint(3, 10)))
        for _ in range(100)
    ]
    lines = []
    current_size = 0
    while current_size < size_bytes:
        line = " ".join(random.choices(words, k=random.randint(5, 15))) + "\n"
        lines.append(line)
        current_size += len(line)
    return "".join(lines)[:size_bytes]


def generate_jpeg(path, target_size_bytes):
    """Generate a JPEG image of approximately the target size."""
    from PIL import Image

    # Start with a reasonable size and adjust quality
    width = max(100, int((target_size_bytes / 3) ** 0.5))
    height = width

    # Create image with random noise for better compression behavior
    img = Image.new("RGB", (width, height))
    pixels = img.load()
    for y in range(height):
        for x in range(width):
            pixels[x, y] = (
                random.randint(0, 255),
                random.randint(0, 255),
                random.randint(0, 255),
            )

    # Binary search for quality that produces target size
    lo, hi = 1, 95
    best_quality = 75
    for _ in range(10):
        mid = (lo + hi) // 2
        img.save(path, "JPEG", quality=mid)
        actual = os.path.getsize(path)
        if actual < target_size_bytes:
            lo = mid + 1
            best_quality = mid
        else:
            hi = mid - 1
            best_quality = mid

    # If still too small, increase image dimensions
    if os.path.getsize(path) < target_size_bytes * 0.5:
        scale = (target_size_bytes / max(os.path.getsize(path), 1)) ** 0.5
        width = int(width * scale * 1.2)
        height = int(height * scale * 1.2)
        img = Image.new("RGB", (width, height))
        pixels = img.load()
        for y in range(height):
            for x in range(width):
                pixels[x, y] = (
                    random.randint(0, 255),
                    random.randint(0, 255),
                    random.randint(0, 255),
                )
        img.save(path, "JPEG", quality=best_quality)

    print(f"  JPEG: {path} ({os.path.getsize(path)} bytes, target {target_size_bytes})")


def generate_png(path, width=200, height=200):
    """Generate a simple PNG image."""
    from PIL import Image

    img = Image.new("RGB", (width, height))
    pixels = img.load()
    for y in range(height):
        for x in range(width):
            pixels[x, y] = (
                (x * 17) % 256,
                (y * 23) % 256,
                ((x + y) * 7) % 256,
            )
    img.save(path, "PNG")
    print(f"  PNG: {path} ({os.path.getsize(path)} bytes)")


def generate_static_gif(path, width=100, height=100):
    """Generate a single-frame GIF."""
    from PIL import Image

    img = Image.new("P", (width, height))
    pixels = img.load()
    for y in range(height):
        for x in range(width):
            pixels[x, y] = (x + y) % 256
    img.save(path, "GIF")
    print(f"  Static GIF: {path} ({os.path.getsize(path)} bytes)")


def generate_animated_gif(path, frames=5, width=50, height=50):
    """Generate a multi-frame animated GIF."""
    from PIL import Image

    images = []
    for i in range(frames):
        img = Image.new("P", (width, height))
        pixels = img.load()
        for y in range(height):
            for x in range(width):
                pixels[x, y] = (x + y + i * 30) % 256
        images.append(img)

    images[0].save(
        path, "GIF", save_all=True, append_images=images[1:], duration=100, loop=0
    )
    print(f"  Animated GIF: {path} ({os.path.getsize(path)} bytes)")


def generate_css(path, target_size):
    """Generate CSS content of approximately the target size."""
    lines = []
    current = 0
    i = 0
    while current < target_size:
        rule = (
            f".class-{i} {{\n"
            f"  color: #{random.randint(0, 0xFFFFFF):06x};\n"
            f"  margin: {random.randint(0, 50)}px;\n"
            f"  padding: {random.randint(0, 30)}px {random.randint(0, 30)}px;\n"
            f"  font-size: {random.randint(8, 48)}px;\n"
            f"  /* {random_text(random.randint(20, 100)).strip()} */\n"
            f"}}\n\n"
        )
        lines.append(rule)
        current += len(rule)
        i += 1
    content = "".join(lines)[:target_size]
    with open(path, "w") as f:
        f.write(content)
    print(f"  CSS: {path} ({os.path.getsize(path)} bytes, target {target_size})")


def generate_js(path, target_size):
    """Generate JavaScript content of approximately the target size."""
    lines = []
    current = 0
    i = 0
    while current < target_size:
        func = (
            f"function func_{i}(a, b) {{\n"
            f"  // {random_text(random.randint(20, 80)).strip()}\n"
            f"  var result = a + b + {random.randint(0, 1000)};\n"
            f"  if (result > {random.randint(100, 10000)}) {{\n"
            f"    console.log('value: ' + result);\n"
            f"  }}\n"
            f"  return result;\n"
            f"}}\n\n"
        )
        lines.append(func)
        current += len(func)
        i += 1
    content = "".join(lines)[:target_size]
    with open(path, "w") as f:
        f.write(content)
    print(f"  JS: {path} ({os.path.getsize(path)} bytes, target {target_size})")


def generate_html(path, num_stylesheets=0, target_size=0):
    """Generate HTML with optional stylesheet references."""
    links = "\n".join(
        f'    <link rel="stylesheet" href="/css/style-{i}.css">'
        for i in range(num_stylesheets)
    )
    body_content = ""
    if target_size > 0:
        body_content = f"<!-- {random_text(target_size)} -->"

    content = f"""<!DOCTYPE html>
<html>
<head>
    <title>Stress Test Page</title>
{links}
</head>
<body>
    <h1>Stress Test</h1>
    <p>Page with {num_stylesheets} stylesheets.</p>
    {body_content}
</body>
</html>"""
    with open(path, "w") as f:
        f.write(content)
    print(f"  HTML: {path} ({os.path.getsize(path)} bytes)")


def generate_url_paths(path, count=10000):
    """Generate a file with unique URL paths for cache key pressure testing."""
    paths = [f"/images/img-{i:05d}.jpg" for i in range(count)]
    with open(path, "w") as f:
        f.write("\n".join(paths))
    print(f"  URL paths: {path} ({count} paths)")


def main():
    print("Generating stress test data...")
    ensure_dir(OUTPUT_DIR)
    ensure_dir(os.path.join(OUTPUT_DIR, "images"))
    ensure_dir(os.path.join(OUTPUT_DIR, "css"))
    ensure_dir(os.path.join(OUTPUT_DIR, "js"))
    ensure_dir(os.path.join(OUTPUT_DIR, "html"))

    # Images at various sizes
    print("\n--- Images ---")
    image_sizes = {
        "img-100k.jpg": 100 * 1024,
        "img-500k.jpg": 500 * 1024,
        "img-1m.jpg": 1024 * 1024,
        "img-5m.jpg": 5 * 1024 * 1024,
        "img-9.5m.jpg": int(9.5 * 1024 * 1024),  # Near 10MB limit
        "img-10.5m.jpg": int(10.5 * 1024 * 1024),  # Over 10MB limit
    }
    for name, size in image_sizes.items():
        generate_jpeg(os.path.join(OUTPUT_DIR, "images", name), size)

    generate_png(os.path.join(OUTPUT_DIR, "images", "test.png"))
    generate_static_gif(os.path.join(OUTPUT_DIR, "images", "static.gif"))
    generate_animated_gif(os.path.join(OUTPUT_DIR, "images", "animated.gif"))

    # CSS at various sizes
    print("\n--- CSS ---")
    css_sizes = {
        "style-1k.css": 1024,
        "style-100k.css": 100 * 1024,
        "style-1.9m.css": int(1.9 * 1024 * 1024),  # Near 2MB limit
        "style-2.1m.css": int(2.1 * 1024 * 1024),  # Over 2MB limit
    }
    for name, size in css_sizes.items():
        generate_css(os.path.join(OUTPUT_DIR, "css", name), size)

    # JS at various sizes
    print("\n--- JavaScript ---")
    js_sizes = {
        "app-1k.js": 1024,
        "app-100k.js": 100 * 1024,
        "app-1.9m.js": int(1.9 * 1024 * 1024),  # Near 2MB limit
        "app-2.1m.js": int(2.1 * 1024 * 1024),  # Over 2MB limit
    }
    for name, size in js_sizes.items():
        generate_js(os.path.join(OUTPUT_DIR, "js", name), size)

    # HTML at various sizes and stylesheet counts
    print("\n--- HTML ---")
    generate_html(os.path.join(OUTPUT_DIR, "html", "minimal.html"), num_stylesheets=0)
    generate_html(os.path.join(OUTPUT_DIR, "html", "few-css.html"), num_stylesheets=5)
    generate_html(os.path.join(OUTPUT_DIR, "html", "many-css.html"), num_stylesheets=20)
    generate_html(
        os.path.join(OUTPUT_DIR, "html", "large-4.9m.html"),
        target_size=int(4.9 * 1024 * 1024),
    )
    generate_html(
        os.path.join(OUTPUT_DIR, "html", "large-5.1m.html"),
        target_size=int(5.1 * 1024 * 1024),
    )

    # Small images that map to unique URL paths (for cache key pressure).
    # We create a handful of actual images and symlink the rest.
    print("\n--- Unique URL images (cache pressure) ---")
    small_img = os.path.join(OUTPUT_DIR, "images", "small.jpg")
    generate_jpeg(small_img, 10 * 1024)  # 10KB base image

    # Generate URL path list
    generate_url_paths(os.path.join(OUTPUT_DIR, "url_paths.txt"))

    # Create a simple index page
    index = """<!DOCTYPE html>
<html>
<head><title>Stress Test Origin</title></head>
<body>
<h1>Stress Test Origin Server</h1>
<p>Content served from tools/stress/testdata/generated/</p>
</body>
</html>"""
    with open(os.path.join(OUTPUT_DIR, "index.html"), "w") as f:
        f.write(index)

    print(f"\nDone! Test data generated in {OUTPUT_DIR}")


if __name__ == "__main__":
    main()
