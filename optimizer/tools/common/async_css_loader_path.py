# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Recompute the async-CSS loader's content-addressed path from the C++ source.

The worker serves the loader at ``/pagespeed_static/async_css.<hash>.js``, where
the hash is an FNV-1a 32 computed at C++ compile time over ``kAsyncCssLoaderJs``
(``src/worker/async_css_loader.h``). Harnesses must not hardcode that path: the
hash rotates whenever the loader body is edited, and a hardcoded copy would go
stale silently.

Reading the in-tree header — the single source of truth the compiler reads — and
recomputing the hash here means a drift can only fail LOUD, as a 404 at a path
nobody serves. That is the built-in alarm for a partial deploy.

Two harnesses need this: ``tools/http-compliance`` (whose worker has no
critical-CSS pass, so the loader is never injected into a page to scrape the
path from, yet is still served unconditionally at preaccess) and
``tools/async-css-probe`` (which must check that the path it scrapes out of the
deferred markup is the path the worker actually serves). It lives here so there
is exactly one recompute to keep correct.
"""

from __future__ import annotations

import os
import re

_REPO_ROOT = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
)
ASYNC_CSS_LOADER_HEADER = os.path.join(
    _REPO_ROOT, "src", "worker", "async_css_loader.h"
)


def async_css_loader_js_bytes(header_path: str = ASYNC_CSS_LOADER_HEADER) -> bytes:
    """The exact loader bytes the C++ compiler hashes."""
    with open(header_path, encoding="utf-8") as f:
        text = f.read()
    # Capture the whole `kAsyncCssLoaderJs = "..." "..." ...;` initializer up to
    # the final closing quote before the statement `;` (the body itself contains
    # `;`, so we must not stop at the first one).
    m = re.search(r'kAsyncCssLoaderJs\s*=\s*(.*?")\s*;', text, re.S)
    assert m, f"kAsyncCssLoaderJs not found in {header_path}"
    chunks = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
    body = "".join(chunks)
    # Fail LOUD and clearly if the parse extracted nothing (e.g. a future loader
    # edit defeats the extraction): otherwise we would hash the empty string to
    # 811c9dc5 and GET a confusing 404 with no hint at the real cause.
    assert body, (
        f"Failed to extract kAsyncCssLoaderJs from {header_path} "
        "(empty parse) — the loader source/format may have changed."
    )
    # The loader body is pure ASCII with no C escape sequences today; decode any
    # that ever appear so the bytes match what the C++ compiler hashes.
    return body.encode("latin-1").decode("unicode_escape").encode("latin-1")


def fnv1a32(data: bytes) -> int:
    h = 0x811C9DC5  # FNV offset basis
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF  # FNV prime, mod 2^32
    return h


def async_css_loader_path_from_source(
    header_path: str = ASYNC_CSS_LOADER_HEADER,
) -> str:
    """Content-addressed loader path, recomputed from the worker header."""
    return f"/pagespeed_static/async_css.{fnv1a32(async_css_loader_js_bytes(header_path)):08x}.js"
