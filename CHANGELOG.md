# Changelog

All notable changes to mod_pagespeed are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.15.0+r22] - 2026-08-08

### Changed

- **Optimized `.pagespeed.` resources now declare `Cache-Control: public,
  immutable`.** The URL of a rewritten resource embeds a hash of its content —
  when the content changes, the URL changes — so the response behind a given
  URL can never change. The one-year `max-age` these resources have always
  carried now comes with an explicit `public` and the standard `immutable`
  directive (RFC 8246): browsers that support it (Firefox and Safari) skip
  revalidating these resources even on a user-triggered reload — the case
  where browsers otherwise revalidate every resource on the page despite a
  valid freshness lifetime — and the explicit `public` makes the responses
  cacheable on CDNs that require it (such as Google Cloud CDN) without extra
  configuration. Other browsers and caches ignore the new directives, so
  behavior there is unchanged. The upgrade is applied only to responses that
  were already publicly cacheable: a resource derived from any `private`,
  `no-cache`, or `no-store` input keeps its restricted caching exactly as
  before, and responses served under a non-matching URL (for example after a
  stale link) keep their existing short, private lifetime.

- **In-place optimization no longer varies its output by request header.** When
  PageSpeed optimizes an image at its original URL (rather than at a rewritten
  `.pagespeed.` URL), it now optimizes that image identically for every client
  and serves the same bytes to all of them. It may still convert between
  formats where the target is universally supported — a photographic PNG still
  becomes a JPEG when `convert_png_to_jpeg` is enabled — but never based on who
  is asking; the request-dependent targets (WebP, AVIF) are no longer chosen in
  place. These responses no longer carry a `Vary: Accept` or `Vary: User-Agent`
  header, and are no longer downgraded to `Cache-Control: private` for Internet
  Explorer, so they are plainly cacheable by browsers, proxies and CDNs with no
  special configuration. Conversion to WebP and AVIF is unchanged on rewritten
  URLs, where the chosen format is part of the URL itself.

### Fixed

- **The JS minifier no longer emits unparseable output for valid JavaScript
  when a comment sits between `let` and its binding.** The tokenizer's `let`
  declaration lookahead skipped whitespace but not comments, so `let` in
  `let /*c*/ row …` (or `let//…` with the binding on the next line) was
  misread as an identifier. On that reading a linebreak after the binding
  looked droppable — but the output re-parses with `let` as a declaration
  keyword, where the linebreak can be required: `let /*c*/ row\n+4;` was
  minified to `let row+4;`, a syntax error that breaks the entire script.
  The lookahead now skips `//` and `/*…*/` comments too, so the declaration
  is recognized and the linebreak is preserved. Minifier output for the
  full differential corpus (including the third-party originals) is
  byte-identical to before — the change only repairs the misread shapes.

- **The JS minifier no longer fuses adjacent operator tokens into a different
  operator when it drops whitespace on unparseable input.** When the minifier
  removes whitespace or comments between two operator characters, the pair
  could re-lex as a single, different operator — `= =` became `==`,
  `& =` became `&=`, `. 0` became the number `.0`, and a dropped comment
  could glue tokens across the gap (`var d = /*c*/ > x` became `var d=>x`).
  Such pairs can only be adjacent in JavaScript that is already unparseable,
  so no valid page changes behavior: minifier output for valid JavaScript is
  byte-identical to before (verified against the full differential corpus,
  including the third-party originals). What changes is that minified output
  for broken input no longer silently turns two operators into one. The
  minifier's decline pass-through (used for constructs it cannot model) also
  no longer fuses the minified prefix with the unmodified remainder at the
  seam when both sides are word characters.

