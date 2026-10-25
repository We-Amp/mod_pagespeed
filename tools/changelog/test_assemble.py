# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Tests for tools/changelog/assemble.py.

Plain functions with no fixtures, so they run under pytest and also from
`python3 -I tools/changelog/assemble.py --self-test`.
"""

import importlib.util
import io
import os
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))


def _load(name):
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


asm = _load("assemble")
tof = _load("to_fragment")

BASE = """# Changelog

Preamble.

## [Unreleased]

### Added

- first added

### Fixed

- first fix

## [1.0.0] - 2026-01-01

### Added

- ancient
"""


def make_root(changelog=BASE, fragments=None):
    td = tempfile.mkdtemp()
    with open(os.path.join(td, "CHANGELOG.md"), "w") as f:
        f.write(changelog)
    os.makedirs(os.path.join(td, "changelog.d"))
    for name, text in (fragments or {}).items():
        with open(os.path.join(td, "changelog.d", name), "w") as f:
            f.write(text)
    return td


def read(td):
    with open(os.path.join(td, "CHANGELOG.md")) as f:
        return f.read()


def run(td, **kw):
    kw.setdefault("use_git", False)
    out = io.StringIO()
    try:
        return asm.run(td, out=out, **kw), out.getvalue()
    except asm.AssembleError as exc:
        return 1, str(exc)


def remaining(td):
    return asm.list_fragment_names(td)


def test_groups_into_existing_categories():
    td = make_root(fragments={
        "aaa-one.added.md": "- second added\n  wrapped\n",
        "bbb-two.fixed.md": "- second fix\n"})
    rc, _ = run(td)
    want = BASE.replace("- first added\n", "- first added\n- second added\n  wrapped\n") \
               .replace("- first fix\n", "- first fix\n- second fix\n")
    assert rc == 0 and read(td) == want and remaining(td) == []


def test_new_category_goes_in_canonical_position():
    td = make_root(fragments={
        "sec-fix.security.md": "- **Update recommended.** fixes it\n",
        "chg-it.changed.md": "- changed it\n",
        "rm-it.removed.md": "- removed it\n"})
    rc, _ = run(td)
    got = read(td)
    order = [l for l in got.split("## [1.0.0]")[0].split("\n") if l.startswith("### ")]
    assert rc == 0
    assert order == ["### Security", "### Added", "### Changed", "### Fixed",
                     "### Removed"], order
    assert "### Changed\n\n- changed it\n\n### Fixed" in got
    assert "- first fix\n\n### Removed\n\n- removed it\n\n## [1.0.0]" in got


def test_new_last_category_appended_with_layout():
    td = make_root(fragments={"kn-it.known-issues.md": "- known thing\n"})
    rc, _ = run(td)
    assert rc == 0
    assert "- first fix\n\n### Known Issues\n\n- known thing\n\n## [1.0.0]" in read(td)


def test_order_by_git_add_time_then_name():
    td = make_root()
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")

    def g(*a, when=None):
        e = dict(env)
        if when:
            e["GIT_COMMITTER_DATE"] = when
            e["GIT_AUTHOR_DATE"] = when
        subprocess.run(["git"] + list(a), cwd=td, check=True, env=e,
                       capture_output=True)

    g("init", "-q")
    g("add", "-A")
    g("commit", "-qm", "base", when="2026-01-01T00:00:00Z")

    def frag(name, text, when):
        with open(os.path.join(td, "changelog.d", name), "w") as f:
            f.write(text)
        g("add", "changelog.d/" + name)
        g("commit", "-qm", name, when=when)

    frag("zzz-early.added.md", "- early\n", "2026-02-01T00:00:00Z")
    frag("aaa-late.added.md", "- late\n", "2026-03-01T00:00:00Z")
    with open(os.path.join(td, "changelog.d", "mmm-untracked.added.md"), "w") as f:
        f.write("- untracked\n")
    rc, _ = run(td, use_git=True)
    got = read(td)
    assert rc == 0
    assert got.index("- early") < got.index("- late") < got.index("- untracked")
    assert remaining(td) == []


def test_suffix_of_existing_line_is_not_already_present():
    text = BASE.replace("- first fix", "- first fix\n  - Fixed a crash.")
    td = make_root(changelog=text, fragments={"aaa-one.fixed.md": "- Fixed a crash.\n"})
    rc, msg = run(td)
    assert rc == 0 and "already present" not in msg
    assert read(td).count("\n- Fixed a crash.\n") == 1


def test_empty_existing_block_gets_blank_line():
    text = BASE.replace("### Fixed\n\n- first fix\n", "### Fixed\n")
    td = make_root(changelog=text, fragments={"aaa-one.fixed.md": "- a fix\n"})
    rc, _ = run(td)
    assert rc == 0 and "### Fixed\n\n- a fix\n\n## [1.0.0]" in read(td), read(td)


def test_mode_of_changelog_is_kept():
    td = make_root(fragments={"aaa-one.added.md": "- more\n"})
    os.chmod(os.path.join(td, "CHANGELOG.md"), 0o644)
    run(td)
    assert os.stat(os.path.join(td, "CHANGELOG.md")).st_mode & 0o777 == 0o644


def test_preview_with_no_fragments_prints_section():
    td = make_root()
    rc, out = run(td, preview=True)
    assert rc == 0 and "- first added" in out and read(td) == BASE


def test_name_tiebreak_without_git():
    td = make_root(fragments={"bbb-two.added.md": "- b\n", "aaa-one.added.md": "- a\n"})
    run(td)
    assert read(td).index("- a\n") < read(td).index("- b\n")


def test_empty_state_leaves_file_untouched():
    td = make_root(fragments={})
    path = os.path.join(td, "CHANGELOG.md")
    os.utime(path, (1, 1))
    rc, _ = run(td)
    assert rc == 0 and read(td) == BASE and os.stat(path).st_mtime == 1
    with open(os.path.join(td, "changelog.d", "README.md"), "w") as f:
        f.write("# readme\n")
    rc, _ = run(td)
    assert rc == 0 and read(td) == BASE and os.stat(path).st_mtime == 1


def test_second_run_is_a_noop():
    td = make_root(fragments={"aaa-one.added.md": "- second added\n"})
    run(td)
    once = read(td)
    rc, _ = run(td)
    assert rc == 0 and read(td) == once


def test_already_present_text_is_skipped_and_removed():
    td = make_root(fragments={"aaa-one.added.md": "- first added\n"})
    rc, msg = run(td)
    assert rc == 0 and read(td) == BASE and remaining(td) == []
    assert "already present" in msg


def test_missing_unreleased_created_after_preamble():
    text = BASE.replace("## [Unreleased]\n\n### Added\n\n- first added\n\n"
                        "### Fixed\n\n- first fix\n\n", "")
    td = make_root(changelog=text, fragments={"aaa-one.fixed.md": "- a fix\n"})
    rc, _ = run(td)
    want = text.replace("## [1.0.0]", "## [Unreleased]\n\n### Fixed\n\n- a fix\n\n## [1.0.0]")
    assert rc == 0 and read(td) == want, read(td)


def test_into_named_release_heading():
    td = make_root(fragments={"late-fix.fixed.md": "- late fix\n"})
    rc, _ = run(td, into="## [1.0.0] - 2026-01-01")
    got = read(td)
    assert rc == 0
    assert got.endswith("- ancient\n\n### Fixed\n\n- late fix\n")
    assert got.startswith(BASE.split("## [1.0.0]")[0])
    rc, _ = run(make_root(), into="## [9.9.9] - 2026-01-01")
    assert rc == 1


def _bad(name, text):
    td = make_root(fragments={name: text})
    rc, msg = run(td)
    assert rc == 1, (name, text)
    assert read(td) == BASE and remaining(td) == [name], (name, text)
    return msg


def test_malformed_fragments_fail_and_write_nothing():
    _bad("Bad_Name.added.md", "- x\n")
    _bad("ab.added.md", "- x\n")
    _bad("good-slug.bogus.md", "- x\n")
    _bad("good-slug.added.txt.md", "- x\n")
    _bad("good-slug.added.md", "- one\n\n- two\n")
    _bad("good-slug.added.md", "### Added\n- x\n")
    _bad("good-slug.added.md", "- x")
    _bad("good-slug.added.md", "- x\n\n")
    _bad("good-slug.added.md", "no bullet\n")
    _bad("good-slug.added.md", "- x \n")
    _bad("good-slug.added.md", "- x\nunindented\n")
    _bad("good-slug.added.md", "")


def test_good_and_bad_together_write_nothing():
    td = make_root(fragments={"aaa-ok.added.md": "- fine\n", "bbb-bad.added.md": "oops\n"})
    rc, _ = run(td)
    assert rc == 1 and read(td) == BASE and len(remaining(td)) == 2


def test_lint_only():
    td = make_root(fragments={"aaa-ok.added.md": "- fine\n"})
    rc, _ = run(td, lint=True)
    assert rc == 0 and read(td) == BASE and remaining(td) == ["aaa-ok.added.md"]


def test_preview_writes_nothing():
    td = make_root(fragments={"aaa-ok.added.md": "- fine\n"})
    rc, out = run(td, preview=True)
    assert rc == 0 and "- fine" in out and "## [Unreleased]" in out
    assert "ancient" not in out
    assert read(td) == BASE and remaining(td) == ["aaa-ok.added.md"]


OPT = """# Changelog

