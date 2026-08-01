#!/usr/bin/env python3
r"""assert-git-repository-vendored.py — guard that every `git_repository`
dependency is vendored so it never has to be git-cloned at build time.

Background (r20 release postmortem). Bazel's `--repository_cache` only covers
`http_archive` deps; a `git_repository` is fetched with a live `git clone`. The
release distro containers (the nginx-distro build) ship WITHOUT git, so an
un-vendored `git_repository` builds fine on every developer machine and every
normal CI leg — and then breaks ONLY at release time, inside a container that
cannot clone it. That is exactly how the AVIF `aom_src` dep bit r20 attempt 1.

The fix is "the cyclone pattern": every `git_repository` is redirected to a
vendored source copy via a `build:vendored --override_repository=<name>=<path>`
line in pagespeed.bazelrc, and `tools/vendor-deps.sh` populates that path. The
release build runs `--config=ci`, which pulls in `--config=vendored`, so the
override is what actually keeps `git` off the release critical path. This guard
fails if any `git_repository` lacks that override (or if the override points at
a directory tools/vendor-deps.sh never creates).

Design notes (fail closed, like the sibling bazelrc guard):

  * SCAN THE WHOLE MACRO TREE. `git_repository` calls are discovered across
    every `bazel/*.bzl` — the files the WORKSPACE loads its deps from — not just
    repositories.bzl, so a dep added in a new .bzl is covered automatically.
  * A `git_repository(` whose `name` this guard cannot parse is a FAILURE, not
    a silent skip: an unparsable dep is precisely the one that would slip an
    un-vendored clone past the gate.
  * The AUTHORITATIVE vendor representation is the pagespeed.bazelrc override,
    because that is the wiring the offline/release build consumes. The
    tools/vendor-deps.sh population is checked as a second, independent
    assertion so an override that points at an empty directory (which would
    reintroduce the release break in a different shape) is also caught.

Run `--self-test` to exercise the pass / no-override / unpopulated / unparsable
matrix.

Usage: tools/assert-git-repository-vendored.py   (run from anywhere in the repo)
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

# The macro directory the WORKSPACE loads dependency definitions from
# (`load("//bazel:repositories.bzl", ...)` etc.). Scanning the whole directory
# rather than a hard-coded file list means a git_repository added to a new
# bazel/*.bzl is covered without editing this guard. Spike/example WORKSPACEs
# under docs/ are deliberately NOT part of the real build graph and excluded.
MACRO_GLOB_DIR = "bazel"

# Bazelrc files that may carry the `build:vendored` override config.
BAZELRC_FILES = ("pagespeed.bazelrc", ".bazelrc")

# The vendoring script that must populate each override's target directory.
VENDOR_SCRIPT = "tools/vendor-deps.sh"

# The `vendored` config is what the release/offline build consumes
# (`build:ci --config=vendored`). Only overrides declared under THIS config
# keep git off the release critical path.
VENDOR_CONFIG = "vendored"

# `git_repository(` (but not the `load(... "git_repository")` statement or a
# comment). Matches the call, then the FIRST `name = "..."` that follows it.
# If the name is not the leading attribute the capture fails and we fail closed.
_GIT_REPO_CALL = re.compile(
    r"(?<![\w.])git_repository\s*\(\s*(?:#[^\n]*\n\s*)*name\s*=\s*\"([^\"]+)\"",
    re.MULTILINE,
)
# Any `git_repository(` call site at all — used to fail closed when a call
# exists but its name did not parse into _GIT_REPO_CALL above.
_GIT_REPO_ANY = re.compile(r"(?<![\w.])git_repository\s*\(")


class GuardError(Exception):
    """A structural problem that must fail the guard rather than pass quietly."""


def repo_root() -> Path:
    out = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        capture_output=True, text=True, check=True,
    )
    return Path(out.stdout.strip())


def _strip_comment(line: str) -> str:
    """Drop a trailing/whole-line `#` comment (no `#` appears inside our tokens)."""
    hash_at = line.find("#")
    return line if hash_at < 0 else line[:hash_at]


def find_git_repositories(bzl_texts: dict[str, str]) -> dict[str, str]:
    """Map git_repository name -> the file it was declared in.

    Fails closed: a `git_repository(` whose name cannot be parsed raises, so an
    un-vendorable dep can never masquerade as "no git_repository found".
    """
    names: dict[str, str] = {}
    for fname, text in bzl_texts.items():
        # Ignore the `load(..., "git_repository")` symbol import and drop
        # per-line `#` comments so a commented-out call is not counted.
        scannable = "\n".join(
            "" if re.search(r"\bload\s*\(", line) else _strip_comment(line)
            for line in text.splitlines()
        )
        parsed = {m.group(1) for m in _GIT_REPO_CALL.finditer(scannable)}
        total = len(_GIT_REPO_ANY.findall(scannable))
        if total != len(parsed):
            raise GuardError(
                f"{fname}: found {total} git_repository() call(s) but could only "
                f"parse {len(parsed)} name(s). Every git_repository must declare "
                f"`name = \"...\"` as its first attribute so this guard can check "
                f"it — an unparsable one could smuggle in an un-vendored clone."
            )
        for name in parsed:
            names[name] = fname
    return names


def parse_vendored_overrides(bazelrc_texts: dict[str, str]) -> dict[str, str]:
    """Map repo name -> vendored path for every `build:<vendored> ...
    --override_repository=<name>=<path>` line across the bazelrc files."""
    overrides: dict[str, str] = {}
    override_re = re.compile(
        r"--override_repository[=\s]+([^=\s]+)=(\S+)"
    )
    config_re = re.compile(rf"^\s*build:{re.escape(VENDOR_CONFIG)}\b")
    for text in bazelrc_texts.values():
        for raw in text.splitlines():
            line = _strip_comment(raw)
            if not config_re.match(line):
                continue
            for name, path in override_re.findall(line):
                overrides[name] = path
    return overrides


def vendor_script_populates(vendor_text: str, vendored_path: str) -> bool:
    """Whether tools/vendor-deps.sh produces the override's target directory.

    The script builds paths dynamically ($VENDOR_DIR/cyclone, $VENDOR_DIR/aom),
    so the literal `vendor/cyclone` never appears. We match on the final path
    component (the dep's vendored dir name), which the script does reference.
    """
    leaf = vendored_path.rstrip("/").split("/")[-1]
    if not leaf:
        return False
    return re.search(rf"(?<![\w-]){re.escape(leaf)}(?![\w-])", vendor_text) is not None


def evaluate(
    git_repos: dict[str, str],
    overrides: dict[str, str],
    vendor_text: str,
) -> list[str]:
    """Return a failure message per git_repository that is not fully vendored."""
    failures: list[str] = []
    for name in sorted(git_repos):
        declared_in = git_repos[name]
        if name not in overrides:
            failures.append(
                f"git_repository '{name}' (declared in {declared_in}) has NO "
                f"vendored override.\n"
                f"      Add to {BAZELRC_FILES[0]} (the cyclone pattern):\n"
                f"        build:{VENDOR_CONFIG} --override_repository="
                f"{name}=vendor/{name}\n"
                f"      and vendor its source in {VENDOR_SCRIPT}. Without this the "
                f"dep is git-cloned at\n"
                f"      build time, which the git-less release distro containers "
                f"cannot do — it breaks\n"
                f"      ONLY at release (r20 postmortem)."
            )
            continue
        vendored_path = overrides[name]
        if not vendor_script_populates(vendor_text, vendored_path):
            failures.append(
                f"git_repository '{name}' has a build:{VENDOR_CONFIG} override to "
                f"'{vendored_path}', but {VENDOR_SCRIPT}\n"
                f"      does not appear to populate that directory — the override "
                f"would resolve to an empty\n"
                f"      tree in the release build. Vendor '{name}' in "
                f"{VENDOR_SCRIPT} (the cyclone pattern)."
            )
    return failures


def load_texts(root: Path) -> tuple[dict[str, str], dict[str, str], str]:
    bzl_dir = root / MACRO_GLOB_DIR
    bzl_texts = {
        str(p.relative_to(root)): p.read_text()
        for p in sorted(bzl_dir.glob("*.bzl"))
    }
    if not bzl_texts:
        raise GuardError(
            f"No {MACRO_GLOB_DIR}/*.bzl macro files found — the dependency "
            f"definitions moved and this guard would scan nothing."
        )
    bazelrc_texts: dict[str, str] = {}
    for name in BAZELRC_FILES:
        path = root / name
        if path.exists():
            bazelrc_texts[name] = path.read_text()
    if not bazelrc_texts:
        raise GuardError(
            f"None of {BAZELRC_FILES} exist — cannot verify vendored overrides."
        )
    vendor_path = root / VENDOR_SCRIPT
    if not vendor_path.exists():
        raise GuardError(f"{VENDOR_SCRIPT} is missing — cannot verify vendoring.")
    return bzl_texts, bazelrc_texts, vendor_path.read_text()


# --- Self-test ---------------------------------------------------------------

def _run_case(bzl: str, bazelrc: str, vendor: str) -> list[str]:
    git_repos = find_git_repositories({"<bzl>": bzl})
    overrides = parse_vendored_overrides({"<rc>": bazelrc})
    return evaluate(git_repos, overrides, vendor)


_COMPLIANT_BZL = '''
load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
def deps():
    git_repository(
        name = "cyclone",
        remote = "https://github.com/We-Amp/cyclone-cache.git",
        commit = CYCLONE_COMMIT,
        build_file_content = cyclone_build_rule,
    )
    git_repository(
        name = "aom_src",
        remote = "https://aomedia.googlesource.com/aom",
        commit = AOM_COMMIT,
        build_file_content = _ALL_SRCS_BUILD_FILE,
    )
'''

_COMPLIANT_RC = (
    "build:vendored --repository_cache=vendor/repo-cache\n"
    "build:vendored --override_repository=cyclone=vendor/cyclone\n"
    "build:vendored --override_repository=aom_src=vendor/aom\n"
)

_COMPLIANT_VENDOR = (
    'CYCLONE_DIR="$VENDOR_DIR/cyclone"\n'
    'AOM_DIR="$VENDOR_DIR/aom"\n'
)


def self_test() -> int:
    bad = 0

    def check(label: str, failures: list[str], should_fail: bool) -> None:
        nonlocal bad
        got = bool(failures)
        if got != should_fail:
            bad += 1
            verb = "should have FAILED" if should_fail else "should have PASSED"
            print(f"SELF-TEST BROKEN: {label!r} {verb}: {failures}",
                  file=sys.stderr)
        else:
            print(f"  ok  {'detects' if should_fail else 'allows '}  {label}")

    # 1. The compliant current state passes.
    check("both deps overridden + vendored",
          _run_case(_COMPLIANT_BZL, _COMPLIANT_RC, _COMPLIANT_VENDOR), False)

    # 2. A git_repository with no override fails (the r20 shape).
    rc_no_aom = (
        "build:vendored --override_repository=cyclone=vendor/cyclone\n"
    )
    check("dep missing its vendored override",
          _run_case(_COMPLIANT_BZL, rc_no_aom, _COMPLIANT_VENDOR), True)

    # 3. Override present but vendor-deps.sh does not populate it.
    vendor_no_aom = 'CYCLONE_DIR="$VENDOR_DIR/cyclone"\n'
    check("override to a dir vendor-deps.sh never creates",
          _run_case(_COMPLIANT_BZL, _COMPLIANT_RC, vendor_no_aom), True)

    # 4. Override declared under the WRONG config does not count.
    rc_wrong_config = (
        "build:vendored --override_repository=cyclone=vendor/cyclone\n"
        "build:linux --override_repository=aom_src=vendor/aom\n"
    )
    check("override under a non-vendored config is not accepted",
          _run_case(_COMPLIANT_BZL, rc_wrong_config, _COMPLIANT_VENDOR), True)

    # 5. Spacing spelling `--override_repository name=path` is honoured.
    rc_space = (
        "build:vendored --override_repository cyclone=vendor/cyclone\n"
        "build:vendored --override_repository aom_src=vendor/aom\n"
    )
    check("space-separated override spelling accepted",
          _run_case(_COMPLIANT_BZL, rc_space, _COMPLIANT_VENDOR), False)

    # 6. An unparsable git_repository (name not first attr) fails closed.
    bad_bzl = (
        'def deps():\n'
        '    git_repository(\n'
        '        remote = "https://example.com/x.git",\n'
        '        name = "x",\n'
        '    )\n'
    )
    try:
        find_git_repositories({"<bzl>": bad_bzl})
    except GuardError:
        print("  ok  detects  git_repository whose name is not parsable "
              "fails closed")
    else:
        bad += 1
        print("SELF-TEST BROKEN: unparsable git_repository name did not raise",
              file=sys.stderr)

    # 7. A commented-out git_repository is not counted.
    commented = (
        'def deps():\n'
        '    # git_repository(name = "old_dep", remote = "x")\n'
        '    pass\n'
    )
    if find_git_repositories({"<bzl>": commented}) == {}:
        print("  ok  allows   commented-out git_repository ignored")
    else:
        bad += 1
        print("SELF-TEST BROKEN: commented git_repository was counted",
              file=sys.stderr)

    if bad:
        print(f"\n{bad} self-test case(s) broken.", file=sys.stderr)
        return 1
    print("\nOK: all self-test cases pass.")
    return 0


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return self_test()

    try:
        bzl_texts, bazelrc_texts, vendor_text = load_texts(repo_root())
        git_repos = find_git_repositories(bzl_texts)
        overrides = parse_vendored_overrides(bazelrc_texts)
    except GuardError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    failures = evaluate(git_repos, overrides, vendor_text)

    if failures:
        print("FAIL: a git_repository dependency is not vendored (release "
              "distro containers have no git):\n", file=sys.stderr)
        for f in failures:
            print("  " + f + "\n", file=sys.stderr)
        return 1

    if not git_repos:
        print("OK: no git_repository dependencies declared.")
        return 0
    listed = ", ".join(sorted(git_repos))
    print(f"OK: every git_repository carries a build:{VENDOR_CONFIG} vendored "
          f"override populated by {VENDOR_SCRIPT}.\n    Checked: {listed}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
