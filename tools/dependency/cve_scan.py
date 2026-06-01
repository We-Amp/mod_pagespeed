#!/usr/bin/env python3
# SPDX-License-Identifier: BUSL-1.1
# Copyright (c) 2024-2026 We-Amp B.V.
"""Vendored-C/C++ CVE matcher for mod_pagespeed 1.1.

Modeled on envoyproxy/envoy's tools/dependency/ CVE scanner: instead of feeding
synthetic CPEs into grype (which cannot match source-built Bazel deps and lacks
per-dep date filtering), we

  1. take the dep list + pinned versions from generate-sbom.py's CPP_DEPS
     (sourced from bazel/repositories.bzl — versions never drift),
  2. read each dep's curated CPE + release_date from cpe-map.yaml,
  3. query the NVD 2.0 CVE feed by `virtualMatchString` (CPE with wildcard
     version) and CACHE the raw response per CPE,
  4. keep only CVEs PUBLISHED AFTER the pinned version's release_date and at or
     above a severity threshold (default: CVSS >= 4.0, i.e. medium+),
  5. minus the cve-ignore.yaml suppression list (each entry justified).

Report-only (v1): exits 0 by default. A per-dep findings table is printed and a
machine-readable JSON report is written (default: tools/dependency/.cve-cache/
report.json, gitignored). The completeness gate (validate-deps.py) is the
blocking part. Pass --fail-on <severity> to flip to blocking (the per-PR ratchet
— see the design record Phase C and the workflow).

Data sources (in priority order, controlled by flags):
  * --snapshot <file>  read the committed compact NVD snapshot
                       (tools/dependency/nvd-snapshot.json). This is the
                       PER-PR path: offline, fast (seconds), reproducible — a
                       PR scanned today and re-run tomorrow gives the same
                       result unless the committed snapshot changed.
  * --offline          read only the per-CPE .cve-cache/ (legacy local cache).
  * (default / --refresh)  hit NVD live, populating .cve-cache/. This is the
                       DAILY-CRON path that also REBUILDS the snapshot via
                       --build-snapshot.

NVD politeness: unauthenticated callers get ~5 req / 30s. We cache aggressively
and sleep between live requests. Set NVD_API_KEY to raise the limit.

Usage:
    tools/dependency/cve_scan.py                  # scan all deps, use cache
    tools/dependency/cve_scan.py --refresh        # ignore cache, re-fetch NVD
    tools/dependency/cve_scan.py --dep curl --dep libpng   # subset
    tools/dependency/cve_scan.py --severity high  # raise threshold
    tools/dependency/cve_scan.py --offline        # .cve-cache only, never hit NVD
    tools/dependency/cve_scan.py --snapshot tools/dependency/nvd-snapshot.json
    tools/dependency/cve_scan.py --refresh --build-snapshot   # daily cron
    tools/dependency/cve_scan.py --json report.json --fail-on high  # blocking
"""

import argparse
import json
import os
import sys
import time
import urllib.parse
import urllib.request
from datetime import datetime, timezone

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS_DIR = os.path.join(REPO_ROOT, "tools")
DEP_DIR = os.path.join(TOOLS_DIR, "dependency")
CPE_MAP_PATH = os.path.join(DEP_DIR, "cpe-map.yaml")
IGNORE_PATH = os.path.join(DEP_DIR, "cve-ignore.yaml")
CACHE_DIR = os.path.join(DEP_DIR, ".cve-cache")
DEFAULT_REPORT = os.path.join(CACHE_DIR, "report.json")
# Committed, compact, reproducible NVD mirror the per-PR job scans offline.
SNAPSHOT_PATH = os.path.join(DEP_DIR, "nvd-snapshot.json")
SNAPSHOT_SCHEMA = 1

