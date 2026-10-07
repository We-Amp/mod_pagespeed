#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
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
  5. minus CVEs the dep's own authoritative OSV feed (cpe-map.yaml `osv_feed`,
     e.g. curl's https://curl.se/docs/vuln.json) records as FIXED at or before
     the pinned version — reported in a separate "fixed in pin per upstream
     feed" section, never silently dropped,
  6. minus the cve-ignore.yaml suppression list (each entry justified; an entry
     with `fixed_in` applies only while the pinned version is >= fixed_in).

Why step 5: NVD publishes upstream advisories days after the upstream release,
so a CVE fixed BY the pinned release is PUBLISHED after its release_date and the
date filter keeps it. The upstream feed is the authority on which versions
carry the fix, so it retires those findings without a hand-written suppression
per CVE. If the feed is missing from the snapshot or cannot be parsed, the
scanner falls back to the NVD findings (+ ignore list) and says so.

Report-only (v1): exits 0 by default. A per-dep findings table is printed and a
machine-readable JSON report is written (default: tools/dependency/.cve-cache/
report.json, gitignored). The completeness gate (validate-deps.py) is the
blocking part. Pass --fail-on <severity> to flip to blocking (the per-PR ratchet
— see the workflow).

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
import urllib.error
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
# Schema 2 = schema 1 + the optional top-level `osv_feeds` map. A schema-1 file
# still loads (it simply carries no feeds, so every dep falls back to NVD-only).
SNAPSHOT_SCHEMA = 2
SNAPSHOT_SCHEMAS_READABLE = (1, 2)

