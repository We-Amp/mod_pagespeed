#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Apply OpenVEX not_affected/fixed suppressions to a grype JSON report, then
fail on any remaining finding at/above a severity cutoff.

Why this exists (issue: Security Gates repo-wide red):
grype's built-in ``--vex`` only works when the scan *source* is a directory or
image — it errors ``source type not supported for VEX`` on an ``sbom:`` source,
which is exactly how the Security Gates scan the committed SPDX SBOM. So we run
grype WITHOUT ``--vex`` and apply the OpenVEX statements here instead. The
OpenVEX file (``sbom/*.vex.json``, validated by validate-vex.py) stays the single
source of truth.

Matching is robust to two grype realities seen on the pinned SBOM:
  * grype may report a namespaced id (e.g. ``BIT-nginx-2026-48142``) whose CVE
    alias lives in ``relatedVulnerabilities`` — we match on the id OR any alias.
  * the artifact ``purl`` can be empty — we match the VEX product by package
    name (+ version when the VEX purl pins one), parsed from the product purl.
"""
import argparse
import json
import re
import sys

# grype severity ordering (Unknown ranks below the cutoff so it never trips
# medium+, but it is still reported).
_SEV_RANK = {
    "negligible": 0, "low": 1, "medium": 2, "high": 3, "critical": 4,
    "unknown": -1,
}


def _rank(sev):
    return _SEV_RANK.get((sev or "unknown").strip().lower(), -1)


def _parse_purl(purl):
    """pkg:TYPE[/NAMESPACE]/NAME@VERSION -> (name, version). Tolerant."""
    if not purl:
        return (None, None)
    body = purl.split("?", 1)[0]
    body = re.sub(r"^pkg:", "", body)
    name_ver = body.split("@", 1)
    version = name_ver[1] if len(name_ver) > 1 else None
    name = name_ver[0].rstrip("/").split("/")[-1]
    return (name.lower() or None, version)


def load_vex_suppressions(paths):
    """Return list of dicts: {vuln, name, version, justification} for
    not_affected / fixed statements."""
    out = []
    for path in paths:
        with open(path) as fh:
            doc = json.load(fh)
        for st in doc.get("statements", []):
            status = (st.get("status") or "").strip().lower()
            if status not in ("not_affected", "fixed"):
                continue
            vobj = st.get("vulnerability")
            if isinstance(vobj, dict):
                vuln = vobj.get("name")
            elif isinstance(vobj, str):
                vuln = vobj
            else:
                vuln = None
            if not vuln:
                continue
            just = st.get("justification") or st.get("impact_statement") or status
            for prod in st.get("products", []):
                pid = prod.get("@id") if isinstance(prod, dict) else prod
                name, version = _parse_purl(pid)
                out.append({"vuln": vuln.strip(), "name": name,
                            "version": version, "justification": just})
    return out


def match_ids(match):
    ids = set()
    v = match.get("vulnerability") or {}
    if v.get("id"):
        ids.add(v["id"].strip())
    for rel in match.get("relatedVulnerabilities", []) or []:
        if rel.get("id"):
            ids.add(rel["id"].strip())
    return ids


def suppresses(supp, ids, art_name, art_version):
    if supp["vuln"] not in ids:
        return False
    if supp["name"] and supp["name"] != (art_name or "").lower():
        return False
    # A VEX purl that pins a version only suppresses that version.
    if supp["version"] and art_version and supp["version"] != art_version:
        return False
    return True


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--grype", required=True, help="grype JSON report")
    ap.add_argument("--vex", action="append", default=[],
                    help="OpenVEX file (repeatable; optional)")
    ap.add_argument("--fail-on", default="medium",
                    help="severity cutoff: negligible|low|medium|high|critical")
    args = ap.parse_args(argv)

    with open(args.grype) as fh:
        report = json.load(fh)
    matches = report.get("matches", []) or []
    supps = load_vex_suppressions([p for p in args.vex if p])
    cutoff = _rank(args.fail_on)

    suppressed, remaining = [], []
    for m in matches:
        v = m.get("vulnerability") or {}
        a = m.get("artifact") or {}
        ids = match_ids(m)
        name, version = a.get("name"), a.get("version")
        hit = next((s for s in supps if suppresses(s, ids, name, version)), None)
        row = {"id": v.get("id"), "sev": v.get("severity"),
               "pkg": f"{name}@{version}", "aliases": sorted(ids - {v.get('id')})}
        if hit:
            row["justification"] = hit["justification"]
            suppressed.append(row)
        else:
            remaining.append(row)

    print(f"grype matches: {len(matches)}  vex-suppressed: {len(suppressed)}  "
          f"remaining: {len(remaining)}  (fail-on: {args.fail_on})")
    if suppressed:
        print("\n-- VEX-suppressed (documented not_affected/fixed) --")
        for r in suppressed:
            al = f" [aka {', '.join(r['aliases'])}]" if r["aliases"] else ""
            print(f"  SUPPRESSED {r['id']}{al} {r['sev']} {r['pkg']}")
            print(f"             ↳ {r['justification']}")
    if remaining:
        print("\n-- Remaining findings --")
        for r in remaining:
            al = f" [aka {', '.join(r['aliases'])}]" if r["aliases"] else ""
            print(f"  {r['sev']:9} {r['id']}{al} {r['pkg']}")

    failing = [r for r in remaining if _rank(r["sev"]) >= cutoff]
    if failing:
        print(f"\nFAIL: {len(failing)} finding(s) at/above '{args.fail_on}' "
              f"not covered by VEX:")
        for r in failing:
            print(f"  {r['sev']} {r['id']} {r['pkg']}")
        return 1
    print(f"\nPASS: no unsuppressed findings at/above '{args.fail_on}'.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