NVD_API = "https://services.nvd.nist.gov/rest/json/cves/2.0"
# NVD asks unauthenticated callers to keep to ~5 requests per rolling 30s.
NVD_SLEEP_SECONDS = 7.0
NVD_PAGE_SIZE = 2000  # NVD 2.0 max resultsPerPage

SEVERITY_FLOOR = {"low": 0.1, "medium": 4.0, "high": 7.0, "critical": 9.0}

# ---------------------------------------------------------------------------
# Reuse generate-sbom.py's enumeration + repositories.bzl version resolution so
# the matcher and the SBOM can never disagree about which deps exist or what
# version is pinned.
# ---------------------------------------------------------------------------
sys.path.insert(0, TOOLS_DIR)
import importlib.util  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    "generate_sbom", os.path.join(TOOLS_DIR, "generate-sbom.py")
)
_gen = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_gen)


def load_cpp_deps():
    """Return [(name, version)] for every C++ dep, from generate-sbom.py."""
    consts = _gen.parse_bzl_constants()
    out = []
    for dep in _gen.CPP_DEPS:
        out.append((dep["name"], _gen.resolve_version(dep, consts)))
    return out


# ---------------------------------------------------------------------------
# Minimal YAML reader (no PyYAML dep on CI runners). Handles exactly the shape
# of cpe-map.yaml and cve-ignore.yaml: a top-level mapping key, nested mappings,
# scalar string values (quoted or bare), comments, and `[]` empty lists.
# ---------------------------------------------------------------------------
def _strip(val):
    """Return a scalar value, dropping any trailing `# comment`.

    Honors quotes: a `#` inside a quoted string is kept; an unquoted trailing
    `#` (preceded by whitespace) starts a comment.
    """
    val = val.strip()
    if val and val[0] in "\"'":
        quote = val[0]
        end = val.find(quote, 1)
        if end != -1:
            return val[1:end]
        return val[1:]
    # Bare scalar: strip an inline comment (whitespace then #).
    hash_idx = val.find("#")
    if hash_idx != -1:
        # Only treat as comment if preceded by whitespace or at start.
        if hash_idx == 0 or val[hash_idx - 1].isspace():
            val = val[:hash_idx]
    return val.strip()


def load_cpe_map(path=CPE_MAP_PATH):
    """Parse cpe-map.yaml -> {dep_name: {cpe, release_date, justification?}}."""
    deps = {}
    cur = None
    in_deps = False
    with open(path) as f:
        for raw in f:
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            indent = len(line) - len(line.lstrip())
            stripped = line.strip()
            if indent == 0:
                in_deps = stripped.rstrip(":") == "deps"
                cur = None
                continue
            if not in_deps:
                continue
            if indent == 2 and stripped.endswith(":") and ":" == stripped[-1]:
                cur = stripped[:-1].strip()
                deps[cur] = {}
                continue
            if indent >= 4 and cur is not None and ":" in stripped:
                key, _, value = stripped.partition(":")
                deps[cur][key.strip()] = _strip(value)
    return deps


def load_ignores(path=IGNORE_PATH):
    """Parse cve-ignore.yaml -> {(cve, dep): justification}.

    Enforces that every entry carries a non-empty justification.
    """
    ignores = {}
    cur = None
    in_ignores = False
    errors = []
    with open(path) as f:
        for raw in f:
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            stripped = line.strip()
            if stripped in ("ignores:", "ignores: []"):
                in_ignores = True
                continue
            if not in_ignores:
                continue
            if stripped.startswith("- "):
                if cur is not None:
                    _finish_ignore(cur, ignores, errors)
                cur = {}
                stripped = stripped[2:].strip()
            if ":" in stripped:
                key, _, value = stripped.partition(":")
                if cur is not None:
                    cur[key.strip()] = _strip(value)
    if cur is not None:
        _finish_ignore(cur, ignores, errors)
    if errors:
        for e in errors:
            print(f"ERROR (cve-ignore.yaml): {e}", file=sys.stderr)
        sys.exit(1)
    return ignores