NVD_API = "https://services.nvd.nist.gov/rest/json/cves/2.0"
# NVD asks unauthenticated callers to keep to ~5 requests per rolling 30s.
NVD_SLEEP_SECONDS = 7.0
NVD_PAGE_SIZE = 2000  # NVD 2.0 max resultsPerPage
# NVD rate-limits (HTTP 429) and intermittently 503s. Without backoff a single
# 429 used to drop a CPE silently from the rebuilt snapshot (the 19->4->0
# degradation incident). Retry with exponential backoff, honoring Retry-After.
NVD_MAX_RETRIES = 6
NVD_BACKOFF_BASE_SECONDS = 8.0

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
    """Parse cve-ignore.yaml -> {(cve, dep): entry}.

    `entry` is the entry's field dict (cve, dep, justification, and the optional
    reference / expires / fixed_in). Enforces that every entry carries a
    non-empty justification.
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
    ignores[(cve, dep)] = entry


# ---------------------------------------------------------------------------
# NVD fetch + cache
# ---------------------------------------------------------------------------
class NvdUnreachable(RuntimeError):
    """Raised when NVD cannot be reached / refresh cannot complete.

    Distinct from a generic RuntimeError (e.g. the no-shrink guard tripping on a
    real data regression) so the daily REPORT-ONLY job can soft-skip on a
    transient upstream outage (rate-limit/503/network) without reddening, while
    genuine logic errors still fail loudly. Subclasses RuntimeError so any caller
    that already fails closed on RuntimeError keeps that behavior. main() maps
    ONLY this exception to exit code 3 (NVD unreachable / refresh skipped).
    """


def cache_path(cpe):
    safe = cpe.replace(":", "_").replace("*", "x").replace("/", "_")
    return os.path.join(CACHE_DIR, f"nvd_{safe}.json")


def _nvd_get(url, headers):
    """GET an NVD URL, retrying on 429/503 with exponential backoff.

    Raises NvdUnreachable after NVD_MAX_RETRIES so a persistently-failing fetch
    surfaces as a distinct, recognizable error (caller fails closed / the daily
    report-only job soft-skips) rather than silently dropping a CPE.
    """
    last_exc = None
    for attempt in range(NVD_MAX_RETRIES):
        req = urllib.request.Request(url, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                return json.loads(resp.read().decode())
        except urllib.error.HTTPError as exc:
            last_exc = exc
            if exc.code not in (429, 503):
                raise
            retry_after = exc.headers.get("Retry-After") if exc.headers else None
            try:
                delay = float(retry_after) if retry_after else None
            except ValueError:
                delay = None
            if delay is None:
                delay = NVD_BACKOFF_BASE_SECONDS * (2 ** attempt)
            print(f"  NVD {exc.code} for {url} — retry {attempt + 1}/"
                  f"{NVD_MAX_RETRIES} in {delay:.0f}s", file=sys.stderr)
            time.sleep(delay)
        except urllib.error.URLError as exc:
            last_exc = exc
            delay = NVD_BACKOFF_BASE_SECONDS * (2 ** attempt)
            print(f"  NVD network error for {url} ({exc}) — retry "
                  f"{attempt + 1}/{NVD_MAX_RETRIES} in {delay:.0f}s",
                  file=sys.stderr)
            time.sleep(delay)
    raise NvdUnreachable(f"NVD fetch failed after {NVD_MAX_RETRIES} retries: "
                         f"{url} ({last_exc})")


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
        data = _nvd_get(url, headers)
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
# Versions + upstream OSV feeds
# ---------------------------------------------------------------------------
def parse_version(text):
    """Parse a dotted-numeric version ("8.22.0", "v1.2") -> comparable tuple.

    Trailing zero components are dropped so "8.22" == "8.22.0". Returns None for
    anything that is not purely dotted numerics (commit hashes, "2.1.12-stable",
    dates with dashes, ...): such a version cannot be ordered, so every rule
    that needs an ordering treats it as "unknown" and fails safe (the finding
    stands). OSV's special `introduced: "0"` parses to (0,) -> ().
    """
    if not isinstance(text, str):
        return None
    t = text.strip()
    if t[:1] in ("v", "V"):
        t = t[1:]
    parts = t.split(".")
    if not t or any(not p.isdigit() for p in parts):
        return None
    nums = [int(p) for p in parts]
    while nums and nums[-1] == 0:
        nums.pop()
    return tuple(nums)


class OsvFeedError(RuntimeError):
    """An upstream OSV feed is missing, unreachable or not in the OSV shape.

    Never fatal: the scanner falls back to NVD-only findings for that dep.
    """


OSV_RANGE_TYPES = ("SEMVER", "ECOSYSTEM")
OSV_EVENT_KINDS = ("introduced", "fixed", "last_affected")


def minimize_osv_feed(raw):
    """Prune a raw OSV feed (a JSON array of OSV records) to what we read.

    Per record: `id`, the CVE ids it covers (`aliases`, plus `id` itself if it
    is a CVE id), and every SEMVER/ECOSYSTEM range's events (introduced / fixed
    / last_affected). GIT ranges, `versions` lists, credits and prose are
    dropped. Records with no CVE id are dropped. Deterministic: records sorted
    by id, aliases sorted, event order preserved (it carries meaning on ties).
    Raises OsvFeedError if the payload is not an OSV array.
    """
    if not isinstance(raw, list):
        raise OsvFeedError("OSV feed is not a JSON array of records")
    out = []
    for rec in raw:
        if not isinstance(rec, dict):
            raise OsvFeedError("OSV feed record is not an object")
        rid = rec.get("id", "")
        aliases = rec.get("aliases") or []
        if not isinstance(aliases, list):
            raise OsvFeedError(f"OSV record {rid}: aliases is not a list")
        cves = {a for a in aliases if isinstance(a, str) and a.startswith("CVE-")}
        if isinstance(rid, str) and rid.startswith("CVE-"):
            cves.add(rid)
        if not cves:
            continue
        ranges = []
        affected = rec.get("affected") or []
        if not isinstance(affected, list):
            raise OsvFeedError(f"OSV record {rid}: affected is not a list")
        for aff in affected:
            if not isinstance(aff, dict):
                raise OsvFeedError(f"OSV record {rid}: affected[] not an object")
            for rng in aff.get("ranges") or []:
                if not isinstance(rng, dict) or rng.get("type") not in OSV_RANGE_TYPES:
                    continue
                events = []
                for ev in rng.get("events") or []:
                    if not isinstance(ev, dict):
                        raise OsvFeedError(f"OSV record {rid}: event not an object")
                    for kind in OSV_EVENT_KINDS:
                        if kind in ev:
                            events.append({kind: str(ev[kind])})
                ranges.append({"type": rng["type"], "events": events})
        out.append({"id": rid, "aliases": sorted(cves), "ranges": ranges})
    if not out:
        raise OsvFeedError("OSV feed carries no CVE-aliased records")
    out.sort(key=lambda r: r["id"])
    return out


def osv_index(entries):
    """Minimized feed entries -> {CVE id: [entry, ...]}.

    Raises OsvFeedError if the (snapshot-stored) entries are malformed, so a
    corrupted snapshot feed falls back instead of crashing or passing.
    """
    if not isinstance(entries, list) or not entries:
        raise OsvFeedError("feed entries missing or empty")
    idx = {}
    for e in entries:
        if (not isinstance(e, dict) or not isinstance(e.get("aliases"), list)
                or not isinstance(e.get("ranges"), list)):
            raise OsvFeedError("malformed feed entry in snapshot")
        for cve in e["aliases"]:
            idx.setdefault(cve, []).append(e)
    return idx


def osv_fixed_in_pin(entries, pinned):
    """Return the fix version if the feed shows `pinned` is FIXED, else None.

    OSV range semantics (ossf.github.io/osv-schema, "Evaluation"): walk a
    range's events in version order; `introduced` <= v marks v affected,
    `fixed` <= v marks it not affected, `last_affected` < v marks it not
    affected. Events at the same version keep the feed's order (curl writes an
    empty interval as introduced X, fixed X).

    Returns the highest `fixed` version <= pinned ONLY when every usable range
    of every record for this CVE leaves `pinned` NOT affected and at least one
    `fixed` event is <= pinned. That is stricter than "some fixed <= pinned":
    curl backports fixes (e.g. fixed 8.20.1 on the 8.20 branch while 8.21.0 is
    still affected), so a lower fixed event alone proves nothing. Anything the
    rule cannot order (unparseable pin or event version, no usable range)
    returns None: the finding stands.
    """
    pv = parse_version(pinned)
    if pv is None or not entries:
        return None
    best = None
    for e in entries:
        for rng in e.get("ranges", []):
            walked = []
            for ev in rng.get("events", []):
                (kind, raw), = ev.items()
                ver = parse_version(raw)
                if ver is None:
                    return None  # an event we cannot order: can't prove fixed
                walked.append((ver, kind, raw))
            if not walked:
                continue
            walked.sort(key=lambda w: w[0])  # stable: ties keep feed order
            affected = False
            for ver, kind, _ in walked:
                if kind == "introduced" and pv >= ver:
                    affected = True
                elif kind == "fixed" and pv >= ver:
                    affected = False
                elif kind == "last_affected" and pv > ver:
                    affected = False
            if affected:
                return None
            for ver, kind, raw in walked:
                if kind == "fixed" and ver <= pv and (best is None or ver > best[0]):
                    best = (ver, raw)
    return best[1] if best else None


def osv_cache_path(url):
    safe = "".join(ch if ch.isalnum() else "_" for ch in url)
    return os.path.join(CACHE_DIR, f"osv_{safe}.json")


def fetch_osv_feed(url, refresh=False, offline=False):
    """Fetch an upstream OSV feed (cached raw under .cve-cache/) -> minimized.

    Raises OsvFeedError on any failure (network, HTTP, JSON, shape).
    """
    cp = osv_cache_path(url)
    raw = None
    if not refresh and os.path.exists(cp):
        try:
            with open(cp) as f:
                raw = json.load(f)
        except (OSError, ValueError):
            raw = None
    if raw is None:
        if offline:
            raise OsvFeedError(f"--offline and no cache for {url}")
        req = urllib.request.Request(
            url, headers={"User-Agent": "mod_pagespeed-cve-scan"})
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                raw = json.loads(resp.read().decode())
        except (urllib.error.URLError, OSError, ValueError) as exc:
            raise OsvFeedError(f"fetch {url}: {exc}") from exc
        os.makedirs(CACHE_DIR, exist_ok=True)
        with open(cp, "w") as f:
            json.dump(raw, f)
    return minimize_osv_feed(raw)


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


def build_snapshot(cpe_to_vulns, path=SNAPSHOT_PATH, allow_shrink=False,
                   feeds=None):
    """Write the deterministic compact snapshot from {cpe: [raw vulns]}.

    `feeds` is {feed_url: minimized entries, or None if this run could not
    fetch it}. A feed that failed is CARRIED FORWARD from the existing snapshot
    (an older copy of an upstream feed can only list fewer fixes, so it never
    retires more findings than a fresh one would); a feed never fetched
    successfully is simply absent, and the scan falls back to NVD-only for it.

    No-shrink guard: refuse to overwrite an existing snapshot with one covering
    FEWER CPEs unless allow_shrink=True. A live rebuild that lost CPEs to NVD
    rate-limiting (or a removed dep) must not silently replace a complete mirror
    with a degraded one — that is exactly how the committed snapshot decayed
    19->4->0 CPEs and blinded the gate. A legitimate CPE removal passes
    --allow-snapshot-shrink explicitly.
    """
    new_cpes = set(cpe_to_vulns)
    old_feeds = {}
    if os.path.exists(path):
        try:
            _, old_feeds = load_snapshot_doc(path)
        except Exception:  # noqa: BLE001  (unreadable/old-schema -> no carry)
            old_feeds = {}
    if not allow_shrink and os.path.exists(path):
        try:
            old_cpes = set(load_snapshot(path))
        except Exception:  # noqa: BLE001  (unreadable/old-schema -> no guard)
            old_cpes = set()
        missing = old_cpes - new_cpes
        if missing:
            raise RuntimeError(
                f"refusing to shrink snapshot {path}: {len(new_cpes)} CPEs "
                f"would replace {len(old_cpes)} — dropping {sorted(missing)}. "
                f"Likely an NVD fetch failure (a CPE silently lost). Re-run, or "
                f"pass --allow-snapshot-shrink if the CPE was intentionally "
                f"removed.")
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
    out_feeds = {}
    for url, entries in sorted((feeds or {}).items()):
        if entries is None:
            if url in old_feeds:
                print(f"  WARNING: OSV feed {url} not refreshed this run — "
                      f"carrying the previous snapshot copy forward",
                      file=sys.stderr)
                out_feeds[url] = old_feeds[url]
            continue
        out_feeds[url] = entries
    if out_feeds:
        snap["osv_feeds"] = out_feeds
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w") as f:
        json.dump(snap, f, indent=1, sort_keys=True)
        f.write("\n")
    return snap


def load_snapshot_doc(path=SNAPSHOT_PATH):
    """Load the committed snapshot -> ({cpe: [vulns]}, {feed_url: entries}).

    The feeds map is empty for a schema-1 snapshot (or one whose daily refresh
    never captured a feed); the scan then falls back to NVD-only per dep.
    """
    with open(path) as f:
        snap = json.load(f)
    schema = snap.get("schema")
    if schema not in SNAPSHOT_SCHEMAS_READABLE:
        raise RuntimeError(
            f"snapshot {path} schema {schema} not in "
            f"{SNAPSHOT_SCHEMAS_READABLE}; rebuild with --refresh "
            f"--build-snapshot")
    feeds = snap.get("osv_feeds", {})
    if not isinstance(feeds, dict):
        feeds = {}
    return snap.get("cpes", {}), feeds


def load_snapshot(path=SNAPSHOT_PATH):
    """Load the committed snapshot -> {cpe: [vulns]} (matcher-shaped)."""
    return load_snapshot_doc(path)[0]


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
def ignore_applies(entry, version):
    """Whether an ignore entry is in force for the pinned `version`.

    An entry without `fixed_in` always applies. With `fixed_in`, it applies
    only while the pinned version is >= fixed_in; an unparseable fixed_in or
    pin cannot be ordered, so the entry does NOT apply (the finding stands and
    validate-deps.py reports the entry as inconsistent).
    """
    fixed_in = entry.get("fixed_in")
    if not fixed_in:
        return True
    fv, pv = parse_version(fixed_in), parse_version(version)
    return fv is not None and pv is not None and pv >= fv


def evaluate_dep(name, version, rel_dt, vulns, floor, ignores, feed_index=None,
                 feed_url=None):
    """Filter one dep's candidate CVEs. Pure: no I/O.

    Order: Rejected -> severity floor -> publish-date filter -> upstream OSV
    feed ("fixed in pin", only when `feed_index` is not None) -> ignore list.
    Returns (findings, fixed_in_pin, ignores_used, ignores_inapplicable).
    """
    findings, fixed, used, inapplicable = [], [], [], []
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
        row = {
            "cve": cid,
            "score": score,
            "severity": sev,
            "published": cve.get("published", ""),
            "description": cve_description(cve)[:240],
        }
        if feed_index is not None:
            fix = osv_fixed_in_pin(feed_index.get(cid), version)
            if fix is not None:
                fixed.append(dict(row, fixed=fix, feed=feed_url))
                continue
        entry = ignores.get((cid, name))
        if entry is not None:
            if ignore_applies(entry, version):
                used.append(cid)
                continue
            inapplicable.append(cid)
        findings.append(row)
    findings.sort(key=lambda f: (-(f["score"] or 0), f["cve"]))
    fixed.sort(key=lambda f: (-(f["score"] or 0), f["cve"]))
    return findings, fixed, sorted(used), sorted(inapplicable)


def scan(deps_subset=None, severity="medium", refresh=False, offline=False,
         snapshot=None, collect=None, snapshot_feeds=None, collect_feeds=None):
    """Match every annotated dep against NVD data and date/severity-filter.

    Data source per CPE:
      * snapshot is not None  -> read snapshot[cpe] (the committed mirror; offline)
      * else                  -> fetch_nvd(cpe, refresh, offline)
    Upstream OSV feed per dep (cpe-map.yaml `osv_feed`):
      * snapshot is not None  -> snapshot_feeds[url] (absent -> fallback)
      * else                  -> fetch_osv_feed(url, refresh, offline)
    A feed that is absent, unreachable or malformed never fails the scan and
    never passes it silently: that dep falls back to NVD findings + ignore list
    and the result carries `osv_feed_status` explaining why.
    If `collect` is a dict, the raw vulns fetched per CPE are stashed into it
    (collect[cpe] = vulns) so the caller can build a fresh snapshot in one pass;
    `collect_feeds` likewise gets {url: minimized entries, or None on failure}.
    """
    cpe_map = load_cpe_map()
    ignores = load_ignores()
    floor = SEVERITY_FLOOR[severity]
    versions = dict(load_cpp_deps())
    if snapshot is not None and snapshot_feeds is None:
        snapshot_feeds = {}

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
        except NvdUnreachable:
            # Upstream outage (rate-limit/503/network): abandon the live refresh
            # immediately rather than burning the full backoff budget on every
            # remaining CPE and then silently shrinking the snapshot. main()
            # maps this to a soft-skip (exit 3); the committed snapshot is kept.
            raise
        except Exception as exc:  # noqa: BLE001  (report-only: never crash)
            results.append({"dep": name, "version": version, "cpe": cpe,
                            "release_date": rel, "error": str(exc),
                            "findings": []})
            continue

        feed_url = meta.get("osv_feed") or None
        feed_index = None
        feed_status = None
        if feed_url:
            entries = None
            try:
                if snapshot is not None:
                    entries = snapshot_feeds.get(feed_url)
                    if entries is None:
                        raise OsvFeedError("feed not in snapshot")
                else:
                    entries = fetch_osv_feed(feed_url, refresh=refresh,
                                             offline=offline)
                feed_index = osv_index(entries)
                feed_status = "ok"
            except OsvFeedError as exc:
                entries = None
                feed_status = f"unavailable ({exc}); NVD-only fallback"
            if collect_feeds is not None and snapshot is None:
                collect_feeds[feed_url] = entries

        findings, fixed, used, inapplicable = evaluate_dep(
            name, version, rel_dt, vulns, floor, ignores, feed_index, feed_url)
        res = {
            "dep": name, "version": version, "cpe": cpe,
            "release_date": rel, "candidate_cves": len(vulns),
            "findings": findings,
            "fixed_in_pin": fixed,
            "ignores_used": used,
        }
        if inapplicable:
            res["ignores_inapplicable"] = inapplicable
        if feed_url:
            res["osv_feed"] = feed_url
            res["osv_feed_status"] = feed_status
        results.append(res)
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
        status = r.get("osv_feed_status")
        if status and status != "ok":
            print(f"      WARNING: upstream OSV feed {r['osv_feed']} {status}")
        for f in fs:
            print(f"      {f['cve']}  CVSS {f['score']} ({f['severity']})  "
                  f"{f['published'][:10]}")
            if f["description"]:
                print(f"         {f['description'][:96]}")
        for cid in r.get("ignores_inapplicable", []):
            print(f"      note: cve-ignore.yaml entry for {cid} not applied "
                  f"(pinned {ver} is below its fixed_in, or unorderable)")
    fixed_rows = [(r, f) for r in results for f in r.get("fixed_in_pin", [])]
    if fixed_rows:
        print("\n" + "-" * width)
        print(f"  Fixed in pin per upstream feed ({len(fixed_rows)}): published "
              f"after release_date, but the dep's")
        print("  own OSV feed records the pinned version as fixed. Not counted "
              "as findings.")
        print("-" * width)
        for r, f in fixed_rows:
            print(f"      {r['dep']} {r.get('version', '?')}  {f['cve']}  "
                  f"CVSS {f['score']} ({f['severity']})  "
                  f"{f['published'][:10]}  fixed={f['fixed']}")
    print("\n" + "=" * width)
    print(f"  TOTAL post-release findings (severity >= {severity}): {total_findings}")
    if fixed_rows:
        print(f"  Fixed in pin per upstream feed (not findings): {len(fixed_rows)}")
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
    ap.add_argument("--allow-snapshot-shrink", action="store_true",
                    help="permit --build-snapshot to write FEWER CPEs than the "
                         "existing committed snapshot (default: refused — a "
                         "shrink is almost always an NVD fetch failure). Use "
                         "only for an intentional CPE removal.")
    ap.add_argument("--fail-on", choices=list(SEVERITY_FLOOR), default=None,
                    help="RATCHET TOGGLE: exit nonzero if any finding at/above "
                         "this severity exists (default: unset = report-only). "
                         "Flip the per-PR job to blocking by setting this. "
                         "Fail-closed: also exits nonzero if any dep could not "
                         "be evaluated (missing from snapshot / fetch error).")
    ap.add_argument("--json", default=DEFAULT_REPORT,
                    help=f"write JSON report here (default: {DEFAULT_REPORT})")
    args = ap.parse_args()

    snapshot = None
    snapshot_feeds = None
    if args.snapshot:
        try:
            snapshot, snapshot_feeds = load_snapshot_doc(args.snapshot)
        except Exception as exc:  # noqa: BLE001
            print(f"ERROR: cannot load snapshot {args.snapshot}: {exc}",
                  file=sys.stderr)
            return 2

    collect = {} if args.build_snapshot else None
    collect_feeds = {} if args.build_snapshot else None
    try:
        results, _ = scan(deps_subset=set(args.dep) or None,
                          severity=args.severity, refresh=args.refresh,
                          offline=args.offline, snapshot=snapshot,
                          collect=collect, snapshot_feeds=snapshot_feeds,
                          collect_feeds=collect_feeds)
    except NvdUnreachable as exc:
        # Soft-skip (exit 3): NVD was unreachable/rate-limited and the live
        # refresh could not complete. The committed snapshot is left untouched;
        # the caller (the daily report-only job) treats exit 3 as a warning, not
        # a failure, and skips committing a partial/empty snapshot. Genuine logic
        # errors do NOT reach here — they keep raising / returning nonzero below.
        print(f"\nNVD unreachable: {exc}", file=sys.stderr)
        print("Snapshot refresh could not complete; the committed snapshot is "
              "retained unchanged. (exit 3 = report-only soft-skip)",
              file=sys.stderr)
        return 3
    total = print_table(results, args.severity, fail_on=args.fail_on)

    os.makedirs(os.path.dirname(args.json) or ".", exist_ok=True)
    with open(args.json, "w") as f:
        json.dump({
            "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "severity_floor": args.severity,
            "total_findings": total,
            "total_fixed_in_pin": sum(
                len(r.get("fixed_in_pin", [])) for r in results),
            "results": results,
        }, f, indent=2)
    print(f"\nJSON report: {args.json}")

    if args.build_snapshot:
        try:
            snap = build_snapshot(collect, args.build_snapshot,
                                  allow_shrink=args.allow_snapshot_shrink,
                                  feeds=collect_feeds)
        except NvdUnreachable as exc:  # defensive: scan() raises first in practice
            print(f"\nNVD unreachable: {exc}", file=sys.stderr)
            print("Committed snapshot retained unchanged. "
                  "(exit 3 = report-only soft-skip)", file=sys.stderr)
            return 3
        except RuntimeError as exc:
            # Genuine data regression (the no-shrink guard tripping on a real CPE
            # loss / removal that was NOT caused by an upstream outage) or another
            # build error. This MUST fail loudly — it is exactly the degradation
            # the guard exists to catch. Exit 1, not the soft-skip 3.
            print(f"\nERROR: snapshot build failed: {exc}", file=sys.stderr)
            return 1
        n_cves = sum(len(v) for v in snap["cpes"].values())
        print(f"Snapshot written: {args.build_snapshot} "
              f"({len(snap['cpes'])} CPEs, {n_cves} CVEs, "
              f"{len(snap.get('osv_feeds', {}))} OSV feed(s))")

    # Ratchet: only --fail-on makes the gate blocking. Default stays report-only.
    if args.fail_on:
        # Fail-closed: a dep we could not evaluate (CPE missing from the
        # snapshot, NVD fetch error, no cpe-map entry) is an UNKNOWN, not a
        # pass. Treating it as clean is exactly how an empty/degraded snapshot
        # silently disabled this gate. A blocking run must be able to evaluate
        # every dep.
        unevaluated = [r["dep"] for r in results if r.get("error")]
        if unevaluated:
            print(f"\nFAIL (fail-closed): {len(unevaluated)} dep(s) could not be "
                  f"evaluated under --fail-on: {', '.join(unevaluated)}. "
                  f"The snapshot is incomplete or NVD was unreachable — refusing "
                  f"to report clean. Rebuild the snapshot (--refresh "
                  f"--build-snapshot) and retry.", file=sys.stderr)
            return 1
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
