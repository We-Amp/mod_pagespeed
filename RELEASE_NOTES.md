# mod_pagespeed 1.15.0+r22 Release Notes

**Release date:** 2026-08-08
**Status:** Stable

## Highlights

- **In-place optimization now serves one variant to every client, and its
  responses no longer carry a `Vary` header.** When PageSpeed optimizes an
  image at its original URL (rather than at a rewritten `.pagespeed.` URL), it
  now optimizes that image identically for every client and serves the same
  bytes to all of them. It no longer picks WebP, AVIF, a mobile quality or a
  Save-Data quality from the request, so its responses carry no `Vary: Accept`
  and no `Vary: User-Agent`, and are no longer downgraded to
  `Cache-Control: private` for Internet Explorer. They are cacheable by
  browsers, proxies and CDNs with no special configuration, and one cache
  entry now serves every client instead of one per browser capability.
  Conversion to a universally supported format still happens in place — a
  photographic PNG still becomes a JPEG when `convert_png_to_jpeg` is enabled
  — but never based on who is asking. WebP and AVIF are still selected on
  rewritten URLs, where the chosen format is part of the URL, so no `Vary`
  header is required. Image inlining into CSS is
  likewise suppressed on the in-place path: whether a browser supports `data:`
  URIs is a per-request property, so CSS optimized in place now always renders
  as the no-inlining variant, byte-identical for every client.

  This is an intermediate step, not the end state: it brings in-place
  optimization in line with the request-independent model ModPageSpeed 2.0
  uses, and future releases will continue modernizing in-place optimization
  by converging on that architecture.

  **What changes for you:** the `in_place_optimize_for_browser` filter and the
  `AllowVaryOn` and `PrivateNotVaryForIE` directives are retired. They are
  still accepted so that an existing configuration keeps loading — you will
  see a warning in the error log — but they no longer have any effect and
  should be removed. If you enabled `in_place_optimize_for_browser` (directly,
  or through the `OptimizeForBandwidth` rewrite level), images served at their
  original URLs will now be recompressed in place rather than converted to
  WebP or AVIF, so those particular responses get larger. Sites whose HTML
  PageSpeed rewrites are unaffected: the smaller WebP and AVIF variants
  continue to be served from the rewritten URLs the HTML points at. If you had
  configured a reverse proxy, CDN or Varnish instance to handle `Vary: Accept`
  or `Vary: User-Agent` on PageSpeed image responses, that configuration is no
  longer needed. One behavioral note: configurations that used
  `AllowVaryOn "None"` or `AllowVaryOn "Accept"` to keep the
  `...QualityForSaveData` settings from being applied no longer get that
  suppression — the Save-Data qualities are now used on rewritten URLs
  whenever they are configured; unset them if you do not want them. Expect a
  one-time re-optimization pass after upgrading.

- **Safari 16+ and Firefox 132+ now receive WebP on rewritten image URLs.**
  Image rewriting chooses an image's output format from the `Accept` header
  the browser sent with the page request. When that header does not list
  image formats, the browser was served the original format instead — a JPEG
  where Chrome received a WebP or an AVIF. Nothing reported an error; the
  only visible symptom was that pages weighed more in those browsers than
  they needed to. Safari 16 and later and Firefox 132 and later are now
  recognised as WebP-capable from the browser identification they send and
  receive the WebP variant, so photographic images that PageSpeed rewrites
  transfer substantially fewer bytes in those browsers.

  **What changes for you:** there is nothing to configure and no cache to
  clear. Newly eligible browsers begin requesting a format that may not have
  been produced yet, so expect a short warm-up during which they are served
  the original image while the WebP is generated in the background — the same
  behaviour as any cold cache. Existing optimized images stay valid and are
  not regenerated. AVIF is unaffected and continues to be offered only to
  browsers that ask for it by name, so these browsers receive WebP rather
  than AVIF.

- **Optimized `.pagespeed.` resources now declare
  `Cache-Control: public, immutable`.** The URL of a rewritten resource
  embeds a hash of its content — when the content changes, the URL changes —
  so the response behind a given URL can never change. The one-year `max-age`
  these resources have always carried now comes with an explicit `public` and
  the standard `immutable` directive (RFC 8246): browsers that support it
  (Firefox and Safari) skip revalidating these resources even on a
  user-triggered reload — the case where browsers otherwise revalidate every
  resource on the page despite a valid freshness lifetime — and the explicit
  `public` makes the responses cacheable on CDNs that require the token (such
  as Google Cloud CDN) without extra configuration. Other browsers and caches
  ignore the new directives, so behavior there is unchanged. The upgrade is
  applied only to responses that were already publicly cacheable: a resource
  derived from any `private`, `no-cache`, or `no-store` input keeps its
  restricted caching exactly as before, and responses served under a
  non-matching URL (for example after a stale link) keep their existing
  short, private lifetime. Responses served at the original URL by in-place
  optimization are also unchanged: they keep exactly the caching lifetime the
  origin gave them.

- **Optimized HTML no longer carries an `s-maxage` directive inviting shared
  caches to store it.** An `s-maxage` directive could survive on optimized
  HTML, allowing a shared cache to store and re-serve an optimized page that
  was intended for a single visitor. It is now stripped. Deployments using
  the downstream-cache integration (`DownstreamCachePurgeLocationPrefix` and
  related directives) are unaffected: that feature intentionally preserves
  the origin's `Cache-Control` on optimized HTML, and continues to. The same
  applies with `ModifyCachingHeaders off`, which disables all of PageSpeed's
  caching-header rewriting including this fix. `s-maxage` handling on
  non-HTML resources is unchanged.

  **What changes for you:** if you operate a shared cache in front of
  mod_pagespeed, an update is recommended.

- **The JavaScript minifier no longer emits unparseable output for valid
  JavaScript when a comment sits between `let` and its binding.** The
  tokenizer's `let` declaration lookahead skipped whitespace but not
  comments, so `let` in `let /*c*/ row …` (or `let//…` with the binding on
  the next line) was misread as an identifier. On that reading a linebreak
  after the binding looked droppable — but the output re-parses with `let` as
  a declaration keyword, where the linebreak can be required:
  `let /*c*/ row` followed by `+4;` on the next line was minified to
  `let row+4;`, a syntax error that breaks the entire script. The lookahead
  now skips `//` and `/*…*/` comments too, so the declaration is recognized
  and the linebreak is preserved. Output for valid JavaScript is unchanged;
  the fix only repairs the misread shapes. If any of your JavaScript hit
  this, an update is recommended.

---

# mod_pagespeed 1.15.0+r21 Release Notes

**Release date:** 2026-08-01
**Status:** Stable

## Highlights

- **Apache: optimization thread counts are now sized from the machine.** The
  two worker pools that do optimization work — `NumRewriteThreads` for HTML,
  CSS and JavaScript, `NumExpensiveRewriteThreads` for image transcoding — were
  meant to be sized from the server's threading model, but the MPM was asked
  about it before httpd had processed its configuration. On a distribution
  package, where the MPM is a loadable module, it answered with zeroes, the MPM
  read as non-threaded, and the server ran one thread in each pool whatever its
  hardware or configuration. On an httpd built from source with the MPM linked
  in, the same question was answered differently on the second configuration
  pass, and the server ran **four threads in each pool per child process** —
  never a chosen number, and never visible, because the line reporting it was
  emitted below the level the log was open at.

  Both directives now default to `auto`. On Apache each pool is then sized at
  half the CPUs the process is actually permitted to use, divided by the
  child-process ceiling httpd reports — `MaxRequestWorkers / ThreadsPerChild`
  on worker and event, `MaxRequestWorkers` on prefork, capped by `ServerLimit`
  in each case — and never drops below one thread. The number comes from httpd
  itself, so a configuration httpd resolves differently to the arithmetic above
  is the one that counts.

  **What changes for you:** on Apache, the counts now depend on your cores and
  your configured child-process ceiling, not on your hardware alone. Because
  that ceiling is high by default — stock `event` allows 16 child processes —
  most Apache servers resolve to one thread per pool.

  If you run a **distribution package**, that is what you were already running
  and nothing changes. **If you built httpd from source**, this is a reduction:
  four threads per pool per child becomes one per pool for a default
  configuration, because four per child across sixteen children was more
  optimization threads than such a machine has cores. If you want the old
  concurrency back, set `NumRewriteThreads` and `NumExpensiveRewriteThreads`
  explicitly — but size them against your whole server rather than one child.
  A server configured with few children on a many-core machine gets more than
  before, which is the case a fixed default could never serve. Expect somewhat
  higher CPU use while a cold cache warms where the counts went up.
  `NumRewriteThreads` and `NumExpensiveRewriteThreads` remain the opt-out and
  still override the computed value entirely; `auto` (or `0`) asks for the
  automatic sizing explicitly.

  **On nginx, Envoy and IIS the thread counts do not change.** Those ports do
  not yet report how many worker processes share the machine, and rather than
  guess a divisor and risk oversubscribing the host, the policy takes its
  minimum: one thread per pool. On nginx and Envoy that is exactly what they
  ran before. IIS is unchanged for a different reason — the IIS module sizes
  its own optimization pools and this release does not touch that code, so IIS
  keeps the counts it has always used. The directive validation and the startup
  log line below do apply to every port.

  **Before upgrading, check for a negative `NumRewriteThreads` or
  `NumExpensiveRewriteThreads`.** A negative value used to be accepted and then
  crash the server process at startup; it is now rejected when the
  configuration is read, with a message naming the directive. On Apache an
  invalid directive value is a fatal configuration error, so a negative value
  left in place will stop httpd from starting after the upgrade. Change it to
  `auto` first. An implausibly large positive value is clamped rather than
  rejected, with a warning naming both the requested and the resolved count.

  Two related fixes come with it. The CPU budget now comes from what the
  process may actually use — the CPU affinity mask, and the CPU quota on the
  process's own cgroup and its ancestors, which covers a container, a
  Kubernetes pod and a systemd unit with `CPUQuota=` alike — instead of the
  host's core count. And the resolved counts, along with the CPU budget and
  child count they were derived from, are written to the error log at startup;
  previously the line was emitted below Apache's default `LogLevel` and never
  reached the log at all. (The same line is emitted on the other ports, but
  nginx's compiled-in default `error_log` level is `error`, so on nginx it
  still takes `error_log ... warn` to see it.)

