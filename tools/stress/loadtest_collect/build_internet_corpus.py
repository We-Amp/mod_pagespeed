#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""build_internet_corpus.py — assemble an internet-variety replay corpus for the
mod_pagespeed shutdown-race stress harness.

WHY: the 99-page single-origin corpus proved the shutdown UAF fix on real
content; this widens the input distribution to ~1000 sites so the CSS/HTML
parsers (where the shutdown statics live — spdlog logger, CSS `kClass`/`kId`)
get hit with a representative slice of the real web, not one site's house style.

WHAT: fetch the Tranco top-N homepages ONCE (single pass, polite, identified UA)
plus a few stylesheets/scripts each, into a FLATTENED docroot that ONE localhost
vhost can serve. Captured `<link rel=stylesheet>` / `<script src>` refs are
rewritten to local same-origin paths, so mod_pagespeed actually fetches, parses,
rewrites and COMBINES them (driving `CssCombineFilter::CssCombiner::CleanParse`
and the IPRO `Harvest` path). Refs we didn't capture stay as-is and just 404
locally — harmless; the HTML still parses.

Flattening is deliberate: serving 1000 real domains with correct sub-resource
resolution would need Host-routing + per-domain LoadFromFile (and the slurp proxy
is defeated at scale by HSTS-preload + 103 Early Hints — see the modern-origin
gotchas memo). One flat docroot sidesteps all of it.

NOT COMMITTED: the corpus this produces (see .gitignore) — it is a large runtime
artifact that lives on the test host. Only this script is committed.

Politeness: single pass, capped concurrency (caps simultaneous DOMAINS; requests
within a domain are sequential), short timeouts, identified UA with a contact,
homepage-only (no crawl), failures skipped. Persist on the test host; never
re-fetch.

Usage:
  build_internet_corpus.py --out /corpus-internet --count 1200
                           [--concurrency 16] [--css-per-page 4] [--js-per-page 2]
                           [--list FILE]   # skip Tranco, use a domain-per-line file

Outputs under --out/:
  pages/<rank>-<domain>.html
  res/<rank>-<domain>-<n>.css|.js
  urls.txt       one path per line (every captured page + resource) — harness --url-file
  manifest.tsv   rank \t domain \t status \t n_css \t n_js
