#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Completeness gate for the vendored-C/C++ CVE matcher.

The anti-drift guard, modeled on Envoy's tools/dependency/validate.py: every
vendored C/C++ dependency declared in bazel/repositories.bzl (enumerated by
generate-sbom.py's CPP_DEPS) MUST carry a cpe-map.yaml entry with:

  * a `release_date` (ISO YYYY-MM-DD), AND
  * either a real `cpe` (`cpe:2.3:a:vendor:product` form) OR an explicit
    `cpe: "N/A"` accompanied by a non-empty `justification`.

A new C++ dep therefore cannot land unscanned — adding it to CPP_DEPS without a
cpe-map.yaml entry fails this gate. It also fails on the reverse drift (a
cpe-map.yaml entry for a dep no longer in CPP_DEPS) and on malformed entries.

Fast and OFFLINE — runs on every PR/push (unlike cve_scan.py, which hits NVD on
a schedule).

Usage:
    tools/dependency/validate-deps.py
Exit 0 if complete and consistent; exit 1 (with ::error annotations) otherwise.
"""

import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS_DIR = os.path.join(REPO_ROOT, "tools")
CPE_MAP_PATH = os.path.join(TOOLS_DIR, "dependency", "cpe-map.yaml")

sys.path.insert(0, TOOLS_DIR)
sys.path.insert(0, os.path.join(TOOLS_DIR, "dependency"))

import importlib.util  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    "generate_sbom", os.path.join(TOOLS_DIR, "generate-sbom.py")
)
_gen = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_gen)

from cve_scan import load_cpe_map  # noqa: E402  (shared parser)

# A `cpe:2.3:a:vendor:product` prefix (version part intentionally omitted).
CPE_RE = re.compile(r"^cpe:2\.3:[aoh]:[^:]+:[^:]+$")
ISO_DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def err(msg):
    print(f"::error::{msg}", file=sys.stderr)


def main():
    cpp_deps = [d["name"] for d in _gen.CPP_DEPS]
    cpe_map = load_cpe_map(CPE_MAP_PATH)

    ok = True

    # 1. Every CPP_DEPS dep has a cpe-map entry.
    for name in cpp_deps:
        meta = cpe_map.get(name)
        if meta is None:
            err(f"dependency '{name}' (in generate-sbom.py CPP_DEPS / "
                f"bazel/repositories.bzl) has NO cpe-map.yaml entry — a new C++ "
                f"dep cannot land unscanned. Add a cpe + release_date (or "
                f"cpe: \"N/A\" + justification).")
            ok = False
            continue

        cpe = meta.get("cpe")
        rel = meta.get("release_date")

        if not rel or not ISO_DATE_RE.match(rel):
            err(f"dependency '{name}' has missing/invalid release_date "
                f"(got {rel!r}; expected YYYY-MM-DD).")
            ok = False

        if not cpe:
            err(f"dependency '{name}' has no cpe field.")
            ok = False
        elif cpe == "N/A":
            if not meta.get("justification"):
                err(f"dependency '{name}' has cpe: \"N/A\" but NO justification "
                    f"(justification is mandatory for N/A).")
                ok = False
        elif not CPE_RE.match(cpe):
            err(f"dependency '{name}' has malformed cpe {cpe!r} — expected "
                f"`cpe:2.3:a:vendor:product` (no version part) or \"N/A\".")
            ok = False

    # 2. No stale cpe-map entries for deps no longer declared.
    extra = set(cpe_map) - set(cpp_deps)
    if extra:
        err(f"cpe-map.yaml has entries for deps NOT in CPP_DEPS "
            f"(stale): {sorted(extra)}")
        ok = False

    if ok:
        real = sum(1 for n in cpp_deps if cpe_map[n].get("cpe") not in (None, "N/A"))
        na = len(cpp_deps) - real
        print(f"OK: all {len(cpp_deps)} vendored C/C++ deps annotated "
              f"({real} with CPE, {na} N/A + justification).")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
