#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Fold the per-PR changelog fragments in changelog.d/ into CHANGELOG.md.

Every pull request that needs a changelog entry adds one small file,
`changelog.d/<slug>.<category>.md`, instead of editing the shared
`[Unreleased]` section (which conflicts on every merge). This tool is run in
the release cut: it appends each fragment to its category in the Unreleased
section, then deletes the fragments. See changelog.d/README.md.

Two layouts are handled, chosen from the heading the file already uses:

  bullets     `## [Unreleased]` with `### Category` blocks of `- ` bullets
  paragraphs  `## Unreleased` with prose paragraphs led by `Category: `

Usage:

    tools/changelog/assemble.py [--into HEADING] [--repo-root DIR]
                                [--preview | --lint] [--no-git]
    tools/changelog/assemble.py --self-test

Run it with `python3 -I`. Standard library only.

    --into HEADING  fold into an existing released heading, for example
                    '## [2.6.0] - 2026-11-02', instead of Unreleased
    --preview       print the assembled section; write nothing
    --lint          check fragment names and content only
    --no-git        delete fragments with unlink and ignore git for ordering
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

# Canonical category order, and the heading each infix maps to.
CATEGORIES = [
    ("security", "Security"),
    ("added", "Added"),
    ("changed", "Changed"),
    ("deprecated", "Deprecated"),
    ("fixed", "Fixed"),
    ("removed", "Removed"),
    ("documentation", "Documentation"),
    ("upgrade-notes", "Upgrade Notes"),
    ("known-issues", "Known Issues"),
]
INFIX_TO_TITLE = dict(CATEGORIES)
TITLE_TO_INFIX = {t: i for i, t in CATEGORIES}
TITLE_RANK = {t: n for n, (_, t) in enumerate(CATEGORIES)}

FRAGMENT_DIR = "changelog.d"
NAME_RE = re.compile(
    r"^([a-z0-9][a-z0-9-]{2,62})\.(%s)\.md$"
    % "|".join(re.escape(i) for i, _ in CATEGORIES))
BRACKET_UNRELEASED = "## [Unreleased]"
PLAIN_UNRELEASED = "## Unreleased"


class AssembleError(Exception):
    pass


# --- parsing ---------------------------------------------------------------

def is_h2(line):
    return line.startswith("## ")


def detect_mode(lines):
    """'bullets' or 'paragraphs', from the heading form the file uses."""
    for line in lines:
        if line.rstrip() == BRACKET_UNRELEASED:
            return "bullets"
        if line.rstrip() == PLAIN_UNRELEASED:
            return "paragraphs"
    for line in lines:
        if is_h2(line):
            return "bullets" if line.startswith("## [") else "paragraphs"
    return "bullets"


def find_section(lines, heading):
    """(start, end) of the section whose heading line is `heading`, or None.

    `end` is the index of the next `## ` heading, or len(lines)."""
    for n, line in enumerate(lines):
        if line.rstrip() == heading:
            end = len(lines)
            for m in range(n + 1, len(lines)):
                if is_h2(lines[m]):
                    end = m
                    break
            return n, end
    return None


def ensure_section(lines, heading):
    """Create `heading` right after the preamble when it is absent."""
    if find_section(lines, heading) is not None:
        return
    at = len(lines)
    for n, line in enumerate(lines):
        if is_h2(line):
            at = n
            break
    new = [heading, ""]
    if at == len(lines) and lines and lines[-1].strip():
        new = [""] + new
    lines[at:at] = new


def last_nonblank_end(lines, start, end):
    """Index just past the last non-blank line in [start, end)."""
    idx = end
    while idx > start and not lines[idx - 1].strip():
        idx -= 1
    return idx


# --- fragments -------------------------------------------------------------

def list_fragment_names(root):
    d = os.path.join(root, FRAGMENT_DIR)
    if not os.path.isdir(d):
        return []
    return sorted(n for n in os.listdir(d)
                  if n.endswith(".md") and n != "README.md"
                  and os.path.isfile(os.path.join(d, n)))


