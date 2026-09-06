#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# CI pytest exit-status guard: a test file whose __main__ block calls
# pytest.main(...) bare swallows pytest's exit status, and a bazel py_test
# running such a file as its main reports PASS while cases inside it fail
# -- a gate that cannot go red.
#
# On enforcement: this job goes red on violation, but branch protection on
# master does not require it (repo-wide status quo: no required status
# checks are configured beyond the dependency scan), so a red guard blocks
# merges by review convention, not by GitHub enforcement.
#
# Two rules over the tracked test python files, enforced by an AST scan
# (python3 stdlib ast, no other dependencies). A line regex cannot see the
# evasions that matter here: multi-line calls, trailing comments or
# semicolons, aliased imports, a space before the paren, and vacuous
# __main__ bodies -- they all parse to exactly the shapes this guard
# exists to reject, while real comments and docstring prose mentioning
# pytest.main(...) never parse to a call at all.
#
#   1. No pytest.main(...) call whose result is dropped. pytest.main() only
#      RETURNS its exit code; the call must be routed out of the process --
#      sys.exit(pytest.main(...)) or raise SystemExit(pytest.main(...)).
#      A bare expression-statement call, or one assigned and never exited
#      through, exits 0 no matter how many cases failed.
#
#   2. Every tracked test_*.py has an `if __name__ == "__main__":` block
#      (either quote style) with a non-vacuous body. A py_test main WITHOUT
#      one -- or with a body of only `pass` / `...` / a docstring --
#      executes its module body and exits 0 having run zero cases:
#      vacuously green, worse than a swallow. (Found live:
#      //test/system/system:test_critical_css_beacon and
#      :test_prioritize_critical_images ran nothing for bazel while direct
#      pytest collected them fine.)
#
# Fast + hermetic: git ls-files over the tracked tree, no Bazel analysis, no
# network. Mirrors tools/ci/check_tests_in_test_tree.sh. Requires python3
# (stdlib only); the bash side stays portable to macOS stock bash 3.2.
#
# Run locally: bash tools/ci/check_pytest_exit_status.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if ! command -v python3 >/dev/null 2>&1; then
  echo "ERROR: python3 not found; the AST scan cannot run and the guard" >&2
  echo "       refuses to pass without its checker." >&2
  exit 1
fi

# All tracked files under test/. git ls-files is deterministic and ignores
# build outputs / bazel-* symlinks. A git failure here must be FATAL: with
# the file list silently empty every rule below passes vacuously, so a
# failed probe would read as green -- the defect shape this guard exists
# to kill.
if ! git ls-files 'test/' > "$WORK/tracked" 2> "$WORK/git-err"; then
  echo "ERROR: git ls-files failed; refusing to run the guard against an" >&2
  echo "       empty file list:" >&2
  sed 's/^/  /' "$WORK/git-err" >&2
  exit 1
fi

# The || true covers grep's no-match exit ONLY (handled by the empty-list
# check below); git failures are fatal above, never swallowed.
grep '\.py$' "$WORK/tracked" | LC_ALL=C sort > "$WORK/pyfiles" || true

if [ ! -s "$WORK/pyfiles" ]; then
  echo "ERROR: no tracked python files under test/; every rule would pass" >&2
  echo "       vacuously, so the guard refuses to report green." >&2
  exit 1
fi

python3 - "$WORK/pyfiles" <<'PYEOF'
"""AST scan for the pytest exit-status guard (see the bash header).

Rule 1: every pytest.main(...) call must be routed out of the process --
directly inside sys.exit(...) or inside SystemExit(...) under a raise.
Anything else (bare expression statement, assignment, return, ...) drops
the status.

Rule 2: every test_*.py must have an `if __name__ == "__main__":` block
whose body is not vacuous (more than pass / ... / a docstring).
"""

import ast
import os
import sys


def collect_aliases(tree):
    """Names bound to pytest.main / sys.exit by from-imports."""
    pytest_main, sys_exit = set(), set()
    for node in ast.walk(tree):
        if isinstance(node, ast.ImportFrom):
            for alias in node.names:
                if node.module == "pytest" and alias.name == "main":
                    pytest_main.add(alias.asname or alias.name)
                elif node.module == "sys" and alias.name == "exit":
                    sys_exit.add(alias.asname or alias.name)
    return pytest_main, sys_exit


def is_main_guard(test):
    """`__name__ == "__main__"` -- either operand order, any quote style."""
    if not isinstance(test, ast.Compare):
        return False
    if len(test.ops) != 1 or not isinstance(test.ops[0], ast.Eq):
        return False
    if len(test.comparators) != 1:
        return False
    operands = (test.left, test.comparators[0])
    has_dunder_name = any(
        isinstance(n, ast.Name) and n.id == "__name__" for n in operands
    )
    has_dunder_main = any(
        isinstance(n, ast.Constant) and n.value == "__main__" for n in operands
    )
    return has_dunder_name and has_dunder_main


def body_is_vacuous(body):
    """True when the block does nothing: pass / ... / only a docstring."""
    stmts = list(body)
    if (
        stmts
        and isinstance(stmts[0], ast.Expr)
        and isinstance(stmts[0].value, ast.Constant)
        and isinstance(stmts[0].value.value, str)
    ):
        stmts = stmts[1:]
    return all(
        isinstance(s, ast.Pass)
        or (
            isinstance(s, ast.Expr)
            and isinstance(s.value, ast.Constant)
            and s.value.value is Ellipsis
        )
        for s in stmts
    )