def _finish_ignore(entry, ignores, errors):
    cve = entry.get("cve")
    dep = entry.get("dep")
    just = entry.get("justification")
    if not cve or not dep:
        errors.append(f"ignore entry missing cve/dep: {entry}")
        return
    if not just:
        errors.append(f"ignore entry for {cve} ({dep}) has no justification "
                      "(justification is MANDATORY)")
        return
    ignores[(cve, dep)] = just


# ---------------------------------------------------------------------------
# NVD fetch + cache
# ---------------------------------------------------------------------------
def cache_path(cpe):
    safe = cpe.replace(":", "_").replace("*", "x").replace("/", "_")
    return os.path.join(CACHE_DIR, f"nvd_{safe}.json")


def fetch_nvd(cpe, refresh=False, offline=False):
    """Fetch all CVEs for a CPE (virtualMatchString), cached per CPE.

    Returns the list of NVD `vulnerabilities` objects.
    """
    cp = cache_path(cpe)
    if not refresh and os.path.exists(cp):
        with open(cp) as f:
            return json.load(f)["vulnerabilities"]
    if offline:
        raise RuntimeError(f"--offline and no cache for {cpe} ({cp})")

    os.makedirs(CACHE_DIR, exist_ok=True)
    match = cpe + ":*:*:*:*:*:*:*:*" if cpe.count(":") == 4 else cpe
    headers = {}
    api_key = os.environ.get("NVD_API_KEY")
    if api_key:
        headers["apiKey"] = api_key

    vulns = []
    start = 0
    while True:
        params = urllib.parse.urlencode({
            "virtualMatchString": match,
            "resultsPerPage": NVD_PAGE_SIZE,
            "startIndex": start,
        })
        url = f"{NVD_API}?{params}"
        req = urllib.request.Request(url, headers=headers)
        with urllib.request.urlopen(req, timeout=60) as resp:
            data = json.loads(resp.read().decode())
        vulns.extend(data.get("vulnerabilities", []))
        total = data.get("totalResults", 0)
        start += data.get("resultsPerPage", 0)
        if start >= total or not data.get("resultsPerPage"):
            break
        time.sleep(NVD_SLEEP_SECONDS)

    payload = {
        "cpe": cpe,
        "virtualMatchString": match,
        "fetched": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "totalResults": len(vulns),
        "vulnerabilities": vulns,
    }
    with open(cp, "w") as f:
        json.dump(payload, f)
    time.sleep(NVD_SLEEP_SECONDS)
    return vulns