- The source tarball no longer contains the `html/` documentation archive.
  Those 82 files are the mod_pagespeed 1.0 documentation, published as the
  `/1.0/` archive on modpagespeed.com; shipping them inside a 1.15 source
  tree placed documentation for a different release next to code it does not
  describe. Nothing else changes: the archive is still published at `/1.0/`,
  and current documentation is at modpagespeed.com — 2.0 under `/docs/`,
  1.15 under `/1.1/docs/`.

- The AVIF still-image encode budget is now configurable on Apache as
  `ModPagespeedAvifTimeoutMs` (server configuration; also accepted inside a
  `<VirtualHost>`). In r20 this setting was only reachable on nginx; on Apache
  it stayed at its 5000 ms default with no way to change it. The tunable
  itself is unchanged: as in r20, a larger budget admits more images to AVIF
  and never selects a lower-quality encoder speed than configured. It only
  becomes settable on Apache now.

- Data-only `<script>` blocks (JSON-LD, plain JSON data, import maps,
  speculation rules, and templates) no longer emit a spurious "Unrecognized
  script" info message. These blocks are deliberate, non-executable markup, so
  the diagnostic was noise; genuinely unrecognized script types still log.

- IPRO recorder statistics are now accurate. `ipro_recorder_failed` counts
  genuine recording failures only (a write/inflate error or a truncated
  response). Previously it also absorbed expected outcomes — non-rewritable
  content types, error (4xx/5xx) and not-modified (304/206) responses, empty
  responses, and load- or size-limited recordings — which each now have their
  own counter (`ipro_recorder_dropped_content_type`, `ipro_recorder_error_status`,
  `ipro_recorder_skipped_transient`, `ipro_recorder_empty`, alongside the
  existing `ipro_recorder_not_cacheable`, `ipro_recorder_dropped_due_to_load`,
  and `ipro_recorder_dropped_due_to_size`). Each recorder outcome is also
  logged for diagnosis.

- A stray `;` after a rule inside an `@media` block — a common hand-authoring
  artifact, as in `@media screen { .a { color: red }; }` — no longer fails the
  stylesheet. Such sheets previously passed through whole: unminified,
  excluded from CSS combining, and skipped by `prioritize_critical_css`. They
  are now handled like any other stylesheet. Sheets that still fail to parse
  are served byte-for-byte unchanged, as before.

- `hint_preload_subresources` again hints `<script type="module">`
  subresources, now using `rel=modulepreload` in the `Link` response header it
  emits (this filter adds no markup to the page). Module scripts stopped being
  hinted in r20: the older `rel=preload; as=script` hint does not match how a
  browser fetches a module, so it could cost an extra fetch rather than save
  one. `rel=modulepreload` matches the module fetch, so the hint is usable.
  Modules carrying `integrity` or `crossorigin="use-credentials"` are left
  unhinted, because a hint for those cannot be matched reliably. Browsers that
  do not act on the hint are unaffected. Servers running mixed versions
  against a shared cache degrade cleanly: older versions skip the new cache
  entries rather than misread them.

- WebP support is now determined from the browser's `Accept` request header
  alone. Which flavours of WebP a browser could handle — lossy, lossless,
  transparent, animated — used to be decided from hand-maintained lists of
  browser version strings. Those lists had to be updated as browsers shipped
  and had gone stale: browsers whose major version number reached three digits
  (current Chrome, Edge and Opera, and Chrome on iOS) were read as incapable of
  animated WebP, and Chrome on iOS also lost lossless and alpha WebP. A browser
  that advertises WebP is now taken to support all of it, matching how AVIF has
  always been handled, so that class of staleness cannot return. Browsers that
  do not advertise WebP are unaffected. Beyond the browsers named above, the
  main beneficiary is Safari, which advertises WebP on image requests but was
  never on the old lists: with `in_place_optimize_for_browser`, Safari can now
  receive transparent and lossless WebP where those filters are enabled and it
  previously received PNG. What a given visitor gets still depends on the
  request headers, and some CDN and proxy configurations hold responses to
  lossy WebP.
  **Upgrade note: the first start after upgrading to r21 re-optimizes some
  images once**, because which WebP features a browser supports forms part of
  the image optimization cache key, and this release changes that
  determination. On a default configuration the browsers named above are the
  ones affected — a minority of visitors, or most current-browser traffic if
  you enable `convert_to_webp_animated`. Sites running
  `in_place_optimize_for_browser` additionally see it for the share of their
  traffic that the old lists never covered. Pages keep being served normally
  and re-optimization proceeds in the background, as with any cold cache; while
  the pass completes, affected images are served in their original form, so
  expect a brief rise in page weight. No purge or manual invalidation of any
  downstream proxy or CDN is required: where an image's optimized output
  changes, it is published under a new rewritten URL and the HTML is updated
  to point at it, while previously rewritten URLs keep resolving and age out
  normally.

- The legacy JavaScript minifier has been removed. The tokenizer-based
  minifier — the default since 1.10.33.0, and the only one that understands
  modern JavaScript — is now the only JavaScript minifier.
  `UseExperimentalJsMinifier` is deprecated and ignored: configurations that
  still set it start normally and log a warning naming the directive, which
  can simply be deleted. Default configurations are unaffected — they were
  already using this minifier. **Upgrade note: sites that set
  `UseExperimentalJsMinifier` explicitly re-optimize their JavaScript once
  after upgrading**; with `on` the output and the rewritten URLs are
  identical and only that one background pass is new, while with `off` the
  minified output itself changes. Pages keep being served normally and
  re-optimization proceeds in the background, as with any cold cache; while
  the pass completes, affected scripts are served in their original form, so
  expect a brief rise in page weight. No purge or manual invalidation of any
  downstream proxy or CDN is required: where a script's optimized output
  changes, it is published under a new rewritten URL and the HTML is updated
  to point at it, while previously rewritten URLs keep resolving and age out
  normally. Two things also begin working on sites that were running `off`:
  `<script type="module">` is now minified, and `include_js_source_maps` now
  produces source maps. If you generated your own `ModPagespeedLibrary`
  signatures for `canonicalize_javascript_libraries` against the legacy
  JavaScript minifier, regenerate them; until then those libraries are
  minified normally instead of canonicalized.

- Automated clients are recognised far more reliably, so the measurement data
  that drives optimization is collected from real browsers only. The list of
  known non-rendering clients had not been updated since 2013 and missed the
  entire current generation: AI assistant fetchers that retrieve a page on a
  person's behalf, agent infrastructure, and the HTTP client libraries and
  command-line tools written since. Traditional crawlers were already
  recognised. These clients no longer run the instrumentation, critical-image
  and critical-CSS beacons, so the data those beacons collect — which
  `prioritize_critical_css`, `inline_preview_images` and image prioritization
  optimize from — reflects what actual visitors render rather than what a
  non-rendering client reported. Matching is exact and case-sensitive against
  the client identifier, so ordinary browsers are unaffected. Applies to all
  supported servers. Note that `curl` and `wget` are now classified as
  automated clients: a page fetched with either for a spot check will not
  contain the beacon scripts, and lazy-loaded images will be served eagerly.

