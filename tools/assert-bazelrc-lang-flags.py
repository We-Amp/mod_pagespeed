#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

r"""assert-bazelrc-lang-flags.py — guard that single-language compiler flags are
never passed through Bazel's language-agnostic --copt / --host_copt.

Background. --copt applies to BOTH the C and the C++ compile of
every target. A flag only one frontend knows about therefore reaches the one
that does not, and GCC answers each such compile with, e.g.:

    cc1: warning: command-line option '-Wno-dangling-reference' is valid for
         C++/ObjC++ but not for C

On our own sources that is only noise. It stops being noise the moment a
cmake() dependency enters the build: rules_foreign_cc bakes the C copts into
CMAKE_C_FLAGS of the generated crosstool_bazel.cmake, and CMake's
check_c_compiler_flag() carries a FAIL_REGEX for exactly that GNU phrasing.
Once it matches, EVERY probed flag is reported unsupported regardless of real
support, and libaom's require_c_flag(-std=c99) aborts configure outright — with
a message naming a flag that has nothing to do with the cause.

Design notes (every one of them learned from a real bypass of an earlier
version of this guard, so please do not "simplify" them away):

  * QUOTING AND SPACING. The prevailing style in these files is
    --copt="-Wfoo", so a check anchored on `=-W` misses the most likely way the
    bug gets reintroduced: copying the adjacent line. Bare, single-quoted,
    double-quoted and space-separated spellings are all handled.

  * ONE TOKENIZATION. A comma inside a flag's value
    (-fno-sanitize=vptr,function) is not a separator, but a comma joining two
    flags (-O2,-Wfoo) is. split_flags() decides once, and BOTH the banned-flag
    check and the fail-closed check consume its output. Splitting differently
    per check previously let `--copt=-O2,-fbrand-new` slip past the
    fail-closed path while `--copt=-fbrand-new` was caught.

  * --per_file_copt COUNTS TOO, and must RESTRICT rather than merely mention.
    Its value is `<regex-list>@<flag>,<flag>`, where the regex part is itself a
    comma-separated list whose elements may be negated with '-'. A
    single-language flag is accepted only when EVERY non-exclusion element is
    anchored to that language's extensions. Testing whether the regex text
    contains ".cc" would wave through `//a/.*\.cc,//net/.*`, `-//a/.*\.cc`
    (an exclusion of C++!) and `.*\.cpp|.*\.c`.

  * FAIL CLOSED, AT BOTH LEVELS. An unrecognized flag is a FAILURE demanding
    classification, not a silent pass — a guard meant to catch the flag nobody
    thought of must not fail open on exactly that case. The same applies to the
    FILES: a bazelrc named here that has gone missing is an error rather than a
    vacuous pass, and `import`/`try-import` are followed so a violation cannot
    hide in an imported rc.

Run `--self-test` to exercise the bypass matrix that encodes all of the above.

Usage: tools/assert-bazelrc-lang-flags.py   (run from anywhere in the repo)
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

# Bazelrc files that configure the real build. Explicit rather than a glob so a
# developer-local (gitignored) user.bazelrc never trips CI.
BAZELRC_FILES = (".bazelrc", "pagespeed.bazelrc")

# --- Flag classification -----------------------------------------------------
# Flags valid for C++/ObjC++ only. Passing these via --copt reaches the C
# frontend. Warning entries are listed by bare name so -W and -Wno- both match.
CXX_ONLY_WARNINGS = {
    "dangling-reference",          # GCC 13+
    "register",                    # -Wregister: C++ only
    "inconsistent-missing-override",  # clang
    "non-virtual-dtor",
    "overloaded-virtual",
    "ctor-dtor-privacy",
    "delete-non-virtual-dtor",
    "invalid-offsetof",
    "reorder",
    "deprecated-copy",
}
CXX_ONLY_FLAGS = {
    "-faligned-allocation",        # clang: C++ only
    "-fno-rtti",
    "-frtti",
    "-nostdinc++",
    "-fno-threadsafe-statics",
}

# Flags valid for C only; via --copt these reach the C++ frontend.
C_ONLY_WARNINGS = {
    "declaration-after-statement",
    "bad-function-cast",
    "missing-prototypes",
    "strict-prototypes",
    "old-style-definition",
    "nested-externs",
    "pointer-sign",
}
C_ONLY_FLAGS: set[str] = set()

# Language-agnostic flags, i.e. legitimately fine as --copt. Prefixes cover the
# open-ended families (includes, defines, optimization level, MSVC /analyze).
AGNOSTIC_PREFIXES = (
    "-I", "-isystem", "-include", "-iquote",
    "-D", "-U",
    "-O",
    "/analyze",
)
# Everything else must be named. Keep sorted-ish and grouped by why it is here.
AGNOSTIC_FLAGS = {
    # codegen / ABI
    "-fPIC", "-fPIE", "-fno-canonical-system-headers",
    "-fstack-protector", "-fstack-protector-strong",
    "-fcf-protection=full", "-mbranch-protection=standard",
    "-fno-omit-frame-pointer", "-fomit-frame-pointer",
    "-fno-optimize-sibling-calls",
    "-fexceptions", "-fno-exceptions",
    # sanitizers (values vary; the -fsanitize family is agnostic)
    "-fno-sanitize-blacklist", "-fsanitize-memory-track-origins=2",
    # warnings that both frontends accept
    "-Wall", "-Wextra",
    "-Wno-deprecated-declarations",
    "-Wno-unknown-warning-option",
    "-Wno-macro-redefined",
    "-Wno-builtin-macro-redefined",
    "-Wno-free-nonheap-object",
    "-Wno-unused-but-set-parameter",
    "-Wunused-but-set-parameter",
}
# Agnostic families whose value part is free-form.
AGNOSTIC_VALUE_PREFIXES = ("-fsanitize=", "-fno-sanitize=", "-fsanitize-")

# A --per_file_copt file-regex element counts as language-restricted only when
# it ENDS in an anchored source extension and contains no alternation or group
# that could widen it. Checking that a regex merely MENTIONS ".cc" would accept
# "//a/.*\.cc,//net/.*" (second element hits every C file under //net),
# "third_party/foo.cc-tools/.*" (extension as a directory substring) and
# ".*\.cpp|.*\.c" (alternation reaching C) — all of which reintroduce the
# cross-language flag leak with
# the guard green. Both `\.` and the `\\.` spelling used in our bazelrc match.
_NO_WIDENING = r"[^|()\[\]]*"
ANCHORED_CXX_EXT = re.compile(_NO_WIDENING + r"\\{1,2}\.(cpp|cc|cxx|hpp|hh)\$?$")
ANCHORED_C_EXT = re.compile(_NO_WIDENING + r"\\{1,2}\.c\$?$")

# Matches --copt / --host_copt / --per_file_copt followed by '=' or whitespace,
# then a single-quoted, double-quoted, or bare (whitespace-delimited) value.
OPT_RE = re.compile(
    r"--(?P<opt>host_copt|per_file_copt|copt)"
    r"(?:=|\s+)"
    r"(?P<val>\"[^\"]*\"|'[^']*'|\S+)"
)

# `import` must resolve; `try-import` is scanned when the file happens to exist
# (local_tsan.bazelrc is NOT gitignored, so a committed one is live in every
# build and must not be invisible to this guard).
IMPORT_RE = re.compile(r"^\s*(?P<kind>try-import|import)\s+(?P<target>\S+)")


class MissingRcError(Exception):
    """A required bazelrc named by this guard (or by `import`) is absent."""


def repo_root() -> Path:
    out = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        capture_output=True, text=True, check=True,
    )
    return Path(out.stdout.strip())


def unquote(value: str) -> str:
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1]
    return value


def split_flags(value: str) -> list[str]:
    """Split a comma-joined option value into individual flags.

    A comma inside a flag's VALUE (-fno-sanitize=vptr,function) is not a flag
    separator; a comma joining two flags (-O2,-Wfoo) is. They are told apart by
    whether the token itself looks like a flag (leading '-' or MSVC '/').
    Both the deny check and the fail-closed check use this ONE tokenization, so
    a flag cannot be banned-checked and classification-checked differently.
    """
    flags: list[str] = []
    for part in value.split(","):
        if not part:
            continue
        if part.startswith(("-", "/")) or not flags:
            flags.append(part)
        else:  # value continuation of the preceding flag
            flags[-1] += "," + part
    return flags or [value]


def warning_name(flag: str) -> str | None:
    """Bare warning name for -W/-Wno-/-Wno-error= flags, else None."""
    if not flag.startswith("-W"):
        return None
    name = flag[2:]
    for prefix in ("no-error=", "error=", "no-"):
        if name.startswith(prefix):
            name = name[len(prefix):]
            break
    return name


def classify(flag: str) -> str:
    """Return 'cxx', 'c', 'agnostic', or 'unknown' for a single flag."""
    if not flag:
        return "agnostic"

    name = warning_name(flag)
    if name is not None:
        if name in CXX_ONLY_WARNINGS:
            return "cxx"
        if name in C_ONLY_WARNINGS:
            return "c"

    if flag in CXX_ONLY_FLAGS:
        return "cxx"
    if flag in C_ONLY_FLAGS:
        return "c"

    if flag.startswith("-std="):  # inherently language-specific
        std = flag[len("-std="):]
        return "cxx" if std.startswith(("c++", "gnu++")) else "c"

    if flag in AGNOSTIC_FLAGS:
        return "agnostic"
    if flag.startswith(AGNOSTIC_PREFIXES):
        return "agnostic"
    if flag.startswith(AGNOSTIC_VALUE_PREFIXES):
        return "agnostic"
    return "unknown"


def split_per_file_copt(value: str) -> tuple[str | None, list[str]]:
    """Split `<regex-list>@<flag>,<flag>` into (regex-list, flags).

    Bazel splits on the FIRST '@': verified against the real option parser,
    where `--per_file_copt=[@-DX` fails to compile the regex `[` while
    `--per_file_copt=a@[@-DX` parses fine, proving the regex part was just `a`.
    """
    if "@" not in value:
        return None, split_flags(value)
    pattern, _, flags = value.partition("@")
    return pattern, split_flags(flags)


def per_file_scope(pattern: str | None) -> str | None:
    """Language a --per_file_copt regex-list is RESTRICTED to, else None.

    Bazel's pattern part is a comma-separated list of regexes, each optionally
    prefixed '-' to EXCLUDE. A single-language flag is safe only when every
    included element is anchored to that language, so:
      * one non-anchored element anywhere in the list  -> no restriction
      * exclusions grant nothing (an excluded C++ regex is the opposite of a
        restriction to C++)
      * a list of only exclusions restricts nothing
    """
    if pattern is None:
        return None
    langs: set[str] = set()
    for element in pattern.split(","):
        element = element.strip()
        if not element or element.startswith("-"):
            continue  # exclusion: never grants scope
        element = element.lstrip("+")
        if ANCHORED_CXX_EXT.fullmatch(element):
            langs.add("cxx")
        elif ANCHORED_C_EXT.fullmatch(element):
            langs.add("c")
        else:
            return None  # widens beyond a single language
    if len(langs) == 1:
        return langs.pop()
    return None


def correct_option(lang: str, opt: str) -> str:
    if lang == "cxx":
        return "--host_cxxopt" if opt == "host_copt" else "--cxxopt"
    return "--conlyopt"


def scan_text(name: str, text: str) -> list[str]:
    """Return failure messages for one bazelrc's contents."""
    failures: list[str] = []
    for lineno, raw in enumerate(text.splitlines(), start=1):
        if raw.lstrip().startswith("#"):
            continue
        for m in OPT_RE.finditer(raw):
            opt = m.group("opt")
            value = unquote(m.group("val"))

            if opt == "per_file_copt":
                pattern, flags = split_per_file_copt(value)
                scope = per_file_scope(pattern)
            else:
                scope = None
                flags = split_flags(value)

            for flag in flags:
                lang = classify(flag)
                if lang in ("cxx", "c"):
                    if scope == lang:
                        continue  # regex already restricts it to that language
                    src = "C++/ObjC++" if lang == "cxx" else "C"
                    other = "C" if lang == "cxx" else "C++"
                    hint = (
                        " (or restrict every non-exclusion element of the "
                        "--per_file_copt regex list to that language's sources)."
                        if opt == "per_file_copt" else "."
                    )
                    failures.append(
                        f"{name}:{lineno}: '{flag}' is {src}-only but is passed "
                        f"via --{opt}, so it also reaches the {other} frontend.\n"
                        f"      {raw.strip()}\n"
                        f"      Use {correct_option(lang, opt)} instead{hint}"
                    )
                elif lang == "unknown":
                    failures.append(
                        f"{name}:{lineno}: '{flag}' is not classified by this "
                        f"guard.\n"
                        f"      {raw.strip()}\n"
                        f"      Add it to AGNOSTIC_FLAGS if it is valid for both C "
                        f"and C++, or to\n"
                        f"      CXX_ONLY_*/C_ONLY_* and respell the line as "
                        f"--cxxopt/--conlyopt, in\n"
                        f"      tools/assert-bazelrc-lang-flags.py. Unrecognized "
                        f"flags fail on purpose: a\n"
                        f"      guard that silently passes what it does not "
                        f"understand cannot catch the\n"
                        f"      flag nobody thought of."
                    )
    return failures


