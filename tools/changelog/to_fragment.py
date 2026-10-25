#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Move a branch's CHANGELOG.md edit into a changelog.d/ fragment.

A branch that added its entry directly under `[Unreleased]` conflicts with
every other such branch. This tool turns that edit into fragments and puts
CHANGELOG.md back to the merge-base content, so the branch rebases cleanly.
Nothing is committed; review the printed files and commit them yourself.

    tools/changelog/to_fragment.py [PR] [--base origin/master] [--slug S]
    tools/changelog/to_fragment.py --self-test

PR (optional) is a pull request number; the slug is then derived from its head
branch name via `gh pr view`. Without it the slug comes from the current
branch. Exit 2 means the edit cannot be converted mechanically (it removes
lines, or touches text outside the Unreleased section): convert it by hand.
"""

import importlib.util
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "assemble", os.path.join(HERE, "assemble.py"))
asm = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(asm)

SLUG_RE = re.compile(r"^[a-z0-9][a-z0-9-]{2,62}$")
HUNK_RE = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


class Refuse(Exception):
    """The edit needs a human."""


def git(root, *args):
    r = subprocess.run(["git"] + list(args), cwd=root, capture_output=True,
                       text=True)
    if r.returncode != 0:
        raise RuntimeError("git %s failed: %s" % (" ".join(args), r.stderr.strip()))
    return r.stdout


def diff_hunks(diff_text):
    """Parse `git diff -U0` text. Returns (added_line_numbers, removed_count)."""
    added, removed, new_no = [], 0, 0
    for line in diff_text.split("\n"):
        m = HUNK_RE.match(line)
        if m:
            new_no = int(m.group(3))
            continue
        if line.startswith("+++") or line.startswith("---"):
            continue
        if line.startswith("-"):
            removed += 1
        elif line.startswith("+"):
            added.append(new_no)
            new_no += 1
    return added, removed


def category_for_line(lines, section_start, lineno, mode):
    """Category infix for 1-based `lineno`, from the nearest ### above it."""
    if mode == "bullets":
        for n in range(lineno - 1, section_start, -1):
            if lines[n].startswith("### "):
                title = lines[n][4:].strip()
                if title in asm.TITLE_TO_INFIX:
                    return asm.TITLE_TO_INFIX[title]
                raise Refuse("unknown category heading '%s'" % title)
        raise Refuse("an added line has no '###' category heading above it")
    return None


def plan_fragments(new_lines, added, mode):
    """Group added lines into {category: [lines]}. Raises Refuse."""
    heading = (asm.BRACKET_UNRELEASED if mode == "bullets"
               else asm.PLAIN_UNRELEASED)
    sec = asm.find_section(new_lines, heading)
    if sec is None:
        raise Refuse("no '%s' section in CHANGELOG.md" % heading)
    start, end = sec
    groups = {}
    order = []
    for lineno in added:
        idx = lineno - 1
        if not (start < idx < end):
            raise Refuse("line %d is outside the Unreleased section" % lineno)
        text = new_lines[idx]
        if text.startswith("### "):
            continue
        if mode == "bullets":
            cat = category_for_line(new_lines, start, lineno, mode)
            groups.setdefault(cat, []).append(text)
            if cat not in order:
                order.append(cat)
        else:
            groups.setdefault(None, []).append(text)
    if mode == "paragraphs":
        paras, cur = [], []
        for text in groups.get(None, []) + [""]:
            if text.strip():
                cur.append(text)
            elif cur:
                paras.append(cur)
                cur = []
        groups = {}
        for p in paras:
            m = re.match(r"^([A-Za-z ]+): ", p[0])
            cat = asm.TITLE_TO_INFIX.get(m.group(1)) if m else None
            if cat is None:
                raise Refuse("paragraph does not start with a known 'Category: '")
            if cat in groups:
                raise Refuse("two added paragraphs for category '%s'" % cat)
            groups[cat] = p
    result = {}
    for cat, body in groups.items():
        while body and not body[0].strip():
            body = body[1:]
        while body and not body[-1].strip():
            body = body[:-1]
        if any(not b.strip() for b in body):
            raise Refuse("a blank line inside the '%s' entry" % cat)
        if body:
            result[cat] = body
    return result


def convert(root, base, slug):
    mb = git(root, "merge-base", base, "HEAD").strip()
    diff = git(root, "diff", "-U0", mb, "HEAD", "--", "CHANGELOG.md")
    if not diff.strip():
        return []
    added, removed = diff_hunks(diff)
    if removed:
        raise Refuse("the edit removes or rewrites existing lines")
    new_lines = git(root, "show", "HEAD:CHANGELOG.md").split("\n")
    mode = asm.detect_mode(new_lines)
    plan = plan_fragments(new_lines, added, mode)
    if not plan:
        return []
    if not SLUG_RE.match(slug):
        raise Refuse("slug '%s' is not <3-63 chars of a-z 0-9 ->; pass --slug" % slug)
    outdir = os.path.join(root, asm.FRAGMENT_DIR)
    targets = {}
    for cat, body in plan.items():
        name = "%s.%s.md" % (slug, cat)
        problems = asm.lint_content(name, "\n".join(body) + "\n", cat, mode)
        if problems:
            raise Refuse("the edit does not make a valid fragment: %s"
                         % "; ".join(problems))
    for cat, body in plan.items():
        path = os.path.join(outdir, "%s.%s.md" % (slug, cat))
        if os.path.exists(path):
            raise Refuse("%s already exists; pass --slug" % os.path.relpath(path, root))
        targets[path] = "\n".join(body) + "\n"
    os.makedirs(outdir, exist_ok=True)
    for path, text in targets.items():
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(text)
    restore_base(root, mb)
    return sorted(targets)


