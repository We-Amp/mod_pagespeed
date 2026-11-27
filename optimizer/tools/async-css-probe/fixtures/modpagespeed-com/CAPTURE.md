# Fixture capture — modpagespeed.com home page

Byte-frozen origin-shaped capture of the modpagespeed.com home page and the
Tailwind v4 stylesheet it loads. **Data only.** The probe harness that consumes
it lands separately; nothing here is executable and nothing here is fetched at
run time.

Why frozen: the async-CSS probe and the critical-CSS validation threshold both
need a stable, realistic input. A live fetch would make the lane's verdict a
function of whatever shipped that morning. This capture drifts from the live
site by design; refreshing it is a deliberate, reviewable change, not an
automatic one.

## What was captured

| File | Bytes | sha256 |
|---|---|---|
| `index.html` | 271544 | `c31007b763f4e55f2d249431d8850959a6b842d92140093f1c0e4b5e46fd3d5e` |
| `BaseLayout.BV5EQjYn.css` | 115789 | `5c368d20b277bfd7d270a53bb61320419551ec8409214d54787a6c6262358bb0` |

Captured **2026-08-01**.

### 2026-09-30 — goldens regenerated for the 2026-09-16 reword, no re-capture

The 2026-09-16 amendment below predicted it: the reworded hero annotations
are inside the tablet and desktop folds, and the rendered lane reported the
fully-styled render diverging from `goldens/fold-tablet.png` (0.07805) and
`goldens/fold-desktop.png` (0.04763) every night from 2026-09-17. Both goldens
were regenerated inside the pinned Playwright image against the forced-mode
stack, exactly as the README describes; the old and new goldens differ by
those same two ratios and by nothing else, and the mobile golden came back
byte-identical (the annotations sit below the 375x667 fold). The capture
itself is unchanged; the owed re-capture still stands.

### Amended 2026-09-16 — surviving retired-licensing copy scrubbed, no re-capture

Third deliberate non-refresh, same rules again: text-only edits, no element
added or removed. The 2026-09-10 pass scrubbed the below-fold and FAQ
instances of the retired licensing model but missed three that contradict
the Apache-2.0 GA reality, now reworded:

