#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Unit tests for tools/dependency/cve_scan.py and validate-deps.py.

Covers the upstream-OSV-feed "fixed in pin" rule and the cve-ignore.yaml
`fixed_in` field (#1095): version comparison, OSV feed parsing (fixture trimmed
from https://curl.se/docs/vuln.json), the drop rule including curl's backport
ranges, the NVD-only fallback when the feed is absent or malformed, the
snapshot round trip, and the validator's cve-ignore.yaml checks: fixed_in
consistency and staleness of every entry (#1104).

Stdlib only (no pytest needed):
    python3 test/tools/dependency/cve_scan_test.py
"""

import contextlib
import importlib.util
import io
import json
import os
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
DEP_DIR = os.path.join(REPO_ROOT, "tools", "dependency")
FIXTURE = os.path.join(HERE, "testdata", "curl-vuln-trimmed.json")
FEED_URL = "https://curl.se/docs/vuln.json"

sys.path.insert(0, DEP_DIR)
import cve_scan  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    "validate_deps", os.path.join(DEP_DIR, "validate-deps.py"))
validate_deps = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(validate_deps)

V = cve_scan.parse_version


def load_fixture():
    with open(FIXTURE) as f:
        return json.load(f)


def feed_index():
    return cve_scan.osv_index(cve_scan.minimize_osv_feed(load_fixture()))


def nvd(cid, score=7.5, published="2026-09-06T00:00:00.000"):
    """A snapshot-shaped (minimized) NVD record."""
    return {"cve": {
        "id": cid, "published": published,
        "descriptions": [{"lang": "en", "value": f"{cid} description"}],
        "metrics": {"cvssMetricV31": [{"cvssData": {
            "baseScore": score, "baseSeverity": "HIGH"}}]},
    }}


REL_8_22 = datetime(2026, 9, 2, tzinfo=timezone.utc)


class VersionTest(unittest.TestCase):

    def test_numeric_not_lexicographic(self):
        self.assertLess(V("8.9.0"), V("8.10.0"))  # "8.9.0" > "8.10.0" as text
        self.assertLess(V("7.64.1"), V("8.0.0"))
        self.assertGreater(V("8.22.0"), V("8.21.99"))

    def test_trailing_zeros_equal(self):
        self.assertEqual(V("8.22"), V("8.22.0"))
        self.assertEqual(V("1.1.4"), V("1.1.4.0"))
        self.assertLess(V("8.22"), V("8.22.1"))

    def test_leading_v_and_zero(self):
        self.assertEqual(V("v1.37.5"), V("1.37.5"))
        self.assertEqual(V("0"), ())  # OSV `introduced: "0"`
        self.assertLess(V("0"), V("0.0.1"))

    def test_unorderable(self):
        for bad in ("2.1.12-stable", "f97695a50e11f5ff6719e129a466bf9204b64a7f",
                    "2025-11-05", "", "8..1", "8.x", None, 8):
            self.assertIsNone(V(bad), bad)


class OsvParseTest(unittest.TestCase):

    def test_fixture_minimizes(self):
        entries = cve_scan.minimize_osv_feed(load_fixture())
        self.assertEqual([e["id"] for e in entries], sorted(
            ["CURL-CVE-2026-19931", "CURL-CVE-2026-8286",
             "CURL-CVE-2026-5773", "CURL-CVE-2000-0973"]))
        e = next(x for x in entries if x["id"] == "CURL-CVE-2026-19931")
        self.assertEqual(e["aliases"], ["CVE-2026-19931"])
        # GIT range + versions/prose dropped; SEMVER events kept in feed order.
        self.assertEqual(len(e["ranges"]), 1)
        self.assertEqual(e["ranges"][0]["type"], "SEMVER")
        self.assertEqual(e["ranges"][0]["events"][-2:],
                         [{"introduced": "8.21.0"}, {"fixed": "8.22.0"}])
        self.assertEqual(set(e), {"id", "aliases", "ranges"})

    def test_deterministic(self):
        raw = load_fixture()
        a = cve_scan.minimize_osv_feed(raw)
        b = cve_scan.minimize_osv_feed(list(reversed(raw)))
        self.assertEqual(json.dumps(a, sort_keys=True),
                         json.dumps(b, sort_keys=True))

    def test_record_without_cve_alias_dropped(self):
        raw = load_fixture() + [{"id": "CURL-NOCVE-1", "aliases": [],
                                 "affected": []}]
        ids = [e["id"] for e in cve_scan.minimize_osv_feed(raw)]
        self.assertNotIn("CURL-NOCVE-1", ids)

    def test_cve_id_as_record_id(self):
        entries = cve_scan.minimize_osv_feed([{"id": "CVE-2030-1",
                                               "affected": []}])
        self.assertEqual(entries[0]["aliases"], ["CVE-2030-1"])

    def test_unparseable_shapes_raise(self):
        for bad in ({"not": "a list"}, "text", [1, 2], [],
                    [{"id": "X", "aliases": "CVE-1"}],
                    [{"id": "CVE-1", "affected": "nope"}]):
            with self.assertRaises(cve_scan.OsvFeedError, msg=repr(bad)):
                cve_scan.minimize_osv_feed(bad)

    def test_index_rejects_malformed_snapshot_entries(self):
        for bad in (None, [], "x", [{"aliases": "CVE-1", "ranges": []}],
                    [{"aliases": ["CVE-1"]}]):
            with self.assertRaises(cve_scan.OsvFeedError, msg=repr(bad)):
                cve_scan.osv_index(bad)


class DropRuleTest(unittest.TestCase):

    def setUp(self):
        self.idx = feed_index()

    def fixed(self, cve, pin):
        return cve_scan.osv_fixed_in_pin(self.idx.get(cve), pin)

    def test_fixed_in_the_pin(self):
        self.assertEqual(self.fixed("CVE-2026-19931", "8.22.0"), "8.22.0")
        self.assertEqual(self.fixed("CVE-2026-19931", "8.23.1"), "8.22.0")
        self.assertEqual(self.fixed("CVE-2026-5773", "8.22.0"), "8.20.0")

    def test_backport_below_pin_does_not_count(self):
        # 8.20.1 (a backport on the 8.20 branch) is <= 8.21.0, but 8.21.0 is
        # inside [introduced 8.21.0, fixed 8.22.0): affected, finding stands.
        self.assertIsNone(self.fixed("CVE-2026-19931", "8.21.0"))
        self.assertIsNone(self.fixed("CVE-2026-19931", "8.20.0"))
        # The backport release itself is fixed.
        self.assertEqual(self.fixed("CVE-2026-19931", "8.20.1"), "8.20.1")

    def test_empty_interval_on_tie(self):
        # CVE-2026-8286 lists `introduced 8.21.0, fixed 8.21.0` (empty).
        self.assertEqual(self.fixed("CVE-2026-8286", "8.21.0"), "8.21.0")
        self.assertIsNone(self.fixed("CVE-2026-8286", "8.20.0"))

    def test_unknown_or_unorderable_stands(self):
        self.assertIsNone(self.fixed("CVE-2099-0001", "8.22.0"))  # not in feed
        self.assertIsNone(self.fixed("CVE-2026-19931", "curl-8_22_0"))
        weird = [{"aliases": ["CVE-1"], "ranges": [{"type": "SEMVER", "events": [
            {"introduced": "0"}, {"fixed": "8.22.0-rc1"}]}]}]
        self.assertIsNone(cve_scan.osv_fixed_in_pin(weird, "8.22.0"))

    def test_before_introduced_is_not_fixed_in_pin(self):
        # Not affected, but there is no fix <= pin: not "fixed in pin".
        self.assertIsNone(self.fixed("CVE-2026-19931", "7.0.0"))

    def test_last_affected(self):
        e = [{"aliases": ["CVE-1"], "ranges": [{"type": "SEMVER", "events": [
            {"introduced": "1.0"}, {"last_affected": "1.4"}]}]}]
        # last_affected alone is not a fix event: never "fixed in pin".
        self.assertIsNone(cve_scan.osv_fixed_in_pin(e, "1.4"))
        self.assertIsNone(cve_scan.osv_fixed_in_pin(e, "1.5"))

    def test_any_record_affected_wins(self):
        a = {"aliases": ["CVE-1"], "ranges": [{"type": "SEMVER", "events": [
            {"introduced": "0"}, {"fixed": "2.0"}]}]}
        b = {"aliases": ["CVE-1"], "ranges": [{"type": "SEMVER", "events": [
            {"introduced": "2.5"}, {"fixed": "3.0"}]}]}
        self.assertEqual(cve_scan.osv_fixed_in_pin([a], "2.6"), "2.0")
        self.assertIsNone(cve_scan.osv_fixed_in_pin([a, b], "2.6"))


class EvaluateDepTest(unittest.TestCase):

    VULNS = [nvd("CVE-2026-19931", 9.8), nvd("CVE-2099-0001", 5.0),
             nvd("CVE-2026-5773", 7.5, published="2026-05-13T00:00:00.000")]

    def run_eval(self, version="8.22.0", ignores=None, idx="feed"):
        if idx == "feed":
            idx = feed_index()
        return cve_scan.evaluate_dep("curl", version, REL_8_22, self.VULNS,
                                     4.0, ignores or {}, idx, FEED_URL)

    def test_feed_drops_and_reports(self):
        findings, fixed, used, _ = self.run_eval()
        self.assertEqual([f["cve"] for f in findings], ["CVE-2099-0001"])
        self.assertEqual([(f["cve"], f["fixed"], f["feed"]) for f in fixed],
                         [("CVE-2026-19931", "8.22.0", FEED_URL)])
        self.assertEqual(used, [])

    def test_date_filter_still_first(self):
        # CVE-2026-5773 predates release_date: neither finding nor fixed row.
        findings, fixed, _, _ = self.run_eval()
        self.assertNotIn("CVE-2026-5773",
                         [f["cve"] for f in findings + fixed])

    def test_affected_pin_keeps_finding(self):
        findings, fixed, _, _ = self.run_eval(version="8.21.0")
        self.assertIn("CVE-2026-19931", [f["cve"] for f in findings])
        self.assertEqual(fixed, [])

    def test_no_feed_is_nvd_only(self):
        findings, fixed, _, _ = self.run_eval(idx=None)
        self.assertEqual(sorted(f["cve"] for f in findings),
                         ["CVE-2026-19931", "CVE-2099-0001"])
        self.assertEqual(fixed, [])

    def test_fixed_in_ignore(self):
        entry = {"cve": "CVE-2099-0001", "dep": "curl", "justification": "j",
                 "fixed_in": "8.22.0"}
        ign = {("CVE-2099-0001", "curl"): entry}
        findings, _, used, inapp = self.run_eval(ignores=ign)
        self.assertEqual(findings, [])
        self.assertEqual(used, ["CVE-2099-0001"])
        self.assertEqual(inapp, [])
        # Pin below fixed_in: the entry does not apply, the finding stands.
        findings, _, used, inapp = self.run_eval(version="8.21.0", ignores=ign)
        self.assertIn("CVE-2099-0001", [f["cve"] for f in findings])
        self.assertEqual(used, [])
        self.assertEqual(inapp, ["CVE-2099-0001"])

    def test_ignore_applies(self):
        f = cve_scan.ignore_applies
        self.assertTrue(f({"justification": "j"}, "deadbeef"))
        self.assertTrue(f({"fixed_in": "8.22.0"}, "8.22.0"))
        self.assertTrue(f({"fixed_in": "8.22.0"}, "8.22.1"))
        self.assertTrue(f({"fixed_in": "8.9.0"}, "8.10.0"))
        self.assertFalse(f({"fixed_in": "8.22.0"}, "8.21.0"))
        self.assertFalse(f({"fixed_in": "8.22.0"}, "deadbeef"))
        self.assertFalse(f({"fixed_in": "latest"}, "8.22.0"))


class ScanFallbackTest(unittest.TestCase):
    """scan() end to end on an in-memory snapshot, with repo inputs mocked."""

    CPE = "cpe:2.3:a:haxx:curl"

    def setUp(self):
        cpe_map = {"curl": {"cpe": self.CPE, "release_date": "2026-09-02",
                            "osv_feed": FEED_URL}}
        self.ignores = {}
        patches = [
            mock.patch.object(cve_scan, "load_cpe_map",
                              lambda *a, **k: cpe_map),
            mock.patch.object(cve_scan, "load_ignores",
                              lambda *a, **k: self.ignores),
            mock.patch.object(cve_scan, "load_cpp_deps",
                              lambda: [("curl", "8.22.0")]),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        self.cpes = {self.CPE: [nvd("CVE-2026-19931", 9.8)]}

    def scan(self, feeds):
        results, _ = cve_scan.scan(snapshot=self.cpes, snapshot_feeds=feeds)
        return results[0]

    def test_with_feed(self):
        r = self.scan({FEED_URL: cve_scan.minimize_osv_feed(load_fixture())})
        self.assertEqual(r["findings"], [])
        self.assertEqual([f["cve"] for f in r["fixed_in_pin"]],
                         ["CVE-2026-19931"])
        self.assertEqual(r["osv_feed_status"], "ok")

    def test_feed_absent_falls_back(self):
        for feeds in ({}, None):
            r = self.scan(feeds)
            self.assertEqual([f["cve"] for f in r["findings"]],
                             ["CVE-2026-19931"])
            self.assertEqual(r["fixed_in_pin"], [])
            self.assertIn("unavailable", r["osv_feed_status"])
            self.assertNotIn("error", r)  # evaluated, so --fail-on still gates

    def test_feed_malformed_falls_back(self):
        r = self.scan({FEED_URL: "garbage"})
        self.assertEqual([f["cve"] for f in r["findings"]], ["CVE-2026-19931"])
        self.assertIn("unavailable", r["osv_feed_status"])

    def test_report_prints_section_and_warning(self):
        ok = self.scan({FEED_URL: cve_scan.minimize_osv_feed(load_fixture())})
        bad = self.scan({})
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            cve_scan.print_table([ok], "medium")
        self.assertIn("Fixed in pin per upstream feed (1)", buf.getvalue())
        self.assertIn("fixed=8.22.0", buf.getvalue())
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            total = cve_scan.print_table([bad], "medium")
        self.assertEqual(total, 1)
        self.assertIn("WARNING: upstream OSV feed", buf.getvalue())


class SnapshotTest(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = os.path.join(self.tmp.name, "snap.json")
        self.cpes = {"cpe:2.3:a:haxx:curl": [nvd("CVE-2026-19931")]}
        self.entries = cve_scan.minimize_osv_feed(load_fixture())

    def build(self, feeds):
        with contextlib.redirect_stderr(io.StringIO()):
            return cve_scan.build_snapshot(self.cpes, self.path, feeds=feeds)

    def test_round_trip(self):
        self.build({FEED_URL: self.entries})
        cpes, feeds = cve_scan.load_snapshot_doc(self.path)
        self.assertEqual(cpes, self.cpes)
        self.assertEqual(feeds[FEED_URL], self.entries)
        with open(self.path) as f:
            self.assertEqual(json.load(f)["schema"], cve_scan.SNAPSHOT_SCHEMA)

    def test_failed_fetch_carries_previous_feed_forward(self):
        self.build({FEED_URL: self.entries})
        self.build({FEED_URL: None})
        self.assertEqual(cve_scan.load_snapshot_doc(self.path)[1][FEED_URL],
                         self.entries)

    def test_failed_fetch_without_previous_is_absent(self):
        self.build({FEED_URL: None})
        self.assertEqual(cve_scan.load_snapshot_doc(self.path)[1], {})

    def test_schema1_loads_without_feeds(self):
        with open(self.path, "w") as f:
            json.dump({"schema": 1, "cpes": self.cpes}, f)
        self.assertEqual(cve_scan.load_snapshot_doc(self.path),
                         (self.cpes, {}))

    def test_unknown_schema_rejected(self):
        with open(self.path, "w") as f:
            json.dump({"schema": 99, "cpes": {}}, f)
        with self.assertRaises(RuntimeError):
            cve_scan.load_snapshot_doc(self.path)


class IgnoreYamlTest(unittest.TestCase):

    def test_fixed_in_parsed(self):
        with tempfile.NamedTemporaryFile("w", suffix=".yaml",
                                         delete=False) as f:
            f.write('ignores:\n'
                    '  - cve: "CVE-1"\n'
                    '    dep: "curl"\n'
                    '    fixed_in: "8.22.0"   # comment\n'
                    '    justification: "fixed in the pin"\n'
                    '  - cve: "CVE-2"\n'
                    '    dep: "curl"\n'
                    '    justification: "not compiled"\n')
        self.addCleanup(os.unlink, f.name)
        ign = cve_scan.load_ignores(f.name)
        self.assertEqual(ign[("CVE-1", "curl")]["fixed_in"], "8.22.0")
        self.assertNotIn("fixed_in", ign[("CVE-2", "curl")])


class ValidateFixedInTest(unittest.TestCase):
    """validate-deps.py check_ignores on fixed_in entries: inconsistent /
    unorderable / stale."""

    CPE = "cpe:2.3:a:awesome:libmemcached"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.snap = os.path.join(self.tmp.name, "snap.json")
        with open(self.snap, "w") as f:
            json.dump({"schema": 2, "cpes": {self.CPE: [
                nvd("CVE-2023-27478", 9.8, "2023-03-07T00:00:00.000")]}}, f)
        self.cpe_map = {"libmemcached": {"cpe": self.CPE,
                                         "release_date": "2023-03-06"}}
        self.ignores = {}
        self.pins = {"libmemcached": "1.1.4"}
        patches = [
            mock.patch.object(cve_scan, "load_cpe_map",
                              lambda *a, **k: self.cpe_map),
            mock.patch.object(cve_scan, "load_ignores",
                              lambda *a, **k: self.ignores),
            mock.patch.object(cve_scan, "load_cpp_deps",
                              lambda: list(self.pins.items())),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def check(self, cve, fixed_in):
        self.ignores = {(cve, "libmemcached"): {
            "cve": cve, "dep": "libmemcached", "justification": "j",
            "fixed_in": fixed_in}}
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            ok = validate_deps.check_ignores(self.ignores, self.pins,
                                             self.cpe_map, self.snap)
        return ok, err.getvalue()

    def test_live_and_consistent(self):
        ok, err = self.check("CVE-2023-27478", "1.1.4")
        self.assertTrue(ok, err)

    def test_above_pin_is_inconsistent(self):
        ok, err = self.check("CVE-2023-27478", "1.1.5")
        self.assertFalse(ok)
        self.assertIn("INCONSISTENT", err)

    def test_unorderable(self):
        ok, err = self.check("CVE-2023-27478", "1.1.4-final")
        self.assertFalse(ok)
        self.assertIn("not both dotted-numeric", err)
        self.pins["libmemcached"] = "deadbeef"
        ok, err = self.check("CVE-2023-27478", "1.1.4")
        self.assertFalse(ok)

    def test_stale(self):
        ok, err = self.check("CVE-2099-0001", "1.1.4")  # matches no finding
        self.assertFalse(ok)
        self.assertIn("STALE", err)

    def test_unknown_dep(self):
        self.ignores = {("CVE-1", "nope"): {"cve": "CVE-1", "dep": "nope",
                                            "justification": "j",
                                            "fixed_in": "1.0"}}
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertFalse(validate_deps.check_ignores(
                self.ignores, self.pins, self.cpe_map, self.snap))


class ValidateStaleTest(unittest.TestCase):
    """validate-deps.py check_ignores on plain (no fixed_in) entries: every
    entry must still suppress a finding in the snapshot (#1104)."""

    CPE = "cpe:2.3:a:libevent_project:libevent"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.snap = os.path.join(self.tmp.name, "snap.json")
        with open(self.snap, "w") as f:
            json.dump({"schema": 2, "cpes": {self.CPE: [
                nvd("CVE-2026-0001", 7.5, "2026-08-20T00:00:00.000"),
                # Published before release_date: dropped by the date filter.
                nvd("CVE-2019-0001", 9.8, "2019-01-01T00:00:00.000"),
                # Below the medium floor.
                nvd("CVE-2026-0003", 2.0, "2026-08-20T00:00:00.000"),
            ]}}, f)
        self.cpe_map = {
            "libevent": {"cpe": self.CPE, "release_date": "2020-07-05"},
            "re2": {"cpe": "N/A", "release_date": "2024-07-02",
                    "justification": "no NVD CPE"},
        }
        self.pins = {"libevent": "2.1.12-stable", "re2": "2024-07-02"}
        self.ignores = {}
        patches = [
            mock.patch.object(cve_scan, "load_cpe_map",
                              lambda *a, **k: self.cpe_map),
            mock.patch.object(cve_scan, "load_ignores",
                              lambda *a, **k: self.ignores),
            mock.patch.object(cve_scan, "load_cpp_deps",
                              lambda: list(self.pins.items())),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def check(self, *keys):
        self.ignores = {(cve, dep): {"cve": cve, "dep": dep,
                                     "justification": "j"}
                        for cve, dep in keys}
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            ok = validate_deps.check_ignores(self.ignores, self.pins,
                                             self.cpe_map, self.snap)
        return ok, err.getvalue()

    def test_live_plain_entry_passes(self):
        ok, err = self.check(("CVE-2026-0001", "libevent"))
        self.assertTrue(ok, err)
        self.assertEqual(err, "")

    def test_cve_not_under_cpe_is_stale(self):
        # The #1104 case: NVD no longer lists the CVE under the dep's CPE.
        ok, err = self.check(("CVE-2026-63495", "libevent"))
        self.assertFalse(ok)
        self.assertIn("CVE-2026-63495 (libevent) is STALE", err)

    def test_date_filtered_is_stale(self):
        ok, err = self.check(("CVE-2019-0001", "libevent"))
        self.assertFalse(ok)
        self.assertIn("STALE", err)

    def test_below_floor_is_stale(self):
        ok, err = self.check(("CVE-2026-0003", "libevent"))
        self.assertFalse(ok)
        self.assertIn("STALE", err)

    def test_rejected_is_stale(self):
        with open(self.snap) as f:
            doc = json.load(f)
        doc["cpes"][self.CPE][0]["cve"]["vulnStatus"] = "Rejected"
        with open(self.snap, "w") as f:
            json.dump(doc, f)
        ok, err = self.check(("CVE-2026-0001", "libevent"))
        self.assertFalse(ok)
        self.assertIn("STALE", err)

    def test_only_the_stale_entry_is_reported(self):
        ok, err = self.check(("CVE-2026-0001", "libevent"),
                             ("CVE-2026-63380", "libevent"))
        self.assertFalse(ok)
        self.assertIn("CVE-2026-63380", err)
        self.assertNotIn("CVE-2026-0001", err)

    def test_na_dep_entry_is_stale(self):
        ok, err = self.check(("CVE-2026-0001", "re2"))
        self.assertFalse(ok)
        self.assertIn("N/A", err)

    def test_unknown_dep_plain_entry_fails(self):
        ok, err = self.check(("CVE-2026-0001", "nope"))
        self.assertFalse(ok)
        self.assertIn("not an annotated C/C++ dep", err)

    def test_unloadable_snapshot_fails(self):
        self.snap = os.path.join(self.tmp.name, "missing.json")
        ok, err = self.check(("CVE-2026-0001", "libevent"))
        self.assertFalse(ok)
        self.assertIn("cannot load snapshot", err)

    def test_no_entries_passes_without_snapshot(self):
        self.snap = os.path.join(self.tmp.name, "missing.json")
        ok, err = self.check()
        self.assertTrue(ok, err)


if __name__ == "__main__":
    unittest.main()
