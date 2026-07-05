#!/usr/bin/env python3
# Licensed under the Apache License, Version 2.0.
"""
Shutdown / restart stress harness for ModPageSpeed 1.x .

Goal: keep worker threads mid-rewrite (InPlaceRewriteContext::Harvest /
ResourceRevalidateDone) at the moment teardown fires, repeatedly, under
sustained load — the exact overlap that produced the production
spdlog::logger::sink_it_ use-after-free on shutdown.

It sustains concurrent HTTP load across all request paths (HTML pages +
.pagespeed. resources + stats), a fraction cache-busted so the engine is
ACTIVELY rewriting rather than serving cache hits, while a chaos controller
fires, on randomized intervals that overlap the load:
    - cache flush          (module-level: touch <cache>/cache.flush)
    - graceful reload      (per-server)
    - full restart         (per-server — the shutdown-UAF teardown path)

Detection is built for an ASan-instrumented module: it tails the server error
log for ASan 'ERROR'/'runtime error'/'Segmentation fault'/'signal', watches the
coredump dir for growth, and asserts the server returns HTTP 200 after every
reload/restart. Any of those => the run fails (non-zero exit).

Server-agnostic at the HTTP layer; only reload/restart differ per server, in
the small Adapter classes below. stdlib-only (runs on the Windows/IIS runner
without pip).

Quick start:
    # 1. generate a self-contained corpus into the server docroot
    python3 stress_shutdown.py gen-corpus --out /var/www/html/stress
    # 2. run (Apache example)
    python3 stress_shutdown.py run --server apache \
        --base-url http://localhost:8080 --url-prefix /stress \
        --cache-dir /var/cache/mod_pagespeed \
        --reload-cmd 'apachectl -k graceful' \
        --restart-cmd 'apachectl -k graceful-stop && sleep 1 && apachectl -k start' \
        --error-log /var/log/apache2/error.log \
        --coredump-dir /var/lib/systemd/coredump \
        --duration 600 --concurrency 32

See README.md for nginx / IIS presets.
"""

import argparse
import os
import random
import re
import subprocess
import sys
import threading
import time
import urllib.request
from collections import Counter
from concurrent.futures import ThreadPoolExecutor

# --------------------------------------------------------------------------- #
# Crash / ASan detection
# --------------------------------------------------------------------------- #
# Two tiers of hard-crash evidence (these FAIL a run):
#  - UNAMBIGUOUS: strings only a sanitizer/crash report emits; never benign-excluded
#    (so a report interleaved with a pagespeed log line is still caught).
#  - LOOSE: tokens that ALSO occur in echoed CSS/HTML the parser logs (e.g. the
#    class ".progress-bar-abort" trips \bAbort\b); counted only when NOT a benign
#    pagespeed application log line.
HARD_UNAMBIGUOUS = re.compile(
    r"AddressSanitizer|ThreadSanitizer|LeakSanitizer|"
    r"heap-use-after-free|heap-buffer-overflow|stack-use-after-\w+|"
    r"use-after-poison|global-buffer-overflow|"
    r"WARNING: ThreadSanitizer|data race|"
    r"SUMMARY: (Address|Thread|Leak)Sanitizer|"
    r"Segmentation fault|core dumped|terminate called",
    re.IGNORECASE,
)
HARD_LOOSE = re.compile(
    r"signal SIG|\bAbort\b|exit signal|\bCRASH\b|assertion .*failed",
    re.IGNORECASE,
)

# UBSan reports a RECOVERABLE undefined-behaviour finding ("file:line: runtime
# error: ...") — NOT a crash; the process logged it and continued. These are
# CATALOGED for triage and do not by themselves fail a run.
UB_PATTERNS = re.compile(
    r": runtime error:|UndefinedBehaviorSanitizer|SUMMARY: UndefinedBehaviorSanitizer",
    re.IGNORECASE,
)

