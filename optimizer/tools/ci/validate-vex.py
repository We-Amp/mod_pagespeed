#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Validate OpenVEX statement files used by the release security gates.

Walks the given directory for ``*.vex.json`` files. For each file:
  * Must be valid JSON.
  * Must declare an OpenVEX ``@context`` (a URI containing ``openvex.dev``).
  * Must carry document-level ``author`` and ``timestamp`` (RFC 3339-ish).
  * Must contain a non-null ``statements`` array.
  * Each statement must carry ``vulnerability``, ``products``, ``status``,
    and ``status`` must be one of the four values OpenVEX 0.2 defines:
    ``not_affected``, ``affected``, ``fixed``, ``under_investigation``.
  * ``not_affected`` statements must carry a ``justification`` from the
    OpenVEX 0.2 enum (or an ``impact_statement``) — a bare not_affected
    is an unsupported claim, not an assessment.
  * ``affected`` statements must carry an ``action_statement``.
  * A ``not_affected`` statement whose ``impact_statement`` carries an
    ``Expires: YYYY-MM-DD`` marker is time-boxed: once that date is in the
    past the file fails validation, so a suppression nobody re-evaluates
    reddens the scan instead of silently outliving its evidence. OpenVEX
    has no expiry field, which is why the marker lives in the free-text
    impact statement; the marker is optional, and a statement without one
    is open-ended (as every pre-existing statement was).

Exit 0 if the directory contains no VEX file (the security-gates job
documents that this is allowed and grype will run without suppressions).

Exit 1 on any structural problem; prints a GitHub-Actions ``::error file=``
annotation pointing at the offending file.

Usage:
    tools/ci/validate-vex.py sbom/
"""

import datetime
import json
import os
import re
import sys

OPENVEX_STATUSES = {"not_affected", "affected", "fixed", "under_investigation"}

# OpenVEX 0.2 justification label enum (required for not_affected unless an
# impact_statement is given).
OPENVEX_JUSTIFICATIONS = {
    "component_not_present",
    "vulnerable_code_not_present",
    "vulnerable_code_not_in_execute_path",
    "vulnerable_code_cannot_be_controlled_by_adversary",
    "inline_mitigations_already_exist",
}

# RFC 3339 date-time, e.g. 2026-06-11T12:00:00Z or with offset/fraction.
# RFC 3339 permits lowercase t/z separators.
TIMESTAMP_RE = re.compile(
    r"^\d{4}-\d{2}-\d{2}[Tt]\d{2}:\d{2}:\d{2}(\.\d+)?([Zz]|[+-]\d{2}:\d{2})$"
)


# Time-box marker inside a not_affected impact_statement, e.g.
# "Expires: 2026-12-02. Surface: ...". Word-bounded so prose such as
# "expires" elsewhere in the statement cannot accidentally arm it.
EXPIRES_RE = re.compile(r"\bExpires:\s*(\d{4}-\d{2}-\d{2})\b")


def fail(path: str, msg: str) -> None:
    print(f"::error file={path}::{msg}", file=sys.stderr)


def expiry_of(stmt: dict) -> "datetime.date | None":
    """Return the statement's Expires date, or None when it has no marker."""
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

    author = doc.get("author")
    if not isinstance(author, str) or not author.strip():
        fail(path, "Missing document-level 'author'")
        return False

    timestamp = doc.get("timestamp")
    if not isinstance(timestamp, str) or not TIMESTAMP_RE.match(timestamp):
        fail(
            path,
            "Missing or malformed document-level 'timestamp' "
            "(expected RFC 3339, e.g. 2026-06-11T12:00:00Z)",
        )
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
        # Structurally inert statements (empty products, unnamed vulnerability)
        # parse fine but match NOTHING in grype — the silent-no-op class this
        # validator exists to catch.
        vuln = stmt["vulnerability"]
        vuln_name = vuln.get("name") if isinstance(vuln, dict) else vuln
        if not isinstance(vuln_name, str) or not vuln_name.strip():
            fail(path, f"statements[{i}].vulnerability has no name — matches nothing")
            return False
        products = stmt["products"]
        if not isinstance(products, list) or not products:
            fail(path, f"statements[{i}].products is empty — matches nothing")
            return False
        for j, prod in enumerate(products):
            pid = prod.get("@id") if isinstance(prod, dict) else prod
            if not isinstance(pid, str) or not pid.strip():
                fail(
                    path,
                    f"statements[{i}].products[{j}] has no @id — matches nothing",
                )
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
            justification = stmt.get("justification")
            if justification is None and not stmt.get("impact_statement"):
                fail(
                    path,
                    f"statements[{i}] is not_affected without a "
                    "'justification' or 'impact_statement' — an unsupported "
                    "claim, not an assessment",
                )
                return False
            if justification is not None and (
                justification not in OPENVEX_JUSTIFICATIONS
            ):
                fail(
                    path,
                    f"statements[{i}].justification='{justification}' not one "
                    f"of {sorted(OPENVEX_JUSTIFICATIONS)}",
                )
                return False
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
        if status == "affected" and not stmt.get("action_statement"):
            fail(
                path,
                f"statements[{i}] is affected without an 'action_statement'",
            )
            return False

    print(f"OK: {path} ({len(statements)} statement(s))")
    return True


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: validate-vex.py <dir>", file=sys.stderr)
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
