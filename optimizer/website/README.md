# modpagespeed.com

Source of the modpagespeed.com site (Astro). See [CLAUDE.md](CLAUDE.md) for the tree, the content rules and the gates; `npm ci`, `npm run dev`, `npm run build`, `npm run check:content` (after a build).

## Before/after evidence for a change

`scripts/visual-diff.mjs` compares two locally served builds, a base and a head. It is opt-in and local: it is not part of `npx vitest run` or `npx playwright test`, and it never builds or starts a server.

Build each side in its own checkout with its own build id, and serve each on an explicit free port:

```
SITE_BUILD_ID=base-<short sha> PRICING_ALLOW_STALE=1 npm run build
HOST=127.0.0.1 PORT=<free port> node dist/server/entry.mjs
```

Screenshots (full page, dark, at 1440x900, 820x1180 and 390x844):

```
node scripts/visual-diff.mjs shots --base http://127.0.0.1:<port> --head http://127.0.0.1:<port> \
  --base-id base-<short sha> --head-id head-<short sha> --out <dir> [--paths <file>] [path...]
```

Without paths it shoots a default set of twelve pages, which includes `/error/404/` and `/error/500/`: the node server answers an unknown URL with a redirect stub, so the designed error pages are reviewed at those two paths. The script checks the `<meta name="build-id">` each origin serves and refuses to run when an id does not match, when the two ids are equal or when an origin is not a loopback address. `<dir>` receives `base/`, `head/` and `diff/` images, `table.md` (`page | viewport | changed %`), `report.json` and `index.html` (a contact sheet). Captures are deterministic: reduced motion, animations off, device scale factor 1, fixed date, empty storage, requests to other hosts refused, fonts and lazy images loaded, the page unscrolled. The telemetry strip and the footer copyright line are masked (`--mask <selector>` adds more), so a change inside them does not show. `--fail-on-diff` exits 1 on any changed pixel, for a change that must not move anything. `--timeout <ms>` (default 45000) limits the navigation and each capture of a page; a very tall page (the configuration reference is over a hundred screens) is captured in one image and may need more on a busy machine. Two builds of the same commit give 0 changed pixels. A few antialiased edge pixels can round differently between two captures, so a pair that differs in exactly that way (same page size, at most 200 pixels, at most two steps per channel) is shot again, up to twice, and the closest pair is kept; any other difference is reported as it is, from the first capture. Every round is in `report.json`, a row whose discarded round differed says so, and `--fail-on-diff` also fails when a discarded round differed by more than noise.

Normalized build-output diff (prerendered HTML and built CSS, after the scoped-style ids, hashed bundle names and the build-id tag are stripped; CSS is compared rule by rule):

```
node scripts/visual-diff.mjs dist --base <base checkout>/website --head <head checkout>/website
```

It prints what differs and exits 1, or `normalized dist diff: empty` and exits 0. Like `shots`, it reads the build-id tag of each side and refuses a side built without `SITE_BUILD_ID`, two equal ids, or the same directory given twice. On-demand routes are not in `dist/`; the screenshots cover those. "Empty" means the same prerendered markup and the same set of CSS rules, not an identical build: the CSS comparison ignores rule order, so a rule that moves between sheets is not a difference and neither is a reordering that changes the cascade, and client JavaScript bundles, images, fonts and other static files are not compared.