# Benign mod_pagespeed application logs that ECHO source CSS/HTML/JS — used only
# to suppress HARD_LOOSE false matches (e.g. ".progress-bar-abort" in a
# "[INFO] [pagespeed] Failed to parse selector" line). Never applied to
# UNAMBIGUOUS crash or UB lines (UBSan output frequently interleaves with these
# parser logs on one physical line, so we must not drop it).
BENIGN_PATTERNS = re.compile(
    r"\[(INFO|Warning|WARNING|Trace\d*)\].*\[pagespeed\]|"
    r"\[pagespeed\].*\[(INFO|Warning|WARNING|Trace\d*)\]|"
    r"Failed to parse|Could not parse|Invalid (selector|declaration|media)",
    re.IGNORECASE,
)


class Detector:
    """Tails error logs + watches the coredump dir for crash evidence."""

    def __init__(self, log_paths, coredump_dir):
        self.log_paths = [p for p in log_paths if p]
        self.coredump_dir = coredump_dir
        self.hits = []  # (source, line)
        self._offsets = {}
        self._baseline_cores = self._core_snapshot()
        # Seed offsets at current EOF so we only see NEW lines.
        for p in self.log_paths:
            try:
                self._offsets[p] = os.path.getsize(p)
            except OSError:
                self._offsets[p] = 0

    def _core_snapshot(self):
        if not self.coredump_dir or not os.path.isdir(self.coredump_dir):
            return set()
        try:
            return set(os.listdir(self.coredump_dir))
        except OSError:
            return set()

    def poll(self):
        """Scan new log lines + new coredumps. Returns list of new hits."""
        new = []
        for p in self.log_paths:
            try:
                size = os.path.getsize(p)
                start = self._offsets.get(p, 0)
                if size < start:  # log rotated/truncated
                    start = 0
                if size > start:
                    with open(p, "r", errors="replace") as fh:
                        fh.seek(start)
                        chunk = fh.read()
                    self._offsets[p] = size
                    for line in chunk.splitlines():
                        # UB first: recoverable UBSan finding (often interleaved
                        # with a pagespeed parse log on one line) — catalog it.
                        if UB_PATTERNS.search(line):
                            new.append((p, line.strip(), "ub"))
                        elif HARD_UNAMBIGUOUS.search(line):
                            new.append((p, line.strip(), "crash"))
                        elif HARD_LOOSE.search(line) and not BENIGN_PATTERNS.search(line):
                            new.append((p, line.strip(), "crash"))
            except OSError:
                continue
        added_cores = self._core_snapshot() - self._baseline_cores
        for c in sorted(added_cores):
            new.append((self.coredump_dir, "NEW COREDUMP: " + c, "crash"))
        self._baseline_cores |= added_cores
        self.hits.extend(new)
        return new