"""
import argparse
import io
import os
import re
import subprocess
import sys
import tempfile
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from html.parser import HTMLParser
from urllib.parse import urljoin, urlparse

TRANCO_ZIP = "https://tranco-list.eu/top-1m.csv.zip"
UA = ("Mozilla/5.0 (compatible; we-amp-corpus/1.0; +https://www.we-amp.com; "
      "single-pass load-test corpus, not a crawler)")


def curl(url, out_path=None, max_time=15, max_bytes=None):
    """Fetch url with curl (handles redirects/103/TLS). Returns (code, ctype,
    final_url, body_bytes_or_None). Body written to out_path if given."""
    cmd = ["curl", "-sL", "--max-time", str(max_time), "--max-redirs", "5",
           "-A", UA, "--compressed", "-w", "%{http_code}\t%{content_type}\t%{url_effective}"]
    tmp = out_path or tempfile.mktemp()
    cmd += ["-o", tmp, url]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=max_time + 5)
    except Exception:
        if not out_path and os.path.exists(tmp):
            os.unlink(tmp)
        return (0, "", url, None)
    meta = (r.stdout or "").strip().split("\t")
    code = int(meta[0]) if meta and meta[0].isdigit() else 0
    ctype = meta[1] if len(meta) > 1 else ""
    final = meta[2] if len(meta) > 2 and meta[2] else url
    body = None
    if out_path is None:
        try:
            with open(tmp, "rb") as f:
                body = f.read()
        except Exception:
            body = None
        if os.path.exists(tmp):
            os.unlink(tmp)
    elif max_bytes and os.path.exists(tmp) and os.path.getsize(tmp) > max_bytes:
        os.unlink(tmp)
        return (code, ctype, final, None)
    return (code, ctype, final, body)


class AssetParser(HTMLParser):
    """Collect stylesheet hrefs and script srcs in document order."""
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.css = []
        self.js = []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "link":
            rel = (a.get("rel") or "").lower()
            href = a.get("href")
            if href and "stylesheet" in rel:
                self.css.append(href)
        elif tag == "script":
            src = a.get("src")
            if src:
                self.js.append(src)


def fetch_tranco(count):
    """Download Tranco top-1m.csv.zip and return [(rank, domain), ...] top `count`."""
    with tempfile.NamedTemporaryFile(suffix=".zip", delete=False) as tf:
        zpath = tf.name
    code, _, _, _ = curl(TRANCO_ZIP, out_path=zpath, max_time=120)
    if code != 200 or not os.path.getsize(zpath):
        sys.exit(f"tranco download failed (http {code}); pass --list FILE instead")
    out = []
    with zipfile.ZipFile(zpath) as z:
        name = z.namelist()[0]
        with z.open(name) as fh:
            for line in io.TextIOWrapper(fh, encoding="utf-8"):
                parts = line.strip().split(",")
                if len(parts) >= 2 and parts[0].isdigit():
                    out.append((int(parts[0]), parts[1]))
                    if len(out) >= count:
                        break
    os.unlink(zpath)
    return out


def process(rank, domain, outdir, css_per_page, js_per_page):
    """Fetch one homepage + its assets into the flattened docroot. Returns
    (status, rel_paths, n_css, n_js). status: 'ok' | 'no-html' | 'fail'."""
    base = f"{rank}-{domain}"
    code, ctype, final, body = curl(f"https://{domain}/", max_time=15)
    if code != 200 or not body or "html" not in ctype.lower() or len(body) < 512:
        return ("no-html" if code else "fail", [], 0, 0)
    try:
        html = body.decode("utf-8", "replace")
    except Exception:
        return ("fail", [], 0, 0)

    p = AssetParser()
    try:
        p.feed(html)
    except Exception:
        pass

    paths = []
    res_dir = os.path.join(outdir, "res")
    n_css = n_js = 0

    def grab(ref, ext, idx):
        absu = urljoin(final, ref)
        if urlparse(absu).scheme not in ("http", "https"):
            return False
        local_name = f"{base}-{idx}.{ext}"
        dest = os.path.join(res_dir, local_name)
        c, ct, _, _ = curl(absu, out_path=dest, max_time=12, max_bytes=4 * 1024 * 1024)
        if c != 200 or not os.path.exists(dest) or not os.path.getsize(dest):
            if os.path.exists(dest):
                os.unlink(dest)
            return False
        # rewrite the HTML ref to the local same-origin path so mod_pagespeed
        # treats it as a rewritable same-origin resource.
        nonlocal html
        local_path = f"/res/{local_name}"
        if ref in html:
            html = html.replace(ref, local_path)
        paths.append(local_path)
        return True

    for i, href in enumerate(dict.fromkeys(p.css)):  # dedupe, keep order
        if n_css >= css_per_page:
            break
        if grab(href, "css", n_css + 1):
            n_css += 1
    for src in dict.fromkeys(p.js):
        if n_js >= js_per_page:
            break
        if grab(src, "js", n_js + 1):
            n_js += 1

    page_rel = f"/pages/{base}.html"
    with open(os.path.join(outdir, "pages", f"{base}.html"), "w", encoding="utf-8") as f:
        f.write(html)
    paths.insert(0, page_rel)
    return ("ok", paths, n_css, n_js)


def main():
    ap = argparse.ArgumentParser(description="Build an internet-variety mod_pagespeed replay corpus.")
    ap.add_argument("--out", required=True)
    ap.add_argument("--count", type=int, default=1200)
    ap.add_argument("--concurrency", type=int, default=16)
    ap.add_argument("--css-per-page", type=int, default=4)
    ap.add_argument("--js-per-page", type=int, default=2)
    ap.add_argument("--list", help="domain-per-line file (skip Tranco)")
    args = ap.parse_args()

    os.makedirs(os.path.join(args.out, "pages"), exist_ok=True)
    os.makedirs(os.path.join(args.out, "res"), exist_ok=True)

    if args.list:
        domains = []
        with open(args.list) as f:
            for i, line in enumerate(f, 1):
                d = line.strip()
                if d and not d.startswith("#"):
                    domains.append((i, d))
                if len(domains) >= args.count:
                    break
    else:
        print(f"downloading Tranco top-{args.count} ...", flush=True)
        domains = fetch_tranco(args.count)
    print(f"crawling {len(domains)} homepages (concurrency={args.concurrency}) ...", flush=True)

    all_paths = []
    manifest = []
    ok = noh = fail = 0
    with ThreadPoolExecutor(max_workers=args.concurrency) as ex:
        futs = {ex.submit(process, r, d, args.out, args.css_per_page, args.js_per_page): (r, d)
                for r, d in domains}
        done = 0
        for fut in as_completed(futs):
            r, d = futs[fut]
            done += 1
            try:
                status, paths, ncss, njs = fut.result()
            except Exception:
                status, paths, ncss, njs = ("fail", [], 0, 0)
            if status == "ok":
                ok += 1
                all_paths.extend(paths)
            elif status == "no-html":
                noh += 1
            else:
                fail += 1
            manifest.append((r, d, status, ncss, njs))
            if done % 100 == 0:
                print(f"  {done}/{len(domains)}  ok={ok} no-html={noh} fail={fail} "
                      f"urls={len(all_paths)}", flush=True)

    manifest.sort()
    with open(os.path.join(args.out, "urls.txt"), "w") as f:
        f.write("\n".join(all_paths) + "\n")
    with open(os.path.join(args.out, "manifest.tsv"), "w") as f:
        for r, d, s, ncss, njs in manifest:
            f.write(f"{r}\t{d}\t{s}\t{ncss}\t{njs}\n")

    n_pages = sum(1 for p in all_paths if p.startswith("/pages/"))
    n_css = sum(1 for p in all_paths if p.endswith(".css"))
    n_js = sum(1 for p in all_paths if p.endswith(".js"))
    print(f"\nDONE  pages={n_pages}  css={n_css}  js={n_js}  total_urls={len(all_paths)}")
    print(f"      ok={ok} no-html={noh} fail={fail}  (of {len(domains)})")
    print(f"      out={args.out}  url-file={os.path.join(args.out, 'urls.txt')}")


if __name__ == "__main__":
    main()