# ---------------------------------------------------------------------------
# Compact, committed NVD snapshot (the reproducible per-PR mirror).
#
# A raw NVD vulnerability object is large (configurations, references, weaknesses,
# CVSS vector strings, multi-language descriptions, change history). The matcher
# only ever reads, per vuln:
#   cve.id, cve.vulnStatus, cve.published, cve.descriptions[lang=en].value,
#   and the *first* CVSS metric under cvssMetricV31/V30/V40/V2 (baseScore +
#   baseSeverity).  minimize_vuln() prunes to exactly that, preserving the SAME
#   nested shape so scan()/cve_severity()/cve_published()/cve_description() run
#   byte-for-byte identically whether fed live, .cve-cache, or the snapshot.
#
# The file is deterministic (sorted CPE keys, vulns sorted by CVE id, sorted JSON
# keys, trailing newline) so the daily-cron diff is clean and a re-run reproduces
# the exact same findings unless the snapshot content actually changed.
# ---------------------------------------------------------------------------
def minimize_vuln(v):
    """Prune a raw NVD `vulnerabilities[]` entry to the fields the matcher reads.

    Keeps the nested `{"cve": {...}}` shape and only the FIRST CVSS metric of the
    highest-priority family (matching cve_severity's preference order), so the
    extraction helpers need no snapshot-specific code path.
    """
    cve = v.get("cve", {})
    out_cve = {
        "id": cve.get("id", ""),
        "published": cve.get("published", ""),
    }
    # vulnStatus only matters when it's "Rejected" (scan() skips those). Keep
    # ONLY that value — NVD flips Analyzed<->Modified routinely without any
    # material change, and retaining it would churn the committed snapshot daily.
    if cve.get("vulnStatus") == "Rejected":
        out_cve["vulnStatus"] = "Rejected"
    # English description, trimmed the same way cve_description reads it.
    desc = cve_description(cve)
    if desc:
        out_cve["descriptions"] = [{"lang": "en", "value": desc}]
    # Single representative CVSS metric, in cve_severity's preference order.
    metrics = cve.get("metrics", {})
    for key in ("cvssMetricV31", "cvssMetricV30", "cvssMetricV40", "cvssMetricV2"):
        arr = metrics.get(key)
        if arr:
            d = arr[0].get("cvssData", {})
            entry = {"cvssData": {
                "baseScore": d.get("baseScore"),
                "baseSeverity": d.get("baseSeverity",
                                      arr[0].get("baseSeverity", "")),
            }}
            # cvssMetricV2 carries baseSeverity as a sibling of cvssData.
            if key == "cvssMetricV2" and arr[0].get("baseSeverity"):
                entry["baseSeverity"] = arr[0].get("baseSeverity")
            out_cve["metrics"] = {key: [entry]}
            break
    return {"cve": out_cve}


def build_snapshot(cpe_to_vulns, path=SNAPSHOT_PATH):
    """Write the deterministic compact snapshot from {cpe: [raw vulns]}."""
    snap = {
        "schema": SNAPSHOT_SCHEMA,
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "source": NVD_API,
        "cpes": {},
    }
    for cpe in sorted(cpe_to_vulns):
        minimized = [minimize_vuln(v) for v in cpe_to_vulns[cpe]]
        minimized.sort(key=lambda m: m["cve"].get("id", ""))
        snap["cpes"][cpe] = minimized
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w") as f:
        json.dump(snap, f, indent=1, sort_keys=True)
        f.write("\n")
    return snap


def load_snapshot(path=SNAPSHOT_PATH):
    """Load the committed snapshot -> {cpe: [vulns]} (matcher-shaped)."""
    with open(path) as f:
        snap = json.load(f)
    schema = snap.get("schema")
    if schema != SNAPSHOT_SCHEMA:
        raise RuntimeError(
            f"snapshot {path} schema {schema} != expected {SNAPSHOT_SCHEMA}; "
            f"rebuild with --refresh --build-snapshot")
    return snap.get("cpes", {})


# ---------------------------------------------------------------------------
# CVE field extraction
# ---------------------------------------------------------------------------
def cve_severity(cve):
    """Best available CVSS base score: prefer v3.1/v3.0, fall back to v2."""
    metrics = cve.get("metrics", {})
    for key in ("cvssMetricV31", "cvssMetricV30", "cvssMetricV40"):
        arr = metrics.get(key)
        if arr:
            d = arr[0].get("cvssData", {})
            return d.get("baseScore"), d.get("baseSeverity", "")
    arr = metrics.get("cvssMetricV2")
    if arr:
        return arr[0].get("cvssData", {}).get("baseScore"), \
            arr[0].get("baseSeverity", "")
    return None, ""


def cve_published(cve):
    # NVD `published` is e.g. "2024-09-11T12:00:00.000" (naive, UTC-implied).
    pub = cve.get("published", "")
    try:
        dt = datetime.fromisoformat(pub.replace("Z", "+00:00"))
    except ValueError:
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt


def cve_description(cve):
    for d in cve.get("descriptions", []):
        if d.get("lang") == "en":
            return d.get("value", "").strip().replace("\n", " ")
    return ""


