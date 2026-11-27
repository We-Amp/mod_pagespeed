---
title: 'Browser analysis with headless Chrome'
description: 'How the optimizer worker renders pages in headless Chrome to extract critical CSS, find the LCP element, measure JS coverage and gate visual regressions.'
order: 54
group: 'Operate'
lastUpdated: 2026-10-22
---

The optimizer worker can render pages in headless Chrome instead of relying on
heuristics alone. Browser analysis reads critical CSS from real CSS Coverage
data, detects the true Largest Contentful Paint element, measures rendered
image dimensions, and — for above-the-fold CSS — confirms in a real render that
the page still looks the same before allowing its stylesheet to be deferred.

Browser analysis is strictly additive. Every failure falls back to the
heuristic path, so pages still get optimized -- just through the faster, less
precise heuristic pipeline. The trade-off is accuracy for latency: a rendered
profile is more precise than a [heuristic critical-CSS estimate](/blog/critical-css-heuristics/),
but it costs a headless render the first time a page template is seen.

## Enabling browser analysis

Browser analysis is off by default. Enable it with the `--enable-browser-analysis`
flag and ensure Chrome (or `chrome-headless-shell`) is available in the
container:

```bash
factory_worker \
  --cache-path /data/cache.vol \
  --enable-browser-analysis \
  --chrome-binary /usr/bin/chrome-headless-shell
```

The Docker release images (`ghcr.io/we-amp/pagespeed-worker` and the combined
`ghcr.io/we-amp/pagespeed-combined`) ship with Chromium pre-installed. In a
container, enable analysis by setting `PAGESPEED_ENABLE_BROWSER_ANALYSIS=true` —
the entrypoint passes `--enable-browser-analysis` for you. No `--shm-size`
tuning is needed: the worker runs Chrome with `--disable-dev-shm-usage`, so it
does not rely on the container's `/dev/shm`.

### On a host install (deb/rpm)

The `pagespeed-optimizer` package ships no browser and only *suggests* one.
Install a Chromium or Chrome the optimizer worker's `pagespeed` user can execute, then
point the optimizer worker at it from `/etc/default/pagespeed-optimizer` -- the default
binary path exists only in the container images:

