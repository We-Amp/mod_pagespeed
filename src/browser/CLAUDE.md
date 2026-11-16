# Browser Module

CDP client, Chrome process management, and browser analysis components for
headless browser optimization.

## Integration Overview

```
Worker (libuv event loop)
  |
  +-- BrowserAnalysisManager (src/worker/)
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
        +-- FontGlyphScanner    -- code point scanning + @font-face
        +-- ScriptCoverageAnalyzer -- Profiler coverage + deferral
```

The worker queues URLs into `AnalysisQueue`. `BrowserAnalysisManager` dequeues
them, spawns/reuses a `ChromeProcess`, and runs the appropriate analyzer. Results
feed back into the worker's cache write pipeline.

## Components

| Component | Files | Purpose |
|-----------|-------|---------|
| CDP Types | `cdp_types.h` | Core CDP types (Command, Response, Event, Config) |
| CDP Client | `cdp_client.h/cc` | JSON-RPC over pipe transport |
| Chrome Process | `chrome_process.h/cc` | Process lifecycle (spawn, recycle, RSS) |
| Template Detector | `template_detector.h/cc` | FNV-1a DOM structure hashing |
| Optimization Profile | `optimization_profile.h/cc` | JSON-serialized per-template profiles |
| Browser CSS Extractor | `browser_css_extractor.h/cc` | CSS Coverage API extraction |
| Page Analyzer | `page_analysis.h/cc` | LCP, fold, CLS, image dimensions |
| Analysis Queue | `analysis_queue.h/cc` | Bounded priority queue with dedup |
| Unused CSS Remover | `unused_css_remover.h/cc` | CSS Coverage data → dead rule removal |
| Visual Regression Gate | `visual_regression_gate.h/cc` | PNG pixel comparison for visual diff |
| Font Glyph Scanner | `font_glyph_scanner.h/cc` | TreeWalker code point scanning + @font-face |
| Script Coverage Analyzer | `script_coverage_analyzer.h/cc` | Profiler domain coverage + deferral advice |

## CDP Pipe Transport

Chrome DevTools Protocol over `--remote-debugging-pipe` (FD 3/4).
Messages are null-byte delimited JSON-RPC. Design decisions documented in
the headless-browser design document (Expert Review appendix):
- B2: `SendCommand()` includes `session_id` parameter
- H5: Chrome pipes on main libuv event loop (not separate loop)
- M2: Per-command `uv_timer_t` timeout (default 30s)
- M3: `CancelAll()` on pipe EOF resolves all pending callbacks
- M10: Large JSON (>64KB) parsed off event loop via `uv_queue_work()`

## Key Constants

| Constant | Value | Purpose |
|----------|-------|---------|
| `kMaxMessageSize` | 10 MB | Reject oversized CDP messages |
| `kLargeMessageThreshold` | 64 KB | Off-thread JSON parsing threshold |
| `kRssCheckIntervalMs` | 5000 | RSS monitoring interval |
| `kKillTimeoutMs` | 5000 | SIGKILL timeout after SIGTERM |

## Chrome Launch Flags

Headless Chrome is spawned with `--remote-debugging-pipe`, `--headless=new`,
`--disable-gpu`, `--user-data-dir=<runtime dir>/chrome`,
`--host-resolver-rules="MAP * ~NOTFOUND"` (SSRF DNS block), and various
isolation flags (`--disable-dev-shm-usage`, `--disable-extensions`,
`--disable-background-networking`, `--disable-default-apps`, `--disable-sync`,
`--disable-translate`, `--metrics-recording-only`, `--no-first-run`).

### The sandbox

`--no-sandbox` used to be pushed **unconditionally**, which made every
headless render unsandboxed. It is now emitted **only** when the operator
passed `--browser-sandbox=off`.

Chrome's Linux sandbox is two layers. Layer 2 (the seccomp-bpf renderer
filter) is Chrome's own. Layer 1 (the namespace/zygote sandbox) needs either
unprivileged user namespaces or a setuid `chrome-sandbox` helper — and the
setuid path is structurally unavailable to this daemon, because its unit sets
`NoNewPrivileges=yes`, under which `execve` of a setuid binary does not
elevate. So: **user namespaces or nothing**, and the daemon's unit must never
set `PrivateUsers=`.

