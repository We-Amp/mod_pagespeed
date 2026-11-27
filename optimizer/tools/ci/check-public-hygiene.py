#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Second half of the public-tree hygiene gate (BLOCKING).

Run by tools/ci/check-public-hygiene.sh after its regex rules. Two checks that
a line-oriented regex cannot express without spelling out non-public names:

  1. Hashed tokens (.github/hygiene-hashed-tokens.txt). Every tracked text
     file is tokenised; each token is lowercased, sha256-hashed and compared
     with the listed hashes. The blocked literals are never written in this
     tree. Tokens are path-run based (backslashes count as "/", a trailing
     ".git" is stripped from a segment): windows of 1-2 pieces joined by "-"
     inside each segment, where pieces are split on any of "-", "_" and ".";
     plus windows of 1-3 consecutive raw segments joined by "/".
  2. Relative markdown links. In every tracked *.md / *.mdx file (fenced code
     blocks and inline code spans skipped; inline, balanced-paren and
     reference-style targets) a relative link target must resolve to a tracked
     file or to a directory holding a tracked file. URLs, mailto:,
     fragment-only and root-relative targets are not checked.

Usage: check-public-hygiene.py [--root DIR] [--tokens FILE]
  DIR defaults to the current directory; FILE defaults to the tracked token
  list (the self-test passes a temporary file of made-up tokens).
"""
import hashlib
import os
import posixpath
import re
import subprocess
import sys

TOKENS = ".github/hygiene-hashed-tokens.txt"
EXCLUDE = {
    TOKENS,
    ".github/hygiene-rules.txt",
    # Byte-frozen capture; see check-public-hygiene.sh.
    "tools/async-css-probe/fixtures/modpagespeed-com/index.html",
}
RUN = re.compile(r"[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*")
REFDEF = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*(<[^>]*>|\S+)")
CODESPAN = re.compile(r"`+[^`]*`+")


def link_targets(line):
    """Inline ](target) with balanced parens, plus reference definitions."""
    line = CODESPAN.sub("", line)
    m = REFDEF.match(line)
    if m:
        yield m.group(1)
    i = 0
    while True:
        i = line.find("](", i)
        if i < 0:
            return
        j, depth = i + 2, 1
        while j < len(line) and depth:
            depth += {"(": 1, ")": -1}.get(line[j], 0)
            j += 1
        if depth == 0:
            yield line[i + 2:j - 1]
        i = j


def sha(s):
    return hashlib.sha256(s.lower().encode()).hexdigest()


def tokens_of(text):
    for run in RUN.findall(text.replace("\\", "/")):
        segs = [s.strip(".") for s in run.split("/")]
        segs = [s[:-4] if s.lower().endswith(".git") else s for s in segs]
        segs = [s for s in segs if s]
        for i in range(len(segs)):
            for n in (1, 2, 3):
                if i + n <= len(segs):
                    yield "/".join(segs[i:i + n])
        for s in segs:
            pieces = [p for p in re.split(r"[-_.]", s) if p]
            for i in range(len(pieces)):
                for n in (1, 2):
                    if i + n <= len(pieces):
                        yield "-".join(pieces[i:i + n])


def main():
    root = os.getcwd()
    tokens = TOKENS
    args = sys.argv[1:]
    while args:
        if len(args) >= 2 and args[0] == "--root":
            root = args[1]
        elif len(args) >= 2 and args[0] == "--tokens":
            tokens = os.path.abspath(args[1])
        else:
            sys.exit("usage: check-public-hygiene.py [--root DIR] [--tokens FILE]")
        args = args[2:]
    os.chdir(root)
    files = subprocess.run(
        ["git", "ls-files", "-z"], check=True, capture_output=True
    ).stdout.decode().split("\0")
    files = [f for f in files if f and os.path.isfile(f) and not os.path.islink(f)]
    tracked = set(files)
    dirs = {posixpath.dirname(f) for f in files}
    for d in list(dirs):
        while d:
            d = posixpath.dirname(d)
            dirs.add(d)

    hashes = {}
    with open(tokens) as fh:
        for ln, line in enumerate(fh, 1):
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 4 or parts[3] not in ("block", "advisory") or not re.fullmatch(r"[0-9a-f]{64}", parts[2]) or parts[1] not in ("all", "md"):
                sys.exit("::error::%s:%d: malformed line" % (tokens, ln))
            hashes[parts[2]] = (parts[0], parts[1], parts[3])

    bad = 0
    for f in files:
        if f in EXCLUDE:
            continue
        with open(f, "rb") as fh:
            data = fh.read()
        if b"\0" in data:
            continue
        text = data.decode("utf-8", "replace")
        is_md = f.endswith((".md", ".mdx"))
        lines = text.split("\n")
        # 1. hashed tokens, per line so a hit reports path:line
        for i, line in enumerate(lines, 1):
            for tok in set(tokens_of(line)):
                hit = hashes.get(sha(tok))
                if hit and (hit[1] == "all" or is_md):
                    if hit[2] == "advisory":
                        print("::warning::hygiene hashed-token class %s (advisory): %s:%d (token sha256 %s)"
                              % (hit[0], f, i, sha(tok)), file=sys.stderr)
                        continue
                    print("::error::hygiene hashed-token class %s: %s:%d (token sha256 %s)"
                          % (hit[0], f, i, sha(tok)), file=sys.stderr)
                    bad += 1
        # 2. relative markdown links
        if is_md:
            fenced = False
            for i, line in enumerate(lines, 1):
                st = line.lstrip()
                if st.startswith("```") or st.startswith("~~~"):
                    fenced = not fenced
                    continue
                if fenced:
                    continue
                for tgt in link_targets(line):
                    t = re.split(r"[#?]", tgt, 1)[0]
                    if t.startswith("<") and t.endswith(">"):
                        t = t[1:-1]
                    else:
                        t = t.split()[0] if t.split() else ""
                    if not t or re.match(r"([a-zA-Z][a-zA-Z0-9+.-]*:|//|/)", t):
                        continue
                    p = posixpath.normpath(posixpath.join(posixpath.dirname(f), t))
                    if p.startswith("..") or p in tracked or p in dirs:
                        if p.startswith(".."):
                            print("::error::dead relative link (escapes repo): %s:%d -> %s" % (f, i, tgt), file=sys.stderr)
                            bad += 1
                        continue
                    print("::error::dead relative link: %s:%d -> %s" % (f, i, p), file=sys.stderr)
                    bad += 1
    if bad:
        print("::error::public-tree hygiene (hashed tokens / links) FAILED: %d hit(s)" % bad, file=sys.stderr)
        return 1
    print("Public-tree hygiene (hashed tokens, markdown links) passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