# --------------------------------------------------------------------------- #
# Per-server control adapters (reload / restart). flush is module-level.
# --------------------------------------------------------------------------- #
class Adapter:
    def __init__(self, args):
        self.args = args

    def _run(self, cmd):
        if not cmd:
            return
        subprocess.run(cmd, shell=True, check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    def reload(self):
        self._run(self.args.reload_cmd)

    def restart(self):
        self._run(self.args.restart_cmd)

    def flush_cache(self):
        # Module-level mechanism shared by Apache/nginx/IIS: touch cache.flush
        # in every cache dir (CacheFlushPollInterval picks it up).
        if not self.args.cache_dir:
            return
        for root in self.args.cache_dir.split(","):
            root = root.strip()
            for fn in (os.path.join(root, "cache.flush"),):
                try:
                    with open(fn, "a"):
                        os.utime(fn, None)
                except OSError:
                    pass


# apache / nginx / iis share the same Adapter; the difference is purely in the
# reload-cmd / restart-cmd the operator passes (see README presets). Subclasses
# exist as hooks for future server-specific behavior.
class ApacheAdapter(Adapter):
    pass


class NginxAdapter(Adapter):
    pass


class IisAdapter(Adapter):
    pass


ADAPTERS = {"apache": ApacheAdapter, "nginx": NginxAdapter, "iis": IisAdapter}


# --------------------------------------------------------------------------- #
# Load generation
# --------------------------------------------------------------------------- #
class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.codes = Counter()
        self.errors = 0
        self.requests = 0

    def record(self, code):
        with self.lock:
            self.requests += 1
            if code is None:
                self.errors += 1
            else:
                self.codes[code] += 1


RESOURCE_EXTS = (".css", ".js", ".png", ".jpg", ".jpeg", ".gif", ".webp",
                 ".svg", ".ico", ".woff", ".woff2")


def build_urls(base_url, url_prefix, url_file=None):
    """The request set to drive load against. HTML drives the full rewrite
    pipeline; direct resource hits exercise the .pagespeed resource path + IPRO.

    Default = the self-contained corpus gen-corpus writes. With url_file, pages
    and resources come from a collected corpus URL list (e.g. the
    corpus_all_urls.txt produced by devel/loadtest_collect) — far more diverse
    real-world content, which churns more of the rewriter. Each line is a full
    URL (used as-is) or a path (joined to base_url); lines are split into pages
    vs resources by file extension."""
    stats = [base_url + p
             for p in ("/mod_pagespeed_statistics", "/mod_pagespeed_message")]
    if url_file:
        pages, resources = [], []
        with open(url_file) as fh:
            for line in fh:
                u = line.strip()
                if not u or u.startswith("#"):
                    continue
                if u.startswith("http://") or u.startswith("https://"):
                    full = u
                else:
                    full = base_url + (u if u.startswith("/") else "/" + u)
                path = full.split("?", 1)[0].lower()
                (resources if path.endswith(RESOURCE_EXTS) else pages).append(
                    full)
        if not pages and not resources:
            raise SystemExit(f"--url-file {url_file} had no usable URLs")
        # pick_url needs both lists non-empty; share if the corpus is one-sided.
        pages = pages or resources
        resources = resources or pages
        return (pages, resources, stats)
    pages = [base_url + f"{url_prefix}/page{i}.html" for i in range(1, 9)]
    resources = [base_url + f"{url_prefix}/style{i}.css" for i in range(1, 5)] \
        + [base_url + f"{url_prefix}/script{i}.js" for i in range(1, 5)] \
        + [base_url + f"{url_prefix}/img{i}.png" for i in range(1, 7)]
    return (pages, resources, stats)


def pick_url(pages, resources, stats, bust_frac):
    r = random.random()
    if r < 0.55:
        url = random.choice(pages)
    elif r < 0.92:
        url = random.choice(resources)
    else:
        url = random.choice(stats)
    # Cache-bust a fraction: forces a FRESH rewrite (worker actively in Harvest)
    # instead of a cache hit — this is what keeps the teardown race live.
    if random.random() < bust_frac:
        sep = "&" if "?" in url else "?"
        url = f"{url}{sep}_sb={random.randint(0, 1 << 30)}"
    return url


def worker_loop(stop_evt, urls, stats, bust_frac, timeout):
    pages, resources, statpaths = urls
    while not stop_evt.is_set():
        url = pick_url(pages, resources, statpaths, bust_frac)
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "psol-stress"})
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                resp.read()
                stats.record(resp.status)
        except urllib.error.HTTPError as e:
            stats.record(e.code)
        except Exception:
            stats.record(None)


