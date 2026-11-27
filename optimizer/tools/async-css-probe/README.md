# Async-CSS probe

Does deferring a stylesheet leave the page looking right?

The optimizer can make a stylesheet non-render-blocking and inline the CSS the
fold needs, so the page paints immediately and the full sheet arrives later. When
the inlined block really does cover the fold, that is free speed. When it does
not, the visitor sees a flash of unstyled content, and nothing in the existing
test suite notices: the unit tests assert on extraction, and the HTTP compliance
suite asserts on protocol behaviour. Neither asks the question this harness
exists for.

Two lanes, one fixture, one compose stack:

| Lane | What it asks | Where it runs | Blocking |
|---|---|---|---|
| **Markup** (`test_async_css_markup.py`) | Did the optimizer emit correct deferral markup? | `async-css-probe-markup` job in `ci.yml`, every PR | No — soaking |
| **Rendered** (`probe_rendered.mjs`) | Does the fold *look* the same before the sheet applies? | `async-css-probe-nightly.yml`, 05:05 daily | No — advisory |

The stack is a sibling of `tools/http-compliance/`, not a modification of it: the
same origin / worker / nginx trio over a shared volume, but serving the
byte-frozen `modpagespeed.com` capture in `fixtures/` and keeping the worker's
critical-CSS pass, which the compliance worker deliberately disables.

## Running it

```bash
# All three modes, building the worker and module from source
./tools/async-css-probe/run_probe.sh

# One mode, against pre-built CI artifacts (much faster)
./tools/async-css-probe/run_probe.sh --prebuilt --mode forced

# Pass anything else through to pytest
./tools/async-css-probe/run_probe.sh --prebuilt -k loader
```

## The deferral primitive lives in one file

`primitive.json` holds how the optimizer makes a stylesheet non-render-blocking:
`rel="preload" as="style"`, with the real media parked in
`data-pagespeed-media` and a loader that flips `rel` back to `stylesheet`.
**Both** lanes read it — the markup lane through `conftest.py`, the rendered
lane through `probe_rendered.mjs` — so a change to the primitive is one edit
here and both lanes record the swap instead of being rewritten around it.
`primitive_attr`/`primitive_value` is the single attribute the rendered lane
sets to put an applied sheet back to not-applying; `primitive_as` is the
companion the markup lane asserts, and is what makes the preload match the
consumer the loader creates (a wrong `as` downloads the sheet twice).
`early_hint_promoted` is `true`: the primitive is itself a preload, so an Early
Hint promotes exactly what the transform promoted.

## The three modes

Both knobs are startup flags, so each mode is a separate stack:

| Mode | Worker argv | Asserts |
|---|---|---|
| `gated` | coverage floor raised to 0.95 | Nothing is deferred; the loader is not injected; critical CSS is still inlined |
| `forced` | floor 0.95 **+** `--unsafe-force-async-css` | It defers, and every part of the deferred markup is correct |
| `shipped` | stock | Records what the product does on this page today |

`gated` and `forced` differ by exactly one flag. That pair is what makes the
diagnostic switch's effect provable, and what shows the gate — not luck — is
what suppresses deferral in `gated`. A single mode would show neither.

Since deferral started requiring a validated profile, the raised floor in
`gated` is no longer the *only* thing refusing that page — this stack has no
browser, so no page is ever validated. The mode is kept anyway: it is the
control that keeps `forced` meaningful (identical argv apart from the switch),
and it stays honest the day a validated profile does reach this fixture.

One assertion in the markup lane is carried but **not exercised** by this
fixture: the `<noscript>` twin must preserve `integrity` and `crossorigin`, and
the captured page's stylesheet has neither, so that branch never runs here. It is
covered at unit level (`html_transform_filter_test.cc`, including the bare
valueless `crossorigin` case). Exercising it end to end needs a second,
SRI-bearing stylesheet in the fixture — worth adding whenever the capture is next
refreshed.

`shipped` is a characterization test, not a wish. This page used to defer under
stock flags: the extractor produces ~45 KB of critical CSS against ~118 KB of
combined CSS (the fixture's 115 KB sheet plus the page's inline styles; the
block was ~25 KB before the fold budget was counted from `<body>`), which
clears the 0.10 byte-ratio floor, so deferral was permitted
although nothing had checked that the inlined block covers the fold. That was the
defect the wider effort is about. Deferral now additionally requires a validated
profile whose stylesheet hash still matches the sheet being served; this fixture
never acquires one, so the stock-flag answer is "render-blocking, critical CSS
still inlined". `DEFERS_WITHOUT_A_VALIDATED_PROFILE` in `conftest.py` records
which of the two is expected, and the shipped-mode test pins it either way.

## `--unsafe-force-async-css`