- **nginx:** a Web Bot Auth signature can now inform that decision, behind the
  new `WebBotAuthBotDetection` directive (server configuration, default off).
  With it on, a request carrying a cryptographically valid Web Bot Auth
  signature (RFC 9421) is treated as an automated client whatever identifier it
  presents — so an agent that identifies honestly is classified correctly even
  when it sends a browser's user-agent string, which no identifier list can
  detect. Only a signature that verifies counts; an absent or failed signature
  changes nothing. Requires `WebBotAuth`, the existing directive that turns
  signature verification on. Off by default, so Web Bot Auth stays observe-only
  for every existing deployment: with the new directive off, a verification
  result still only labels the request — it populates the `$x_verified_bot`
  nginx variable, which you can log or pass to your own configuration, and the
  opt-in verified-request statistics — exactly as in r20.

- `defer_javascript` no longer sends deferred markup to automated clients.
  When `defer_javascript` (or `disable_javascript`) is enabled, `<script>`
  elements are rewritten into a form only PageSpeed's client-side runtime can
  execute. A client that does not run that runtime received a page whose
  scripts never ran and whose external JavaScript was never even requested —
  script-dead markup it had no way to act on. Automated clients are now served
  the page's normal, unmodified script markup instead. This covers the filters
  that share the same gate: `defer_javascript`, `disable_javascript`,
  `defer_iframe`, `fix_reflow`, and the `support_noscript` fallback they share,
  so such a client gets clean markup rather than clean markup plus a stray
  `<noscript>` redirect banner. Browsers are unaffected, and `defer_javascript`
  remains off by default.

  This deliberately includes search-engine crawlers, which previously received
  the deferred form. They now receive the page exactly as it is authored —
  normal markup, not a degraded version of it, and without the serialized
  script execution the deferral runtime imposes. `lazyload_images` has behaved
  this way for automated clients for years. No configuration change is
  required. As with the client recognition above, an automated client that
  presents a browser's exact user-agent string is still served the deferred
  form unless a verified Web Bot Auth signature identifies it.

- **JavaScript minification: generators that yield object literals are
  minified again.** A file containing `yield {…}` — or a same-line
  `await {…}` or `for (x of {…})` — was served in its original, unminified
  form: the minifier could not rule out that the braces opened a block
  rather than the operand, and declined the whole file. On a single line
  the braces can only be the operand, so such files are now fully minified.
  The genuinely ambiguous form — a line break between the keyword and the
  brace, where the two readings differ — is still declined and served
  unmodified, as before.

- **JavaScript minification: a class with a bare field directly before a
  generator method is no longer broken by minification.** The line break
  after a bare field — `x` on its own line, followed by `*gen() {…}` — is
  what ends the field declaration; the minifier removed it, fusing the field
  and the generator method into one invalid declaration, so the minified
  script failed to parse where the original ran. The line break is now
  preserved. Static (`static x`), computed-name (`[expr]`), and private
  (`#x`) bare fields were affected the same way and are covered by the same
  fix.

- **JavaScript minification: an object literal whose generator method is
  followed by further members is minified again.** A file containing
  `{ *gen() {…}, b: 2 }` — a generator method (named anything, including
  `await` or `yield`) with a comma and another member after it — was served
  in its original, unminified form: the minifier mis-modeled the separator
  after the completed method body and declined the whole file. Such files
  are now fully minified.

- **JavaScript minification: a line break before an arrow's `=>` is now
  preserved.** JavaScript forbids a line break between an arrow head and
  its `=>`, so `a = x` followed by `=> y` on the next line is already a
  syntax error. The minifier dropped that line break and emitted `a=x=>y` —
  turning broken input into valid but different code, masking the authoring
  error. The line break is now kept, so invalid input is served as it was
  written.

- **JavaScript minification: a division operator no longer merges into a
  retained IE conditional-compilation comment.** When minification removed
  the space between a division `/` and a retained `/*@ ... @*/` comment, the
  `/` and the comment's opening `/*` fused into `//` — a line comment that
  swallowed the rest of the line and silently changed what the script
  computes, with both forms valid so nothing failed loudly. A separating
  space is now kept whenever the two would otherwise join, and the
  equivalent hazard after such a comment is guarded the same way.

- **JavaScript minification: the space between a bare `0` and a following
  property access is now kept.** `0 .toString()` minified to
  `0.toString()`, where the period is absorbed as the literal's decimal
  point, turning valid code into a script that fails to parse. The space is
  now preserved after a bare `0`; other numeric literals are unaffected.

- **JavaScript minification: the line break after a postfix `++`/`--` is no
  longer removed when it is load-bearing.** A line break separating a
  completed postfix `++`/`--` expression from a next statement that begins
  with an opening parenthesis, or with a leading-dot number such as `.5`,
  is what keeps the two statements apart: without it the code re-parses as
  a call or member access on the value just incremented, which the browser
  rejects as a syntax error — but the minifier removed it and reported
  success, so the script was served broken with nothing logged. Such line
  breaks are now preserved (including when carried inside a comment). Line
  breaks that a following binary operator genuinely continues are still
  removed, and already-correct minified output is byte-for-byte unchanged.

- **HTML parsing: the text of merged character runs is no longer held
  until end of parse.** When the parser coalesces adjacent character
  tokens into one — a routine step before the rewrite filters run — the
  token it merged away kept its copy of the text alive for the rest of the
  parse, so a text-heavy page held more peak memory than it needed to. The
  merged-away token now releases its text the moment it is retired. What
  is served is unchanged.

- The HTML parser is hardened against malformed markup, cross-porting the
  robustness fixes ModPageSpeed 2.0 accumulated for the same code. Certain
  malformed HTML could crash the worker process or trip undefined behaviour
  while a page was being parsed for rewriting. Such markup is now handled
  safely and the page is served. **Update recommended.**

  Two pieces of modern markup are now recognised where they were previously
  unknown. A page whose doctype is `<!DOCTYPE html SYSTEM
  "about:legacy-compat">` — the long form the HTML standard reserves for
  generators that cannot emit the short `<!DOCTYPE html>`, such as XSLT
  output — was classified as having an unknown doctype; it is now treated as
  HTML5 (XHTML5 for XML content types), like any other HTML5 page. And the
  `crossorigin`, `integrity` and `template` attributes and elements are now
  known keywords rather than unrecognised names, which is groundwork only:
  parsing and rewriting of pages that use them is unchanged in this release.

- **CSS: selectors with functional pseudo-classes are no longer mangled.**
  CSS minification could not represent the parenthesized arguments of
  `:where()`, `:is()`, `:not()`, `:has()`, `:nth-child()` and friends —
  common in Tailwind v4 and modern CSS resets — so it reported a selector
  error and silently dropped the argument text: `.prose :where(h2)` minified
  to `.prose :where`, a selector no browser matches. The parser now captures
  the balanced argument text verbatim and re-emits it on serialization, so
  these selectors round-trip intact. As a side effect, rulesets that were
  previously passed through byte-for-byte as opaque regions are now fully
  parsed, so their declarations are minified and their URLs rewritten like
  any other ruleset.
- **CSS: declarations with a spaced `+` addition operator are no longer
  dropped.** CSS minification silently discarded any declaration whose value
  contained a spaced `+` — `calc(1px + 2px)` or a custom property such as
  `--x: 1px + 2px` — because the CSS parser treated a `+` not directly
  attached to a number as a number-parsing error (e.g.
  `h1 { width: calc(1px + 2px); }` became `h1 {}`). The parser now lexes
  such a `+` as an operator value, so these declarations parse and
  round-trip; a `+` directly attached to a number still parses as a signed
  number. The CSS parser's regression coverage for modern constructs was
  expanded alongside this fix (ported from the 2.0 test suite): `calc()`
  with `var()` operands, calc-operand custom properties, and unicode-range
  lexing shapes.

---

# mod_pagespeed 1.15.0+r20 Release Notes

**Release date:** 2026-07-23
**Status:** Stable

## Overview

Security and correctness release for the 1.15 line, hardening output escaping,
input validation, and rewrite correctness across the HTML rewriter filters.
**Update recommended.**

The largest addition is **AVIF image support**, bringing the image path to
parity with the WebP support it has shipped for years: opt-in AVIF encoding on
Apache, nginx, and IIS, served only to browsers that advertise the format.

The release also ships a filter modernization batch: new opt-in filters for
critical images and speculation rules, revived Google Fonts CSS inlining,
Core Web Vitals reporting from the instrumentation beacon, and support for
modern JavaScript and CSS constructs that previously passed through
unoptimized.

**Provenance.** Most of the issues addressed here are long-standing defects
that originate in the upstream mod_pagespeed codebase (originally developed by
Google as open source) on which the 1.15 line is built; each was verified
against the published upstream source. A few are gaps in functionality added
more recently (Content-Security-Policy handling and stylesheet charset
fidelity). All are now fixed.