| Distribution | Package | Binary |
| --- | --- | --- |
| Debian 12 | `apt-get install chromium` | `/usr/bin/chromium` |
| Ubuntu 24.04 | Google Chrome's .deb (there is no apt `chromium`; the `chromium-browser` package is a snap stub, and a snap cannot run under the service unit's sandbox). Alternatively pin Debian bookworm's `chromium`, the recipe the container images use. | `/usr/bin/google-chrome-stable` |
| Google Chrome (.deb / .rpm), any distro | vendor package | `/usr/bin/google-chrome-stable` |
| EL9 with EPEL (not exercised by the package's own verification) | `dnf install chromium` | `/usr/bin/chromium-browser` |

```bash
# /etc/default/pagespeed-optimizer
OPTIMIZER_OPTS="--api-socket --enable-browser-analysis --chrome-binary /usr/bin/chromium"
```

Restart the service and read `GET /v1/health` over the API socket:
`browser_sandbox` must be `"on"` and `browser.chrome_running` `true`. The
shipped service unit runs Chrome sandboxed as-is; the sandbox needs
unprivileged user namespaces, and if the kernel refuses them the optimizer worker
reports `browser_sandbox: "unavailable"`, names the cause in the journal, and
keeps serving without browser analysis (it never starts an unsandboxed
browser on its own -- that is the explicit `PAGESPEED_BROWSER_SANDBOX=off`
opt-out). The packaged service unit enforces a system-call allow-list (since
2.1.0), and Chrome's own sandbox needs a handful of calls that list does not
admit; the package ships that re-admission list installed and active as a
vendor drop-in (`20-browser-analysis.conf` next to the unit), so nothing
needs installing. A host that does not run browser analysis can mask it with
an empty same-named file under
`/etc/systemd/system/pagespeed-optimizer.service.d/`, after which browser
analysis refuses to initialize and names the restriction. The package's own
verification starts a real Chromium under the shipped default and asserts it
renders without a kill.

## Architecture

```text
Worker (libuv event loop)
  |
  +-- BrowserAnalysisManager
        |
        +-- AnalysisQueue        -- bounded priority queue with dedup
        |
        +-- ChromeProcess        -- spawn/recycle/RSS monitor
        |     |
        |     +-- CdpClient      -- JSON-RPC over pipe (FD 3/4)
        |
        +-- BrowserCssExtractor  -- CSS Coverage API -> critical CSS
        +-- PageAnalyzer         -- LCP, fold, CLS, image dims
        +-- UnusedCssRemover     -- dead rule removal
        +-- VisualRegressionGate -- PNG pixel diff validation
        +-- FontGlyphScanner     -- code point scanning + @font-face
        +-- ScriptCoverageAnalyzer -- Profiler coverage + deferral
```

`BrowserAnalysisManager` owns the Chrome lifecycle, analysis queue, and the
CDP pipeline. It runs on the main libuv event loop (where CDP must operate).
Worker thread pool threads enqueue analysis requests via `uv_async_send()`.

### CDP pipe transport

Chrome DevTools Protocol communication happens over `--remote-debugging-pipe`
(file descriptors 3 and 4), not over a WebSocket. Messages are null-byte
delimited JSON-RPC. This avoids the overhead and port management of the
WebSocket debugging protocol.

Design decisions:

- Per-command `uv_timer_t` timeout (default 30s)
- Large CDP messages (>64KB) parsed off the event loop via `uv_queue_work()`
- `CancelAll()` on pipe EOF resolves all pending callbacks

## How it works

1. The worker thread runs `HtmlScanner::Scan()` to extract page structure
2. `TemplateDetector::HashStructure()` computes an FNV-1a hash of the DOM
   structure, identifying the page template
3. `LookupProfile()` checks the cache for an existing `OptimizationProfile`
   for this template hash
4. **Profile found**: browser-validated critical CSS and LCP data are used
   instead of heuristics
5. **No profile**: `EnqueueAnalysis()` sends the request to the main event
   loop via `uv_async_send()`
6. `DrainQueue()` dequeues items and runs the analysis pipeline across
   three viewports: Mobile (375x667), Tablet (768x1024), Desktop (1440x900)
7. The resulting `OptimizationProfile` is stored in the cache with
   `SentinelId::kBrowserProfile`

### CSS cache inlining

Before passing HTML to Chrome, the worker resolves `<link rel="stylesheet">`
tags against the Cyclone cache and injects `<style>` blocks into the HTML.
This enables Chrome's CSS Coverage API to compute real coverage percentages
instead of returning 0% for external stylesheets. The
[CSS cache inlining trick for the Coverage API](/blog/css-cache-inlining-for-coverage-api/)
explains why the raw external-stylesheet path reports 0% and how the inline
step recovers accurate numbers.

Guards prevent abuse: 50 stylesheet cap, 2MB per-stylesheet cap, 10MB total
HTML cap.

## Analysis components

| Component                  | Purpose                                                                                                                            |
| -------------------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| **BrowserCssExtractor**    | Uses Chrome's CSS Coverage API to identify which CSS rules are actually used on each viewport. Produces per-viewport critical CSS. |
| **PageAnalyzer**           | Detects the real LCP element, measures fold position, computes CLS, and reads rendered image dimensions.                           |
| **UnusedCssRemover**       | Takes Coverage data and removes dead rules from stylesheets.                                                                       |
| **VisualRegressionGate**   | Renders two versions of a page and compares the above-the-fold region pixel-by-pixel. Drives the critical-CSS check below.         |
| **FontGlyphScanner**       | Scans the DOM with TreeWalker for code points used on the page. Maps them to @font-face declarations for future subsetting.        |
| **ScriptCoverageAnalyzer** | Uses Chrome's Profiler domain to measure JS code coverage. Identifies scripts safe to defer.                                       |

## What the pixel comparison actually gates

One optimization is gated on it: **deferring a stylesheet**. During analysis,
the page is rendered twice at each viewport — once with its whole stylesheet,
once with only the above-the-fold block that would be inlined — and the two
above-the-fold regions are compared. The stylesheet is made non-render-blocking
only when they match. If the check does not run, or does not pass, the
stylesheet stays render-blocking and no above-the-fold CSS is inlined next to
it (a stylesheet applied by script, or placed after the page content, keeps the
inlined block, because first paint does not wait for it), so the page loses the
deferral and keeps everything else.

The check is tied to the exact stylesheet it was made against, so publishing new
CSS re-checks before deferring again, and a page whose analysis has not finished
yet keeps its stylesheet render-blocking.

The other optimizations are **not** gated this way. Lazy-loading, image sizing,
script deferral and the rest are applied on their own evidence — coverage data,
measured dimensions, fold position — and are not pixel-compared before use.

### Three things the check cannot see

The comparison render is deliberately sealed off from the network and from
scripts, so that analysing a page can never be turned into a way of reaching
something else. Two consequences follow, and neither is a setting:

- **Scripts do not run.** A fold whose layout is established by JavaScript is
  compared against a version of the page the visitor never sees.
- **Images, web fonts and imported stylesheets do not load.** They are absent
  from *both* renders, so a fold that depends on a background image or a
  `@font-face` looks the same in each, and the difference the visitor would see
  is not visible to the check.

The third is about *which* pages a passed check covers:

- **The check is per template, not per page.** Pages that share a template
  share one confirmation. The above-the-fold CSS is still worked out per page,
  but the confirmation that it covers the fold was made on whichever page of
  that template was analysed. A sibling page whose fold needs something that
  one did not can be deferred on its confirmation.

If a page's above-the-fold appearance depends on any of these, verify it
yourself before relying on stylesheet deferral there — or turn deferral off for
that site with `--no-async-css`. In particular, seeing a flash on one page of a
template is a reason to turn deferral off for the site, not to expect the check
to have caught it.

Note also that pages whose above-the-fold CSS already accounts for most of
their stylesheet are not deferred at all — inlining most of a sheet and then
downloading it again is a loss — so those pages are never checked, because
there is nothing to authorize.

## Script coverage analysis

When browser analysis is enabled, the `ScriptCoverageAnalyzer` component uses
Chrome's Profiler domain to measure JavaScript code coverage. This identifies
scripts that are safe to defer, improving page load performance by reducing
parser-blocking JavaScript. The same Coverage-API technique drives
[removing unused JavaScript with Chrome coverage](/blog/remove-unused-javascript-chrome-coverage/).

### How it works

1. The analyzer loads the page with JavaScript enabled (Profiler + Coverage APIs)
2. Each external script's coverage is measured during page load
3. Scripts are classified into deferral categories based on coverage data and
   execution timing

### Deferral categories

| Category             | Description                                                        |
| -------------------- | ------------------------------------------------------------------ |
| `kSafeToDefer`       | Script has low main-thread impact; safe to add `defer`             |
| `kCandidateForAsync` | Script is independent; could use `async` instead                   |
| `kAlreadyAsync`      | Script already has `async` or `defer` attribute                    |
| `kKeepSynchronous`   | Script must execute synchronously (DOM-dependent, inline handlers) |

### SSRF defense

Script analysis enables JavaScript execution in Chrome (required for accurate
coverage measurement). The other three SSRF defense layers remain active:
network offline mode, Fetch interception, and DNS-level blocking. Chrome cannot
make outbound connections even with JavaScript enabled.

### Configuration

Script analysis has no flag of its own: it runs whenever browser analysis is
enabled with `--enable-browser-analysis`.

Script analysis results feed into the optimization policy engine, which decides
whether to enable script deferral for each URL template.

## Optimization policy

The optimization policy engine computes per-template decisions about optional
HTML transforms based on browser analysis data. It runs after profile generation
and stores the policy alongside the optimization profile in cache.

### Policy fields

| Field                     | Condition                   | Description                                          |
| ------------------------- | --------------------------- | ---------------------------------------------------- |
| `async_css_enabled`       | Avg CSS coverage < 50%      | Advises that async loading is worth considering for this template. Advisory only — it does not enable deferral. Deferral additionally requires a confirmed above-the-fold result for the page, bound to the stylesheet being served. |
| `script_deferral_enabled` | Deferrable scripts detected | Enable `defer` attribute on safe scripts             |

### Stats counters

| Counter                          | Description                                 |
| -------------------------------- | ------------------------------------------- |
| `policy.computed`                | Total optimization policies computed        |
| `policy.async_css_enabled`       | Times async CSS was enabled by policy       |
| `policy.async_css_suppressed_low_coverage` | Times stylesheet deferral was refused because the inlined above-the-fold CSS was too thin to bridge first paint |
| `policy.async_css_suppressed_unvalidated` | Times stylesheet deferral was refused because the page has no confirmed above-the-fold result bound to the stylesheet being served (not analyzed yet, or the stylesheet changed since) |
| `policy.async_css_record_dropped_empty_derivation` | Times a page's confirmed above-the-fold result was set aside because no above-the-fold CSS could be measured for that specific page, so the confirmation does not describe what would be inlined |
| `policy.script_deferral_enabled` | Times script deferral was enabled by policy |

These counters appear in `/v1/stats` JSON, `/v1/metrics` Prometheus output,
the management socket `STATS` command, and the web console metrics page.

## Chrome process management

### Lifecycle

1. `ChromeProcess::Start()` spawns Chrome with headless flags and pipe transport
2. CDP commands flow through `CdpClient` for page analysis
3. After each page, `IncrementPageCount()` checks the recycle threshold
4. At the recycle threshold, `Stop()` sends SIGTERM (then SIGKILL after 5s)
5. A fresh Chrome process starts for the next batch

### Launch flags

Chrome is spawned with strict isolation flags:

- `--headless=new` -- new headless mode
- `--remote-debugging-pipe` -- FD 3/4 pipe transport
- `--disable-gpu` -- no GPU required
- `--user-data-dir` -- a private, per-run profile directory the worker pins
  and creates 0700
- `--host-resolver-rules="MAP * ~NOTFOUND"` -- DNS-level SSRF block
- `--disable-dev-shm-usage` -- avoids /dev/shm exhaustion in containers
- Various isolation flags (`--disable-extensions`, `--disable-background-networking`,
  `--no-first-run`, etc.)

### Sandbox

Chrome parses untrusted page content, so its own sandbox has to be on.
`--browser-sandbox` decides what happens when it cannot be:

| Value               | Behaviour                                                                                                                                                                                 |
| ------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `require` (default) | The worker probes for the sandbox at startup. If it is unavailable, **browser analysis refuses to start** with a message naming the cause and the remedy, and the worker keeps serving without it. It never falls back to an unsandboxed browser. |
| `off`               | Deliberate opt-out. Chrome runs with `--no-sandbox`, and a warning is logged at startup and on every browser start.                                                                        |

There is no `auto`. `GET /v1/health` reports the resolved state as
`browser_sandbox` (`"on"`, `"unavailable"`, `"off"`, `"disabled"`), so a
monitoring check can see an unsandboxed browser without reading logs. The
setting is also readable from `PAGESPEED_BROWSER_SANDBOX`.

The sandbox needs an unprivileged user namespace and a non-root user. The
container images drop the worker to the unprivileged `pagespeed` user (UID 918)
before it starts, so inside them the remaining requirement is a seccomp profile
that allows the namespace flags; the default Docker and Kubernetes profiles do
not, and the probe reports `unavailable` until you supply one. Set
`PAGESPEED_BROWSER_SANDBOX=off` only to accept an unsandboxed browser
deliberately.

The probe runs inside whatever syscall filter the worker itself is running
under, so a filter too tight for Chrome surfaces here, at startup, rather than
as a Chrome crash later. The three refusals read differently on purpose: a
kernel that denies unprivileged user namespaces, a syscall filter that killed
the probe, and a probe that did not answer in time. For the syscall-filter
case, relax the syscall filter or namespace restriction the worker runs under,
or — in a container — start it with a Chrome-compatible seccomp profile rather
than the runtime's default one. `GET /v1/health` reports whether a filter is
attached at all as `syscall_filter` (`"filtered"`, `"none"`, `"unknown"`);
note that a stock container already reports `"filtered"` because of its
runtime's default profile, so the field does not by itself tell you which
profile is in force.

### RSS monitoring

On Linux, the worker reads `/proc/pid/status` VmRSS every 5 seconds. When
Chrome exceeds `--chrome-max-memory` (default 512MB), the worker stops it
and starts a fresh instance. This prevents memory leaks from accumulating
across hundreds of pages.

## SSRF defense (4 layers)

Browser analysis operates on cached content, not live network requests. Four
layers prevent Chrome from making any outbound connections (the reasoning
behind this air-gapped design is covered in
[air-gapped headless fetch and SSRF pinning](/blog/air-gapped-headless-fetch-ssrf-pinning/)):

1. `Network.emulateNetworkConditions({offline: true})` -- blocks all network
2. `Fetch.enable` + `Fetch.requestPaused` -- intercept and fail all requests
3. `Emulation.setScriptExecutionDisabled({value: true})` -- no JS execution
   (CSS extractor and visual regression gate)
4. `--host-resolver-rules="MAP * ~NOTFOUND"` -- Chrome-level DNS block

Font Glyph Scanner and Script Coverage Analyzer enable JavaScript (they need
it for accurate analysis) but still enforce the other three layers.

## Configuration flags

| Flag                           | Default                          | Description                                |
| ------------------------------ | -------------------------------- | ------------------------------------------ |
| `--enable-browser-analysis`    | off                              | Enable browser analysis pipeline           |
| `--chrome-binary`              | `/usr/bin/chrome-headless-shell` | Path to Chrome binary                      |
| `--chrome-recycle-interval`    | 100                              | Pages per Chrome instance before restart   |
| `--chrome-page-timeout`        | 60000                            | Per-page analysis timeout in ms            |
| `--chrome-max-memory`          | 512                              | Max Chrome RSS in MB before forced restart |
| `--chrome-startup-timeout`     | 10000                            | Chrome startup timeout in ms               |
| `--browser-queue-size`         | 1000                             | Max queued analysis requests               |
| `--browser-profile-ttl`        | 86400                            | Profile cache lifetime in seconds (24h)    |
| `--no-browser-critical-css`    | (enabled)                        | Disable browser-based critical CSS         |
| `--no-browser-lazy-loading`    | (enabled)                        | Disable browser-based lazy load decisions  |
| `--no-browser-lcp-preload`     | (enabled)                        | Disable browser-based LCP detection        |
| `--no-browser-image-sizing`    | (enabled)                        | Disable browser-based image dimensions     |

## Monitoring

### Stats counters

Browser analysis stats appear in the management socket `STATS` and
`BROWSER-STATUS` commands, and in the web console dashboard:

| Counter                                   | Description                            |
| ----------------------------------------- | -------------------------------------- |
| `browser.profiles_generated`              | Templates analyzed and cached          |
| `browser.profiles_used`                   | Cache hits on existing profiles        |
| `browser.analysis_errors`                 | Failures (timeout, Chrome crash, etc.) |
| `browser.chrome_crashes`                  | Chrome process crashes                 |
| `browser.queue_depth`                     | Current queue size                     |
| `browser.scripts_analyzed`                | Scripts evaluated by browser analysis  |
| `browser.scripts_deferrable`              | Scripts identified as safe to defer    |
| `browser.css_inlining_attempted`          | CSS inlining attempts                  |
| `browser.css_inlining_stylesheets_cached` | Stylesheets found in cache             |
| `browser.css_inlining_bytes_inlined`      | Total CSS bytes injected               |

### Management socket

The `BROWSER-STATUS` command on the management socket returns detailed JSON
including Chrome state, queue contents, and per-profile statistics:

```bash
echo "BROWSER-STATUS" | socat - UNIX-CONNECT:/data/pagespeed.sock.mgmt
```

## Error handling

Every failure falls back to the heuristic path:

| Failure                     | Behavior                                     |
| --------------------------- | -------------------------------------------- |
| Chrome binary not found     | Heuristic only, no retry                     |
| Chrome fails to start       | Retry after 2 seconds                        |
| Chrome crashes mid-analysis | Cancel current item, restart Chrome after 2s |
| Analysis timeout            | Skip item, process next in queue             |
| Cache read failure          | Skip item                                    |
| Queue full                  | Head-drop oldest item                        |

The worker logs all browser analysis errors at the `warning` level. Monitor
them in the debug console (/logs) or via the management socket.

## Troubleshooting

### Chrome not available (503 errors in waterfall/diff)

The web console's waterfall viewer and visual diff features return 503 when
Chrome is not running. Check:

1. Is `--enable-browser-analysis` set?
2. Does the Chrome binary exist at the configured path?
3. In Docker: is the worker image the full variant (not the minimal image)?

### High chrome_crashes count

Frequent Chrome crashes usually indicate memory pressure:

- Lower `--chrome-recycle-interval` to restart Chrome more often
- Lower `--chrome-max-memory` to catch leaks earlier
- Check container memory limits -- Chrome needs at least 256MB headroom

### Profiles not being generated

If `profiles_generated` stays at zero while traffic flows:

1. Check `queue_depth` -- if it stays at 0, analysis requests are not being
   enqueued. Verify `--enable-browser-analysis` is set.
2. Check `analysis_errors` -- errors during analysis prevent profile creation.
3. Check `css_inlining_stylesheets_cached` -- if external CSS is not yet
   cached, the worker waits for it before running browser analysis.

### Visual regression gate false positives

The visual regression gate disables JavaScript (SSRF defense). Pages that
rely on CSS-in-JS frameworks (styled-components, Emotion, etc.) will show
differences because their styles are injected by JavaScript. This is a known
limitation. The heuristic path optimizes these pages correctly.

## Next steps

- [Web Console](/docs/workbench/) — Use the waterfall viewer and visual diff
  tools powered by browser analysis
- [HTTP API Reference](/docs/http-api/) — BROWSER-STATUS management command
  and /v1/stats browser counters
- [Configuration Reference](/docs/configuration/) — All browser analysis flags
- [Troubleshooting](/docs/troubleshooting/) — Chrome not found, CDP failures,
  and analysis timeout diagnostics
- [AI readability scanner](/ai-readability/) — a free hosted tool that renders
  any URL in headless Chromium and compares its raw HTML against the rendered
  DOM, showing what a non-JavaScript crawler sees