CLI-only, undocumented, and never to be used on production traffic. It exists so
this harness can exercise deferred markup while the sufficiency gate is, quite
correctly, refusing to produce any. See `src/worker/unsafe_force_async_css.h` for
why it is not a config-file key, and expect a loud warning on stderr whenever a
worker starts with it.

## Warm-up, and why it is not incidental

Two ordering facts decide whether this harness measures anything real:

1. **The stylesheet must be cached before the page is first optimized.** Until it
   is, the worker derives its critical block from the page's own inline CSS
   alone and stores that. The result reads like a product bug and is a warm-up
   race. So readiness is probed on the stylesheet, never on the page under test.
2. **`X-PageSpeed: HIT` does not mean "the optimizer has run".** The front end
   stores and serves the origin bytes while the worker optimizes out of band, so
   a HIT arriving 0.3s in is routinely the unmodified page. Asserting on it reads
   as "the optimizer did nothing" — the most misleading way this harness can
   fail. The optimizer's output is by definition not the origin's bytes, so the
   fixtures wait for exactly that.

The rendered lane has a third: the front end stores one variant per request shape
(`Vary: Accept-Encoding, User-Agent`), so the browser must warm its *own* variant
by navigating repeatedly. Warming with `fetch()` alone warms a variant nobody
measures. See "Warm-up (rendered lane)" below for what that costs if skipped.

## Golden screenshots

`goldens/fold-{mobile,tablet,desktop}.png` are the fully-styled renders at the
three profile viewports (`browser_analysis_manager.h`,
`AnalysisContext::kViewportWidths` / `kViewportHeights`). The rendered lane diffs
the critical-CSS-only render against them, using the product's own comparison:
a pixel differs when any channel differs by more than `kChannelTolerance = 2`,
and the verdict is the differing-pixel ratio against `kDefaultThreshold = 0.005`
(`src/browser/visual_regression_gate.h`). Lane and product agree by construction.

Each viewport is rendered as its device class, so each is served the variant,
and the critical block, a visitor of that class gets. The front end picks the
class from the User-Agent (`ParseViewport` in
`lib/classify/capability_mask.cc`): the mobile viewport sends an Android phone
Chrome UA (`Android` + `Mobile`, so `kMobile`), the tablet an Android tablet
Chrome UA (`Android` without `Mobile`, so `kTablet`; iPadOS Safari sends a
desktop `Macintosh` UA and would land in `kDesktop`), and the desktop keeps
Playwright's own UA. Until the unsized-media fix every viewport sent the desktop UA and
was served the desktop block. Since User-Agent emulation the product's renders send the
same phone and tablet UAs, and both read them from
`src/browser/device_emulation.json` (`user_agent_overrides`; a mobile viewport
at most `phone_max_width` px wide is the phone): the lane sets Playwright's
`userAgent` and repeats the product's exact `Emulation.setUserAgentOverride`
call (UA string, `navigator.platform`, client hints) on the page's CDP session.

The layout mode is the product's own, from one file: every analysis render
sets its device metrics from `src/browser/device_emulation.json` (through
`src/browser/device_emulation.h`, which `device_emulation_test` holds equal to
the file), and the rendered lane reads the same file. A viewport at most
`mobile_max_width` (768) px wide renders as a mobile device (`isMobile`):
Chromium honours the meta viewport and widens the layout viewport when the
document overflows, as a phone or a tablet in portrait does. Without it an
overflow only adds a scrollbar outside the screenshot, which is how an overflow far below
the fold once went unseen. Wider viewports render as a desktop window. A mobile device is also a
touch device (`mobile_has_touch`): `hasTouch`, plus the product's
exact `Emulation.setTouchEmulationEnabled` call with `max_touch_points` on the
page's CDP session, so `(hover: none)` and `(pointer: coarse)` match on the
phone and the tablet and `(hover: hover)` / `(pointer: fine)` do not, as in the
product's renders and on a visitor's phone; the desktop has no touch. Before
measuring, the lane checks that the served page sees what the product's render
of that class sees (`assertDeviceParity`): the four hover/pointer queries,
`navigator.maxTouchPoints`, `'ontouchstart' in window`, the UA string and
`navigator.platform`, and `navigator.userAgentData.mobile` / `.platform` where
the page is a secure context (the nightly serves plain http, so not there); a
mismatch is an infrastructure failure, not a finding. The nightly mounts the
file into the container (`PROBE_DEVICE_EMULATION`), since only this directory
is mounted otherwise.