### Security

- The nginx bundled with the NuGet sidecar package and its container image is
  updated to 1.30.4, picking up the July 2026 upstream nginx security fixes —
  including CVE-2026-42533, a request-processing memory-safety defect that the
  upstream nginx advisory reports as exploited in the wild. Update recommended for sidecar deployments. If you
  build the nginx module against your own nginx, build against 1.30.4 or
  later.
- Fixed a cross-site scripting issue where crafted CSS could break out of an
  inlined `<style>` element when CSS optimization was enabled.
- Fixed a cross-site scripting issue in local-storage cache inlining where a
  crafted image attribute could inject script on repeat page views.
- Fixed a cross-site scripting issue where a crafted image `id` could inject
  script during inline-image deduplication.
- Fixed a cross-site scripting issue where a crafted image URL could be
  reflected unescaped into inline image-preview JavaScript.
- Hardened image optimization against crafted image dimensions that could
  bypass the resolution limit and trigger excessive memory use
  (denial-of-service).
- Fixed a CSS dependency-parsing defect that could misread stylesheet contents
  (out-of-bounds read / dropped `@import` rules).
- Fixed a crash on the image-spriting path that could be triggered by a
  malformed image file declaring invalid dimensions (denial-of-service). Such
  inputs are now rejected and the page is served with the original images;
  sprite sets mixing an unusable image with valid ones now sprite the valid
  subset. A related defensive guard covers the inline image-preview path,
  which is not reachable from end-user input.
- Hardened the JavaScript minifier against crafted scripts that could drive
  unbounded memory growth during parsing, exhausting server memory
  (denial-of-service). Parsing depth is now bounded; a script that exceeds the
  bound is passed through byte-for-byte unminified, which is the minifier's
  existing behavior for any input it declines to process. Ordinary
  JavaScript — including large bundles and heavily nested framework output —
  is unaffected. **Affects all 1.15 releases up to and including r19; update
  recommended for any deployment that optimizes JavaScript it does not
  control.**
- Hardened the HTML parser against crafted documents that could drive memory
  use to the size of the input regardless of configuration
  (denial-of-service). A hard ceiling now applies to how much a single HTML
  token may accumulate, independent of the configurable parse-size limit,
  whose semantics are unchanged. Documents that trip the ceiling fall back to
  being passed through rather than rewritten. **Affects all 1.15 releases up
  to and including r19; update recommended for any deployment that rewrites
  HTML it does not control.**
- Fixed a content-integrity defect where an out-of-range numeric HTML
  character reference decoded to an arbitrary, unrelated character instead of
  being rejected. Out-of-range references are now reported as a decoding
  error and the original escaped text is kept verbatim. References within the
  valid Unicode range are unaffected.
- Defense-in-depth output-escaping consistency across several rewriter filters
  (these paths are not reachable from end-user input; no action required).
- Defense-in-depth division-by-zero guard in the responsive-image sizing
  path (this path is not reachable from end-user input; no action required).
- Defense-in-depth bounds guard on a JavaScript string- and
  regular-expression-scanning path (no out-of-range access was reachable; the
  affected inputs already ended in the existing graceful error).
- Defense-in-depth null guard on an HTML tag-close path (not reachable on this
  release; the guard protects the invariant against future drift).
- Fixed a startup race in HTML keyword-table initialization where concurrent
  first-time initialization could construct the shared table twice and publish
  it without synchronization. Initialization is now thread-safe.

### Features

- **AVIF image support.** Images can now be optimized to AVIF, alongside the
  existing WebP path, on Apache, nginx, and IIS. Four new filters, all
  **opt-in**, cover the same ground WebP does:
  - `convert_jpeg_to_avif` — convert JPEG sources to AVIF.
  - `convert_to_avif_lossless` — prefer lossless AVIF where it wins
    (also the path for images with alpha).
  - `convert_to_avif_animated` — convert animated images to animated AVIF.
  - `recompress_avif` — re-encode images that are already AVIF.

  Behavior worth knowing before you enable them:
  - **AVIF is not part of `rewrite_images` or any rewrite level, by design.**
    AV1 encoding costs substantially more CPU than WebP, so folding it into
    `rewrite_images` would be a silent cost increase for every existing
    deployment on upgrade. Enable the filters you want explicitly.
  - AVIF is served only to browsers that advertise it (`Accept: image/avif`).
    There is no user-agent allowlist: the request header alone decides.
    Clients that do not advertise AVIF keep getting the WebP or original-format
    result exactly as before.
  - The encoder picks the **smaller** of the AVIF, WebP, and original outputs
    per image, so enabling AVIF cannot make an image larger; if AVIF encoding
    fails or times out, the rewrite falls back through WebP to the original
    format rather than failing the image.
  - EXIF, ICC color profiles, and XMP are carried across AVIF re-encoding
    under the existing metadata-retention options. Images carrying a C2PA
    content-provenance manifest are **skipped, never stripped** — such images
    are served as authored.
  - Because AV1 encoding is slow relative to WebP, every still-image encode is
    admitted against a time budget (`AvifTimeoutMs`, default 5000 ms) before it
    starts, and the encoder speed is derived from that budget and the image's
    pixel count: a larger budget admits more images and never selects a
    lower-quality speed than configured. Images that cannot fit the budget even
    at the fastest speed are left to the WebP/original path. An absolute
    100-megapixel ceiling applies regardless of budget.
  - **Known limitation, animated AVIF:** that budget-derived speed selection
    applies to **still images only**. An animated sequence always encodes at
    the configured encoder speed, so animated AVIF encodes cost considerably
    more per image than stills and do not get faster when `AvifTimeoutMs` is
    lowered — a long animated encode is bounded by the abort applied between
    frames rather than by the budget. Account for this before enabling
    `convert_to_avif_animated` over a large animated-image inventory. A future
    release is expected to extend budget-derived speed selection to animated
    sequences.
  - The module now links the AV1 encoder and decoder, so the installed module
    is **appreciably larger** than in r19. Plan package and disk footprint
    accordingly.
  - A full family of `image_avif_*` statistics (rewrites, per-source-format
    timeouts, budget overruns, and success/failure timings) is registered
    automatically, so encode failures and timeouts are visible on the
    statistics page.
- New opt-in filter `prioritize_critical_images`: sets `fetchpriority="high"`
  on the first two images the critical-images beacon has reported above the
  fold, so the browser front-loads the fetches that determine Largest
  Contentful Paint. The filter is a strict no-op without beacon data (a wrong
  guess would prioritize a below-the-fold image at the LCP image's expense),
  an author-supplied `fetchpriority` always wins, and it backs off on
  `Save-Data` requests, AMP documents, and disallowed URLs. It rewrites
  attributes only and injects no scripts; enabling it also turns on
  critical-images beaconing. It is not part of any rewrite level's filter set
  — enable it explicitly.
- New opt-in filter `insert_speculation_rules`: injects a same-origin prefetch
  ruleset (`<script type="speculationrules">`) so supporting browsers prefetch
  a link as the user starts interacting with it; other browsers ignore the
  tag. The filter backs off when the page already carries its own ruleset,
  when a Content-Security-Policy forbids inline scripts, on non-200,
  cookie-setting, or `no-store` responses, and on AMP documents. It is not
  part of any rewrite level's filter set — enable it explicitly.
- Google Fonts CSS inlining is revived: the default size cap
  (`GoogleFontCssInlineMaxBytes`) rises from 3 KiB to 48 KiB. Real Font
  Service responses run ~6–15 KiB, so the old cap rejected essentially every
  one and the filter never fired. A scheme-qualified
  `<link rel="preconnect" href="…://fonts.gstatic.com" crossorigin>` hint is
  now emitted ahead of the first recognized font stylesheet, whether the
  loader CSS ends up inlined or not, unless the author already warms that host
  with a usable `crossorigin` preconnect. **Upgrade note:** "not inlined"
  verdicts cached under the old cap keep applying until they expire (up to a
  day), so inlining ramps up as the cache re-warms.
- `hint_preload_subresources` now emits font preload hints (`rel=preload;
  as=font; crossorigin`, up to four per page) harvested from `@font-face`
  rules in the page's collected CSS. Fonts are discovered two hops late (HTML,
  then CSS, then the font file), so a hint saves the longest fetch chain.
  Harvesting is deliberately conservative: woff2 sources only, only faces
  gated to media needed to render, and only faces whose `unicode-range` covers
  printable ASCII. Fonts referenced only from `@import`ed stylesheets are
  collected once `flatten_css_imports` is enabled. Fleets running mixed
  versions against a shared cache degrade cleanly: older binaries skip the new
  cache entries.
