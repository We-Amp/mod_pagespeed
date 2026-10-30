#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Controlled workload for the console verification rig.

Runs INSIDE the rig container (stdlib only). The expected numbers come from
what the client observes, never from a plan:

  gen --images DIR        create the fixture set on both vhosts: 6 CSS, 6 JS
                          and 12 images (png/jpeg/gif) taken from DIR, plus a
                          woff2/svg pair that cannot be optimized. One CSS and
                          one JS file are already minimal.
  run [--rounds K]        warm every URL until it is optimized, then issue K
                          requests per URL (twice that on the second vhost)
                          with Accept-Encoding br, gzip and identity, plus one
                          conditional (304) and one HEAD request per URL, and
                          record each response's Content-Length and
                          Content-Encoding. Snapshots the module and optimizer
                          statistics before and after, and writes
                          /var/lib/console-audit/expected.json.
  capture OUTDIR          save the raw JSON of every console leaf.
"""
import argparse
import http.client
import json
import os
import random
import shutil
import sys
import time

VHOSTS = {
    # name -> (port, Host header, document root)
    "a": (80, "site-a.test", "/var/www/html"),
    "b": (81, "site-b.test", "/var/www/site-b"),
}
STATE_DIR = "/var/lib/console-audit"
ASSET_DIR = "audit"
ENCODINGS = ["br", "gzip", "identity"]
IMAGE_SOURCES = [
    "Beach.jpg", "Puzzle.jpg", "Puzzle2.jpg", "OptPuzzle.jpg",
    "blocking_rewrite_test_dont_reuse_1.jpg", "BikeCrashIcn.png",
    "Cuppa.png", "pagespeed_logo.png", "IronChef2.gif", "CradleAnimation.gif",
    "PageSpeedAnimation.gif", "PageSpeedAnimationSmall.gif",
]


def type_of(name):
    ext = name.rsplit(".", 1)[-1].lower()
    if ext == "css":
        return "css"
    if ext == "js":
        return "js"
    if ext in ("png", "jpg", "jpeg", "gif", "webp"):
        return "image"
    return "other"


def gen_assets(images_dir):
    rng = random.Random(20261030)
    files = {}
    for i in range(1, 6):
        rules = []
        for r in range(40 * i):
            rules.append(
                "/* rule %d of sheet %d */\n.c%d-%d {\n    color : #%06x ;\n"
                "    margin : %dpx   %dpx ;\n}\n\n" % (r, i, i, r, rng.randrange(1 << 24), r % 9, r % 5))
        files["style%d.css" % i] = "".join(rules).encode()
    files["style6.css"] = b"a{color:red}"  # already minimal
    for i in range(1, 6):
        lines = []
        for r in range(30 * i):
            lines.append(
                "// helper %d of script %d\nfunction helper_%d_%d ( value ) {\n"
                "    var result = value * %d ;\n    return result ;\n}\n\n" % (r, i, i, r, r + 2))
        files["script%d.js" % i] = "".join(lines).encode()
    files["script6.js"] = b"var a=1;"  # already minimal
    for name in IMAGE_SOURCES:
        with open(os.path.join(images_dir, name), "rb") as f:
            files[name] = f.read()
    files["font.woff2"] = bytes(rng.randrange(256) for _ in range(4096))
    files["icon.svg"] = (
        b'<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">'
        b'<rect width="8" height="8"/></svg>')
    for _, _, root in VHOSTS.values():
        d = os.path.join(root, ASSET_DIR)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        for name, body in files.items():
            with open(os.path.join(d, name), "wb") as f:
                f.write(body)
        os.system("chmod -R a+rX %s" % d)
    return files


def request(port, host, path, method="GET", headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    h = {"Host": host}
    h.update(headers or {})
    conn.request(method, path, headers=h)
    resp = conn.getresponse()
    body = resp.read()  # http.client never decodes: len(body) is the transfer size
    out = {
        "status": resp.status,
        "bytes": len(body),
        "content_length": resp.getheader("Content-Length"),
        "content_encoding": resp.getheader("Content-Encoding") or "identity",
        "etag": resp.getheader("ETag"),
        "body": body,
    }
    conn.close()
    return out


def admin_json(port, host, path):
    r = request(port, host, path)
    try:
        return json.loads(r["body"].decode("utf-8", "replace").lstrip(")]}'\n"))
    except ValueError:
        return {"_status": r["status"], "_unparsed": True}


def ipro(variables):
    """Only the in-place counters: the full dump is captured as its own leaf."""
    return {k: v for k, v in variables.items() if k.startswith("ipro_daemon_")}


def snapshot():
    snap = {"taken_at": time.time(), "module": {}, "optimizer": {}}
    for name, (port, host, _) in VHOSTS.items():
        j = admin_json(port, host, "/pagespeed_admin/stats_json")
        snap["module"][name] = ipro(j.get("variables", j))
    snap["module"]["global"] = ipro(
        admin_json(80, "site-a.test", "/pagespeed_global_admin/stats_json").get("variables", {}))
    d = admin_json(80, "site-a.test", "/pagespeed_admin/v1/daemon/stats")
    snap["optimizer"] = {
        k: d.get(k) for k in ("serve_savings", "serve_savings_by_host", "serve_classes", "verdicts") if k in d
    }
    snap["optimizer_status_ok"] = "serve_savings" in d
    return snap


def settle(limit=40):
    """Wait until the optimizer's serve totals stop moving (they flush lazily)."""
    last = None
    for _ in range(limit):
        d = admin_json(80, "site-a.test", "/pagespeed_admin/v1/daemon/stats")
        cur = json.dumps([d.get("serve_savings"), d.get("serve_savings_by_host")], sort_keys=True)
        if cur == last:
            return
        last = cur
        time.sleep(1.5)