- **Optimized HTML no longer carries an `s-maxage` directive inviting shared
  caches to store it.** Optimized pages are served with
  `Cache-Control: max-age=0, no-cache`, but an `s-maxage` value could survive
  alongside it — inherited from the origin response, or from the short
  shared-cache window PageSpeed itself adds while a resource awaits
  optimization. Because `s-maxage` overrides `max-age` for shared caches, a
  CDN or proxy could briefly hold and re-serve one visitor's optimized page
  to other visitors. The page can embed image URLs whose format was chosen
  for the original requester's browser, so in setups with a shared cache in
  front this could intermittently serve images in a format the receiving
  browser cannot display. Deployments using the downstream-cache integration
  (`DownstreamCachePurgeLocationPrefix` and related directives) are
  unaffected: that feature intentionally preserves the origin's
  `Cache-Control` on optimized HTML, and continues to. The same applies with
  `ModifyCachingHeaders off`, which disables all of PageSpeed's caching-header
  rewriting including this fix. `s-maxage` handling on non-HTML resources is
  unchanged. If you operate a shared cache in front of mod_pagespeed, an
  update is recommended.

- **Safari 16+ and Firefox 132+ now receive WebP on rewritten image URLs.**
  Image rewriting chooses an image's output format from the `Accept` header the
  browser sent with the page request. Safari has never listed image formats in
  that header, and Firefox stopped doing so in version 132, so neither browser
  was offered WebP and both were served the original format instead — a JPEG
  where Chrome received a WebP or an AVIF. Nothing reported an error; the only
  visible symptom was that pages weighed more in those browsers than they
  needed to. Both are now recognised as WebP-capable from the browser
  identification they send: Safari 16 and later, Firefox 132 and later. On one
  representative photograph, an affected browser previously transferred 554 KB
  where Chrome transferred 127 KB; it now receives the WebP at 292 KB. Real
  pages will vary, but the saving applies to every photographic image on the
  page. The floor is Safari 16 deliberately: whether Safari 14 and 15 can
  decode WebP depends on the version of macOS under them, which the browser
  identification cannot reveal, and serving WebP to the one combination that
  cannot decode it would render broken images — Safari 16 is the first version
  free of that ambiguity. There is nothing to configure and no cache to clear.
  Newly eligible browsers begin requesting a format that may not have been
  produced yet, so expect a short warm-up during which they are served the
  original image while the WebP is generated in the background — the same
  behaviour as any cold cache. Existing optimized images stay valid and are not
  regenerated. AVIF is unaffected and continues to be offered only to browsers
  that ask for it by name, so these browsers receive WebP rather than AVIF
 .

### Upgrade Notes

- **In-place optimization: retired directives and one output change.** The
  `in_place_optimize_for_browser` filter and the `AllowVaryOn` and
  `PrivateNotVaryForIE` directives are retired. They are still accepted so that
  an existing configuration keeps loading — you will see a warning in the error
  log — but they no longer have any effect and should be removed. If you enabled
  `in_place_optimize_for_browser` (directly, or through the
  `OptimizeForBandwidth` rewrite level), images served at their original URLs
  will now be recompressed in place rather than converted to WebP or AVIF, so
  those particular responses get larger. Sites whose HTML PageSpeed rewrites are
  unaffected: the smaller WebP and AVIF variants continue to be served from the
  rewritten URLs the HTML points at. If you had configured a reverse proxy, CDN
  or Varnish instance to handle `Vary: Accept` or `Vary: User-Agent` on
  PageSpeed image responses, that configuration is no longer needed. One
  behavioral note: configurations that used `AllowVaryOn "None"` or
  `AllowVaryOn "Accept"` to keep the `...QualityForSaveData` settings from
  being applied no longer get that suppression — the Save-Data qualities are
  now used on rewritten URLs whenever they are configured; unset them if you
  do not want them. Expect a one-time re-optimization pass after upgrading.

## [1.15.0+r21] - 2026-08-01

### Added

- Module scripts (`<script type="module">`) are hinted again: collected module
  dependencies are emitted as `rel=modulepreload` in the `Link` response
  header. Modules carrying `integrity` or `crossorigin="use-credentials"` are
  left unhinted. Mixed-version deployments sharing a cache degrade cleanly
 .
- nginx: new server-scope directive `WebBotAuthBotDetection` (default off).
  When enabled, a cryptographically verified Web Bot Auth signature (RFC 9421)
  classifies the request as an automated client regardless of its user-agent
  string. With it off — the default — Web Bot Auth verification remains
  observe-only, exactly as before.

### Removed