def resolve_rc(root: Path, spec: str) -> Path:
    return root / spec.replace("%workspace%/", "").replace("%workspace%", "")


def collect_rc_files(root: Path) -> list[tuple[str, Path]]:
    """Bazelrc files to scan, following import/try-import.

    Raises MissingRcError when a file this guard names, or one pulled in by a
    hard `import`, is absent: a rename must break the guard loudly rather than
    silently reduce it to scanning nothing.
    """
    found: list[tuple[str, Path]] = []
    seen: set[Path] = set()
    queue: list[tuple[str, bool]] = [(n, True) for n in BAZELRC_FILES]
    while queue:
        spec, required = queue.pop(0)
        path = resolve_rc(root, spec)
        if path in seen:
            continue
        seen.add(path)
        if not path.exists():
            if required:
                raise MissingRcError(spec)
            continue  # optional try-import that simply is not present
        try:
            display = str(path.relative_to(root))
        except ValueError:
            display = spec
        found.append((display, path))
        for line in path.read_text().splitlines():
            m = IMPORT_RE.match(line)
            if m:
                queue.append((m.group("target"), m.group("kind") == "import"))
    return found


# --- Self-test ---------------------------------------------------------------
# (label, bazelrc line, should_fail). These encode every bypass that has
# defeated a version of this guard; keep them passing.
SELF_TEST_CASES = [
    # Spelling variants of the original cross-language flag leak.
    ("bare",                 "build:linux --copt=-Wno-dangling-reference", True),
    ("double-quoted",        'build:linux --copt="-Wno-dangling-reference"', True),
    ("single-quoted",        "build:linux --copt='-Wno-dangling-reference'", True),
    ("space-separated",      "build:linux --copt -Wno-dangling-reference", True),
    ("comma-joined",         "build:linux --copt=-O2,-Wno-dangling-reference", True),
    ("host_copt",            "build:macos --host_copt=-Wno-register", True),
    ("-Wno-error= form",     "build:linux --copt=-Wno-error=dangling-reference", True),
    ("non-W C++-only flag",  "build:macos --copt=-faligned-allocation", True),
    ("C++-only -std",        "build:linux --copt=-std=c++20", True),
    ("C-only warning",       "build:linux --copt=-Wdeclaration-after-statement", True),
    # Fail-closed, including behind an agnostic prefix (finding 2).
    ("unknown flag fails closed",
     "build:linux --copt=-fsome-brand-new-thing", True),
    ("unknown behind -O prefix",
     "build:linux --copt=-O2,-fbrand-new-thing", True),
    ("unknown behind -D prefix",
     "build:linux --copt=-DFOO,-fbrand-new-thing", True),
    ("unknown behind -fsanitize= prefix",
     "build:linux --copt=-fsanitize=address,-fbrand-new-thing", True),
    # --per_file_copt must RESTRICT, not merely mention (finding 1).
    ("per_file_copt broad",
     "build:x --per_file_copt=//pagespeed/.*@-Wno-dangling-reference", True),
    ("per_file_copt comma-list with one broad element",
     "build:x --per_file_copt=//pagespeed/.*\\.cc,//net/.*@-Wno-dangling-reference",
     True),
    ("per_file_copt exclusion of C++ sources",
     "build:x --per_file_copt=-//foo/.*\\.cc,//bar/.*@-Wno-dangling-reference",
     True),
    ("per_file_copt alternation reaching .c",
     "build:x --per_file_copt=.*\\.cpp|.*\\.c@-std=c++23", True),
    ("per_file_copt group reaching .c",
     "build:x --per_file_copt=.*\\.(cpp|c)@-std=c++23", True),
    ("per_file_copt extension as directory substring",
     "build:x --per_file_copt=third_party/foo.cc-tools/.*@-Wno-dangling-reference",
     True),
    # Must NOT fire.
    ("per_file_copt scoped to .cpp",
     "build:linux --per_file_copt=external/cyclone/.*\\.cpp@-std=c++23", False),
    ("per_file_copt scoped to .cc",
     "build:linux --per_file_copt=pagespeed/.*\\.cc@-nostdinc++", False),
    ("per_file_copt comma-list, every element C++",
     "build:x --per_file_copt=//a/.*\\.cc,//b/.*\\.cpp@-std=c++23", False),
    ("correct cxxopt spelling",
     "build:linux --cxxopt=-Wno-dangling-reference", False),
    ("agnostic define",      'build:linux --copt="-DFOO=1"', False),
    ("agnostic include",     'build:linux --copt="-Iexternal/apr/"', False),
    ("comma inside one flag's value",
     "build:asan --copt -fno-sanitize=vptr,function", False),
    ("agnostic per_file_copt over a broad comma-list",
     "build:win-asan --per_file_copt=//pagespeed/.*,//net/.*@-fsanitize=address",
     False),
    ("comment line ignored",
     "# build:linux --copt=-Wno-dangling-reference", False),
]