- The instrumentation beacon now reports Core Web Vitals — LCP, CLS, and INP —
  plus navigation timing, collected with `PerformanceObserver` and sent in a
  single `sendBeacon` POST when the page is hidden. This replaces the legacy
  on-load image GET and the `beforeunload` beacon; the `beforeunload` handler
  disabled the browser's back/forward cache, so instrumented pages are
  eligible for it again, and a visit restored from it is measured and
  beaconed as its own page view. Four new histograms (LCP, CLS, INP, TTFB)
  appear on the admin console automatically, and beacons sent by pages cached
  before the upgrade are still accepted. `ReportUnloadTime` is deprecated to a
  no-op.
- The tokenizer-based JavaScript minifier (`UseExperimentalJsMinifier`) now
  handles modern syntax — `??`, `??=`, `?.`, optional catch binding,
  destructuring declarations, `super`, dynamic `import()`/`import.meta`, and
  module statement forms — where it previously rejected most ES2015+ input and
  silently passed modern bundles through unminified. Input it still cannot
  model keeps its original bytes, as before.
- `<script type="module">` is now a first-class script kind; previously every
  JavaScript filter skipped module scripts. `rewrite_javascript` minifies them
  (tokenizer-based minifier only), preserving import specifiers and the
  resource directory so relative imports keep resolving. Combining treats a
  module as a barrier — scripts on either side still combine among themselves
  — and inlining, outlining, and disabling leave modules alone, since those
  rewrites would change import resolution or execution timing. Modules are
  never relocated to another host by rewriting or cache extension (their
  fetches are CORS-mode) and are never substituted by library
  canonicalization.
- CSS inside `@supports`, `@layer`, and `@container` blocks, and media queries
  using level-4 range syntax such as `(width >= 768px)`, previously failed to
  parse — so everything inside them passed through unminified and unoptimized,
  which for framework bundles that wrap the whole stylesheet in `@layer` meant
  the entire file. These constructs now parse: such stylesheets minify, images
  referenced inside the blocks are rewritten, inlined, and cache-extended like
  any others, and `prioritize_critical_css` collects and inlines critical
  selectors inside them while preserving `@layer` cascade order. Sheets that
  still fail to parse are served byte-for-byte unchanged, as before.
- `insert_dns_prefetch` now emits `<link rel="preconnect">` for the first two
  domains of its stable-domain list (dns-prefetch for the rest): preconnect
  warms the whole connection (DNS + TCP + TLS) where dns-prefetch only
  resolves the name. Preconnect hints are scheme-qualified and keep
  non-default ports, and the filter no longer emits hints an author already
  provides. The legacy IE9-only `rel=prefetch` variant is removed. **Rollout
  note for mixed-version fleets sharing a cache:** until the affected
  property-cache entries expire, an older binary reading entries written by
  this version can emit malformed preconnect hrefs of the form
  `//https://host`, which browsers ignore.
- `prioritize_critical_images` and native-mode `lazyload_images` now handle
  images that use `srcset` without a `src` attribute: the beacon reports the
  candidate the browser actually displays, beacon-critical candidates get
  `fetchpriority="high"`, and the rest are lazy-loaded under the same
  first-image LCP protection as `src` images, honoring author
  `loading`/`fetchpriority`/`decoding` attributes.

### Correctness

- Fixed a Google Analytics snippet-detection bug where analytics markup could
  be misidentified across requests, leading to missing analytics on some pages.
- Under a strict Content-Security-Policy that permits inline styles but not
  inline scripts, critical-CSS prioritization could render pages with the
  non-critical styles missing; such pages are now left unchanged.
  **Update recommended for sites served with a Content-Security-Policy.**
- Fixed a text-integrity issue where an external stylesheet with no declared
  character set and non-ASCII content could be inlined with garbled bytes;
  such stylesheets are now left unchanged.
- Fixed an input-handling defect where images with extreme author-specified
  dimensions could produce an invalid responsive-image candidate.
- CSS minification could incorrectly strip units from zero terms inside the
  math functions `calc()`, `-webkit-calc`, `min()`, `max()`, and `clamp()` —
  after any nested function such as `var()` (e.g. `calc(var(--x) - 0px)`
  became `calc(var(--x) - 0)`), and throughout `min()`/`max()`/`clamp()`/
  `-webkit-calc` themselves — producing invalid CSS that browsers drop.
  Math-function context is now recognized for all of these functions and
  preserved across nesting.
- `combine_javascript` now requires the Content-Security-Policy to permit
  inline scripts before combining, not just `unsafe-eval`. The filter replaces
  script tags with small inline bootstrap scripts; under policies such as
  `unsafe-eval` without `unsafe-inline` (or `strict-dynamic`/nonce-based
  policies) the combined scripts loaded but never executed. Affected pages now
  keep their original, working script tags.
  **Update recommended for sites served with a Content-Security-Policy.**
- Fixed a `Vary` header merge defect in in-place optimization: when a response
  already carried one `Vary` token (such as `Accept`), another needed token
  (such as `User-Agent` or `Save-Data`) was not added, so a downstream cache
  could serve the wrong variant of a resource. Tokens are now merged
  individually; existing tokens are never removed or duplicated.
- Fixed a class of defects in the tokenizer-based JavaScript minifier
  (`UseExperimentalJsMinifier`) that fused valid statements into a syntax
  error: a line opening with `(` or a regular-expression literal following an
  `import`/`export` declaration, a plain `let`/`var` declaration, or a
  block-bodied arrow function could be joined onto the previous line when the
  source relied on automatic semicolon insertion. These declaration boundaries
  are now modeled; input the minifier cannot model is still passed through
  byte-for-byte.
- Both JavaScript minifiers now treat a block comment containing a line break
  as a line break, as the language specification requires. Previously such a
  comment was collapsed to a plain space, so a statement boundary that relied
  on it disappeared: code of the shape `return/*<newline>*/x` was minified to
  `return x`, silently changing what the script returned, and comparable
  inputs were fused into outright syntax errors. Conditional-compilation
  comments are still retained verbatim.
- Pages that combine a `<base>` element with a Content-Security-Policy
  `base-uri` directive are no longer excluded from optimization outright.
  Where the policy provably neutralizes every `<base>` element (`base-uri
  'none'` or an empty source list), the browser ignores the tag, so it cannot
  affect relative-URL resolution and rewriting now proceeds. Any `base-uri`
  value that could still match a `<base>` — `'self'`, host or scheme lists,
  `*` — backs off exactly as before, as do pages with no such directive.
  Strict policies of this shape previously paid a rewriting penalty for being
  strict.
- IIS: fixed a use-after-free in the server's internal fetcher when a request
  completed synchronously, which could crash the worker process. Deletion of
  the completed fetch is now deferred until the originating call has fully
  returned.
- Admin console: license management controls are no longer hidden when the
  global admin console is served at a renamed path. The console now trusts the
  authoritative flag the server already returns instead of inferring the answer
  from the URL. Server-side enforcement was never affected — control
  visibility was the only thing wrong.
- Scripts carrying Subresource Integrity (`integrity=`) are now left untouched
  by JavaScript rewriting and minification (`rewrite_javascript`), by
  combining (an integrity-bearing script acts as a barrier; scripts on either
  side still combine among themselves), by outlining of inline scripts, and —
  stylesheets included — by cache extension when it would relocate the
  resource to another host (domain sharding or mapping). Previously such a
  rewrite changed or moved the bytes so the hash no longer matched, and the
  browser blocked a resource that was valid as authored.
- `inline_javascript` no longer inlines external scripts carrying `async` or
  `defer`: those attributes are ignored on inline scripts, so inlining
  silently turned a deferred script into a parser-blocking one that ran
  mid-parse, out of order. Scripts carrying only one of the `for`/`event`
  attribute pair — which per HTML5 never execute — are likewise left alone by
  inlining and combining, where previously the rewrite made them run.
- HTML responses whose bytes depend on the `Save-Data` request header now
  carry `Vary: Save-Data`, so a downstream cache cannot serve the data-saver
  variant to a full-data client or vice versa. Existing `Vary` tokens are
  preserved.
- CSS minification now recognizes the math functions `calc()`, `min()`,
  `max()`, and `clamp()` case-insensitively, as the specification requires;
  uppercase or mixed-case forms (e.g. `CALC(100% - 0px)`) previously had
  units incorrectly stripped from zero terms, producing invalid CSS that
  browsers drop.
- Stylesheets containing an `@import` rule that uses syntax the CSS parser
  does not understand — such as cascade layers
  (`@import url(x) layer(base);`) or other unrecognized import syntax — are
  no longer import-flattened, a
  transformation that could drop or reorder rules. Other optimizations
  (minification, image rewriting) still apply to such stylesheets.