`ProbeBrowserSandbox()` (`src/browser/browser_sandbox.{h,cc}`) is the single
decision input: it forks a child that attempts `unshare(CLONE_NEWUSER)`, and
it reports uid 0 as unavailable because Chrome itself refuses to sandbox as
root. Sysctl and AppArmor state is read for the log line as a **diagnostic
only** — never as the decision. `--browser-sandbox` takes exactly
`require|off`; there is deliberately no `auto`, because an `auto` that
silently falls back to `--no-sandbox` is the defect being fixed.

| Mode | Probe | Result |
|------|-------|--------|
| `require` (default) | available | Chrome runs sandboxed; `/v1/health` reports `browser_sandbox: "on"` |
| `require` | unavailable | **browser analysis refuses to initialize**, one named ERROR, `browser_sandbox: "unavailable"`. The daemon keeps serving — analysis is optional and default-off, and it never falls back to an unsandboxed browser |
| `off` | not consulted | `--no-sandbox` is re-added, a WARNING banner is logged at startup and on every spawn, `browser_sandbox: "off"` |

**The probe's failure modes are distinct, and that is load-bearing.**
`ReapWithTimeout` returns a `ReapResult`, not a bare int, and
`BrowserSandboxProbeReason()` maps a `BrowserSandboxProbeOutcome` to the clause
the startup refusal quotes. H4 folded "killed by a signal" into the same `-1`
as "timed out", so a `SystemCallFilter=`/`RestrictNamespaces=` denial and a
loaded machine produced the identical unactionable line. `SIGSYS` now gets its
own reason, naming the mechanism and pointing at the browser-analysis docs and
the container remedy — deliberately NOT at a concrete drop-in file. The two
transient outcomes — a probe that does not answer inside its budget, and a
fork that fails under resource pressure — say nothing about the capability
being probed, so they are retried (bounded) before any outcome is reported;
only a persistent stall surfaces as the timeout reason at all. The
drop-in now exists (`20-browser-analysis.conf`, shipped active by the package
as a vendor drop-in), and the message still does not name it: the same probe runs under a
systemd unit, in a container under the runtime's profile, and embedded in a
host process, so a message naming a systemd path would be wrong in two of
those three (a unit test asserts the clause names no `.conf`). This is the whole
coupling H6 relies on: the daemon runs inside whatever filter its service
runs under, so a too-tight profile is reported by the probe at startup instead
of by a SIGSYS'd Chrome later. The reason function is pure and
platform-independent so the exact wording (which
`tools/packaging/smoke-browser-sandbox.sh` leg 6 greps for) is unit-testable
without arranging a real kernel denial.

Whether a filter is attached at all is a separate, read-only answer:
`src/worker/syscall_filter.{h,cc}` reads `/proc/self/status` and publishes
`syscall_filter` (`"filtered"` | `"none"` | `"unknown"`) on `/v1/health` and
`/v1/stats`. Never from config, and deliberately no flag — the daemon does not
install the filter (the service manager does, before `execve`; in a container
the runtime's default profile does), and a flag naming a control the process
does not own is the inversion an earlier design decision forbids.

**`"filtered"` means A filter is attached, not OUR profile.** Measured, and
easy to get backwards. The shipped unit carries both an enforcing
`SystemCallFilter=` and `SystemCallLog=~@system-service`, and systemd
implements the latter AS a seccomp filter too (it logs a matching call and
permits it), so **a package install reports `"filtered"` whether or not the
operator has opted out of enforcement** — and a **stock container also reports
`"filtered"`**, because container runtimes apply a default seccomp profile to
every container. Any prose that says "containers report none", or that reads
`"filtered"` as "the enforcing profile is in force", is wrong — check it
against `/proc/self/status` before writing it; the posture question is
answered by `systemctl show -p SystemCallFilter`. The non-vacuous observation
is the filter COUNT (`Seccomp_filters:`), which is what the rig compares
against an unfiltered process in the same container.

**The unit's own profile is ENFORCING by default since 2.1** (the allow-list
`@system-service` minus
`@privileged @resources` plus `RestrictNamespaces=yes` is in the unit itself,
never a drop-in, so no drop-in can be installed "alone" and define a bare
list). The browser re-admissions are the separate `20-browser-analysis.conf`
layer, kept separable so it can go back to opt-in on its own; the documented
opt-out is `90-syscall-filter-off.conf.example` copied into `/etc`, which
resets both directives and survives upgrades because the package never writes
there. `deploy/pagespeed-optimizer.syscall-census.md` is the measured basis,
and `tools/packaging/verify-daemon-systemd.sh` carries the `h6-*` legs —
including a positive control that is run twice, once under the shipped default
and once under the shipped opt-out, because a control that cannot fail is not
evidence. Measured there and worth knowing before touching the profile:
`unshare(2)` is INSIDE `@system-service`, so it is `RestrictNamespaces=` and
not the syscall allow-list that governs this probe.