def run(rounds):
    os.makedirs(STATE_DIR, exist_ok=True)
    names = sorted(n for n in os.listdir(os.path.join(VHOSTS["a"][2], ASSET_DIR)))
    originals = {n: os.path.getsize(os.path.join(VHOSTS["a"][2], ASSET_DIR, n)) for n in names}

    # 1. Warm each URL, per vhost, until the identity answer stops changing
    # size (optimized) or the attempt budget runs out.
    optimized = {}
    for vname, (port, host, _) in VHOSTS.items():
        for n in names:
            path = "/%s/%s" % (ASSET_DIR, n)
            last, tries = None, 0
            while tries < 25:
                tries += 1
                r = request(port, host, path, headers={"Accept-Encoding": "identity"})
                if r["status"] != 200:
                    break
                if last is not None and r["bytes"] == last:
                    break
                last = r["bytes"]
                time.sleep(0.5)
            optimized[vname + ":" + n] = {"original": originals[n], "settled_bytes": last, "tries": tries}
        # A second pass per encoding so stored encodings can be produced.
        for _ in range(3):
            for n in names:
                for enc in ("br", "gzip"):
                    request(port, host, "/%s/%s" % (ASSET_DIR, n), headers={"Accept-Encoding": enc})
            time.sleep(1)
    time.sleep(2)

    before = snapshot()

    # 2. Measured requests.
    agg = {}
    conditional = {"requests": 0, "not_modified": 0}
    head = {"requests": 0}
    for vname, (port, host, _) in VHOSTS.items():
        k = rounds * (2 if vname == "b" else 1)
        for n in names:
            t = type_of(n)
            path = "/%s/%s" % (ASSET_DIR, n)
            etag = None
            for enc in ENCODINGS:
                for _ in range(k):
                    r = request(port, host, path, headers={"Accept-Encoding": enc})
                    if r["status"] != 200:
                        raise SystemExit("workload: %s %s -> %d" % (host, path, r["status"]))
                    cell = agg.setdefault(vname, {}).setdefault(t, {}).setdefault(
                        r["content_encoding"], {"hits": 0, "bytes": 0})
                    cell["hits"] += 1
                    cell["bytes"] += r["bytes"]
                    t_all = agg[vname][t].setdefault("_original_identity_bytes", 0)
                    agg[vname][t]["_original_identity_bytes"] = t_all + originals[n]
                    etag = r["etag"] or etag
            if etag:
                r = request(port, host, path, headers={"Accept-Encoding": "identity", "If-None-Match": etag})
                conditional["requests"] += 1
                conditional["not_modified"] += 1 if r["status"] == 304 else 0
            h = request(port, host, path, method="HEAD", headers={"Accept-Encoding": "identity"})
            head["requests"] += 1
    settle()
    after = snapshot()

    # 3. Which request kinds the optimizer counts as served: one probe each,
    # measured as a hit-count delta on a file the optimizer serves.
    probes = {}
    for kind in ("get", "conditional", "head"):
        port, host, _ = VHOSTS["a"]
        path = "/%s/style1.css" % ASSET_DIR
        etag = request(port, host, path, headers={"Accept-Encoding": "identity"})["etag"]
        settle()
        pre = snapshot()["optimizer"]["serve_savings"]["css"]["hits"]
        if kind == "get":
            request(port, host, path, headers={"Accept-Encoding": "identity"})
        elif kind == "conditional":
            request(port, host, path, headers={"Accept-Encoding": "identity", "If-None-Match": etag})
        else:
            request(port, host, path, method="HEAD", headers={"Accept-Encoding": "identity"})
        settle()
        probes[kind] = snapshot()["optimizer"]["serve_savings"]["css"]["hits"] - pre

    expected = {
        "rounds": rounds,
        "vhosts": {v: {"port": p, "host": h} for v, (p, h, _) in VHOSTS.items()},
        "assets": {"original_bytes": originals, "type": {n: type_of(n) for n in names}},
        "warm": optimized,
        "client": agg,
        "conditional": conditional,
        "head": head,
        "probe_hit_deltas": probes,
        "before": before,
        "after": after,
    }
    with open(os.path.join(STATE_DIR, "expected.json"), "w") as f:
        json.dump(expected, f, indent=1, sort_keys=True)
    print("workload: %d URLs x 2 vhosts, expected.json written" % len(names))