- A stylesheet whose only encoding declaration is a leading `@charset` rule
  could lose that declaration when import-flattening was enabled but no
  imports were inlined (including results served from the flatten cache),
  leaving the browser to guess the encoding of non-ASCII content. The
  declaration is now preserved in that case; stylesheets that do have imports
  inlined keep the standards-required behavior of dropping it. Note that an
  already-minified stylesheet with no imports that carries `@charset` now
  serializes byte-identically, so under the default configuration its rewrite
  is dropped as a no-op — including any in-CSS image rewrites it previously
  kept alive.
- CSS scanning now recognizes `URL()` and `@IMPORT` case-insensitively, as
  the specification requires. Uppercase references were previously invisible
  to URL rebasing when stylesheets were combined, inlined, or outlined, and
  to URL rewriting inside style attributes, which could leave stale or
  unexpectedly relative URLs in place.
- Fixed a `local_storage_cache` defect where unavailable or failing browser
  storage (for example in some private-browsing modes) made an inlined
  resource disappear from the page entirely — and could break every inlined
  resource on the page — instead of falling back to the network. Inlining
  now degrades gracefully. The cookie that records which resources a browser
  already holds in local storage is now scoped to the whole site (`path=/`)
  instead of the current page's directory, so the server recognizes them
  site-wide.
- Removed an obsolete Firefox workaround from `defer_javascript` that ran
  deferred inline scripts through a `data:` URL (for a Firefox bug fixed in
  2013). Content-Security-Policy rules that permit inline scripts do not
  permit `data:` script URLs, so under such policies every deferred inline
  script was blocked on Firefox and the page broke.
- `cache_partial_html` no longer triggers the no-script redirect machinery
  for clients without JavaScript: like `defer_iframe`, the filter name is
  still accepted for configuration compatibility but has no rewriting
  effect, so it must not mark pages as requiring script execution.
- `elide_attributes` now also elides values matching current-HTML defaults
  in HTML5 documents — `loading=eager`, `decoding=auto`, and
  `fetchpriority=auto` on images, `media=all` on stylesheet links, and
  `fetchpriority=auto` on links and scripts — and strips values from the
  modern boolean attributes `open` (dialog), `disabled` (fieldset),
  `allowfullscreen` (iframe), and `playsinline` (video). Entries for
  long-dead markup (`command`, `keygen`, `seamless`, and similar) were
  removed and no longer alter such elements.
- `remove_quotes` now also strips quotes from attribute values containing
  `/`, so most URL-valued attributes (such as `href="/foo/bar"`) are emitted
  unquoted. Values ending in `/` are kept unambiguous by the output writer's
  existing guard.
- The canonical link inserted by the no-script redirect handling is now built
  as a real element. Output is unchanged under the default configuration;
  attribute-level filters such as `remove_quotes` now apply to it
  consistently, as they do to all other elements.
- Fixed a diagnostic message in low-resolution image resizing that printed
  the target width twice instead of width×height.

### Statistics

- `show_ads_snippets_not_converted` now reflects recognized-but-unconverted
  AdSense snippets (it previously always reported 0).
- `num_js_inlined` no longer over-counts scripts that are intentionally left
  external in XHTML documents.
- Fixed a rare corruption of the image byte-savings counter that could occur
  under a non-default configuration where an optimized image ended up larger
  than its input.
- The reported page-load time (the Beacon Reported Load Time histogram and the
  `total_page_load_ms` average) is now measured from navigation start, so it
  additionally includes DNS, connection setup, and time to first byte. The
  histogram steps up once at rollout; this is a measurement change, not a site
  regression.
- Telemetry for scripts skipped by administrator configuration no longer
  records them as author opt-outs (`has_pagespeed_no_defer`), matching the
  module-script and CSP-backoff paths.
- `flatten_imports_unparseable_import` counts stylesheets for which
  import-flattening was declined because an `@import` rule uses syntax the
  CSS parser does not understand (such as cascade layers or other
  unrecognized import syntax).
- The `image_ongoing_rewrites` gauge no longer leaks a count on a failure
  path that could not be triggered in practice (defensive fix).
- Histogram percentiles (median, 90th, 95th, 99th) are no longer reported as a
  `-5000` no-data placeholder when a histogram has too few samples to estimate
  them. The cells are now rendered empty instead — for the raw `/histograms`
  output as well as the admin console, which previously hid the placeholder for
  itself only. Anything scraping `/histograms` should expect an empty value
  rather than `-5000`.
- Latency and duration statistics (HTTP cache lookup and insert, cache hit and
  insert, fetch, and backend first-byte latency) are now measured against a
  monotonic clock. A backward step of the system wall clock — from a time
  sync, a hypervisor, or a misbehaving time daemon — previously produced
  negative recorded durations. Absolute timestamps used for HTTP `Date`
  headers and for cache freshness are deliberately still taken from the wall
  clock and are unaffected.
- New `image_avif_*` statistics cover AVIF rewrites, per-source-format encode
  timeouts, budget overruns, and success/failure timings.

### Configuration

- New AVIF options, mirroring their WebP counterparts: `AvifRecompressionQuality`
  (default 60), `AvifRecompressionQualityForSmallScreens` (50),
  `AvifAnimatedRecompressionQuality` (50), `AvifQualityForSaveData` (45), and
  `AvifTimeoutMs` (5000). They take effect only when one of the AVIF filters is
  enabled; see the AVIF entry under Features.
  - Note on `AvifTimeoutMs` in r20: on nginx it is settable in the `http`
    block (`pagespeed AvifTimeoutMs 3000;`), but it is not exposed as an
    Apache `ModPagespeed*` directive in r20, so on Apache it stays at its
    default. This is corrected in r21, where `ModPagespeedAvifTimeoutMs`
    becomes available in the server configuration.
- **IIS: `pagespeed.config` now has a single, documented resolution order.**
  The module previously probed the configuration locations independently from
  several code paths, so when the installed copies diverged, editing one of
  them could appear to have no effect at all. All paths now use one resolver,
  with this precedence (lowest first; the last file that defines a setting
  wins):
  1. `%ProgramData%\We-Amp\PageSpeed\pagespeed.config` — machine-global base.
  2. `%ProgramData%\We-Amp\IISWebSpeed\pagespeed.config` — legacy fallback,
     for upgrades from IISpeed.
  3. `<site physical path>\pagespeed.config` — per-site override,
     authoritative when present.

  Within each location `pagespeed.config` is still preferred over the legacy
  `iiswebspeed.config`. **For a standard single-site installation, behavior is
  unchanged** — this matches what request handling already did. On startup the
  module now logs which configuration is in effect and warns when several
  configuration files exist with differing contents, which is the diagnostic
  for the "my edit did nothing" case.
- New option `AsyncMetadataL2Writes` (default **off**). When enabled, writes to
  the disk-backed second tier of the metadata cache are deferred to a
  background worker instead of running on the rewrite-completion path, so slow
  disk write latency no longer amplifies into serving-throughput loss and
  tail-latency spikes during a cache-populating miss storm. Reads stay
  synchronous. With the option off, behavior is identical to previous
  releases. Enabling it is safe by construction — served content is
  content-hash-addressed, so a deferred write can at worst cost a
  re-optimization, never wrong or stale bytes — but it is default-off this
  release while it accrues production soak.
- The Universal Analytics filters are deprecated — the service stopped
  processing hits in July 2023, so the trackers these filters inject or
  rewrite reported to a discontinued service. Enabling `insert_ga` or setting
  `AnalyticsID` logs a deprecation warning at configuration load (disabling
  stays silent), and `make_google_analytics_async` is now a no-op whose name
  still parses, so existing configurations keep loading. Experiments no longer
  auto-enable `insert_ga`: the A/B framework keeps variant assignment (the
  `PageSpeedExperiment` cookie) and the experiment id in the instrumentation
  beacon, reporting is bring-your-own-analytics, and an experiment spec can
  still opt in explicitly with `enable=insert_ga`.
- `defer_iframe` is deprecated: the filter name was accepted but had no effect
  on its own (iframe deferral is built into `defer_javascript` and
  `disable_javascript`). The name still parses for compatibility, logs a
  deprecation warning at configuration load, and no longer triggers the
  no-script redirect machinery. Configurations using `defer_iframe` can simply
  remove it.
- The legacy JavaScript minifier is deprecated: it remains available this
  release via `UseExperimentalJsMinifier off`, but explicitly selecting it
  logs a deprecation warning at configuration load, and it will be removed in
  a future release. The tokenizer-based minifier (the default since
  1.10.33.0) now minifies modern JavaScript it previously passed through
  unoptimized, backed by a stress corpus of real-world, bundled, and
  synthetic JavaScript: full coverage, with zero parse, semantic, or
  idempotence failures.
