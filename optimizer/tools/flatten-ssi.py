#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Flatten Apache SSI includes from the legacy mod_pagespeed website.

Reads HTML files from reference/mod_pagespeed/html/ containing Apache SSI
directives (<!--#include virtual="..." -->), resolves them recursively, and
writes standalone HTML files to $WEBSITE_DIR/public/1.0/.

WEBSITE_DIR names the modpagespeed.com site tree, which lives in the public
repository (We-Amp/pagespeed-optimizer, website/). Default: <repo root>/website. The script fails with a
clear message when that directory does not exist.

Non-HTML assets (CSS, images, JS) are copied as-is.  A dismissable archive
banner is injected into every HTML page that has a <body> tag.

Usage:
    python3 tools/flatten-ssi.py          # from repo root
    ./tools/flatten-ssi.py                # if executable
"""

import os
import re
import shutil
import sys

# ---------------------------------------------------------------------------
# Paths (relative to repository root)
# ---------------------------------------------------------------------------
REPO_ROOT = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
)
INPUT_DIR = os.path.join(REPO_ROOT, "reference", "mod_pagespeed", "html")
WEBSITE_DIR = os.path.abspath(
    os.environ.get("WEBSITE_DIR")
    or os.path.join(REPO_ROOT, "website")
)
OUTPUT_DIR = os.path.join(WEBSITE_DIR, "public", "1.0")

# Files whose names start with "_" are SSI fragments -- we include their
# content into other files but do not emit them as standalone pages.
FRAGMENT_PREFIX = "_"

# Maximum recursion depth for SSI includes (safety net).
MAX_INCLUDE_DEPTH = 10

# Regex matching  <!--#include virtual="..." -->
# Allows optional whitespace around the directive.
SSI_RE = re.compile(r'<!--\s*#include\s+virtual="([^"]+)"\s*-->', re.IGNORECASE)

# Absolute-path references we want to rewrite so they stay inside /1.0/.
# We match href="/ and src="/ but NOT href="// (protocol-relative) and
# NOT href="https?://" (absolute URLs).
ABS_PATH_RE = re.compile(r"""((?:href|src|srcset)\s*=\s*")(/(?!/))""", re.IGNORECASE)

# ---------------------------------------------------------------------------
# Archive banner (inline-styled, dismissable)
# ---------------------------------------------------------------------------
BANNER_HTML = """\
<div id="archive-banner" style="background-color:#e1f5fe;padding:12px 24px;\
text-align:center;font-family:sans-serif;font-size:14px;line-height:1.6;\
position:relative;border-bottom:1px solid #b3e5fc;">
  <span>This is the archived documentation for mod_pagespeed 1.0. \
Looking for ModPageSpeed 2.0? Visit \
<a href="https://modpagespeed.com" style="color:#0277bd;\
font-weight:bold;">modpagespeed.com</a></span>
  <button onclick="this.parentElement.style.display='none'" \
style="position:absolute;right:12px;top:50%;transform:translateY(-50%);\
background:none;border:none;font-size:18px;cursor:pointer;\
color:#0277bd;padding:0 4px;" \
aria-label="Dismiss">&times;</button>
</div>
"""


# ---------------------------------------------------------------------------
# SSI resolution
# ---------------------------------------------------------------------------
def resolve_ssi(content: str, base_dir: str, depth: int = 0) -> str:
    """Recursively replace <!--#include virtual="..."--> with file contents."""
    if depth > MAX_INCLUDE_DEPTH:
        print(
            f"  WARNING: max include depth ({MAX_INCLUDE_DEPTH}) exceeded",
            file=sys.stderr,
        )
        return content

    def _replace(match: re.Match) -> str:
        virtual_path = match.group(1)
        include_file = os.path.normpath(os.path.join(base_dir, virtual_path))
        if not os.path.isfile(include_file):
            print(f"  WARNING: include not found: {include_file}", file=sys.stderr)
            return f"<!-- include not found: {virtual_path} -->"
        with open(include_file, encoding="utf-8", errors="replace") as f:
            included = f.read()
        # The included fragment may itself contain SSI directives.
        return resolve_ssi(included, os.path.dirname(include_file), depth + 1)

    return SSI_RE.sub(_replace, content)


# ---------------------------------------------------------------------------
# Link rewriting
# ---------------------------------------------------------------------------
def rewrite_absolute_paths(content: str) -> str:
    r"""Rewrite absolute paths so they resolve under /1.0/.

    href="/doc/foo"  -->  href="/1.0/doc/foo"
    src="/we-amp.jpg" --> src="/1.0/we-amp.jpg"

    Protocol-relative (//cdn.example.com) and full URLs are untouched.
    """
    return ABS_PATH_RE.sub(r"\g<1>/1.0/", content)


# ---------------------------------------------------------------------------
# Banner injection
# ---------------------------------------------------------------------------
def inject_banner(content: str) -> str:
    """Insert the archive banner right after the opening <body...> tag."""
    # Match <body> with optional attributes.
    body_re = re.compile(r"(<body[^>]*>)", re.IGNORECASE)
    m = body_re.search(content)
    if m:
        insert_pos = m.end()
        return content[:insert_pos] + "\n" + BANNER_HTML + content[insert_pos:]
    return content


# ---------------------------------------------------------------------------
# Main processing
# ---------------------------------------------------------------------------
def is_html(filename: str) -> bool:
    return filename.lower().endswith((".html", ".htm"))


def is_fragment(filename: str) -> bool:
    return os.path.basename(filename).startswith(FRAGMENT_PREFIX)


def process_html(src_path: str, dst_path: str) -> None:
    """Read an HTML file, flatten SSI includes, inject banner, write output."""
    with open(src_path, encoding="utf-8", errors="replace") as f:
        content = f.read()

    base_dir = os.path.dirname(src_path)
    content = resolve_ssi(content, base_dir)
    content = rewrite_absolute_paths(content)
    content = inject_banner(content)

    os.makedirs(os.path.dirname(dst_path), exist_ok=True)
    with open(dst_path, "w", encoding="utf-8") as f:
        f.write(content)


def copy_asset(src_path: str, dst_path: str) -> None:
    """Copy a non-HTML asset verbatim."""
    os.makedirs(os.path.dirname(dst_path), exist_ok=True)
    shutil.copy2(src_path, dst_path)


def main() -> None:
    if not os.path.isdir(WEBSITE_DIR):
        print(f"ERROR: website tree not found at {WEBSITE_DIR}", file=sys.stderr)
        print(
            "Set WEBSITE_DIR to the public repository's website/ checkout "
            "(We-Amp/pagespeed-optimizer).",
            file=sys.stderr,
        )
        sys.exit(1)
    if not os.path.isdir(INPUT_DIR):
        print(f"ERROR: input directory not found: {INPUT_DIR}", file=sys.stderr)
        sys.exit(1)

    # Idempotent: remove previous output.
    if os.path.exists(OUTPUT_DIR):
        print(f"Cleaning output directory: {OUTPUT_DIR}")
        shutil.rmtree(OUTPUT_DIR)
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    html_count = 0
    asset_count = 0
    skipped_count = 0

    for dirpath, _dirnames, filenames in os.walk(INPUT_DIR):
        rel_dir = os.path.relpath(dirpath, INPUT_DIR)
        dst_dir = os.path.join(OUTPUT_DIR, rel_dir) if rel_dir != "." else OUTPUT_DIR

        for filename in sorted(filenames):
            src_path = os.path.join(dirpath, filename)
            dst_path = os.path.join(dst_dir, filename)

            if filename == ".htaccess":
                # Skip Apache config files -- not needed for static hosting.
                skipped_count += 1
                continue

            if is_html(filename):
                if is_fragment(filename):
                    # SSI fragments are inlined into other files; skip them.
                    skipped_count += 1
                    continue
                process_html(src_path, dst_path)
                html_count += 1
            else:
                copy_asset(src_path, dst_path)
                asset_count += 1

    print(f"Done.  HTML: {html_count}  Assets: {asset_count}  Skipped: {skipped_count}")
    print(f"Output: {OUTPUT_DIR}")


if __name__ == "__main__":
    main()