# ---------------------------------------------------------------------------
# Scan
# ---------------------------------------------------------------------------
def scan(deps_subset=None, severity="medium", refresh=False, offline=False,
         snapshot=None, collect=None):
    """Match every annotated dep against NVD data and date/severity-filter.

    Data source per CPE:
      * snapshot is not None  -> read snapshot[cpe] (the committed mirror; offline)
      * else                  -> fetch_nvd(cpe, refresh, offline)
    If `collect` is a dict, the raw vulns fetched per CPE are stashed into it
    (collect[cpe] = vulns) so the caller can build a fresh snapshot in one pass.
    """
    cpe_map = load_cpe_map()
    ignores = load_ignores()
    floor = SEVERITY_FLOOR[severity]
    versions = dict(load_cpp_deps())

    results = []
    for name, version in load_cpp_deps():
        if deps_subset and name not in deps_subset:
            continue
        meta = cpe_map.get(name)
        if meta is None:
            # completeness gate would have caught this; be loud anyway.
            results.append({"dep": name, "version": version, "cpe": None,
                            "error": "no cpe-map.yaml entry", "findings": []})
            continue
        cpe = meta.get("cpe", "")
        rel = meta.get("release_date", "")
        if cpe == "N/A":
            results.append({"dep": name, "version": version, "cpe": "N/A",
                            "release_date": rel, "skipped": "N/A",
                            "justification": meta.get("justification", ""),
                            "findings": []})
            continue

        try:
            rel_dt = datetime.fromisoformat(rel + "T00:00:00+00:00")
        except ValueError:
            rel_dt = None

        try:
            if snapshot is not None:
                if cpe not in snapshot:
                    raise RuntimeError(
                        f"CPE {cpe} not in snapshot — rebuild with "
                        f"--refresh --build-snapshot")
                vulns = snapshot[cpe]
            else:
                vulns = fetch_nvd(cpe, refresh=refresh, offline=offline)
                if collect is not None:
                    collect[cpe] = vulns
        except Exception as exc:  # noqa: BLE001  (report-only: never crash)
            results.append({"dep": name, "version": version, "cpe": cpe,
                            "release_date": rel, "error": str(exc),
                            "findings": []})
            continue

        findings = []
        for v in vulns:
            cve = v.get("cve", {})
            cid = cve.get("id", "")
            if cve.get("vulnStatus") == "Rejected":
                continue
            score, sev = cve_severity(cve)
            if score is None or score < floor:
                continue
            pub = cve_published(cve)
            if rel_dt and pub and pub < rel_dt:
                continue  # pre-release-date: pinned version postdates the CVE
            if (cid, name) in ignores:
                continue
            findings.append({
                "cve": cid,
                "score": score,
                "severity": sev,
                "published": cve.get("published", ""),
                "description": cve_description(cve)[:240],
            })
        findings.sort(key=lambda f: (-(f["score"] or 0), f["cve"]))
        results.append({
            "dep": name, "version": version, "cpe": cpe,
            "release_date": rel, "candidate_cves": len(vulns),
            "findings": findings,
        })
    return results, versions


def print_table(results, severity, fail_on=None):
    width = 88
    print("=" * width)
    print(f"  Vendored C/C++ CVE scan (mod_pagespeed 1.15) — severity >= {severity}")
    print(f"  CVEs filtered to those published after each dep's pinned release_date")
    print("=" * width)
    total_findings = 0
    for r in results:
        dep = r["dep"]
        ver = r.get("version", "?")
        if r.get("error"):
            print(f"\n[!] {dep} {ver}: {r['error']}")
            continue
        if r.get("skipped") == "N/A":
            print(f"\n[-] {dep} {ver}: CPE N/A — {r.get('justification', '')[:64]}")
            continue
        fs = r["findings"]
        total_findings += len(fs)
        tag = "OK " if not fs else "!! "
        print(f"\n[{tag}] {dep} {ver}  ({r['cpe']})  "
              f"candidates={r.get('candidate_cves', 0)}  post-release-findings={len(fs)}")
        for f in fs:
            print(f"      {f['cve']}  CVSS {f['score']} ({f['severity']})  "
                  f"{f['published'][:10]}")
            if f["description"]:
                print(f"         {f['description'][:96]}")
    print("\n" + "=" * width)
    print(f"  TOTAL post-release findings (severity >= {severity}): {total_findings}")
    if fail_on:
        print(f"  Gate: --fail-on {fail_on} — nonzero exit if findings exist.")
    else:
        print(f"  Report-only: exit 0 regardless of findings.")
    print("=" * width)
    return total_findings