LEAVES = [
    # fixture name, port, path under the host, query
    ("stats_json.vhost-a", 80, "/pagespeed_admin/stats_json"),
    ("stats_json.vhost-b", 81, "/pagespeed_admin/stats_json"),
    ("stats_json.global", 80, "/pagespeed_global_admin/stats_json"),
    ("config.vhost-a", 80, "/pagespeed_admin/config"),
    ("config.global", 80, "/pagespeed_global_admin/config"),
    ("histograms.vhost-a", 80, "/pagespeed_admin/histograms"),
    ("cache.vhost-a", 80, "/pagespeed_admin/cache"),
    ("physical_caches.vhost-a", 80, "/pagespeed_admin/cache?physical_caches"),
    ("message_history.vhost-a", 80, "/pagespeed_admin/message_history"),
    ("message_history.vhost-b", 81, "/pagespeed_admin/message_history"),
    ("message_history.grouped.global", 80, "/pagespeed_global_admin/message_history?grouped=1&window_s=3600"),
    ("graphs.global", 80, "/pagespeed_global_admin/graphs?json=1"),
    ("console.global", 80, "/pagespeed_global_admin/console?json=1"),
    ("rule_packs.vhost-a", 80, "/pagespeed_admin/rule_packs?json=1&limit=1000"),
    ("rule_packs.global", 80, "/pagespeed_global_admin/rule_packs?json=1&limit=1000"),
    ("daemon_health.global", 80, "/pagespeed_global_admin/v1/daemon/health"),
    ("daemon_stats.global", 80, "/pagespeed_global_admin/v1/daemon/stats"),
    ("daemon_stats.vhost-a", 80, "/pagespeed_admin/v1/daemon/stats"),
    ("daemon_stats.vhost-b", 81, "/pagespeed_admin/v1/daemon/stats"),
    ("daemon_cooldowns.global", 80, "/pagespeed_global_admin/v1/daemon/cooldowns"),
    ("daemon_cache_urls.global", 80, "/pagespeed_global_admin/v1/daemon/cache/urls?limit=50"),
    ("daemon_logs.global", 80, "/pagespeed_global_admin/v1/daemon/logs?limit=100"),
]


def capture(outdir):
    os.makedirs(outdir, exist_ok=True)
    manifest = {}
    for name, port, path in LEAVES:
        host = "site-b.test" if port == 81 else "site-a.test"
        r = request(port, host, path)
        text = r["body"].decode("utf-8", "replace")
        stripped = text.lstrip(")]}'\n") if text.startswith(")]}") else text
        try:
            parsed = json.loads(stripped)
            fname, payload = name + ".json", json.dumps(parsed, indent=1, sort_keys=True) + "\n"
        except ValueError:
            fname, payload = name + ".txt", text
        with open(os.path.join(outdir, fname), "w") as f:
            f.write(payload)
        manifest[name] = {"file": fname, "path": path, "status": r["status"]}
    with open(os.path.join(outdir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
        f.write("\n")
    print("capture: %d leaves" % len(manifest))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen")
    g.add_argument("--images", required=True)
    r = sub.add_parser("run")
    r.add_argument("--rounds", type=int, default=3)
    c = sub.add_parser("capture")
    c.add_argument("outdir")
    a = ap.parse_args()
    if a.cmd == "gen":
        gen_assets(a.images)
    elif a.cmd == "run":
        run(a.rounds)
    else:
        capture(a.outdir)


if __name__ == "__main__":
    sys.exit(main())
