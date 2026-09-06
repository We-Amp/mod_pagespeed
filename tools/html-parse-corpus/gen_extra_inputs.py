#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
"""Deterministic generated inputs for the HTML parse corpus.

The hand-written battery lives in seeds/ (checked in). This script produces
the inputs that are too large to check in comfortably, byte-stably, into the
directory named by --dest:

  large-doc.html          ~1.2MB mixed document; comfortably under the
                          lexer's unconditional 10MB token ceiling
                          (kMaxTokenSize), exercising volume without
                          tripping the limit. Status: ok.
  oversize-comment.html   a single comment token over 10MB: trips
                          size_limit_exceeded mid-token. Status: size-limit.
  oversize-chars.html     a single characters run over 10MB: same trip via
                          the literal buffer. Status: size-limit.

Deterministic: no timestamps, no randomness; same version -> same bytes.
The goldens manifest pins this script's sha256.

Stdlib only.
"""

import argparse
import os

TEN_MB = 10 * 1024 * 1024


def large_doc():
    chunks = []
    size = 0
    i = 0
    while size < 1024 * 1024:
        chunk = ('<div class="row-%d" data-x="a&amp;b"><p>text %d '
                 '<a href="/p?id=%d">link</a></p><!-- c%d --></div>\n'
                 % (i, i, i, i))
        chunks.append(chunk)
        size += len(chunk)
        i += 1
    return ("<!DOCTYPE html>\n<html><body>\n" + "".join(chunks) +
            "</body></html>\n").encode("ascii")


def oversize_comment():
    return b"<!-- " + b"x" * (TEN_MB + 1024) + b" -->\n<p>after</p>\n"


def oversize_chars():
    return b"<div>" + b"y" * (TEN_MB + 1024) + b"</div>\n"


GENERATED = {
    "large-doc.html": large_doc,
    "oversize-comment.html": oversize_comment,
    "oversize-chars.html": oversize_chars,
}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dest", required=True,
                   help="directory to write the generated inputs into")
    args = p.parse_args()
    os.makedirs(args.dest, exist_ok=True)
    for name, fn in sorted(GENERATED.items()):
        path = os.path.join(args.dest, name)
        with open(path, "wb") as f:
            f.write(fn())
        print("wrote %s (%d bytes)" % (path, os.path.getsize(path)))


if __name__ == "__main__":
    main()