`--user-data-dir` is now always pinned by the daemon (0700, inside the unit's
`RuntimeDirectory`). It used to be unset, leaving Chrome to pick a path of its
own choosing against a `ProtectSystem=strict` sandbox.

**The container images still run as root** (see issue #1429 for the shared-GID
contract that has to settle first), so browser analysis in them needs the
documented `PAGESPEED_BROWSER_SANDBOX=off` opt-out until they gain a `USER`
directive. The We-Amp-operated compose files set it explicitly.

## Lifecycle

1. Create `ChromeProcess(loop, config)` + `SetExitCallback()`
2. `Start()` spawns Chrome, creates `CdpClient`, attaches pipes
3. Use `cdp_client()->SendCommand(...)` for CDP operations
4. `IncrementPageCount()` after each page — returns true at recycle threshold
5. `Stop()` sends SIGTERM, then SIGKILL after 5s timeout
6. Exit callback fires — safe to destroy or restart

**Important:** Call `Stop()` and wait for exit callback before destroying
`ChromeProcess`. The destructor is a safety net (immediate SIGKILL), not the
normal cleanup path.

## RSS Monitoring

Linux only (reads `/proc/pid/status` VmRSS field). Returns 0 on other
platforms. Monitors the main Chrome process only, not renderer subprocesses.
Auto-triggers `Stop()` when `max_rss_mb` is exceeded.

## SSRF Defense (4 Layers)

Both CSS extractor and page analyzer use defense-in-depth:
1. `Network.emulateNetworkConditions({offline: true})` — blocks all network
2. `Fetch.enable` + `Fetch.requestPaused` — intercept and fail/serve all requests
3. `Emulation.setScriptExecutionDisabled({value: true})` — no JS (CSS extractor)
4. `--host-resolver-rules="MAP * ~NOTFOUND"` — Chrome-level DNS block

## Session Timeout Pattern (CRITICAL)

Both `BrowserCssExtractor::Session` and `PageAnalyzer::Session` use a
`uv_timer_t` session timeout with `weak_ptr<Session>` capture. The timer
callback MUST clean up `t->data` BEFORE calling `FinishError()`, because
`Finish()` also tries to clean up the timer. Without this ordering:
- `Finish()` deletes the `weak_ptr` → timer callback double-frees it
- `Finish()` calls `uv_close()` → timer callback double-closes it

Correct pattern:
```cpp
[](uv_timer_t* t) {
  auto* wp = static_cast<std::weak_ptr<Session>*>(t->data);
  auto s = wp->lock();
  delete wp;            // Clean up BEFORE calling FinishError
  t->data = nullptr;    // So Finish() sees data==nullptr and skips
  if (s) s->FinishError("session timeout");
  uv_close(...);
}
```

## Threading Model

All CDP I/O runs on the main libuv event loop. Large CDP messages (>64KB) are
parsed on the libuv thread pool via `uv_queue_work()`. A `shared_ptr<bool>`
alive flag prevents use-after-free if the client is destroyed while a parse
is in-flight.

## CdpClient Patterns

- **Reusable read buffer**: `alloc_buffer_` (vector<char>) avoids per-read
  heap allocation. OnAlloc resizes only when needed.
- **Running offset in OnRead**: Messages are null-byte delimited. Process
  all messages with a running offset, then single `erase(0, start)` at the
  end — avoids O(n²) from repeated `erase(0, pos+1)`.
- **WriteContext owns data**: `std::string data` directly in WriteContext,
  not a separate `new char[]` + memcpy.
- **Unsigned command IDs**: `uint32_t next_id_` avoids signed overflow UB.
  Skip 0 on wrap: `if (next_id_ == 0) next_id_ = 1;`
- **Destructor null guard**: Destructor nulls `read_pipe_->data` to prevent
  use-after-free from queued `OnRead` callbacks.