- The legacy JavaScript minifier. The tokenizer-based minifier (the default
  since 1.10.33.0) is now the only JavaScript minifier.
  `UseExperimentalJsMinifier` remains accepted on every port, is ignored, and
  logs a deprecation warning; configurations carrying it start normally.
  Sites that had explicitly set it re-optimize their JavaScript once in the
  background after upgrading; sites that ran the legacy minifier also gain
  module minification and source-map support.

### Changed

- The known automated-client list used for optimization decisions has been
  brought up to date (it predated the current generation of AI assistant
  fetchers, HTTP client libraries, and command-line tools), and token matching
  now recognizes parenthesized user-agent comment forms. `curl` and `wget`
  are now classified as automated clients.
- `defer_javascript` (and the filters sharing its gate: `disable_javascript`,
  `defer_iframe`, `fix_reflow`, `support_noscript`) no longer applies to
  automated clients, including search-engine crawlers: they receive the page's
  normal authored script markup instead of deferred markup that only
  PageSpeed's client-side runtime can execute.
- WebP support is now determined from the browser's `Accept` request header
  alone, instead of from hand-maintained lists of browser version strings. A
  browser that advertises WebP is taken to support every flavour of it, as AVIF
  already was; browsers that do not advertise WebP are unaffected. This also
  fixes browsers whose major version number reached three digits (current
  Chrome, Edge and Opera, and Chrome on iOS) being read as incapable of
  animated WebP, and Chrome on iOS losing lossless/alpha WebP. Sites using
  `convert_to_webp_animated`, `convert_to_webp_lossless` or
  `in_place_optimize_for_browser` should expect a one-time re-optimization pass
  after upgrading.
  No purge or manual invalidation of a downstream or CDN cache is required:
  where an image's optimized output changes, it is published under a new
  rewritten URL that the HTML is updated to point at, while previously
  rewritten URLs keep resolving and age out normally.

### Fixed

- JavaScript minification now makes the correct regex-versus-division decision
  after `await` and after the `of` of a `for...of` loop: a regular-expression
  literal in that position keeps its interior spacing instead of being
  rewritten into a different pattern (e.g. `return await /a +b/.test(s)`),
  matching the existing `yield` behavior. Line breaks after these words are
  preserved whenever removing one could change how the script re-parses.
- JavaScript minification can no longer assemble a comment delimiter that was
  not in the input: deleting whitespace no longer welds a division or
  regex-closing slash onto a following `*` (forming `/*` and silently
  commenting out the rest of the script), and the same guard now covers
  retained IE conditional-compilation comments next to a slash. Scripts in
  which `await` or `yield` may really be plain variable names and a safe
  rewrite cannot be guaranteed are now declined — served byte-for-byte
  unchanged — instead of minified wrongly.
- JavaScript minification could corrupt a script in which a line break
  separates a postfix `++`/`--` from a next statement that begins with an
  opening parenthesis, or with a leading-dot number such as `.5`. That line
  break is what keeps the two statements apart — without it the code re-parses
  as a call or member access on the value just incremented, which the browser
  rejects as a syntax error — but the minifier removed it and reported
  success, so the script was served broken with nothing logged. Such line
  breaks are now preserved (including when carried inside a comment). Line
  breaks that a following binary operator genuinely continues are still
  removed, and already-correct minified output is byte-for-byte unchanged.
- A stray `;` after a rule inside an `@media` block no longer makes the whole
  stylesheet fall back to its original bytes: such sheets now minify, combine,
  and participate in `prioritize_critical_css` like any other stylesheet.
  Sheets that still fail to parse are served byte-for-byte unchanged, as
  before.
- JavaScript minification no longer merges a division operator into a retained
  IE conditional-compilation comment (`/*@ ... @*/`). The `/` and the comment's
  opening `/*` could fuse into `//`, turning the rest of the line into a
  comment and silently changing what the script computes. A separating space
  is now kept whenever the two would otherwise join, and the equivalent hazard
  after such a comment is guarded the same way.
- JavaScript minification no longer removes the space between a bare `0`
  literal and a following property access (`0 .toString()`). Removing it made
  the period parse as a decimal point, turning valid code into a script that
  fails to parse. Other numeric literals are unaffected.

## [1.15.0+r20] - 2026-07-23

### Changed