- The experimental gRPC "central controller" was removed, along with its
  dedicated controller process and the gRPC build dependency. The
  `ExperimentalCentralControllerPort`, `ExperimentalPopularityContestMaxInFlight`,
  and `ExperimentalPopularityContestMaxQueueSize` options are deprecated: they
  still parse for compatibility but are ignored, and log a deprecation warning
  when set. There is no behavior change for configurations that did not set
  these options — the default work-bound expensive-operation throttling and
  named-lock rewrite scheduling are unchanged. The controller's
  experimental-only statistics counters (`num_rewrites_requested`,
  `num_rewrites_succeeded`, `num_rewrites_failed`, the popularity-contest and
  queued-controller gauges, and `controller_reconnect_time`) are no longer
  registered; they read as zero under default configurations, so dashboards
  scraping them will see them go missing rather than zero.
- Domain sharding (`ShardDomain`) is deprecated: it is an HTTP/1-era
  workaround that hurts performance with HTTP/2 and HTTP/3, which multiplex
  over a single connection. The directive still works for compatibility but
  logs a deprecation warning, deduplicated to at most once per process per
  shard declaration. Configurations using `ShardDomain` can simply remove
  the directive.
- The long-inert filters `div_structure`, `explicit_close_tags`,
  `mobilize_precompute`, `split_html`, `split_html_helper`, and
  `flush_subresources` are deprecated: the names still parse for
  compatibility and log a deprecation warning at configuration load but have
  no effect. (`flush_subresources` also no longer adds head-section domains
  to the `insert_dns_prefetch` hint list.) Configurations using them can
  simply remove them.
- `ForbidFilters` now also gates previously-rewritten URLs carrying the
  shared cache-extension (`.pagespeed.ce.`) and image (`.pagespeed.ic.`)
  markers: such URLs are no longer served once every filter that can produce
  them is forbidden. Previously, forbidding those filters stopped new
  rewrites but could not stop already-rewritten URLs from being served.

## Platform Support

| Platform | Module | Status |
|----------|--------|--------|
| **Apache 2.4+** | `mod_pagespeed.so` | Stable |
| **Nginx 1.26+** | `ngx_pagespeed_module.so` | Stable |
| **IIS 10+** | `pagespeed_iis.dll` | Stable |

---

# mod_pagespeed 1.15.0+r19 Release Notes

**Release date:** 2026-07-17
**Status:** Stable

## Overview

Cache upgrade-safety release, plus zero-copy serving corrections. Update
recommended.

## Highlights

### Caching: version-safe cache files

- The cache now lives in a file fingerprinted by the bundled cache library's
  on-disk format version — not the mod_pagespeed release version. Format
  changes are anticipated to be infrequent, so most future upgrades will keep
  the cache warm. When the format does change (as it does in this release),
  old and new worker processes never open the same file, which removes a class
  of cache-corruption risk during upgrades, when both could briefly overlap on
  one cache file.
- **Upgrade note: the first start after upgrading to r19 begins with a cold
  cache.** Pages keep being served normally and re-optimization proceeds in the
  background, as with any cold cache.
- The previous version's cache file is left on disk untouched, so **rolling
  back to the previous package is warm** — it finds its cache exactly as it
  left it. Once you are confident you will not roll back, you can delete the
  older files in the cache directory to reclaim disk. Nothing is deleted
  automatically. (Cache files are sparse; apparent size overstates actual disk
  use.)
- Fixed: when a cache-directory hash bucket filled up entirely with
  current-version entries, new writes to that bucket were silently dropped (the
  entry was simply never cached, so affected resources were re-optimized on
  every request). Writes now land by evicting an existing entry, and a new
  `bucket_full_evictions` statistic makes the condition observable. The effect
  was most likely during upgrade-day write storms into a cold cache.

### Zero-copy serving (opt-in, now available on all three platforms)

- Cached resources can be served directly from the memory-mapped cache without
  copying the payload — available on nginx, Apache, and IIS.
- A correction to the r18 notes: they described zero-copy serving as on by
  default on nginx, but common configurations silently made every request
  ineligible, so it rarely engaged. That defect is fixed — and with r19 the
  feature is uniformly **opt-in on every platform** while it accrues production
  soak. On-by-default is planned for a future revision.
- To enable it:
  - nginx: `pagespeed CycloneZeroCopy on;`
  - Apache: `ModPagespeedCycloneZeroCopy on` and
    `ModPagespeedCycloneZeroCopyServe on`
  - IIS: `pagespeed CycloneZeroCopy on` and `pagespeed CycloneZeroCopyServe on`
- A new `zerocopy_serve_ineligible` statistic counts requests that fell back to
  copied serving, and a one-time log message explains the first fallback (on
  Apache this message needs `LogLevel info`; the statistic is always on).

### Performance and reliability

- The bundled cache library gains a lock-free read path — cache hits no longer
  take a lock — and no longer syncs to disk on every cache write (durability is
  periodic, and the power-loss window stays bounded), plus a hardening batch
  covering crash recovery and startup edge cases. Under write-heavy load,
  removing the per-write disk sync measured an order of magnitude higher
  sustained throughput in internal testing.
- Memory-mapped cache reads are verified before being promoted into the
  in-memory tier, hardening the serving path against torn or damaged entries.
- IIS: fixed a defect where optimization of a site's own sub-resources (CSS,
  JavaScript, images) could fail to converge on machines whose name resolution
  prefers the IPv6 loopback — pages then kept serving their original resources
  for minutes at a time. The server's internal fetches now pin the loopback
  address family explicitly and fall back to the other family automatically.
- Windows/IIS: cache-invalidation updates (purge requests) after the first one
  were silently discarded — the on-disk purge state never advanced, so later
  purges did not take effect across restarts or between worker processes.
  Atomic file replacement on Windows now works as intended and purges apply
  reliably.
- Windows/IIS: a cross-process guard now prevents one worker process from
  resetting a shared cache file while another process still has it mapped,
  closing a corruption window in multi-worker setups; two new statistics
  (`resets_gate_verified`, `resets_under_degraded_gate`) make gate health
  observable.
- IIS: fixed a race in the server's internal fetcher where a sub-resource fetch
  could be spuriously canceled just after its response had arrived. Because a
  failed internal fetch is remembered for several minutes, a single spurious
  abort could stall re-optimization of the affected resource well beyond the
  moment of failure, surfacing as intermittent optimization stalls.
- Fixed on all three platforms: behind a TLS-terminating proxy (when the
  `X-Forwarded-Proto` header is honored), the server's internal fetches for a
  page's own sub-resources combined the page's `https` scheme with the
  plain-HTTP listener port — a connection that could never succeed — so the
  affected CSS, JavaScript, and images were repeatedly re-fetched and never
  optimized. Internal fetches now use the protocol the listener actually
  speaks.
- Cache write-failure warnings are now rate-limited, so a persistent storage
  condition cannot flood the error log.
- The legacy JavaScript minifier (used when `UseExperimentalJsMinifier` is off)
  now minifies files containing ES2015 template literals; r18 passed such files
  through unmodified, r19 optimizes them.

### Experimental

- The native fetcher (`UseNativeFetcher`, nginx) remains off by default. Native
  HTTPS support has been introduced, so the fetcher can now retrieve `https://`
  resources directly. Enabling it requires a `resolver` directive in the nginx
  configuration.

## Platform Support

| Platform | Module | Status |
|----------|--------|--------|
| **Apache 2.4+** | `mod_pagespeed.so` | Stable |
| **Nginx 1.26+** | `ngx_pagespeed_module.so` | Stable |
| **IIS 10+** | `pagespeed_iis.dll` | Stable |

---

# mod_pagespeed 1.15.0+r18 Release Notes

**Release date:** 2026-07-11
**Status:** Stable

## Overview

Performance, caching, and correctness release for the 1.15 line, with security
hardening across input validation and output escaping. Update recommended.

## Highlights

### Performance

- Cached resources are now served with far less copying. Cache hits are
  served from the memory-mapped cache by reference (`CycloneZeroCopyServe`) —
  on by default on nginx, and an experimental opt-in on Apache and IIS — and
  an experimental fully zero-copy serve mode (`CycloneZeroCopy`, off by
  default) is available. Apache additionally streams optimized resource
  responses instead of double-buffering them, and nginx serves cached
  responses on HTTP/2 and HTTP/3 through a bounded copy ring.
- The default file cache size is raised to 1 GB.

### Caching

- Metadata and page-property cache entries are now stored in a dedicated
  small-object volume on file caches of ~256 MB or larger, keeping them warm
  across restarts independent of payload traffic. **Upgrade note: the first
  restart after upgrading rebuilds the payload cache once** on caches at or
  above that size (re-optimization proceeds normally from a cold payload
  cache; metadata is unaffected). Set `FileCacheSmallTierPercent 0` to
  disable.
- The cache's RAM tier is now sized independently of
  `LRUCacheKbPerProcess` via the new `CycloneRamCacheKb` directive
  (default 0: disabled — memory-mapped cache hits are already served from
  page cache).
