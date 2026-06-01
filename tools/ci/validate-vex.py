#!/usr/bin/env python3
# SPDX-License-Identifier: BUSL-1.1
# Copyright (c) 2024-2026 We-Amp B.V.

"""Validate OpenVEX statement files used by the release security gates.

Walks the given directory for ``*.vex.json`` files. For each file:
  * Must be valid JSON.
  * Must declare an OpenVEX ``@context`` (a URI containing ``openvex.dev``).
  * Must contain a non-null ``statements`` array.
  * Each statement must carry ``vulnerability``, ``products``, ``status``,
    and ``status`` must be one of the four values OpenVEX 0.2 defines:
    ``not_affected``, ``affected``, ``fixed``, ``under_investigation``.

Exit 0 if the directory contains no VEX file (the security-gates job
documents that this is allowed and grype will run without suppressions).

Exit 1 on any structural problem; prints a GitHub-Actions ``::error file=``
annotation pointing at the offending file.

Usage:
    tools/ci/validate-vex.py sbom/
"""

import json
import os
import sys

OPENVEX_STATUSES = {"not_affected", "affected", "fixed", "under_investigation"}


def fail(path: str, msg: str) -> None:
    print(f"::error file={path}::{msg}", file=sys.stderr)


def validate(path: str) -> bool:
    """Return True if `path` is a structurally valid OpenVEX document."""
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
