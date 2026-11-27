#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Structural sanity check for the curated SPDX 2.3 SBOM.

The release workflow's security gate runs this after `generate-sbom.py
--verify`; the drift check proves the committed document matches the
generator, this proves the document is a well-formed SPDX 2.3 JSON document:

  * spdxVersion is "SPDX-2.3", the document SPDXID is "SPDXRef-DOCUMENT",
    packages is a non-empty array and creationInfo.created is a string;
  * every SPDXID (document, packages, files, snippets) is a valid idstring:
    "SPDXRef-" followed by letters, digits, "." and "-" only -- SPDX 2.3
    allows nothing else, so an underscore in a package id is rejected;
  * every element id is unique within the document;
  * every relationship end names a declared element (or NOASSERTION/NONE).

Problems are reported as GitHub workflow annotations (::error ...) so they
surface on the job summary; the exit status is 1 on any problem.

Usage:
    tools/sbom/check-spdx-structure.py [path/to/document.spdx.json]

Python standard library only.
"""

import json
import re
import sys

DEFAULT_PATH = "sbom/mod_pagespeed-2.1.spdx.json"
IDSTRING = re.compile(r"^SPDXRef-[A-Za-z0-9.-]+$")
# A relationship may point at these instead of an element in the document.
EXTERNAL_TARGETS = {"NOASSERTION", "NONE"}


def check(doc):
    """Return the list of problems found in the parsed document."""
    problems = []
    if doc.get("spdxVersion") != "SPDX-2.3":
        problems.append(f"spdxVersion={doc.get('spdxVersion')!r}, want 'SPDX-2.3'")
    if doc.get("SPDXID") != "SPDXRef-DOCUMENT":
        problems.append(f"SPDXID={doc.get('SPDXID')!r}, want 'SPDXRef-DOCUMENT'")
    pkgs = doc.get("packages")
    if not isinstance(pkgs, list) or not pkgs:
        problems.append("packages must be a non-empty array")
    if not isinstance(doc.get("creationInfo", {}).get("created"), str):
        problems.append("creationInfo.created must be a string")

    # Every element that carries an SPDXID, with a label for the report.
    elements = [("document", doc)]
    for section in ("packages", "files", "snippets"):
        items = doc.get(section)
        if isinstance(items, list):
            elements += [(f"{section}[{i}]", e) for i, e in enumerate(items)]

    seen = {}
    for label, element in elements:
        spdx_id = element.get("SPDXID") if isinstance(element, dict) else None
        name = element.get("name", "") if isinstance(element, dict) else ""
        where = f"{label} ({name})" if name and label != "document" else label
        if not isinstance(spdx_id, str):
            problems.append(f"{where}: SPDXID {spdx_id!r} must be a string")
            continue
        if not IDSTRING.match(spdx_id):
            problems.append(
                f"{where}: SPDXID {spdx_id!r} is not a valid idstring "
                "(want ^SPDXRef-[A-Za-z0-9.-]+$)"
            )
        # Register the id even when malformed so a relationship that points at
        # it is not reported a second time as dangling.
        if spdx_id in seen:
            problems.append(
                f"{where}: SPDXID {spdx_id!r} already used by {seen[spdx_id]}"
            )
        else:
            seen[spdx_id] = where

    for i, rel in enumerate(doc.get("relationships") or []):
        for end in ("spdxElementId", "relatedSpdxElement"):
            target = rel.get(end) if isinstance(rel, dict) else None
            if target in EXTERNAL_TARGETS or target in seen:
                continue
            problems.append(
                f"relationships[{i}].{end} {target!r} does not name an element "
                "in this document"
            )
    return problems


def main(argv):
    path = argv[1] if len(argv) > 1 else DEFAULT_PATH
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"::error file={path}::{exc}", file=sys.stderr)
        return 1
    problems = check(doc)
    if problems:
        for p in problems:
            print(f"::error file={path}::{p}", file=sys.stderr)
        print(f"FAIL: {len(problems)} problem(s) in {path}", file=sys.stderr)
        return 1
    print(
        f"OK: {path} is well-formed SPDX 2.3 -- {len(doc['packages'])} packages, "
        f"{len(doc.get('relationships') or [])} relationships, all ids valid and unique"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