Since the tablet-as-mobile change the tablet renders as a mobile device too (it rendered as a
desktop window before, as the product did). That moved one golden,
`fold-tablet.png`, by 0.01111 (8,735 px): the fixture's `hidden md:flex` nav
row (`md:flex` applies from 768 px) is 776 px wide, wider than the tablet, so
the document is 800 px wide. A desktop window keeps its 768 px layout viewport
and the nav overflows into a horizontal scroll; a mobile device widens the
layout viewport to 801 px. The page itself stays at device width (body,
header, main and the h1 sit at the same pixels in both modes); what moves is
the fixture's two `position: fixed` bottom-right buttons, which anchor to the
widened 801×1067 layout viewport and shift about 33 px right and 43 px down,
half off-screen (6,978 of the differing pixels). The rest is glyph
anti-aliasing on the nav labels. With that nav hidden the two modes render
byte-identically (0 px), and
so do the regenerated `fold-mobile.png` and `fold-desktop.png` (dropping
`hasTouch` from the phone changed nothing). Touch emulation turned touch back on
for the phone and the tablet; the fixture has no hover/pointer media query
that paints the fold differently, so the goldens are expected to hold.
User-Agent emulation changed the phone's and the tablet's UA strings to the product's
(Chrome's reduced Android form, `Android 10; K`, instead of a Pixel 8 and a
Galaxy Tab) and added `navigator.platform` and client hints; they classify to
the same classes, and nothing in the fixture reads the UA, the platform or
`ontouchstart`, so the goldens are expected to hold as well.

**Regenerate only inside the pinned image**, or the goldens encode a different
Chrome's rasterization:

```bash
# with the stack already up in forced mode; NET is the compose network of that
# stack (run_probe.sh names the project async-css-probe-$$)
NET=$(docker network ls --filter name=async-css-probe --format '{{.Name}}' | head -1)
docker run --rm --network="$NET" \
  --user "$(id -u):$(id -g)" -e HOME=/tmp -e npm_config_cache=/tmp/.npm \
  -v "$PWD/tools/async-css-probe":/probe -w /tmp \
  -v "$PWD/src/browser/device_emulation.json":/device_emulation.json:ro \
  -e PROBE_BASE_URL=http://nginx:8080 \
  -e PROBE_GOLDEN_DIR=/probe/goldens \
  -e PROBE_PRIMITIVE=/probe/primitive.json \
  -e PROBE_DEVICE_EMULATION=/device_emulation.json \
  -e ASYNC_CSS_PROBE_REGENERATE_GOLDENS=1 \
  mcr.microsoft.com/playwright:v1.58.2-noble \
  bash -c 'npm install --no-save --no-audit --no-fund --silent playwright@1.58.2 pngjs@7 >/dev/null 2>&1 \
           && cp /probe/probe_rendered.mjs /tmp/ && node /tmp/probe_rendered.mjs'
```

The image tag is pinned in `async-css-probe-nightly.yml` and must match the one
the goldens were generated in. Regenerating is a deliberate, reviewable change:
a golden refreshed to match a regression hides the regression.

## Promotion criteria

Both lanes land non-blocking on purpose, and each has an explicit bar to clear.

**Markup lane → the required-checks gate**: 20 consecutive green runs on
unrelated PRs, p95 runtime under 8 minutes, and zero infra-only reds. Promotion
is one line added to `required-checks-gate.needs` in `ci.yml`. The lane already
runs unfiltered on every PR, because the gate fails on `skipped` — path-filtering
and gate membership are mutually exclusive.

**Rendered lane → blocking**: the Playwright image pinned **by digest**, the
goldens regenerated inside that pinned image, and 30 consecutive clean nightlies.
Until then a finding opens or refreshes one tracking issue
(`async-css-probe-finding`) and uploads the `async-css-probe-evidence` artifact;
the next clean run closes it. A cross-Chrome-version pixel diff on a blocking
check is a flake generator. Only scheduled runs on the default branch count toward
the 30.

## Rendered lane: how the flash window is reconstructed

The obvious method — intercept the deferred stylesheet, hold the response, and
screenshot while the browser genuinely sits in the flash window — was implemented
and does not work here, for one measured reason: **enabling route interception
changes which variant answers.** Responses vary on `Accept-Encoding` and
`User-Agent`, and the intercepted navigation is served the origin-bytes variant,
never converging on the deferred one however long it is warmed. Everything
measured under interception was measured on the wrong document.

So the lane loads the page normally, screenshots it fully styled, then puts the
deferred `<link>`s back to the non-applying primitive — undoing exactly what the
loader did — and screenshots again. Same DOM, same subresource state; the only
difference is that the deferred stylesheet does not apply. That is the CSS state
of the flash window, reconstructed rather than observed: exact in the dimension
being measured, and free of the variant divergence that makes the observed
version unusable.

### Fonts and fold images are in the fixture

They were not, and the gap was the lane's largest. The capture now carries the
three web fonts and the two logos the fold renders with, so the deferred
stylesheet's `@font-face` rules resolve against real files and the
reconstructed flash window shows text falling back to system metrics — the
resource-dependent flash, which **only this lane will ever cover**: the
in-product validation gate blocks every subresource in its render as an SSRF
defence, so it is blind to this class permanently and by design.

