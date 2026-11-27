# Examples gallery data generator

Generates the measured before/after data behind the live optimization gallery at
[modpagespeed.com/examples/](https://modpagespeed.com/examples/).

## What it does

`generate.mjs` reads the catalog in [`website/src/data/examples.ts`](../../website/src/data/examples.ts)
and, for each example, fetches the demo page from the live Apache + mod_pagespeed
1.1 backend twice:

- **before** — `…/<page>?PageSpeed=off` (optimization off)
- **after** — `…/<page>?PageSpeedFilters=<filter>` (only that filter)

Subresources are measured on both sides with the same request. The demo host
optimizes resources in place, so a plain request can return the original on one
side and an optimized copy on the other. A resource URL the filter did not
rewrite is therefore fetched with `?PageSpeed=off` on both sides (the original
bytes), while a rewritten `.pagespeed.` URL and resources on other hosts are
fetched as served. See `resource-fetch-url.mjs`.

It then captures a source diff and measures the byte/request impact, writing
[`website/src/data/examples-data.json`](../../website/src/data/examples-data.json),
which the Astro pages read at build time. The Astro pages render the live
before/after iframes regardless; this committed data adds the source diff and the
measured-impact tiles.

## When to run

Run it **after the demo host has the example pages deployed** and **before
building/deploying the website**, then commit the regenerated
`examples-data.json`.

It also doubles as a **cache warm-up**: resource-rewriting filters are async on a
cold cache (the first request for a novel filter set returns pass-through HTML),
so the run polls each "after" URL until it settles, backing off between polls
(12 polls over about a minute), which leaves the demo backend's cache warm for
real visitors. When the body does not settle, the after URL is polled again with
a desktop browser user agent (6 polls): some filters, such as `lazyload_images`
and `defer_javascript`, only act for a browser.

## Status

Each entry records why it has, or has no, source diff, in `status`
(classification in `settle-status.mjs`):

| `status` | Meaning | What the evidence shows |
| --- | --- | --- |
| `settled` | The filter changed the body and the change held. `diff` and `savings` show it. | `userAgent` says which user agent got it. |
| `header` | The body did not change; the response gained a resource hint header (`Link: rel=preload`, `dns-prefetch`, `preconnect`) that the original lacks. | `hintHeader` holds the added entries. |
| `beacon` | The only change is the beacon instrumentation. The filter acts after a real browser reports back, which a fetch cannot do. | `beaconInstrumented: true`; the stored `after.html` is the instrumented page. |
| `not-applied` | The body never changed and no hint header appeared, for every user agent tried, over the whole polling window: the filter does not apply on this host. | `userAgentsTried`, `requests`, `windowSeconds`, `bodyChanged: false`, and `moduleHeader` (the module did handle the page). |
| `unsettled` | The body changed but never held. The rewrite is still in progress; run again later. | `bodyChanged: true`. |

`savings` is `null` for every status other than `settled`: the after page is
then not the filter's effect (an instrumented first visit, or the original), so
its figures would mislead. The capture (`before`, `after`, `diff`) is kept as
evidence; for `beacon` it shows the instrumentation the filter adds on a first
visit. A `beacon` entry counts as changed between runs only when its status
changes, because its beacon script carries a per-response nonce.

`settled` (boolean) is kept and always equals `status === "settled"`. Entries
written before `status` existed are given one from their stored capture on the
next run (`legacyStatus` in `settle-status.mjs`).

How the website should read it: take `status`, falling back to
`settled ? "settled" : "unsettled"` for an older file. Show the diff and the
measured impact only for `settled`. For the other statuses, say why there is
no diff from the status itself rather than from a fixed per-filter list:
`header` can quote `statusEvidence.hintHeader`, `beacon` explains the browser
report, and `not-applied` says plainly that the demo host does not apply the
filter today, quoting what was tried. `unsettled` keeps today's "will appear
once processed" wording.

## Usage

Requires Node ≥ 22 (uses native fetch + TypeScript import of the catalog).

```bash
cd pagespeed-optimizer   # the repository root
node tools/examples-gallery/generate.mjs                       # all examples
node tools/examples-gallery/generate.mjs --only combine_css,rewrite_images
DEMO_ORIGIN=https://demo-httpd-1.1.modpagespeed.com node tools/examples-gallery/generate.mjs
```

`--only` merges into the existing JSON (partial runs don't wipe other entries).

## Tests

Subresource extraction lives in `extract-resources.mjs` and the status
classification in `settle-status.mjs`, so both can be tested on their own. It reads `<link href>`, `<script src>` and `<img src>` values whether
they are double-quoted, single-quoted or unquoted (filters such as
`remove_quotes` emit unquoted values). The unit tests run with the website's
unit suite:

```bash
cd website && npx vitest run test/examples-gallery-extract.test.ts test/examples-gallery-fetch-url.test.ts test/examples-gallery-settle-status.test.ts
```

## Output shape

```jsonc
{
  "generated_at": "ISO-8601",
  "origin": "https://demo-httpd-1.1.modpagespeed.com",
  "examples": {
    "combine_css": {
      "status": "settled",     // settled | header | beacon | not-applied | unsettled
      "settled": true,         // status === "settled" (kept for older readers)
      "statusEvidence": {
        "userAgent": "default",            // UA of the stored after capture: default | browser
        "userAgentsTried": ["default"],
        "requests": 2,                     // after-page polls made
        "windowSeconds": 1,                // time spent polling
        "moduleHeader": "2.8.1",           // X-Mod-Pagespeed on the after response
        "bodyChanged": true,
        "beaconInstrumented": false
        // "hintHeader": "…"               // header status only
      },
      "attempts": 2,
      "before": { "html": "…", "htmlBytes": 0, "requestCount": 0, "resourceBytes": 0, … },
      "after":  { "html": "…", "htmlBytes": 0, "requestCount": 0, "resourceBytes": 0, … },
      "savings": { "htmlBytesPct": 0, "requests": 0, "resourceBytesPct": 0, "totalBytesPct": 0 }, // null unless settled
      "diff": [{ "t": "-|+| |@", "s": "line" }]
    }
  }
}
```

Entries that error, or whose status is not `settled`, still render from whatever
was captured; the gallery degrades to live-iframes-only for those.
