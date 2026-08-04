# Shutdown / restart stress harness (`tools/stress/`)

Stress-tests the process **teardown** path of ModPageSpeed 1.x under sustained
load, to surface use-after-free / crash bugs that only happen when a worker
thread is mid-rewrite while the server is reloading/restarting. Built for
a tracked `spdlog::logger::sink_it_` use-after-free on Apache shutdown) and its
cousins, but it exercises any teardown-time hazard.

It is **server-agnostic at the HTTP layer** — the same load + chaos + detection
core runs against Apache, nginx, and IIS. Only the *reload* and *restart*
commands differ per server (passed on the CLI). Cache flush is a module-level
mechanism (`touch <cache>/cache.flush`) and is identical everywhere.

`stdlib-only` Python 3 — no `pip install`, so it runs on the Windows/IIS runner
as-is.

## What it does

1. **Sustained load** across all request paths: HTML pages (drive the full
   rewrite pipeline), `.pagespeed.`/IPRO resources, and stats handlers. A
   configurable fraction of requests are **cache-busted** (`?_sb=…`) so the
   engine is *actively rewriting* (`InPlaceRewriteContext::Harvest`) rather than
   serving cache hits — that is what keeps the teardown race live.
2. **Chaos** on jittered intervals that overlap the load: cache flush, graceful
   reload, full restart (the shutdown-UAF teardown path).
3. **Detection** (built for an **ASan** build): tails the server error log and
   watches the coredump dir, splitting evidence into two classes:
   - **Hard crashes** (FAIL): `AddressSanitizer`/`ThreadSanitizer` reports,
     `heap-use-after-free`/`data race`/`Segmentation fault`/abort/coredump.
     *Unambiguous* tokens are matched even when interleaved with other log
     output; *loose* tokens (`Abort`, `CRASH`, `exit signal`) are matched only
     when the line isn't a benign mod_pagespeed app log — the diverse corpus
     echoes CSS class names like `.progress-bar-abort` that would otherwise
     false-match.
   - **Recoverable UB** (catalog, not FAIL): UBSan `… runtime error: …` findings
     are *logged-and-continued* by a `halt_on_error=0` build, so they are
     reported as a deduped catalog for triage, not a pass/fail criterion.
   Plus: the server must return to HTTP 200 after every reload/restart. A run
   PASSES iff zero hard crashes **and** healthy after every teardown.

## Why ASan

A residual use-after-free frequently does **not** segfault immediately, so a
release build can pass a restart storm while silently corrupting memory. Run the
harness against an **ASan-instrumented module** so the UAF is reported
deterministically. (Build the ASan `.so`/module the same way CI builds the
`--config=clang-asan` targets, install it, and point the harness at that server.)

## Usage

```bash
# 1. Generate a self-contained rewrite corpus into the server's docroot.
python3 stress_shutdown.py gen-corpus --out /var/www/html/stress

# 2. Run. The corpus is served at <base-url>/<url-prefix>.
python3 stress_shutdown.py run --server apache \
    --base-url http://localhost:8080 --url-prefix /stress \
    --cache-dir /var/cache/mod_pagespeed \
    --duration 600 --concurrency 32 \
    --reload-cmd  'apachectl -k graceful' \
    --restart-cmd 'apachectl -k graceful-stop && sleep 1 && apachectl -k start' \
    --error-log    /var/log/apache2/error.log \
    --coredump-dir /var/lib/systemd/coredump
```

Exit code `0` = no crash/ASan hit and the server stayed healthy across every
reload/restart; `1` = a crash/ASan/health failure was detected; `2` = the server
wasn't healthy at start (corpus not served / server not up).

### Per-server control presets

| | reload | restart | error log | coredumps |
|---|---|---|---|---|
| **apache** | `apachectl -k graceful` | `apachectl -k graceful-stop && sleep 1 && apachectl -k start` | `/var/log/apache2/error.log` | `coredumpctl` dir `/var/lib/systemd/coredump` |
| **nginx** | `nginx -s reload` | `nginx -s quit && sleep 1 && nginx` | nginx `error_log` path | same |
| **iis** | `appcmd recycle apppool /apppool.name:<pool>` | `iisreset /restart` (or pool stop+start) | Windows Event Log / stdout redirect | WER (`%LOCALAPPDATA%\CrashDumps`) |

Cache flush (all three): `--cache-dir <FileCachePath>` — the harness touches
`<dir>/cache.flush`. Pass multiple dirs comma-separated for per-vhost / secondary
/ ipro caches.