def main():
    ap = argparse.ArgumentParser(description="Vendored C/C++ CVE matcher")
    ap.add_argument("--dep", action="append", default=[],
                    help="scan only these deps (repeatable)")
    ap.add_argument("--severity", choices=list(SEVERITY_FLOOR), default="medium")
    ap.add_argument("--refresh", action="store_true",
                    help="ignore cache; re-fetch NVD")
    ap.add_argument("--offline", action="store_true",
                    help="use only cached NVD responses; never hit the network")
    ap.add_argument("--snapshot", nargs="?", const=SNAPSHOT_PATH, default=None,
                    help="scan against the committed compact NVD snapshot "
                         f"(default file: {SNAPSHOT_PATH}); offline + fast. "
                         "This is the per-PR path.")
    ap.add_argument("--build-snapshot", nargs="?", const=SNAPSHOT_PATH, default=None,
                    help="after a (live/cache) scan, write the compact snapshot "
                         f"to this path (default: {SNAPSHOT_PATH}). Used by the "
                         "daily cron to refresh the committed mirror.")
    ap.add_argument("--fail-on", choices=list(SEVERITY_FLOOR), default=None,
                    help="RATCHET TOGGLE: exit nonzero if any finding at/above "
                         "this severity exists (default: unset = report-only). "
                         "Flip the per-PR job to blocking by setting this.")
    ap.add_argument("--json", default=DEFAULT_REPORT,
                    help=f"write JSON report here (default: {DEFAULT_REPORT})")
    args = ap.parse_args()

    snapshot = None
    if args.snapshot:
        try:
            snapshot = load_snapshot(args.snapshot)
        except Exception as exc:  # noqa: BLE001
            print(f"ERROR: cannot load snapshot {args.snapshot}: {exc}",
                  file=sys.stderr)
            return 2

    collect = {} if args.build_snapshot else None
    results, _ = scan(deps_subset=set(args.dep) or None, severity=args.severity,
                      refresh=args.refresh, offline=args.offline,
                      snapshot=snapshot, collect=collect)
    total = print_table(results, args.severity, fail_on=args.fail_on)

    os.makedirs(os.path.dirname(args.json) or ".", exist_ok=True)
    with open(args.json, "w") as f:
        json.dump({
            "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "severity_floor": args.severity,
            "total_findings": total,
            "results": results,
        }, f, indent=2)
    print(f"\nJSON report: {args.json}")

    if args.build_snapshot:
        snap = build_snapshot(collect, args.build_snapshot)
        n_cves = sum(len(v) for v in snap["cpes"].values())
        print(f"Snapshot written: {args.build_snapshot} "
              f"({len(snap['cpes'])} CPEs, {n_cves} CVEs)")

    # Ratchet: only --fail-on makes findings fatal. Default stays report-only.
    if args.fail_on and total > 0:
        # Re-evaluate at the requested gate severity (independent of --severity).
        gate_floor = SEVERITY_FLOOR[args.fail_on]
        gate_hits = sum(
            1 for r in results for f in r.get("findings", [])
            if (f.get("score") or 0) >= gate_floor)
        if gate_hits > 0:
            print(f"\nFAIL: {gate_hits} finding(s) at/above '{args.fail_on}' "
                  f"(--fail-on). Exiting nonzero.", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