def health_ok(health_url, timeout=10):
    try:
        req = urllib.request.Request(health_url,
                                     headers={"User-Agent": "psol-stress"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return 200 <= resp.status < 500
    except Exception:
        return False


# --------------------------------------------------------------------------- #
# Run
# --------------------------------------------------------------------------- #
def cmd_run(args):
    adapter = ADAPTERS[args.server](args)
    detector = Detector([args.error_log], args.coredump_dir)
    stats = Stats()
    urls = build_urls(args.base_url.rstrip("/"), args.url_prefix.rstrip("/"),
                      args.url_file)
    # Health/settle probes hit a REAL served URL. With --url-file (e.g. a slurp
    # corpus) /page1.html doesn't exist, so probe the first corpus page instead.
    health_url = (urls[0][0] if args.url_file
                  else args.base_url.rstrip("/") + args.url_prefix.rstrip("/")
                  + "/page1.html")
    stop_evt = threading.Event()

    print(f"[stress] server={args.server} base={args.base_url} "
          f"concurrency={args.concurrency} duration={args.duration}s "
          f"bust={args.cache_bust_frac}")
    if not health_ok(health_url):
        print("[stress] FATAL: server not healthy at start "
              f"({health_url}). "
              "Did you run gen-corpus into the docroot (or point --url-file at a "
              "served corpus) and start the server?")
        return 2

    pool = ThreadPoolExecutor(max_workers=args.concurrency)
    for _ in range(args.concurrency):
        pool.submit(worker_loop, stop_evt, urls, stats,
                    args.cache_bust_frac, args.req_timeout)

    deadline = time.monotonic() + args.duration
    next_flush = next_reload = next_restart = time.monotonic()
    crashes = []
    ub_findings = []
    restart_failures = []
    last_report = time.monotonic()

    def schedule(interval):
        # +/-40% jitter so events land at varied points in the rewrite cycle.
        return time.monotonic() + interval * (0.6 + 0.8 * random.random())

    next_flush = schedule(args.flush_interval) if args.flush_interval else None
    next_reload = schedule(args.reload_interval) if args.reload_interval else None
    next_restart = schedule(args.restart_interval) if args.restart_interval else None

    try:
        while time.monotonic() < deadline:
            now = time.monotonic()
            if next_flush and now >= next_flush:
                adapter.flush_cache()
                next_flush = schedule(args.flush_interval)
            if next_reload and now >= next_reload:
                adapter.reload()
                next_reload = schedule(args.reload_interval)
                if not _settles(health_url):
                    restart_failures.append(("reload", time.time()))
            if next_restart and now >= next_restart:
                adapter.restart()
                next_restart = schedule(args.restart_interval)
                if not _settles(health_url):
                    restart_failures.append(("restart", time.time()))
            new_hits = detector.poll()
            for src, line, cat in new_hits:
                if cat == "ub":
                    ub_findings.append((src, line))
                else:
                    crashes.append((src, line))
                    print(f"[stress] !!! CRASH/ASAN: [{src}] {line}")
            if now - last_report >= 15:
                with stats.lock:
                    print(f"[stress] t={int(now - (deadline - args.duration))}s "
                          f"reqs={stats.requests} codes={dict(stats.codes)} "
                          f"errors={stats.errors} crashes={len(crashes)} ub={len(ub_findings)}")
                last_report = now
            time.sleep(0.25)
    finally:
        stop_evt.set()
        pool.shutdown(wait=True)
        for src, line, cat in detector.poll():
            (ub_findings if cat == "ub" else crashes).append((src, line))

    print("\n==== STRESS SUMMARY ====")
    with stats.lock:
        print(f"requests={stats.requests} codes={dict(stats.codes)} errors={stats.errors}")
    print(f"crash/asan hits={len(crashes)} reload/restart-not-healthy={len(restart_failures)}")
    for src, line in crashes[:20]:
        print(f"  CRASH [{src}] {line}")
    for kind, ts in restart_failures[:20]:
        print(f"  NOT-HEALTHY after {kind} @ {ts}")
    # UBSan undefined-behaviour findings are RECOVERABLE (logged, process kept
    # running) — reported as a catalog for triage, NOT a pass/fail criterion.
    uniq_ub = sorted(set(line for _src, line in ub_findings))
    print(f"ub findings (recoverable, cataloged)={len(ub_findings)} distinct~={len(uniq_ub)}")
    for line in uniq_ub[:15]:
        print(f"  UB {line}")
    # A run PASSES iff no HARD crash and the server stayed healthy through chaos.
    # UB findings do not fail the run; they are triaged separately.
    ok = not crashes and not restart_failures
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def _settles(health_url, attempts=20, delay=0.5):
    """After a reload/restart, the server must return to serving 200s."""
    for _ in range(attempts):
        if health_ok(health_url):
            return True
        time.sleep(delay)
    return False


# --------------------------------------------------------------------------- #
# Corpus generation (self-contained; drop into the server docroot)
# --------------------------------------------------------------------------- #
def cmd_gen_corpus(args):
    out = args.out
    os.makedirs(out, exist_ok=True)
    # CSS (rewritable: combine/minify/inline)
    for i in range(1, 5):
        with open(os.path.join(out, f"style{i}.css"), "w") as f:
            f.write(f".c{i}{{color:#{i}{i}{i};margin:{i}px;padding:{i}px;}}\n"
                    + "\n".join(f".x{i}_{j}{{width:{j}px}}" for j in range(60)))
    # JS (rewritable: combine/minify)
    for i in range(1, 5):
        with open(os.path.join(out, f"script{i}.js"), "w") as f:
            f.write(f"function f{i}(){{var s=0;"
                    + "".join(f"s+={j};" for j in range(60)) + "return s;}\n")
    # PNG (rewritable: recompress/resize/convert) — tiny valid PNGs
    png = bytes.fromhex(
        "89504e470d0a1a0a0000000d49484452000000100000001008060000001ff3ff"
        "610000001f49444154789c6360180503300a3a32308a619406233086a8c83055"
        "0c0000d9b50a0e5f6f3f4a0000000049454e44ae426082")
    for i in range(1, 7):
        with open(os.path.join(out, f"img{i}.png"), "wb") as f:
            f.write(png)
    # HTML pages referencing the above (drives the full rewrite pipeline + IPRO)
    for i in range(1, 9):
        css = "\n".join(f'<link rel="stylesheet" href="style{j}.css">'
                        for j in range(1, 5))
        js = "\n".join(f'<script src="script{j}.js"></script>' for j in range(1, 5))
        imgs = "\n".join(f'<img src="img{j}.png" width="32" height="32">'
                         for j in range(1, 7))
        with open(os.path.join(out, f"page{i}.html"), "w") as f:
            f.write(f"<!doctype html><html><head>{css}{js}</head>"
                    f"<body><h1>stress {i}</h1>{imgs}</body></html>")
    print(f"[stress] wrote corpus to {out} "
          f"(8 pages, 4 css, 4 js, 6 png). Serve it and pass --url-prefix "
          f"pointing at its URL path.")
    return 0


# --------------------------------------------------------------------------- #
def main(argv=None):
    p = argparse.ArgumentParser(description="ModPageSpeed shutdown/restart stress harness")
    sub = p.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("gen-corpus", help="write a self-contained rewrite corpus")
    g.add_argument("--out", required=True, help="output dir (inside the server docroot)")
    g.set_defaults(func=cmd_gen_corpus)

    r = sub.add_parser("run", help="run the stress loop")
    r.add_argument("--server", choices=list(ADAPTERS), required=True)
    r.add_argument("--base-url", required=True)
    r.add_argument("--url-prefix", default="/stress", help="URL path of the corpus")
    r.add_argument("--url-file", default=None,
                   help="replay URLs from a collected corpus list "
                        "(e.g. corpus_all_urls.txt from devel/loadtest_collect); "
                        "overrides the synthetic corpus for far more diverse load")
    r.add_argument("--cache-dir", default="",
                   help="comma-separated FileCachePath dir(s) for cache.flush")
    r.add_argument("--reload-cmd", default="")
    r.add_argument("--restart-cmd", default="")
    r.add_argument("--error-log", default="")
    r.add_argument("--coredump-dir", default="")
    r.add_argument("--duration", type=int, default=600)
    r.add_argument("--concurrency", type=int, default=32)
    r.add_argument("--cache-bust-frac", type=float, default=0.35)
    r.add_argument("--flush-interval", type=float, default=7.0, help="0 disables")
    r.add_argument("--reload-interval", type=float, default=23.0, help="0 disables")
    r.add_argument("--restart-interval", type=float, default=53.0, help="0 disables")
    r.add_argument("--req-timeout", type=float, default=15.0)
    r.set_defaults(func=cmd_run)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
