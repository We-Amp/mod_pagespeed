#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Validate OpenVEX statement files used by the release security gates.

Walks the given directory for ``*.vex.json`` files. For each file:
  * Must be valid JSON.
  * Must declare an OpenVEX ``@context`` (a URI containing ``openvex.dev``).
  * Must contain a non-null ``statements`` array.
  * Each statement must carry ``vulnerability``, ``products``, ``status``,
    and ``status`` must be one of the four values OpenVEX 0.2 defines:
    ``not_affected``, ``affected``, ``fixed``, ``under_investigation``.
  * A ``not_affected`` statement whose ``impact_statement`` carries an
    ``Expires: YYYY-MM-DD`` marker is time-boxed: once that date is in the
    past the file fails validation, so a suppression nobody re-evaluates
    reddens the scan instead of silently outliving its evidence. OpenVEX
    has no expiry field, which is why the marker lives in the free-text
    impact statement; the marker is optional, and a statement without one
    is open-ended (as every pre-existing statement was). Same convention as
    pagespeed-optimizer's tools/ci/validate-vex.py — keep the two in sync.

Exit 0 if the directory contains no VEX file (the security-gates job
documents that this is allowed and grype will run without suppressions).

Exit 1 on any structural problem; prints a GitHub-Actions ``::error file=``
annotation pointing at the offending file.

Usage:
    tools/ci/validate-vex.py sbom/
    tools/ci/validate-vex.py --self-test