def lint_content(name, text, category, mode):
    """Return a list of problems with one fragment's text."""
    errs = []
    if "\r" in text:
        return ["%s: carriage returns; use LF line endings" % name]
    if not text:
        return ["%s: empty" % name]
    if not text.endswith("\n"):
        errs.append("%s: must end with a newline" % name)
    elif text.endswith("\n\n"):
        errs.append("%s: ends with more than one newline" % name)
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines = lines[:-1]
    for n, line in enumerate(lines, 1):
        where = "%s:%d" % (name, n)
        if not line.strip():
            errs.append("%s: blank line (one entry per fragment, no blank lines)"
                        % where)
            continue
        if line != line.rstrip():
            errs.append("%s: trailing whitespace" % where)
        if line.startswith("#"):
            errs.append("%s: heading line; the category comes from the file name"
                        % where)
        if mode == "bullets":
            if n == 1 and not line.startswith("- "):
                errs.append("%s: first line must start with '- '" % where)
            elif not (line.startswith("- ") or line.startswith("  ")):
                errs.append("%s: continuation lines are indented two spaces"
                            % where)
    if mode == "paragraphs" and lines:
        want = INFIX_TO_TITLE[category] + ": "
        if not lines[0].startswith(want):
            errs.append("%s: must start with '%s'" % (name, want))
    return errs


def load_fragments(root, mode):
    """Read and lint every fragment. Returns [(name, category, text)]."""
    errs, frags = [], []
    for name in list_fragment_names(root):
        m = NAME_RE.match(name)
        if not m:
            errs.append(
                "%s: bad name; use <slug>.<category>.md with category one of: %s"
                % (name, ", ".join(i for i, _ in CATEGORIES)))
            continue
        with open(os.path.join(root, FRAGMENT_DIR, name), encoding="utf-8",
                  newline="") as f:
            text = f.read()
        problems = lint_content(name, text, m.group(2), mode)
        if problems:
            errs.extend(problems)
            continue
        frags.append((name, m.group(2), text))
    if errs:
        raise AssembleError("\n".join(errs))
    return frags


def added_time(root, name, use_git):
    """Commit time that added the fragment; untracked sorts last."""
    if use_git:
        try:
            out = subprocess.run(
                ["git", "log", "--diff-filter=A", "-1", "--format=%ct", "--",
                 FRAGMENT_DIR + "/" + name],
                cwd=root, capture_output=True, text=True, check=True).stdout
            if out.strip():
                return int(out.strip())
        except (OSError, subprocess.CalledProcessError, ValueError):
            pass
    return sys.maxsize if use_git else 0


def fragment_order(root, frags, use_git):
    return sorted(frags, key=lambda f: (added_time(root, f[0], use_git), f[0]))


# --- insertion -------------------------------------------------------------

def h3_blocks(lines, start, end):
    """[(title, heading_index, block_end)] for ### blocks inside the section."""
    heads = [n for n in range(start, end) if lines[n].startswith("### ")]
    out = []
    for k, n in enumerate(heads):
        nxt = heads[k + 1] if k + 1 < len(heads) else end
        out.append((lines[n][4:].strip(), n, nxt))
    return out


def insert_bullets(lines, heading, category, body):
    title = INFIX_TO_TITLE[category]
    start, end = find_section(lines, heading)
    blocks = h3_blocks(lines, start, end)
    for t, n, bend in blocks:
        if t == title:
            at = last_nonblank_end(lines, n + 1, bend)
            if at == n + 1:
                body = [""] + body
            lines[at:at] = body
            return
    for t, n, bend in blocks:
        if t in TITLE_RANK and TITLE_RANK[t] > TITLE_RANK[title]:
            lines[n:n] = ["### " + title, ""] + body + [""]
            return
    at = last_nonblank_end(lines, start + 1, end)
    new = ["", "### " + title, ""] + body
    lines[at:at] = new
    after = at + len(new)
    if after < len(lines) and lines[after].strip():
        lines.insert(after, "")


def paragraphs(lines, start, end):
    """[(first, last_exclusive, title_or_None)] for the section's paragraphs."""
    out, n = [], start + 1
    while n < end:
        if not lines[n].strip():
            n += 1
            continue
        first = n
        while n < end and lines[n].strip():
            n += 1
        title = None
        for t in TITLE_RANK:
            if lines[first].startswith(t + ": "):
                title = t
        out.append((first, n, title))
    return out