def main():
    with open(sys.argv[1], encoding="utf-8") as fh:
        paths = [ln.strip() for ln in fh if ln.strip()]

    dropped, missing, vacuous = [], [], []

    for path in paths:
        try:
            with open(path, encoding="utf-8") as fh:
                tree = ast.parse(fh.read(), filename=path)
        except (OSError, SyntaxError, ValueError) as exc:
            print(f"ERROR: cannot parse {path}: {exc}", file=sys.stderr)
            print(
                "an unparseable tracked file cannot be checked; refusing "
                "to pass the guard without checking it",
                file=sys.stderr,
            )
            sys.exit(2)

        pytest_main_aliases, sys_exit_aliases = collect_aliases(tree)

        parent = {}
        for node in ast.walk(tree):
            for child in ast.iter_child_nodes(node):
                parent[child] = node

        def is_pytest_main(node):
            if not isinstance(node, ast.Call):
                return False
            func = node.func
            if (
                isinstance(func, ast.Attribute)
                and func.attr == "main"
                and isinstance(func.value, ast.Name)
                and func.value.id == "pytest"
            ):
                return True
            return isinstance(func, ast.Name) and func.id in pytest_main_aliases

        def is_sys_exit(node):
            if not isinstance(node, ast.Call):
                return False
            func = node.func
            if (
                isinstance(func, ast.Attribute)
                and func.attr == "exit"
                and isinstance(func.value, ast.Name)
                and func.value.id == "sys"
            ):
                return True
            return isinstance(func, ast.Name) and func.id in sys_exit_aliases

        def result_routed(call):
            """True iff this pytest.main() call's result exits the process."""
            p = parent.get(call)
            # sys.exit(pytest.main(...))
            if p is not None and is_sys_exit(p) and call in p.args:
                return True
            # raise SystemExit(pytest.main(...))
            if (
                isinstance(p, ast.Call)
                and isinstance(p.func, ast.Name)
                and p.func.id == "SystemExit"
                and call in p.args
            ):
                return isinstance(parent.get(p), ast.Raise)
            return False

        for node in ast.walk(tree):
            if is_pytest_main(node) and not result_routed(node):
                dropped.append((path, node.lineno))
                break  # one entry per file is enough

        if os.path.basename(path).startswith("test_"):
            guards = [
                n
                for n in ast.walk(tree)
                if isinstance(n, ast.If) and is_main_guard(n.test)
            ]
            if not guards:
                missing.append(path)
            elif all(body_is_vacuous(g.body) for g in guards):
                vacuous.append(path)

    fatal = False

    if dropped:
        print(
            "ERROR: the following files call pytest.main(...) without routing",
            file=sys.stderr,
        )
        print(
            "       its exit status out of the process; a bazel py_test",
            file=sys.stderr,
        )
        print(
            "       running one as its main reports PASS on a red suite",
            file=sys.stderr,
        )
        print("      :", file=sys.stderr)
        for path, lineno in dropped:
            print(f"  - {path}:{lineno}", file=sys.stderr)
        print("", file=sys.stderr)
        print("Fix: route the status out of the process --", file=sys.stderr)
        print(
            '  raise SystemExit(pytest.main([__file__, "-v"]))', file=sys.stderr
        )
        print(
            "or sys.exit(pytest.main(...)), or the framework's run_pytest() "
            "helper.",
            file=sys.stderr,
        )
        fatal = True

    if missing:
        print(
            "ERROR: the following test_*.py files have NO __main__ block; "
            "used as a",
            file=sys.stderr,
        )
        print(
            "       bazel py_test main they exit 0 having run zero cases:",
            file=sys.stderr,
        )
        for path in missing:
            print(f"  - {path}", file=sys.stderr)
        print("", file=sys.stderr)
        print("Fix: append", file=sys.stderr)
        print('  if __name__ == "__main__":', file=sys.stderr)
        print(
            '      raise SystemExit(pytest.main([__file__, "-v"]))',
            file=sys.stderr,
        )
        fatal = True

    if vacuous:
        print(
            "ERROR: the following test_*.py files have a __main__ block with "
            "a",
            file=sys.stderr,
        )
        print(
            "       vacuous body (pass / ... / docstring only); as a bazel "
            "py_test",
            file=sys.stderr,
        )
        print(
            "       main they exit 0 having run zero cases -- the defect",
            file=sys.stderr,
        )
        print("       defect reintroduced:", file=sys.stderr)
        for path in vacuous:
            print(f"  - {path}", file=sys.stderr)
        print("", file=sys.stderr)
        print("Fix: give the block a real body --", file=sys.stderr)
        print('  if __name__ == "__main__":', file=sys.stderr)
        print(
            '      raise SystemExit(pytest.main([__file__, "-v"]))',
            file=sys.stderr,
        )
        fatal = True

    if fatal:
        print("", file=sys.stderr)
        print(
            "FAILED: pytest exit-status guard "
            "(tools/ci/check_pytest_exit_status.sh).",
            file=sys.stderr,
        )
        sys.exit(1)

    sys.exit(0)


if __name__ == "__main__":
    main()
PYEOF

echo "OK: no tracked test python file drops pytest.main()'s exit status, and"
echo "    every tracked test_*.py has a non-vacuous __main__ block."
exit 0
