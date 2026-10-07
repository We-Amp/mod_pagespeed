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

It also checks the version-scoped parts of the scan inputs:

  * a cpe-map.yaml `osv_feed` must be an https:// URL;
  * every cve-ignore.yaml entry must name an annotated dep (one with a pin
    and a cpe-map.yaml entry);
  * an entry with `fixed_in` must have a pinned version and fixed_in that are
    both dotted numerics, with pinned >= fixed_in (else INCONSISTENT: the
    entry claims a fix the pin does not carry, and the scanner is already
    ignoring it);
  * every entry must still suppress a finding when scanning the committed
    snapshot (tools/dependency/nvd-snapshot.json). One that suppresses
    nothing is STALE and must be pruned, the same rule as a stale cpe-map
    entry: NVD re-mapped or rejected the CVE, rescored it below the floor, the
    publish-date filter or the upstream feed drops it, or the entry was wrong.
    A dead suppression that stays would silently hide the CVE again if NVD
    ever re-lists it, without anyone re-reading the justification. Its text
    stays in git history for when that happens.

The snapshot moves only through the daily refresh PR (bot/nvd-snapshot-refresh,
opened by the scheduled CVE scan). An entry the refreshed data makes stale
fails that PR's completeness gate, and the refresh job lists it in the PR body;
prune it on that branch so the snapshot and the cleanup land together.

Fast and OFFLINE — runs on every PR/push (unlike cve_scan.py, which hits NVD on
a schedule).

Usage:
    tools/dependency/validate-deps.py [--snapshot PATH]
Exit 0 if complete and consistent; exit 1 (with ::error annotations) otherwise.
"""

import argparse
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

from cve_scan import (  # noqa: E402  (shared parsers + matcher)
    SNAPSHOT_PATH,
    load_cpe_map,
    load_cpp_deps,
    load_ignores,
    load_snapshot_doc,
    parse_version,
    scan,
)

# A `cpe:2.3:a:vendor:product` prefix (version part intentionally omitted).
CPE_RE = re.compile(r"^cpe:2\.3:[aoh]:[^:]+:[^:]+$")
ISO_DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def err(msg):
    print(f"::error::{msg}", file=sys.stderr)


def check_ignores(ignores, pins, cpe_map, snapshot_path):
    """Validate every cve-ignore.yaml entry against the pins and the snapshot.

    Each entry must name an annotated dep; a `fixed_in` entry must agree with
    the pin; and every entry must still suppress a finding in the snapshot.
    Returns True if all entries pass.
    """
    ok = True
    candidates = {}
    for (cve, dep), entry in sorted(ignores.items()):
        fixed_in = entry.get("fixed_in")
        pin = pins.get(dep)
        if dep not in cpe_map or pin is None:
            err(f"cve-ignore.yaml: {cve} names dep '{dep}', which is not an "
                f"annotated C/C++ dep, so the entry can never suppress a "
                f"finding. Fix the dep name or prune the entry.")
            ok = False
            continue
        if fixed_in:
            fv, pv = parse_version(fixed_in), parse_version(pin)
            if fv is None or pv is None:
                err(f"cve-ignore.yaml: {cve} ({dep}) fixed_in {fixed_in!r} vs "
                    f"pinned {pin!r}: not both dotted-numeric versions, so the "
                    f"entry can never apply. Drop fixed_in (and justify the "
                    f"entry another way) or fix the value.")
                ok = False
                continue
            if pv < fv:
                err(f"cve-ignore.yaml: {cve} ({dep}) is INCONSISTENT: fixed_in "
                    f"{fixed_in} is above the pinned {pin}, so the pin does not "
                    f"carry the fix and the entry is not applied. Bump the dep "
                    f"or re-triage the CVE.")
                ok = False
                continue
        candidates[(cve, dep)] = entry
    if not candidates:
        return ok

    snap_rel = os.path.relpath(snapshot_path, REPO_ROOT)
    try:
        cpes, feeds = load_snapshot_doc(snapshot_path)
    except Exception as exc:  # noqa: BLE001
        err(f"cannot load snapshot {snapshot_path} to check cve-ignore.yaml "
            f"entries for staleness: {exc}")
        return False
    results, _ = scan(deps_subset={d for _, d in candidates},
                      snapshot=cpes, snapshot_feeds=feeds)
    used = {(cve, r["dep"]) for r in results for cve in r.get("ignores_used", [])}
    errors = {r["dep"]: r["error"] for r in results if r.get("error")}
    skipped = {r["dep"] for r in results if r.get("skipped")}
    for (cve, dep) in sorted(candidates):
        fixed_in = candidates[(cve, dep)].get("fixed_in")
        label = f"{dep}, fixed_in {fixed_in}" if fixed_in else dep
        if dep in errors:
            err(f"cve-ignore.yaml: {cve} ({label}): cannot check for "
                f"staleness, the snapshot scan could not evaluate {dep}: "
                f"{errors[dep]}")
            ok = False
        elif dep in skipped:
            err(f"cve-ignore.yaml: {cve} ({label}) is STALE: {dep} has "
                f"cpe: \"N/A\" in cpe-map.yaml, so the scanner never matches "
                f"it and the entry suppresses nothing. Prune it.")
            ok = False
        elif (cve, dep) not in used:
            err(f"cve-ignore.yaml: {cve} ({label}) is STALE: it suppresses no "
                f"finding against {snap_rel}: NVD does not list the CVE "
                f"under {dep}'s CPE (re-mapped or never analyzed), rejected "
                f"or rescored it below the floor, or the publish-date filter "
                f"or upstream feed drops it. Prune it; the justification "
                f"stays in git history, and if NVD re-lists the CVE the scan "
                f"reports it again for re-triage.")
            ok = False
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--snapshot", default=SNAPSHOT_PATH,
                    help="snapshot the cve-ignore.yaml staleness check scans "
                         f"(default: {SNAPSHOT_PATH})")
    args = ap.parse_args()

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

        feed = meta.get("osv_feed")
        if feed is not None and not feed.startswith("https://"):
            err(f"dependency '{name}' has osv_feed {feed!r} — expected an "
                f"https:// URL of the upstream project's OSV feed.")
            ok = False

    # 2. No stale cpe-map entries for deps no longer declared.
    extra = set(cpe_map) - set(cpp_deps)
    if extra:
        err(f"cpe-map.yaml has entries for deps NOT in CPP_DEPS "
            f"(stale): {sorted(extra)}")
        ok = False

    # 3. Every ignore entry names an annotated dep, agrees with the pin
    #    (fixed_in) and still suppresses a finding in the snapshot (not stale).
    ignores = load_ignores()
    if not check_ignores(ignores, dict(load_cpp_deps()), cpe_map,
                         args.snapshot):
        ok = False

    if ok:
        real = sum(1 for n in cpp_deps if cpe_map[n].get("cpe") not in (None, "N/A"))
        na = len(cpp_deps) - real
        print(f"OK: all {len(cpp_deps)} vendored C/C++ deps annotated "
              f"({real} with CPE, {na} N/A + justification); "
              f"{len(ignores)} cve-ignore.yaml entr"
              f"{'y' if len(ignores) == 1 else 'ies'} consistent and live.")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