### Knobs that maximize shutdown-UAF reproduction
- `--cache-bust-frac 0.35` — higher = more fresh rewrites in flight.
- `--restart-interval` low (e.g. `15`) — more teardown events overlapping load.
- Configure the served vhost with **IPRO** (`InPlaceResourceOptimization on`)
  and short cache TTLs so `ResourceRevalidateDone`/`Harvest` (the exact shutdown-UAF
  frame) churns continuously.

## Leveraging the legacy load tooling

This harness is purpose-built for the *teardown* path (load **+** restart/reload
chaos **+** sanitizer detection). The repo's older release-process load tools are
pure load generators — no chaos — but they bring two things worth reusing: a
**realistic corpus** and a **non-ASan memory checker**.

### Realistic corpus replay (`--url-file`)
Instead of the synthetic 8-page corpus, replay a collected set of real URLs.
`--url-file` lines are full URLs (used as-is) or paths (joined to `--base-url`),
split into pages vs resources by extension. Real corpora exercise far more of
the rewriter (more filters, more selector/parse paths — exactly how the CSS
`UnicodeText` UAF surfaced) than the synthetic set. A 99-page real corpus
(modpagespeed.com) was replayed under ASan with restart-every-12s chaos —
118.8k requests / 600s / 39 graceful-stop teardowns, **zero ASan hits** — to
confirm the shutdown-UAF fix holds on real content.

**Step 1 — enumerate the URL set** with the headless-Chrome collector. It drives
a real browser, so JS/lazy sub-resources are discovered (and slurped) too:

```bash
devel/loadtest_collect/loadtest_collect_corpus.sh pages.txt corpus.tar.bz2
# -> a slurp dir + corpus_all_urls.txt (the collector's %r access log).
```

> The collector (`devel/loadtest_collect/collect.js`, `puppeteer-core`; set
> `CHROME_PATH`) replaces the abandonware **phantomjs** driver and waits for
> `networkidle2`. Modern-origin gotchas it handles / you must handle:
> - It disables Chrome's http→https auto-upgrade (`HttpsUpgrades`) and scopes
>   requests to the page hostnames; the **record vhost must strip
>   `Strict-Transport-Security`** (else Chrome relearns HSTS and upgrades).
> - **HSTS-*preloaded* domains can't be captured** through the plaintext slurp
>   proxy (Chrome upgrades them a priori) — capture them from a non-preloaded
>   host or exclude them.
> - **Clear the record FileCachePath between runs**: a warm cache serves hits
>   without re-fetching, so those resources never get re-slurped (silently
>   incomplete dump).

**Step 2 — serve the corpus and replay.** Two ways, pick by your origin:

*(a) Slurp read-only* — works when the origin returns plain final responses.
Serve the slurp dir read-only and point `--url-file` at `corpus_all_urls.txt`.

*(b) curl → docroot* — **required when the origin emits `103 Early Hints`**
(modern mod_pagespeed 2.0 / nginx do). The 1.x serf slurp fetcher records the
interim 103 (no body) for HTML, so the slurp replay 500s; `curl` follows 103 to
the final 200, so mirror the enumerated URLs to a docroot and serve that:

```bash
# Mirror the enumerated corpus to a docroot (curl handles 103 Early Hints):
devel/loadtest_collect/build_docroot.sh corpus_all_urls.txt \
    https://example.com /corpus/example.com /tmp/corpus_replay_paths.txt
# Serve /corpus/example.com with mod_pagespeed (RewriteLevel CoreFilters,
# InPlaceResourceOptimization on, LoadFromFile mapping the serving host to the
# docroot) and replay the emitted paths:
python3 stress_shutdown.py run --server apache \
    --base-url http://localhost:8080 \
    --url-file /tmp/corpus_replay_paths.txt \
    --cache-dir /path/to/FileCachePath --restart-interval 12 ...
```

Cache-busting (`--cache-bust-frac`) works in mode (b) — a busted resource URL
still serves (query ignored for the file) and forces a fresh IPRO rewrite — so
keep it nonzero to drive `Harvest` continuously. In mode (a) a busted URL misses
the slurp dump, so use `--cache-bust-frac 0` there and rely on `cache.flush`.

## Internet-variety multi-sanitizer matrix

To widen the input distribution beyond one origin's house style — and to check
**more than ASan** — `build_internet_corpus.py` + a small matrix driver replay
~1000 real homepages across a sanitizer × filter-set grid.

**Corpus** (`devel/loadtest_collect/build_internet_corpus.py`): fetches the
Tranco top-N homepages ONCE (single pass, polite, identified UA) plus a few
stylesheets/scripts each, into a **flattened** docroot one localhost vhost can
serve (`pages/<rank>-<domain>.html`, `res/<rank>-<domain>-N.css|js`). Captured
`<link>`/`<script>` refs are rewritten to local same-origin paths so the
rewriter actually fetches, parses and **combines** them. Flattening sidesteps
the multi-domain serving trap (slurp is defeated at scale by HSTS-preload + 103;
per-domain `LoadFromFile` is unmanageable). The corpus is a large runtime
artifact — **gitignored, never committed**; only the build script is.