Two determinism pins came with those assets, and neither is emulation for its
own sake. `reducedMotion: "reduce"`, because the captured logo SVG animates a
blinking cursor inside `<img>` and would otherwise put a few dozen coin-flip
pixels into every diff; the SVG honours the preference itself. And a bounded
wait on `document.fonts.ready` before each screenshot, because a shot taken
mid-swap is a fallback-font render that reads as exactly the flash this lane
hunts.

### What a green rendered run does not establish

* **Only the stylesheet's absence is reconstructed.** A flash caused by ordering,
  or by anything that exists only in the real timing window, is out of scope by
  construction. The fonts are in the browser cache by the time the flash state
  is reconstructed, so what is measured is the fold losing its `@font-face`
  *declarations*, not a font still on the wire.
* **No `@import`.** The captured stylesheet has none, so a flash caused by a
  nested import chain is not exercised here.
* **Layout established by JS is not covered.** The page's island bundles are
  deliberately not in the capture (`/_astro/QuickMessage*.js` and `/api/geo`
  still 404 — behaviour and data, not fold paint), so anything the fold's
  geometry owes to script is absent from both screenshots equally.
* **The measured configuration never ships.** Deferral is forced past the
  sufficiency gate so there is deferred markup to render at all; the findings
  file records the worker argv it measured against.

### Reading a finding: what did the extractor inline?

The rendered lane measures the fold with only the inlined critical block
applying, so the first question a flash finding raises is what that block
contains. `dump_critical_css` prints it without a stack: the same scanner,
the same combined-stylesheet assembly and the same extractor the worker runs
when no browser profile exists (the probe stack has no browser, so that is
always its case).

```bash
bazel build //tools/async-css-probe:dump_critical_css
bazel-bin/tools/async-css-probe/dump_critical_css \
  --fixture tools/async-css-probe/fixtures/modpagespeed-com \
  --viewport desktop > critical.css        # stats on stderr
```

Two more fixtures exist for block-size comparisons across CSS frameworks,
not for the rendered lane: `fixtures/bootstrap-5.3.3` (stock
`bootstrap.min.css` 5.3.3 from jsDelivr, sha256
`3c8f27e6009ccfd710a905e6dcf12d0ee3c6f2ac7da05b0572d3e0d12e736fc8`, with a
navbar / hero / cards page) and `fixtures/tailwind-v3` (a landing page and the
`tailwind.css` that `npx tailwindcss@3.4.17 --minify` generates for it from
the three `@tailwind` directives and a default config). Both CSS files are
MIT and keep their license comments. `--max-wholesale-media-bytes N`
overrides the extractor's wholesale threshold for an @media block.

That is how the first three months of findings were read: the block carried
the sheet's `@font-face` rules and the whole `@property`/theme boilerplate but
not one utility rule of the fold, because the element budget was counted
from `<html>` and this page's `<body>` is element 42. Splice a candidate block
into a served page with the deferred `<link>` left as `rel="preload"` and no
loader, and the pinned image renders the flash state of that block directly.

### Warm-up (rendered lane)

The lane pre-warms **once**, before the viewport loop, and it warms over
**plain HTTP** — the markup lane's exact sequence — before confirming through
the browser. It does so for each device class in turn (each class's User-Agent
is its own variant, so warming one warms nothing the others are served),
within one 180-second budget shared by all three: the first class pays for the
stylesheet and the cold optimizer, the others usually take a few seconds.

That ordering is the finding, and it is not the obvious one. The optimizer
produces one variant per capability mask, and on a cold stack **the browser's own
repeated navigation does not converge**: measured flat at the origin's bytes for a
full 180 seconds, no deferred markup ever appearing. Warm over plain HTTP first
and the browser is served the deferred page on its *first* navigation, in under
two seconds. Warming through the browser alone would have filed a false finding
every night, since the nightly always starts from `down -v`.

A warm failure is an infrastructure outcome: it goes in its own section of the
findings file, never into the product list, because it is not evidence about the
fold.

## Fixture

`fixtures/modpagespeed-com/` is a byte-frozen capture — the HTML, its
stylesheet, and the fold's three web fonts and two logos. `CAPTURE.md` records
each file's URL, date, byte size, sha256, and the evidence that the capture is
origin-shaped rather than already-optimized. It is never fetched live: a live
fetch would make the lane's verdict a function of whatever shipped that morning.
Refreshing it is a deliberate, reviewable change — and one that moves the fold,
so it means regenerating the goldens in the same change.

The directory **mirrors the site's URL space**: a file at `<fixture>/a/b` is
served at `/a/b`, `index.html` at `/`, and anything not on disk is a 404.
`probe_origin.py` builds its route table from that walk at startup, so adding a
subresource to a future capture is a file drop, not a code change.