def self_test() -> int:
    bad = 0
    for label, line, should_fail in SELF_TEST_CASES:
        got = bool(scan_text("<self-test>", line))
        if got != should_fail:
            bad += 1
            verb = "should have FAILED" if should_fail else "should have PASSED"
            print(f"SELF-TEST BROKEN: {label!r} {verb}: {line}", file=sys.stderr)
        else:
            print(f"  ok  {'detects' if should_fail else 'allows '}  {label}")

    # The import-following / missing-file contract (finding 3).
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        try:
            collect_rc_files(root)
        except MissingRcError:
            print("  ok  detects  missing bazelrc is a hard error")
        else:
            bad += 1
            print("SELF-TEST BROKEN: missing bazelrc did not raise",
                  file=sys.stderr)

        (root / ".bazelrc").write_text("import %workspace%/imported.bazelrc\n")
        (root / "pagespeed.bazelrc").write_text("")
        (root / "imported.bazelrc").write_text(
            "build:linux --copt=-Wno-dangling-reference\n")
        names = [n for n, _ in collect_rc_files(root)]
        if "imported.bazelrc" in names:
            print("  ok  detects  import is followed")
        else:
            bad += 1
            print(f"SELF-TEST BROKEN: import not followed: {names}",
                  file=sys.stderr)

    if bad:
        print(f"\n{bad} self-test case(s) broken.", file=sys.stderr)
        return 1
    print(f"\nOK: {len(SELF_TEST_CASES)} flag cases + 2 file cases pass.")
    return 0


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return self_test()

    try:
        rc_files = collect_rc_files(repo_root())
    except MissingRcError as exc:
        print(f"FAIL: bazelrc '{exc}' is named by this guard (or by an "
              f"`import`) but does not exist.\n"
              f"      It was probably renamed or removed. Update BAZELRC_FILES "
              f"in\n      tools/assert-bazelrc-lang-flags.py — a guard that "
              f"scans nothing must not pass.", file=sys.stderr)
        return 1

    failures: list[str] = []
    for name, path in rc_files:
        failures.extend(scan_text(name, path.read_text()))

    if failures:
        print("FAIL: language-agnostic Bazel option carries a single-language "
              "compiler flag:\n", file=sys.stderr)
        for f in failures:
            print("  " + f + "\n", file=sys.stderr)
        return 1

    scanned = ", ".join(n for n, _ in rc_files)
    print(f"OK: no single-language compiler flag is passed via "
          f"--copt/--host_copt/--per_file_copt, and every value is "
          f"classified.\n    Scanned: {scanned}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
