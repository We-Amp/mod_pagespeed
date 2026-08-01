# Changelog

All notable changes to mod_pagespeed are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