## SSRF Defense — Phase 4 Components

| Component | Network Offline | Fetch Intercept | JS Disabled | DNS Block |
|-----------|:-:|:-:|:-:|:-:|
| Unused CSS Remover | N/A | N/A | N/A | N/A |
| Visual Regression Gate | Yes | Yes (fail all) | Yes | Yes |
| Font Glyph Scanner | Yes | Yes (fail all) | **No** (needs JS) | Yes |
| Script Coverage Analyzer | Yes | Yes (serve cache) | **No** (needs JS) | Yes |

Font Glyph Scanner and Script Coverage Analyzer enable JS for accurate analysis
but block all network via offline mode + Fetch interception + DNS blocking.

## Known Limitations (Phase 4)

- **Font subsetting approximation**: All page code points are assigned to every
  @font-face declaration. True per-font subsetting requires parsing font files
  (cmap table) to know which glyphs each font covers.
- **`modifies_dom_before_paint` never populated**: ScriptCoverageAnalyzer's
  `ScriptInfo::modifies_dom_before_paint` field is always false. Detecting DOM
  mutations before paint would require MutationObserver injection, deferred.
- **`document.write` detection inline-only**: Only checks inline script text
  content, not external script source. External scripts that call
  `document.write` are not flagged.
- **Visual regression JS limitation**: VisualRegressionGate disables JS
  (SSRF defense), so CSS-in-JS pages may show false positives.
- **`<noscript>` and the JS-off renders**: with script execution
  disabled Blink renders `<noscript>` content and applies its CSS. The two
  JS-off renders must measure what a scripting browser renders, so only their
  documents go through `StripNoscriptElements` (`src/worker/noscript_strip.h`):
  the coverage render's (`browser_internal::BuildAnalysisDocuments`, after the
  inliner, which needs a loadCSS `<noscript>` twin) and both validation
  documents (`BuildValidationDocuments`, BEFORE the stylesheet removal, with
  head-prelude placement carried over from the unstripped page). The page
  analysis, script coverage and agent renders run scripts and get the page
  unchanged. The removal is a spec tokenizer plus a light tree model (foreign
  content, integration points), and it FAILS CLOSED: an unreliable result, or
  one whose kept start tags disagree with HtmlScanner's `elements`
  (`NoscriptStripAgreesWithScan`: the same names in order for `<body>` and the
   200 elements after it, then a bounded shortest edit script over the
  whole document that refuses any run of 3+ elements on one side only and
   more than 2 * max(3, 1%) edits, with deletions held to 1 per run and
   3 in all, counted gross so insertions cannot pair them away),
  refuses validation and skips the coverage
  render. The gate also refuses a uniform-colour reference render
  (`RegressionResult::reference_uniform`). Never strip the markup the scanner
  reads for the combined stylesheet: that changes the validation hash. Pages
  whose `<noscript>` renders with scripts off (`noscript_affects_render`) get a
  salted binding (`ValidationBindingFor`), and so do sheets that escape a quote,
  brace or comma outside a string (`CombinedCssHasStructuralEscape`:
  misread before that fix, so their records are made again once). Keep JS disabled; enabling it is not
  the fix. `tools/async-css-probe/noscript_compare.mjs` checks the removal
  against Chromium. Comments: the removal, the validator's placement replay
  and the injector all read them with `html_scan::ScanComment`, which is the
  serve path's vendored HtmlLexer reading and a browser's (`<!-->` and
  `<!--->` end at once, `--!>` ends a comment); `html_css_injector_test`
  checks it against the lexer. The replay writes comments as the serve path
  writes them (`html_scan::CommentText`). The removal reads `<noscript>`
  content as raw text and the lexer as markup, so where a removed
  `<noscript>` and a comment the lexer reads partially overlap,
  `BuildValidationDocuments` refuses.
- **Inline scripts get no per-element coverage**: V8's precise coverage
  collapses ALL inline scripts onto the document URL, so no per-element join
  exists. Inline scripts are reported (`is_inline`, empty url) but classify
  `kNoCoverageData` and are never deferred.
- **Cross-origin scripts can never earn coverage evidence**: the analysis
  browser is offline (SSRF defense) and only same-origin cached bytes are
  mapped, so cross-origin scripts stay blocked, classify `kNoCoverageData`,
  and are never deferred.