"""

import contextlib
import datetime
import io
import json
import os
import re
import sys
import tempfile

OPENVEX_STATUSES = {"not_affected", "affected", "fixed", "under_investigation"}

# Time-box marker inside a not_affected impact_statement, e.g.
# "Expires: 2026-12-02. Surface: ...". Word-bounded so prose such as
# "expires" elsewhere in the statement cannot accidentally arm it.
EXPIRES_RE = re.compile(r"\bExpires:\s*(\d{4}-\d{2}-\d{2})\b")


def fail(path: str, msg: str) -> None:
    print(f"::error file={path}::{msg}", file=sys.stderr)


def expiry_of(stmt: dict) -> "datetime.date | None":
    """Return the statement's Expires date, or None when it has no marker.

    Raises ValueError on a marker that matches the shape but is not a real
    calendar date (e.g. 2026-13-45).
    """
    text = stmt.get("impact_statement")
    if not isinstance(text, str):
        return None
    m = EXPIRES_RE.search(text)
    if not m:
        return None
    return datetime.date.fromisoformat(m.group(1))


def validate(path: str, today: "datetime.date | None" = None) -> bool:
    """Return True if `path` is a structurally valid OpenVEX document.

    `today` is injectable for tests; it defaults to the current UTC date.
    """
    if today is None:
        today = datetime.datetime.now(datetime.timezone.utc).date()
    try:
        with open(path) as f:
            doc = json.load(f)
    except json.JSONDecodeError as exc:
        fail(path, f"Not valid JSON: {exc}")
        return False
    except OSError as exc:
        fail(path, f"Cannot read file: {exc}")
        return False

    ctx = doc.get("@context", "")
    if not isinstance(ctx, str) or "openvex.dev" not in ctx:
        fail(path, "Missing OpenVEX @context (https://openvex.dev/ns/...)")
        return False

    statements = doc.get("statements")
    if not isinstance(statements, list):
        fail(path, "Missing or non-array .statements")
        return False

    for i, stmt in enumerate(statements):
        if not isinstance(stmt, dict):
            fail(path, f"statements[{i}] is not an object")
            return False
        for required in ("vulnerability", "products", "status"):
            if required not in stmt:
                fail(path, f"statements[{i}] missing required field '{required}'")
                return False
        status = stmt["status"]
        if status not in OPENVEX_STATUSES:
            fail(
                path,
                f"statements[{i}].status='{status}' not one of "
                f"{sorted(OPENVEX_STATUSES)}",
            )
            return False
        if status == "not_affected":
            vuln = stmt["vulnerability"]
            vuln_name = vuln.get("name") if isinstance(vuln, dict) else vuln
            try:
                expires = expiry_of(stmt)
            except ValueError:
                fail(
                    path,
                    f"statements[{i}] ({vuln_name}) has a malformed "
                    "'Expires: YYYY-MM-DD' marker in impact_statement",
                )
                return False
            if expires is not None and expires < today:
                fail(
                    path,
                    f"statements[{i}] ({vuln_name}) suppression expired on "
                    f"{expires.isoformat()} — re-evaluate the finding and "
                    "either bump the dependency or renew the statement with "
                    "a new 'Expires: YYYY-MM-DD' and refreshed evidence",
                )
                return False

    print(f"OK: {path} ({len(statements)} statement(s))")
    return True


def _vex_doc(impact_statement, status="not_affected"):
    return {
        "@context": "https://openvex.dev/ns/v0.2.0",
        "statements": [
            {
                "vulnerability": {"name": "GHSA-test-0000-0000"},
                "products": [{"@id": "pkg:npm/example@1.0.0"}],
                "status": status,
                "justification": "vulnerable_code_not_in_execute_path",
                "impact_statement": impact_statement,
            }
        ],
    }


def self_test() -> int:
    """Fixture suite for the Expires time-box: expired/malformed fail, the
    rest pass. `today` is pinned so the cases never rot."""
    today = datetime.date(2026, 10, 3)
    cases = [
        # (name, document, expected validate() result)
        ("no marker is open-ended", _vex_doc("Not reachable. Re-evaluate on bump."), True),
        ("future marker passes", _vex_doc("Expires: 2026-12-02. Surface: x."), True),
        ("marker on today passes", _vex_doc("Expires: 2026-10-03. Surface: x."), True),
        ("marker yesterday fails", _vex_doc("Expires: 2026-10-02. Surface: x."), False),
        ("malformed marker date fails", _vex_doc("Expires: 2026-13-45. Surface: x."), False),
        ("marker anywhere in prose", _vex_doc("Dev-only. Expires: 2025-01-01 (renew)."), False),
        ("lowercase prose does not arm", _vex_doc("the token expires: 2025-01-01 server side"), True),
        ("marker ignored on non-not_affected", _vex_doc("Expires: 2025-01-01.", status="fixed"), True),
        ("absent impact_statement is fine", _vex_doc(None), True),
        ("bad status still fails", _vex_doc("x", status="maybe"), False),
    ]
    failed = []
    with tempfile.TemporaryDirectory() as d:
        for n, (name, doc, want) in enumerate(cases):
            path = os.path.join(d, f"case{n}.vex.json")
            with open(path, "w") as f:
                json.dump(doc, f)
            # Swallow the per-case output: the expected-failure cases emit
            # `::error file=` lines, which GitHub Actions would otherwise turn
            # into annotations on a PASSING step.
            sink = io.StringIO()
            with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
                got = validate(path, today=today)
            if got != want:
                failed.append(f"{name}: want {want}, got {got}\n{sink.getvalue()}")
    if failed:
        for line in failed:
            print(f"FAIL: {line}", file=sys.stderr)
        print(f"FAIL: {len(failed)} of {len(cases)} self-test cases", file=sys.stderr)
        return 1
    print(f"OK: all {len(cases)} self-test cases behave as specified.")
    return 0


def main() -> int:
    if len(sys.argv) == 2 and sys.argv[1] == "--self-test":
        return self_test()
    if len(sys.argv) != 2:
        print("usage: validate-vex.py <dir> | --self-test", file=sys.stderr)
        return 2

    sbom_dir = sys.argv[1]
    if not os.path.isdir(sbom_dir):
        print(f"::error::Not a directory: {sbom_dir}", file=sys.stderr)
        return 2

    vex_files = sorted(
        os.path.join(sbom_dir, name)
        for name in os.listdir(sbom_dir)
        if name.endswith(".vex.json")
    )
    if not vex_files:
        print(f"No VEX file found in {sbom_dir} — nothing to validate.")
        return 0

    ok = True
    for path in vex_files:
        if not validate(path):
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