def insert_paragraphs(lines, heading, category, body):
    title = INFIX_TO_TITLE[category]
    start, end = find_section(lines, heading)
    paras = paragraphs(lines, start, end)
    same = [p for p in paras if p[2] == title]
    if same:
        at = same[-1][1]
        lines[at:at] = [""] + body
        return
    for first, _, t in paras:
        if t is not None and TITLE_RANK[t] > TITLE_RANK[title]:
            lines[first:first] = body + [""]
            return
    at = last_nonblank_end(lines, start + 1, end)
    new = [""] + body
    lines[at:at] = new
    after = at + len(new)
    if after < len(lines) and lines[after].strip():
        lines.insert(after, "")


def assemble_lines(lines, heading, frags, mode):
    """Fold frags into lines in place. Returns the names already present."""
    ensure_section(lines, heading)
    skipped = []
    for name, category, text in frags:
        start, end = find_section(lines, heading)
        section = "\n".join(lines[start:end]) + "\n"
        # Match at a line start: an entry that is merely the tail of an
        # existing line is a different entry.
        if ("\n" + text) in ("\n" + section):
            skipped.append(name)
            continue
        body = text.split("\n")[:-1]
        if mode == "bullets":
            insert_bullets(lines, heading, category, body)
        else:
            insert_paragraphs(lines, heading, category, body)
    return skipped


# --- driver ----------------------------------------------------------------

def write_atomic(path, text):
    d = os.path.dirname(path) or "."
    fd, tmp = tempfile.mkstemp(prefix=".changelog.", dir=d)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        if os.path.exists(path):
            shutil.copymode(path, tmp)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def remove_fragment(root, name, use_git):
    rel = FRAGMENT_DIR + "/" + name
    if use_git:
        r = subprocess.run(["git", "rm", "-q", "-f", "--", rel], cwd=root,
                           capture_output=True)
        if r.returncode == 0:
            return
    path = os.path.join(root, rel)
    if os.path.exists(path):
        os.unlink(path)


def read_changelog(root):
    path = os.path.join(root, "CHANGELOG.md")
    if not os.path.exists(path):
        return path, []
    with open(path, encoding="utf-8", newline="") as f:
        return path, f.read().split("\n")


def run(root, into=None, preview=False, lint=False, use_git=True, out=None):
    out = out or sys.stdout
    path, lines = read_changelog(root)
    if into:
        if find_section(lines, into) is None:
            raise AssembleError("no section '%s' in CHANGELOG.md" % into)
        mode = "bullets" if into.startswith("## [") else "paragraphs"
        heading = into
    else:
        mode = detect_mode(lines)
        heading = BRACKET_UNRELEASED if mode == "bullets" else PLAIN_UNRELEASED
    frags = load_fragments(root, mode)
    if lint:
        out.write("OK: %d fragment(s) pass the lint.\n" % len(frags))
        return 0
    if not frags:
        notice = "No fragments in %s/; nothing to do.\n" % FRAGMENT_DIR
        if preview:
            sec = find_section(lines, heading)
            sys.stderr.write(notice)
            if sec:
                out.write("\n".join(lines[sec[0]:sec[1]]).rstrip("\n") + "\n")
        else:
            out.write(notice)
        return 0
    frags = fragment_order(root, frags, use_git)
    new = list(lines)
    skipped = assemble_lines(new, heading, frags, mode)
    if preview:
        start, end = find_section(new, heading)
        out.write("\n".join(new[start:end]).rstrip("\n") + "\n")
        return 0
    if new != lines:
        write_atomic(path, "\n".join(new))
    for name, _, _ in frags:
        remove_fragment(root, name, use_git)
    out.write("Folded %d fragment(s) into '%s'%s.\n" % (
        len(frags) - len(skipped), heading,
        (" (%d already present, removed)" % len(skipped)) if skipped else ""))
    return 0


def self_test():
    here = os.path.dirname(os.path.abspath(__file__))
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "test_assemble", os.path.join(here, "test_assemble.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.run_all()


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    into, root, preview, lint, use_git = None, None, False, False, True
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--into" and args:
            into = args.pop(0)
        elif a == "--repo-root" and args:
            root = args.pop(0)
        elif a == "--preview":
            preview = True
        elif a == "--lint":
            lint = True
        elif a == "--no-git":
            use_git = False
        else:
            print(__doc__, file=sys.stderr)
            return 2
    if root is None:
        root = os.path.abspath(os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    try:
        return run(root, into, preview, lint, use_git)
    except AssembleError as exc:
        print("FAILED:\n%s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