def restore_base(root, merge_base):
    content = git(root, "show", "%s:CHANGELOG.md" % merge_base)
    with open(os.path.join(root, "CHANGELOG.md"), "w", encoding="utf-8",
              newline="") as f:
        f.write(content)


def slug_from(root, pr):
    if pr:
        r = subprocess.run(["gh", "pr", "view", pr, "--json", "headRefName",
                            "--jq", ".headRefName"], cwd=root,
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise Refuse("gh pr view %s failed: %s" % (pr, r.stderr.strip()))
        branch = r.stdout.strip()
    else:
        branch = git(root, "branch", "--show-current").strip()
    return re.sub(r"[^a-z0-9-]+", "-", branch.lower().replace("/", "-")).strip("-")


def self_test():
    ok = True

    def check(name, cond):
        nonlocal ok
        print("  %-52s %s" % (name, "ok" if cond else "FAIL"))
        ok = ok and cond

    def repo(td, base_text, branch_text):
        def g(*a):
            subprocess.run(["git"] + list(a), cwd=td, check=True,
                           capture_output=True)
        g("init", "-q", "-b", "master")
        g("config", "user.email", "t@example.invalid")
        g("config", "user.name", "t")
        write(td, base_text)
        g("add", "-A")
        g("commit", "-q", "-m", "base")
        g("checkout", "-q", "-b", "feature/x-thing")
        write(td, branch_text)
        g("commit", "-qam", "edit")

    def write(td, text):
        with open(os.path.join(td, "CHANGELOG.md"), "w") as f:
            f.write(text)

    base = ("# Changelog\n\n## [Unreleased]\n\n### Added\n\n- old one\n\n"
            "## [1.0.0] - 2026-01-01\n\n### Fixed\n\n- ancient\n")
    edit = ("# Changelog\n\n## [Unreleased]\n\n### Added\n\n- old one\n"
            "- new one\n  wrapped\n\n### Fixed\n\n- fresh fix\n\n"
            "## [1.0.0] - 2026-01-01\n\n### Fixed\n\n- ancient\n")
    with tempfile.TemporaryDirectory() as td:
        repo(td, base, edit)
        files = convert(td, "master", slug_from(td, None))
        names = [os.path.basename(f) for f in files]
        check("slug from branch, one file per category",
              names == ["feature-x-thing.added.md", "feature-x-thing.fixed.md"])
        with open(files[0]) as f:
            check("bullet text kept exactly", f.read() == "- new one\n  wrapped\n")
        with open(os.path.join(td, "CHANGELOG.md")) as f:
            check("CHANGELOG.md restored to merge-base", f.read() == base)
        try:
            convert(td, "master", "feature-x-thing")
            check("existing fragment is not overwritten", False)
        except Refuse:
            check("existing fragment is not overwritten", True)
    with tempfile.TemporaryDirectory() as td:
        repo(td, base, base.replace("- old one", "- reworded one"))
        try:
            convert(td, "master", "some-slug")
            check("an edit that rewrites a line is refused", False)
        except Refuse:
            check("an edit that rewrites a line is refused", True)
    with tempfile.TemporaryDirectory() as td:
        repo(td, base, base + "\n- stray\n")
        try:
            convert(td, "master", "some-slug")
            check("an addition outside Unreleased is refused", False)
        except Refuse:
            check("an addition outside Unreleased is refused", True)
    with tempfile.TemporaryDirectory() as td:
        repo(td, base, base.replace("- old one", "- old one\n  more words"))
        try:
            convert(td, "master", "some-slug")
            check("a continuation-only edit is refused", False)
        except Refuse:
            with open(os.path.join(td, "CHANGELOG.md")) as f:
                check("a continuation-only edit is refused, file kept",
                      f.read().count("more words") == 1
                      and not os.path.exists(os.path.join(td, "changelog.d")))
    with tempfile.TemporaryDirectory() as td:
        pbase = "# Changelog\n\n## Unreleased\n\nFixed: a.\n\n## 1.0 - 2026-01-01\n\nFixed: b.\n"
        pedit = ("# Changelog\n\n## Unreleased\n\nFixed: a.\n\n"
                 "Changed: new text\nwrapped.\n\n## 1.0 - 2026-01-01\n\nFixed: b.\n")
        repo(td, pbase, pedit)
        files = convert(td, "master", "para-slug")
        check("paragraph layout converts by prefix",
              [os.path.basename(f) for f in files] == ["para-slug.changed.md"])
    print("OK: to_fragment self-test passed." if ok else "FAIL: to_fragment self-test.")
    return 0 if ok else 1


def main(argv):
    args, pr, base, slug = argv[1:], None, "origin/master", None
    if args == ["--self-test"]:
        return self_test()
    while args:
        a = args.pop(0)
        if a == "--base" and args:
            base = args.pop(0)
        elif a == "--slug" and args:
            slug = args.pop(0)
        elif a.isdigit() and pr is None:
            pr = a
        else:
            print(__doc__, file=sys.stderr)
            return 2
    root = os.path.abspath(os.path.join(HERE, "..", ".."))
    try:
        if slug is None:
            slug = slug_from(root, pr)
        files = convert(root, base, slug)
    except Refuse as exc:
        print("REFUSED: %s -- convert by hand." % exc, file=sys.stderr)
        return 2
    except RuntimeError as exc:
        print("FAILED: %s" % exc, file=sys.stderr)
        return 1
    if not files:
        print("CHANGELOG.md has no changes against %s; nothing to do." % base)
    for f in files:
        print(os.path.relpath(f, root))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