- New cache observability counters on the admin console's caches page.

### Correctness and configuration

Configuration validation is stricter in this release; previously-accepted
invalid configurations may now fail to load, which is intentional:

- An invalid filter name in `?PageSpeedFilters=` now consistently rejects the
  whole query (rejection was previously position-dependent).
- Out-of-range values for bounded options now fail configuration load instead
  of being silently accepted: image qualities (-1..100), progressive JPEG
  scans (-1..10), `RewriteRandomDropPercentage` (0..100),
  `HttpCacheCompressionLevel` (-1..9).
- The `AddResourceHeader` limit of 20 headers is enforced exactly.
- Directive and option-scope matching is now case-consistent across all
  server ports, so scope enforcement can no longer be sidestepped by casing.
- The legacy JavaScript minifier (used when `UseExperimentalJsMinifier` is
  off) now passes files containing template literals through unmodified
  instead of corrupting them.

### Critical CSS and Content-Security-Policy

- `prioritize_critical_css` and other script-injecting filters now honor a
  restrictive Content-Security-Policy when `HonorCsp` is enabled, backing off
  instead of injecting scripts the policy would block, and the CSP policy
  engine received a set of correctness fixes.
- Inlined critical CSS preserves stylesheet charset fidelity and link
  attributes, and handles `@import`/`@keyframes` rules correctly.
- The critical-CSS beacon is viewport-aware, and beacon truncation is now
  observable in statistics instead of silently starving extraction.
- `lazyload_images` gains a native mode that emits `loading="lazy"` on
  below-the-fold images instead of injecting the JavaScript loader.

### IIS

- Fixed a defect in the IIS loopback fetcher where a sub-resource fetch that
  completed asynchronously could be treated as an empty response, suppressing
  optimization of the parent page for five minutes at a time — pages were
  intermittently served in their original form. Update recommended for IIS
  deployments.
- Configuration parsing is hardened: a malformed configuration line can no
  longer crash the module at startup, unknown options are reported instead of
  silently ignored, and option scoping is now enforced on IIS as on the other
  ports.

### Security hardening

This release hardens input validation and output escaping across the
rewriter, beacon handling, and configuration parsing. The bundled HTTPS
fetch library is updated to curl 8.21.0, which addresses a batch of
recently published curl vulnerabilities. No exploitation is known; update
recommended.

### Reliability

- The bundled Cyclone cache library is updated: deterministic teardown,
  key-verified directory election (a rare collision can no longer associate a
  cache entry with the wrong key), periodic directory sync for a tighter
  power-loss window (including on Windows), a fix for a startup race where
  multiple server processes opening the cache concurrently could corrupt or
  spuriously fail cache initialization (recovery after a crash during cache
  creation is now automatic), and roughly 4 MB less memory per cache stripe.
- A worker-pool sequence could be recycled while work was still queued;
  scheduler alarms are now driven from the event loop on nginx; the
  experimental native fetcher (`UseNativeFetcher`) gains native TLS support.
- Admin console reliability and usability pass.

## Platform Support

| Platform | Module | Status |
|----------|--------|--------|
| **Apache 2.4+** | `mod_pagespeed.so` | Stable |
| **Nginx 1.26+** | `ngx_pagespeed_module.so` | Stable |
| **IIS 10+** | `pagespeed_iis.dll` | Stable |

---

# mod_pagespeed 1.15.0 Release Notes

**Release date:** 2026-06-01
**Status:** Stable

## Overview

mod_pagespeed 1.15.0 renumbers the maintained Apache-lineage line from 1.1 to
1.15. 1.15 is the direct successor to Google's final mod_pagespeed
release (1.14.36.1) and reads as the newest maintained build on the package and
version surfaces (dnf/yum/apt, WHM EasyApache 4) where the prior "1.1" numbering
looked older than Google's line.

This is a version-identity change: there is **no functional change** from the
1.1.0 line. Package names (`mod-pagespeed`, `nginx-module-pagespeed`,
`ea-apache24-mod_pagespeed`) and repository channels are unchanged, so existing
install commands keep working. The `X-Mod-Pagespeed` response header now reports
`1.15.0.0`. For the full change history see [CHANGELOG.md](CHANGELOG.md).

---

_Historical release notes for earlier releases follow._

# mod_pagespeed 1.1.0-beta.1 Release Notes

**Release date:** 2026-02-25
**Status:** Pre-release (beta)

## Overview

This is the first release of mod_pagespeed under We-Amp stewardship. The
codebase has been extensively modernized: new build system (Bazel 7.x),
modern C++ (C++20/23), and three new deployment platforms alongside the
original Apache module.

## Platform Support

| Platform | Module | Status | Tested |
|----------|--------|--------|--------|
| **Apache 2.4+** | `mod_pagespeed.so` | Stable | 195 pass, 14 skip |
| **Nginx 1.26/1.27** | `ngx_pagespeed_module.so` | Stable | 169 pass, 40 skip |
| **Envoy** | `pagespeed_filter.so` / `envoy_pagespeed` | Experimental | 189 pass, 20 skip |
| **IIS 10+** | `pagespeed_iis.dll` | Experimental | 350 pass (Python), 19 pass (C++) |

## Quick Start

### Apache

```bash
# Ubuntu/Debian
sudo dpkg -i mod-pagespeed-beta_1.1.0-beta.1_amd64.deb
sudo a2enmod pagespeed
sudo systemctl restart apache2

# RHEL/Rocky
sudo rpm -i mod-pagespeed-beta-1.1.0-beta.1.x86_64.rpm
sudo systemctl restart httpd
```

### Nginx

```bash
tar xzf ngx_pagespeed-1.1.0-beta.1-linux-x86_64.tar.gz
sudo cp ngx_pagespeed-1.1.0-beta.1/ngx_pagespeed_module.so /usr/lib/nginx/modules/
# Add to nginx.conf: load_module modules/ngx_pagespeed_module.so;
sudo nginx -t && sudo systemctl restart nginx
```

### Envoy

```bash
tar xzf envoy-pagespeed-1.1.0-beta.1-linux-x86_64.tar.gz
cd envoy-pagespeed-1.1.0-beta.1
sudo mkdir -p /var/cache/pagespeed
./envoy_pagespeed -c pagespeed-envoy.yaml.sample
```

### IIS

Requires: Visual C++ Redistributable 2022

```powershell
Expand-Archive pagespeed-iis-1.1.0-beta.1-win-x64.zip C:\inetpub\pagespeed
Stop-Service W3SVC
New-WebGlobalModule -Name PageSpeedModule -Image 'C:\inetpub\pagespeed\pagespeed_iis.dll'
Start-Service W3SVC
```

### Build from Source

```bash
docker compose up -d
docker compose exec dev bash
bazel build --config=clang-libstdcxx13 //:libmod_pagespeed.so
```

See [docs/install-apache.md](docs/install-apache.md), [docs/install-nginx.md](docs/install-nginx.md),
[docs/install-envoy.md](docs/install-envoy.md), or [docs/install-iis.md](docs/install-iis.md) for
detailed instructions.

## What's New

- **Bazel build system** replacing GYP/gyp_chromium
- **C++20/C++23** with Clang + GCC 13 libstdc++
- **Cyclone cache** for high-performance disk caching
- **CurlUrlAsyncFetcher** replacing Serf (linked against BoringSSL)
- **Envoy filter** with IPRO, HTML rewriting, Redis cache, Prometheus metrics
- **Nginx dynamic module** for 1.26.x and 1.27.x
- **IIS native module** with full HTML rewriting and IPRO
- **Python test framework** shared across all platforms
- **GitHub Actions CI/CD** with automated release packaging
- **Docker development environment**

See [CHANGELOG.md](CHANGELOG.md) for full details.

## Known Limitations

### Envoy (Experimental)

- `combine_css` may timeout due to async worker pool coordination
- IPRO cache lifetime differs from Apache behavior
- `X-PSA-Blocking-Rewrite` header not supported (Envoy is non-blocking)
- See [docs/envoy-limitations.md](docs/envoy-limitations.md)

### IIS (Experimental)

- IPRO async cache returns misses (Redis/disk caches not functional for IPRO)
- Beacon data silently discarded
- Requires `allowDoubleEscaping="true"` in IIS configuration
- See [docs/iis-limitations.md](docs/iis-limitations.md)

### Nginx

- Lazyload images filter times out on some configurations
- Inline preview images filter times out
- See [docs/test-catalog.md](docs/test-catalog.md) for per-platform skip details

## Reporting Issues

Please report issues at: https://github.com/we-amp/mod_pagespeed/issues

Include:
- Platform (Apache/Nginx/Envoy/IIS) and version
- Operating system and version
- Configuration snippet (relevant PageSpeed directives)
- Steps to reproduce