Intro.

## Unreleased

Fixed: first fix, which
wraps.

Changed: first change.

## 2.5.2 - 2026-10-21

Security: old.
"""


def test_paragraph_layout():
    td = make_root(changelog=OPT, fragments={
        "fix-two.fixed.md": "Fixed: second fix.\n",
        "sec-one.security.md": "Security: patched. **Update recommended.**\n",
        "add-one.added.md": "Added: a thing.\n",
        "kn-one.known-issues.md": "Known Issues: a limit.\n"})
    rc, _ = run(td)
    want = """# Changelog

Intro.

## Unreleased

Security: patched. **Update recommended.**

Added: a thing.

Fixed: first fix, which
wraps.

Fixed: second fix.

Changed: first change.

Known Issues: a limit.

## 2.5.2 - 2026-10-21

Security: old.
"""
    assert rc == 0 and read(td) == want, read(td)


def test_paragraph_prefix_must_match_category():
    td = make_root(changelog=OPT, fragments={"fix-two.fixed.md": "Changed: wrong.\n"})
    rc, _ = run(td)
    assert rc == 1 and read(td) == OPT


def test_golden_current_unreleased_round_trips():
    """Turn master's own [Unreleased] bullets into fragments with
    to_fragment.py, assemble them onto the pre-edit file: no difference."""
    with open(os.path.join(REPO, "CHANGELOG.md"), encoding="utf-8") as f:
        real = f.read()
    lines = real.split("\n")
    sec = asm.find_section(lines, asm.BRACKET_UNRELEASED)
    assert sec is not None, "CHANGELOG.md has no [Unreleased] section"
    start, end = sec
    pre = "\n".join(lines[:start + 1] + [""] + lines[end:])
    with tempfile.TemporaryDirectory() as td:
        def g(*a):
            subprocess.run(["git"] + list(a), cwd=td, check=True,
                           capture_output=True)
        g("init", "-q", "-b", "master")
        g("config", "user.email", "t@example.invalid")
        g("config", "user.name", "t")
        with open(os.path.join(td, "CHANGELOG.md"), "w", encoding="utf-8") as f:
            f.write(pre)
        g("add", "-A")
        g("commit", "-q", "-m", "pre")
        g("checkout", "-q", "-b", "golden")
        with open(os.path.join(td, "CHANGELOG.md"), "w", encoding="utf-8") as f:
            f.write(real)
        g("commit", "-qam", "edit")
        try:
            tof.convert(td, "master", "golden-run")
        except tof.Refuse as exc:
            raise AssertionError(
                "golden test cannot convert the current [Unreleased]: %s" % exc)
        rc, _ = run(td)
        assert rc == 0
        assert read(td) == real, "assembled Unreleased differs from CHANGELOG.md"
        assert remaining(td) == []


def run_all():
    names = sorted(n for n in globals() if n.startswith("test_"))
    failed = 0
    for n in names:
        try:
            globals()[n]()
            print("  ok    %s" % n)
        except Exception as exc:  # noqa: BLE001 - report every failure
            failed += 1
            print("  FAIL  %s: %s: %s" % (n, type(exc).__name__, exc))
    if failed:
        print("FAIL: %d of %d changelog tests" % (failed, len(names)))
        return 1
    print("OK: all %d changelog tests pass." % len(names))
    return 0


if __name__ == "__main__":
    raise SystemExit(run_all())