- The first hero annotation ("Install and run unlicensed … Production use
  requires a commercial license — but the software never locks you out.")
  now reads that the product is free and open source under Apache-2.0 with
  every feature included and no license key.
- The second hero annotation ("Licensed per site …") now reads Apache-2.0,
  no per-image or per-request metering, support and trust are what is sold;
  the pricing link stands.
- The closing CTA line ("Free to run unlicensed — license for production,
  cancel anytime.") now reads free and open source (Apache-2.0), commercial
  support available.

271634 -> 271554 bytes; the sha256 in the table above is updated to match.
The hero annotations are fold-visible: the reword adds and removes no
element and keeps the line structure, so no layout moves, but the reworded
glyphs do differ from `goldens/fold-*.png` — the non-blocking rendered lane
may flag until the owed re-capture (post-GA site deploy) regenerates the
goldens. A script-island comment naming the "Unlicensed/Community" pricing
ladder rows was considered and left: it describes pricing mechanics, not
licensing terms.

### Amended 2026-09-10 — pricing and retired-licensing copy scrubbed, no re-capture

Second deliberate non-refresh, same rules as the first: edits inside `<script>`
data islands and below-the-fold pricing copy, nothing else, so no element moves
and the rendered lane's fold goldens stay valid. What changed, and why:

- The `fs-pricing-data` JSON island carried the live price list (8 plans x 251
  countries, numeric `price` and localized `display` strings). Every `price`
  is now `0` and every `display` is `"TBA"` — a fixture stays a fixture, but
  the published tree is not the place for a price list.
- The four `[data-fs-price]` fallback spans (`$99`/`$79`) read `TBA`.
- The `AggregateOffer` JSON-LD block stated tier prices, a price-validity date
  and licensing terms; the numbers are zeroed and the description is "Pricing
  to be announced."
- Three sentences describing the retired licensing model were removed from
  below-fold copy and one FAQ answer; the surrounding sentences stand
  unchanged. As the 2026-09-16 amendment above records, this pass missed the
  same model in the two hero annotations and the closing CTA line — the
  "never locks you out" sentence among them — so this bullet's original
  wording over-claimed a complete removal; the 2026-09-16 pass scrubbed
  those survivors.

287068 -> 271634 bytes. A real refresh — new HTML against the redesigned
site, possibly a new stylesheet filename, and regenerated goldens — is still
owed the next time the site deploys, exactly as the first amendment says.

### Amended 2026-09-08 — three comments, no re-capture

`index.html` is no longer byte-identical to the 2026-08-01 fetch. Three comments
inside the pricing component's inline script cited internal decision records by
number. The site stopped serving those citations at source, and a frozen copy of
our own marketing page should not be left holding the last ones. Each now reads
the way the page reads today — the reason rather than the citation — and nothing
else in the file changed: 287072 → 287068 bytes.

This is deliberately **not** a refresh. It edits comment text inside `<script>`
blocks and nothing else, so it moves no element, touches no stylesheet, and
leaves the rendered lane's goldens valid. The stop-conditions above are about a
capture the optimizer has rewritten; nothing here was re-fetched, so they do not
apply. A real refresh — new HTML, possibly a new stylesheet filename, and
regenerated goldens — is still owed the next time the site deploys.

The fixture directory mirrors the site's URL space — a file at `<fixture>/a/b`
is served at `/a/b`, `index.html` at `/` — so the stylesheet lives at
`_astro/BaseLayout.BV5EQjYn.css`, the path `index.html` references. Adding a
file to the capture means putting it at the path the page asks for; there is no
route list to update (`probe_origin.py`).

## Exact commands

```bash
curl -sS -o index.html \
  'https://modpagespeed.com/?PageSpeed=off'

curl -sS -o BaseLayout.BV5EQjYn.css \
  'https://modpagespeed.com/_astro/BaseLayout.BV5EQjYn.css?PageSpeed=off'
```

## Why `?PageSpeed=off` is mandatory

The capture must be **origin-shaped** — what the optimizer receives, not what it
produces. Freezing already-optimized output would make the probe measure the
optimizer against its own previous output, which can only ever agree with
itself.

`?PageSpeed=off` was verified to actually disable rewriting at capture time:

- The stylesheet differs between the two fetches — 115789 bytes with the
  parameter versus 115653 without, the larger one still carrying the
  whitespace a minifier removes (`..., inset 0 1px ...` vs `...,inset 0 1px ...`
  at byte 509). So the parameter demonstrably suppresses the CSS rewrite.
- `index.html` is byte-identical with and without the parameter, and carries no
  rewrite markers (no `data-pagespeed-*` attributes, no `.pagespeed.` URLs).
  Its `<link rel="stylesheet" href="/_astro/BaseLayout.BV5EQjYn.css">` is the
  original, unmodified reference. HTML-level async-CSS deferral is currently
  switched off in this deployment, which is consistent with both fetches
  agreeing.

If a future refresh finds that the parameter no longer suppresses rewriting —
the stylesheet fetches identical with and without it, or the HTML comes back
carrying rewrite markers — **stop and do not commit the capture**. A rewritten
fixture is worse than a stale one: it is silently wrong in the direction that
makes the probe pass.

## Shape of the capture, for reviewers

The stylesheet is a Tailwind v4 build: 5 cascade layers (`theme`, `base`,
`components`, `utilities`, `properties`), 22 `@media` blocks, and **59
`@property` registrations**. Those registrations are exactly the class of
at-rule the extractor used to drop, and the layer/media wrappers are exactly the
context a browser-coverage byte slice loses — so this fixture exercises both
halves of the extractor fix on realistic input rather than on a synthetic sheet.

## Fold subresources (added 2026-08-02)

The HTML and stylesheet alone left every font and logo the page requests
returning 404. Both screenshots the rendered lane takes were affected equally,
so nothing went red — the lane simply had no way to see a flash caused by a web
font or an image arriving, which is a whole class of the failure it exists to
catch, and the one class the in-product validation gate can never cover (its
render blocks all subresources as an SSRF defence). These five files close it.

| File | URL | Bytes | sha256 |
|---|---|---|---|
| `fonts/inter-variable-subset.woff2` | `/fonts/inter-variable-subset.woff2` | 151832 | `9f55a0aadfa846c5095a829a0d2c7f5091153758e2cc5af8f358f6098850786c` |
| `fonts/plex-mono-600.woff2` | `/fonts/plex-mono-600.woff2` | 15620 | `0d1f0b8d0722224e32e9f28261bdc86c79115be73444ae5eceb73976a1bcdf83` |
| `fonts/plex-mono-400.woff2` | `/fonts/plex-mono-400.woff2` | 14708 | `08949f728dc52d528e69b1667d15c89a5686a4ee9a296ff90983985f99c380f7` |
| `logo.svg` | `/logo.svg` | 1701 | `aaed9c3713b2f3ef454bfe7d1a4c397e1a33d90fa94c9701f0723e09440ba83a` |
| `logo-dark.svg` | `/logo-dark.svg` | 1946 | `6906ff6a0a3ce87b30488eaff5e8c65f353a18ae261b5e1bd4e6b286c2daab02` |

Captured **2026-08-02** from `https://modpagespeed.com/<path>`.

```bash
for p in fonts/inter-variable-subset.woff2 fonts/plex-mono-600.woff2 \
         fonts/plex-mono-400.woff2 logo.svg logo-dark.svg; do
  mkdir -p "$(dirname "$p")"
  curl -sS -o "$p" "https://modpagespeed.com/$p"
done
```

### `?PageSpeed=off` on these five

Each was fetched **both** with and without the parameter and the two are
byte-identical in all five cases, so the copies here are the plain fetches and
are origin-shaped either way. That is the expected result rather than a
surprise: these are static assets the deployment does not rewrite — woff2 is
opaque to it, and neither SVG comes back carrying a rewrite marker (`pagespeed`
occurs in both only as the wordmark text `mod_pagespeed`, and neither URL is a
`.pagespeed.` form).

The stop condition above still applies to the whole capture: if a refresh finds
the parameter no longer suppresses rewriting **on the HTML or the stylesheet**,
stop and do not commit. For these five the signal is different — they should
keep fetching identical with and without it; a pair that starts to *differ*
means the deployment began rewriting them, and the `?PageSpeed=off` copy is the
one to keep.

### What is deliberately still missing

`/_astro/QuickMessage*.js` and `/api/geo` are requested by the page and stay
404. Neither paints the fold — one is an island's behaviour, one is data — and
freezing a script into the capture would make the lane's verdict depend on what
that script happened to do on capture day. `/fonts/plex-mono-500.woff2` is
declared `@font-face` in the stylesheet but no fold text uses that weight, so
it is never requested; capture it if that changes.

## Refreshing

Re-run the commands, re-verify the `?PageSpeed=off` evidence above, update the
tables (bytes + sha256), the capture date, and the stylesheet filename if its
content hash moved. Land it as its own reviewable change. A capture refresh
that moves the fold means regenerating the rendered lane's goldens in the
pinned Playwright image in the same change (`../../README.md`).

### Amended 2026-10-08 — one CSS comment reworded, no re-capture

A CSS comment in the inline style block was reworded (no rule or markup
changed): 271554 -> 271544 bytes; the sha256 in the table above is updated
to match.