**Matrix** (`cell_run.sh` = one cell; `run_matrix.sh` = the grid; `configs/` =
the vhosts): cells run **strictly sequentially** (one shared Apache rig / port /
caches). Each cell swaps in its sanitizer module + filter config, restarts
Apache under the right runtime `LD_PRELOAD` + `*SAN_OPTIONS` (a per-cell restart
script re-injects them so chaos-restarts keep the sanitizer), runs the harness
with **long + gentle** chaos, truncates the error log per cell, and writes a
machine-readable summary + a deduped findings file.

```bash
# build the corpus on the test host (NOT committed):
python3 devel/loadtest_collect/build_internet_corpus.py --out /corpus-internet --count 1500
# build extra sanitizer modules (ASan module you already have):
bash tools/stress/build_sanitizer_module.sh ubsan   # combined ASan+UBSan, recoverable
bash tools/stress/build_sanitizer_module.sh tsan
# run the whole grid (sequential; auto-skips a cell whose module is absent):
nohup bash tools/stress/run_matrix.sh > /tmp/matrix.log 2>&1 &
# verdict: triage every finding + completeness critic (multi-agent):
#   tools/stress/verify_matrix.workflow.js
```

**Which sanitizers actually apply here** (load-bearing — the honest answer to
"do we check *all* sanitizer issues?"):

| Sanitizer | Verdict | Why |
|---|---|---|
| **ASan** | **Authoritative** | The shutdown bug is a use-after-free; ASan is purpose-built for it (it caught both of the shutdown bug's statics). |
| **UBSan** | **Useful, combined** | Built combined with ASan, **recoverable** (`halt_on_error=0`) so a stress run *catalogs every UB site* the diverse corpus triggers instead of aborting on the first. (`-fsanitize=undefined` was historically dropped from the asan config because benign `nonnull` UB — e.g. `memcmp(NULL,NULL,0)` on two empty `UnicodeText` — fails CI; these are triaged, not fixed.) |
| **TSan** | **Low-confidence probe** | Apache forks workers that then spawn the rewrite thread pool → TSan kills every child unless `die_after_fork=0`. Even then, post-fork tracking is best-effort and apache/APR are **uninstrumented**, so in-server TSan races are not authoritative. The sound TSan vehicle is a *fully-instrumented concurrent test* (the current unit repro is single-threaded — a follow-up). |
| **MSan** | **Skipped** | Needs an instrumented libc++ (not available in this toolchain). |

### valgrind as a non-ASan memory checker (complementary)
Where an ASan build isn't available, run a **longer** pass with the server under
`valgrind --tool=memcheck` and a mature load generator (`siege`, see
`devel/siege/`). valgrind is far slower (~20–50×) but needs no instrumented
build, so it's a good nightly complement to the fast ASan run:

```bash
# server (operator): start httpd under valgrind, e.g.
valgrind --tool=memcheck --leak-check=full --error-exitcode=99 \
    httpd -X -d $APACHE_ROOT  2> valgrind.log &
# load + chaos: drive this harness as usual (it adds the restart/reload chaos
# that siege/valgrind alone don't), pointed at the valgrind'd server.
```

The harness's detector already keys on the same crash signatures the upstream
release scraper (`devel/scrape_error_log_for_crashes.sh`) uses —
`exit signal` / `CRASH` — in addition to ASan/UBSan reports, so it catches both
instrumentation modes.

## Roadmap
- **Manual first** (this harness) against an ASan module — Apache leg, then nginx.
- **Daily CI** later: Apache + nginx share one Linux Docker leg (ASan module +
  this harness for N minutes, fail on any hit); IIS runs on the Windows runner
  via the `iis` adapter. Wire as a scheduled job, not a per-PR gate (it's long).
- **Corpus**: nightly job could collect a fresh corpus (`loadtest_collect`) and
  feed `--url-file` for broader real-world rewrite coverage.

## Status
load + chaos + ASan/coredump detection + synthetic corpus generator +
`--url-file` real-corpus replay (headless-Chrome collector + curl→docroot
builder), Apache/nginx ready, IIS adapter present (commands via CLI). Exercised
on Apache under ASan with both the synthetic corpus and a 99-page real corpus
(118.8k req / 600s / 39 teardowns, zero hits) for the shutdown-UAF fix. Not yet wired into
CI.