- Pages that set a strict Content-Security-Policy `base-uri` policy blocking all
  `<base>` elements (e.g. `base-uri 'none'`) now retain more optimization. When
  the policy guarantees the browser will ignore a `<base>` tag, the optimizer no
  longer conservatively disables rewriting on that page; other `base-uri`
  policies remain handled conservatively.

### Fixed

- License management in the admin console is now offered based on the server's
  authoritative global-admin determination. Operators who serve the global
  admin console at a custom (renamed) path can now purchase, apply, and activate
  licenses instead of finding those controls unexpectedly hidden.
- Histogram percentile stats (median/90/95/99) for very-small sample counts are
  now omitted instead of shown as a -5000 no-data placeholder in the admin
  console and `/histograms` output.
- **IIS: unified `pagespeed.config` resolution to a single documented source of
  truth.** The IIS module previously resolved its configuration through more
  than one independent code path, so editing one of the installed config files
  could appear to have no effect. Resolution now follows one documented
  precedence chain, shared by config load, change-detection, and the
  engage/serve gate; the module logs which file is in effect and warns when
  more than one config file exists with differing content. Behavior for a
  standard single site is unchanged.

### Upgrade Notes

- **IIS configuration resolution is now unified and documented — no files on
  your system were changed, deleted, moved, or overwritten by this upgrade.**
  IIS installs ship a `pagespeed.config` in two places: a machine-global
  default under `%ProgramData%\We-Amp\PageSpeed\`, and a per-site copy in each
  site's web root. Both are preserved across upgrades (they always have been).
  What changed is that the resolution order is now one documented rule:

  1. `%ProgramData%\We-Amp\PageSpeed\pagespeed.config` — machine-global base default
  2. `%ProgramData%\We-Amp\IISWebSpeed\pagespeed.config` — legacy fallback (upgrade-from-IISpeed installs only)
  3. `<site physical path>\pagespeed.config` — per-site override, **authoritative when present**

  The per-site file in the site's web root takes precedence; the `%ProgramData%`
  locations act as the base layer underneath it. If you previously edited one
  copy and did not see the expected effect, re-check which file you edited — for
  a standard single site, edit the copy in that site's web root. On startup the
  module now logs the file it resolved as effective and warns when a second
  config with different content is present, so any earlier ambiguity about which
  file applies is now visible in the log.

## [1.15.0] - 2026-06-01

### Changed

- **Version line renumbered 1.1 -> 1.15**. mod_pagespeed 1.15 is the
  maintained continuation of the Apache-lineage codebase and the direct successor
  to Google's final mod_pagespeed release (1.14.36.1); the prior "1.1" numbering
  read as older than Google's line on package/version surfaces. This is a
  version-identity change with no functional change from the 1.1.0 line. Package
  names (`mod-pagespeed`, `nginx-module-pagespeed`, `ea-apache24-mod_pagespeed`)
  and the runtime behavior are unchanged; the `X-Mod-Pagespeed` header now reports
  `1.15.0.0`.

## [1.1.0-beta.1] - 2026-02-25

First release under We-Amp stewardship. This is a complete modernization of
the mod_pagespeed codebase, expanding from Apache-only to four deployment
platforms while preserving the full set of 40+ optimization filters.

### Added

#### New Platforms
- **Envoy filter** (experimental): PageSpeed as an Envoy HTTP filter with IPRO
  and HTML rewriting support. Uses `CurlUrlAsyncFetcher` (linked against
  BoringSSL) for independent outbound fetching. Includes standalone binary
  (~212MB) and shared library (~113MB).
- **nginx module** (stable): Dynamic module (`ngx_pagespeed_module.so`) for
  nginx 1.26.x stable and 1.27.x mainline. Full IPRO and HTML rewriting.
- **IIS module** (experimental): Native C++ IIS module (`pagespeed_iis.dll`)
  for Windows Server. Supports IPRO, HTML rewriting, streaming responses with
  chunked transfer encoding, and the full admin UI.

#### Build System
- **Bazel 7.x build system** replacing the legacy GYP/gyp_chromium toolchain.
  WORKSPACE-based dependencies with bzlmod disabled.
- **Docker development environment** with `docker-compose.yml` providing
  dev container, Redis, and Memcached services.
- **Clang + GCC 13 libstdc++** build configuration (`--config=clang-libstdcxx13`)
  for C++20/C++23 support.
- **Sanitizer configs**: `--config=clang-asan` (AddressSanitizer) and
  `--config=clang-tsan` (ThreadSanitizer).
- **Windows cross-compilation** via `--config=windows --config=clang-cl`
  using clang-cl toolchain.
- **GitHub Actions CI/CD** replacing Travis CI, with workflows for unit tests,
  Apache/Nginx/Envoy system tests, and automated release packaging.

#### Core Improvements
- **C++20 standard** (C++23 for Cyclone cache files via `per_file_copt`).
- **Cyclone cache** integration for high-performance disk caching.
- **CurlUrlAsyncFetcher** replacing Serf for HTTP fetching, linked against
  BoringSSL for HTTPS support.
- **APR decoupling**: Core libraries no longer depend on Apache Portable Runtime
  for non-Apache deployments.
- **libcurl linked against BoringSSL** on Linux (same instance used by Envoy,
  no symbol conflicts).

#### Testing
- **Python test framework** (`test/system/pagespeed_test_framework/`) shared
  across all four platforms with `fetch_until` polling, statistics delta
  checking, and WebP negotiation helpers.
- **Comprehensive system test suites**:
  - Apache: 195 pass, 14 skip
  - Nginx: 169 pass, 40 skip
  - Envoy: 189 pass, 20 skip
  - IIS: 350 pass (Python) + 19 pass (C++ unit)
- **pytest markers** for test categorization: `@pytest.mark.ipro`,
  `@pytest.mark.html_rewrite`, `@pytest.mark.slow`, etc.

#### Envoy-Specific
- Admin authentication with token, IP allowlist, and rate limiting.
- Circuit breaker for resource fetching.
- Redis cache backend support.
- Prometheus metrics endpoint at `/stats/prometheus`.
- Health endpoint at `/pagespeed/health`.

#### IIS-Specific
- `web.config` XML configuration parsing.
- Windows shared memory (`WindowsSharedMem`) for cross-process state.
- BCrypt API for cryptographic operations.
- Performance counter integration with Windows `perfmon.exe`.
- IIS Express support for development and testing.

### Changed

- **Version scheme**: Moved from Google's `1.15.0.0` four-part versioning to
  semantic versioning (`1.1.0-beta.1`).
- **Branding**: Updated from Google Inc. to We-Amp.
- **Apache 2.2 support dropped**: Only Apache 2.4+ is supported (single `.so`).
- **Build default paths**: Packaging scripts updated from GYP `out/Release/`
  to Bazel `bazel-bin/pagespeed/` paths.
- **`version.h` generation**: Now includes git short hash as `LASTCHANGE` and
  supports `PRERELEASE` field for pre-release versions.

### Removed

- GYP/gyp_chromium build system.
- Apache 2.2 module (`mod_pagespeed_ap24.so` split).
- Google-hosted update repository integration (cron jobs retained but
  repository config cleared).
- Travis CI configuration (replaced by GitHub Actions).

### Known Issues

#### Envoy (Experimental)
- `combine_css` may timeout due to async worker pool coordination.
- IPRO cache lifetime returns upstream `max-age` minus cache time instead of
  `implicit_cache_ttl_ms`.
- `X-PSA-Blocking-Rewrite` header is not supported (Envoy is non-blocking).
- Version header shows placeholders when built without workspace status.

#### IIS (Experimental)
- IPRO async cache stubs return misses (Redis/disk caches not functional).
- Beacon data silently discarded.
- Requires `allowDoubleEscaping="true"` in IIS for combined resource URLs.

## Previous Releases (Google Era)

For releases prior to We-Amp stewardship, see the
[Google mod_pagespeed release notes](https://www.modpagespeed.com/doc/release_notes).

The last official Google release was **1.14.36.1** (August 2020).
Version 1.15.0.0 was assigned in December 2018 but never released.

[1.1.0-beta.1]: https://github.com/we-amp/mod_pagespeed/releases/tag/v1.1.0-beta.1
