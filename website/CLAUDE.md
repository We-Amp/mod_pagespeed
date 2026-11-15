# CLAUDE.md — modpagespeed.com (`website/`)

This directory is the modpagespeed.com site: the marketing pages, the product
documentation, the blog, the examples gallery, the demo and calculator pages, and
the machine-readable surfaces (`/llms.txt`, `/api/product.json`, the Helm chart
repository under `/charts/`). The root `CLAUDE.md` covers the rest of the repository;
this file covers only the site.

**This tree is the site's source of truth.** Every change to the site is made here
and lands on `main` through a pull request, like any other change in this repository.
Nothing is imported from elsewhere and nothing here is overwritten by an export.
Deploys are done by the operator from this tree; CI in this repository builds and
tests the site but never deploys it.

## Stack

- Astro 7 with the MDX integration, Tailwind CSS 4 (`@tailwindcss/vite`), TypeScript.
- Server output through `@astrojs/node` in standalone mode: `npm run build` writes the
  static assets and prerendered pages to `dist/client/` and the server entry to
  `dist/server/entry.mjs`. The `Dockerfile` here packages that output into the
  container the site runs from.
- Prettier (`prettier-plugin-astro`) and ESLint (`eslint-plugin-astro`); `astro check`
  for types.

## Commands

Run everything from `website/`.

| Command | Purpose |
|---|---|
| `npm ci` | Install. npm is canonical; `package-lock.json` is the only lockfile. |
| `npm run dev` | Dev server at `http://localhost:4321`. |
| `PRICING_ALLOW_STALE=1 npm run build` | Production build. The prebuild step fetches localized prices; without network or credentials it refuses unless this variable is set, in which case it builds from the committed `src/data/fastspring-pricing.json`. CI always sets it. |
| `npm run format` / `npm run format:check` | Prettier over `src/`, `tests/`, `test/`, `scripts/` and the root configs (never the Markdown content). Run before staging; CI runs `format:check`. |
| `npm run lint` | ESLint plus `astro check`. CI runs it; warnings and hints do not fail it, errors do. |
| `npx vitest run` | Unit tests and the sync/content gates in `test/`. Fast; run this first. |
| `npx playwright test` | Browser suite in `tests/`. Starts the dev server itself (`webServer` in `playwright.config.ts`); run `npx playwright install chromium` once. |
| `npm run gen:llms` | Regenerate `public/llms.txt` and `public/llms-full.txt` from the docs collection, and `public/.well-known/ai-plugin.json` from its template (see "The agent files"). |
| `MOD_PAGESPEED_SRC=<clone> npm run gen:references` | Regenerate `src/data/reference/*.json` from the mod_pagespeed clone at the tag in `source-pin.json` plus this tree's worker and thin-module sources, and re-render the generated regions of `configuration.md` and `worker-configuration.md`. |

The prebuild step also regenerates the blog title cards (`public/og-cards/`) and
the agent files, so a build never ships stale generated output.

Astro 7 detects an AI-agent session and then starts `astro dev` as a background
process (`npx astro dev status|logs|stop` manage it). Playwright's `webServer` sees
that parent exit and fails with "Process from config.webServer exited early", so in
such a session start the dev server first (`npm run dev`, which backgrounds itself), run
`npx playwright test` with `CI` unset so it reuses the running server, and finish with
`npx astro dev stop`. A human terminal and CI are not affected.

## Stylesheet strategy

The site CSS stays an external sheet (Astro's default). Production does not inline critical CSS at present: the filter is off for this site until We-Amp/mod_pagespeed#713 is fixed, so the sheet blocks the first view and is cached after that.
Inlining the whole sheet at build time would add its gzip size to every page view after the first and hand the critical-CSS filter a second copy to process once it is back on. Fonts are the remaining LCP lever, guarded by `test/font-budget.test.ts`.

## Theme and error pages

- **The site is dark-only.** `BaseLayout.astro` always renders `<html class="dark">` with
  `color-scheme: dark`; there is no toggle and no light theme on screen. Review, screenshot
  and contrast-check pages in dark only.
- The light values in the `@theme` block of `src/styles/global.css` are only the defaults
  Tailwind needs to generate its utilities; the `.dark` block overrides them
  and is what visitors see on screen. Do not design against the light values or treat
  them as a second screen theme.
- **The one light rendering is print.** The `@media print` block in `global.css` re-declares
  the tokens with the same light values and prints code blocks in the light half of the
  Shiki theme. Keep the two sets of light values in step, check the print view when a
  change touches tokens, and do not remove the light Shiki theme.
- **Review the 404 page locally at `/error/404/`** (and the 500 page at `/error/500/`). The
  local production server (`node dist/server/entry.mjs`) answers an unknown URL with a 404
  status and a bare redirect stub, not the designed page; in production the web server
  maps failing URLs onto the prerendered `/error/` pages. The stub is expected, not a bug.

## Site search

Search is [Pagefind](https://pagefind.app/), a static index built after the Astro build:
the `postbuild` script (`scripts/build-search-index.mjs`) indexes `dist/client/` and writes
`dist/client/pagefind/v<version>/`, which the node server serves at `/pagefind/v<version>/`.
The Pagefind version is in the path (`src/lib/pagefind-bundle.mjs`, baked into the dialog by
`astro.config.mjs`) because the bundle's file names never change and the web server caches
script and style files for a year: a returning visitor must never pair a cached old
`pagefind.js` with a new index. Never reference a bare `/pagefind/` URL. Which pages are
indexed is decided in the HTML: `BaseLayout.astro` puts `data-pagefind-body` on `<main>`
for every page that is not noindex and not a utility route (`/api/`, `/go/`, `/buy/`,
`/error/`, 404, 500); pages without the attribute, including the static `/1.0/` archive,
are skipped, and `data-pagefind-ignore` keeps navigation chrome out of the excerpts.
`SearchDialog.astro` (rendered by the layout) is a native `<dialog>` opened by the header
buttons, the docs hub search box, `/` and Ctrl/Cmd+K; it lazy-loads Pagefind's UI from
the versioned bundle directory on first open, so a page that never opens search loads no
search code.
`astro dev` has no build output and therefore no index: the dialog says so instead of
searching, and the Playwright suite asserts that message. Pagefind runs WebAssembly, so a
Content-Security-Policy on the serving host must allow `'wasm-unsafe-eval'` in `script-src`;
the dialog probes for that before loading and reports a blocked policy visibly. The docs
sitemap `lastmod` comes from the same git-derived date as the page (`src/lib/git-date.ts`).
The static pages and the generated route families date from git too
(`src/lib/page-lastmod.mjs`); each example page dates from its own catalog entry and capture.
Git dates need full history: a shallow clone answers none, so its build falls back to the
frontmatter dates and leaves the other sitemap entries undated. Check a date change in a
full-history clone (`git clone --filter=blob:none`).

Result order is tuned in two places. At build time the layout gives `<main>` a
`data-pagefind-weight` and, for the pages people search for by name, a short result title
(`src/lib/search-index.ts`, from the path alone; no page file carries search attributes). At
search time the dialog passes Pagefind `ranking` options. Neither is covered by the test
suites, because the dev server has no index: after changing either, build, serve the build and
run `node scripts/search-probes.mjs http://127.0.0.1:<port>`, which prints where each probe
query's page lands. The dialog mirrors Pagefind's result count into a visually hidden
`role="status"` element and shows a fixed row of links when a search finds nothing; the 404
page opens the dialog with the last segment of the missing path as the query. The index
step fails the build when a Pagefind output file is missing or empty
(`scripts/lib/search-index-output.mjs`); rebuild if it does.

## Directory map

```
src/pages/                   File-based routes: one .astro (or .ts endpoint) per route.
                             The live route set is `git ls-files 'website/src/pages/*'`;
                             do not keep a hand-written site map anywhere.
src/content/docs/            Product documentation (.md, .mdx) -> /docs/<slug>/
src/content/blog/            Blog posts (.md) -> /blog/<slug>/; frozen once published
src/content/how-it-works/    Architecture explainers -> /how-it-works/<slug>/
src/content/releases-2.1/    release.yaml: the version manifest of the current line
src/content/releases-2.0/    Manifests of the predecessor lines (frozen; read-only
src/content/releases-1.1/    data for the pages that still describe them)
src/content.config.ts        Collection schemas: the frontmatter contracts below
src/components/              UI components. src/components/release/ holds the
                             release-aware components; its README.md is the manual.
                             src/components/home/ holds the home page, one file per
                             section; src/pages/index.astro keeps the metadata, the
                             JSON-LD and the shared style block. Those files are
                             hand-formatted (.prettierignore) and their copy is
                             scanned by test/content-accuracy.test.ts like a page.
src/data/                    product-facts.mjs (single-source facts; re-exports the
                             repo-root shared/product-facts.mjs), agent-files.mjs
                             (the prose and page list of the generated agent
                             files), filters.ts (a typed view of
                             reference/filters.json), reference/ (generated JSON,
                             overlays and notes; see below), examples.ts +
                             examples-data.json, demo-metrics.json,
                             fastspring-pricing.json, JSON-LD builders
src/layouts/BaseLayout.astro The one layout: the page shell, the critical-CSS
                             dark-mode overrides, the global stylesheet import
src/layouts/parts/           The layout's pieces: HeadMeta (meta tags, JSON-LD),
                             SiteHeader (nav), SiteFooter (links, the provenance
                             notice). Menu and footer links: src/data/nav.ts
src/lib/release.ts           getRelease(line) and artifactUrl(): typed manifest access
src/lib/docs-markdown.mjs    The docs collection as Markdown: the render behind
                             /docs/<slug>.md and public/llms-full.txt
src/assets/fonts/            The web fonts (woff2): imported by global.css and the
                             HeadMeta preloads, so the build serves them from hashed
                             /_astro/ URLs; the font licenses stay in public/fonts/
public/                      Served as-is: images, og-cards/, charts/ (the
                             Helm repository: packaged charts + index.yaml), robots.txt,
                             llms.txt, llms-full.txt, .well-known/ai-plugin.json
scripts/                     Build-time generators: fetch-pricing.js, generate-llms.mjs,
                             generate-title-cards.mjs, generate-references.mjs +
                             render-references.mjs (lib/reference-parsers.mjs);
                             the ai-plugin.json template in scripts/llms-templates/
test/                        vitest: unit tests and the gates listed below (CI runs these)
tests/                       Playwright specs (the browser suite)
```

## Content model

**Docs** (`src/content/docs/*.md|mdx`): `title`, `description`, `order` (position
within its group), `group` (sidebar section), `lastUpdated` (drives sitemap lastmod
and `dateModified`), optional `datePublished`, optional `faq` (q/a pairs rendered as
FAQPage JSON-LD; every pair must also appear verbatim in the page body).

**Blog** (`src/content/blog/*.md`): `title`, `description`, `date`, `author`, `tags`,
`draft`, optional `lastUpdated`, `coverImage`, `product`, `pinned`, `howTo`, `faq`.
A published post is frozen content: it keeps the version strings and URLs of its day,
and it is exempt from the version guards for that reason.

**How it works** (`src/content/how-it-works/*.md`): `title`, `description`, `order`,
optional `summary`, `lastUpdated`, `datePublished`, `faq`.

**Versions.** `src/content/releases-2.1/release.yaml` is the single version source for
the current line: `release.semver`, `release.tag`, `released_on`, the artifact channels
and the compatibility matrix. Rule: **never hand-pin a version or a `/releases/vX.Y.Z/`
URL in non-frozen content.** Pages call `getRelease('2.1')` from `src/lib/release.ts`;
MDX docs use the components in `src/components/release/` (`<Version />`,
`<CompatMatrix />`, `<ImageTag />`, ...). Plain `.md` cannot render components: a docs
page that needs version content becomes `.mdx`. The one tolerated literal is a
`ghcr.io/we-amp/pagespeed-*:X.Y.Z` image tag inside a `.md` install snippet, and CI
checks that it equals the manifest semver. Both rules are enforced by the `Website`
workflow (`.github/workflows/website.yml`), which also fails when the manifest lags
the latest stable `v2.*` tag of the public mod_pagespeed repository by more than a
few hours.

**Unreleased behaviour.** Docs on `main` can describe behaviour that is merged but
not released yet; a reader on the released version who copies it gets an error. Wrap
that text in a next-release marker, with `after` set to the last released version
when you write it:

```md
:::next-release{after="2.8.2"}
The new paragraph, list or table, unchanged.
:::
```

It renders as a callout titled "From the next release" (`[Label]` overrides the title,
as on the other callouts) and as a `> **From the next release**` blockquote in the
Markdown copies. Write the marker at the top level, starting at the first column with
three colons: the Markdown copies convert only those (the check also finds nested ones).
Wrap whole blocks only, headings included (a section that is new goes in the marker with its heading;
it keeps its anchor but stays out of "On this page", see `src/lib/rehype-next-release-headings.mjs`): a new sentence inside an existing paragraph
moves into its own marker block after it, a changed table row goes back to its
released text with the new row in a one-row table inside the marker, and text that
replaced released text keeps the released text outside the marker. Frontmatter (a
FAQ answer) cannot hold a marker, so it keeps the released wording. For a generated
reference entry, add it to `src/data/reference/next-release.json` (region, entry name,
`after`) and run `node scripts/render-references.mjs`; the renderer wraps the entry's
block, heading included, and leaves it out of the region's index links and counts.
Module directives cannot be marked (the directive finder needs that region flat), so the
renderer refuses them; describe an unreleased module directive in a docs-page marker. `test/next-release-markers.test.ts` fails on a marker without `after` and on
every marker whose `after` is older than the released version, the newer of
`release.semver` in the 2.1 manifest and `MODULE_PACKAGE_VERSION` in
`src/data/product-facts.mjs`. So it fires on the release's own site update, which
removes the markers (keep the content, drop the `:::` lines and the JSON entries).

## Gates in `test/`

`npx vitest run` runs them all; the two build-output gates skip themselves when
`dist/` is absent and run for real after `npm run build`.

- `content-accuracy.test.ts` — denylist of known-wrong product claims (phantom
  versions, wrong platform or edition claims) over content, pages, data, templates and
  the generated agent files; the canary fixtures prove every rule still fires.
- `anchor-gate.test.ts` — after a build: every deep-link anchor listed in
  `test/fixtures/legacy-doc-anchors.txt` resolves in `dist/client/docs/`.
- `anchor-source-gate.test.ts` — the same anchor contract computed from the markdown
  source, so it needs no build and always runs.
- `release-notes-ids-gate.test.ts` — after a build: every id in
  `test/fixtures/release-notes-ids.txt` exists on the built `/docs/release-notes/` page.
- `sync/airead-gate-sync.test.ts` — the gate predicates inlined into
  `/ai-readability/` still match their golden contract.
- `sync/console-facts-sync.test.ts` — the repo-root `shared/product-facts.mjs` (also
  consumed by the management console) and `src/data/product-facts.mjs` stay in lockstep.
- `reference/source-parity.test.ts` — `worker-flags.json` and
  `thin-module-directives.json` equal a fresh parse of `src/worker/main.cc` and
  `src/nginx/ngx_pagespeed_module.cc`; the module data names the pinned tag.
- `reference/filters-pages.test.ts` — every filter in `filters.json` is a row of
  `/docs/filters/` and has a section on exactly one group page.
- `reference/configuration-pages.test.ts` — every directive and flag has its block
  on the two configuration pages, the generated regions equal a fresh render, and
  every fragment link on them resolves.
- `sync/llms-generated.test.ts` — the committed `public/llms*.txt` and
  `ai-plugin.json` are byte-equal to a fresh render; every docs page is in the
  index and, in full, in `llms-full.txt`; the Distribution section names every
  channel and package; no retired framing outside the release-notes entries;
  `/docs/<slug>.md` serves the same body.
- `sync/source-publication-sync.test.ts` — license and publication facts agree across
  `product-facts.mjs`, `/api/product.json` and `/license/`, and retired commercial
  wording (license keys, trials, per-site pricing) stays out.
- `sync/terms-version-sync.test.ts` — the terms-of-service version stamp is identical
  on `/terms/` and the `/pricing/` quote form.
- `next-release-markers.test.ts` — every `:::next-release` marker and
  `reference/next-release.json` entry names the released version in `after`; it
  fails once a newer version is released (see "Unreleased behaviour").

## Generated references

`src/data/reference/{module-directives,filters}.json` are generated from the public
mod_pagespeed tree at the `v2.*` tag in `src/data/reference/source-pin.json`, and
`worker-flags.json` / `thin-module-directives.json` from this tree (`scripts/generate-references.mjs`);
`scripts/render-references.mjs` renders them into the marked regions of
`configuration.md` and `worker-configuration.md`. The `website-reference-drift` CI job
re-clones the pin and fails on any diff. At release time bump `source-pin.json` to the
new tag, run `MOD_PAGESPEED_SRC=<clone> npm run gen:references` and commit the JSON and
the two pages. Prose the source does not carry lives in `src/data/reference/notes/*.md`
and the two `*-overlay.json` files; an entry that is not released yet is listed in
`next-release.json` (see "Unreleased behaviour"); never hand-edit the JSON or a generated region.

The Markdown of the directive reference stays flat (an `h3` per area, an `h4` per
directive). `src/lib/rehype-directive-blocks.mjs` marks it when the page is rendered:
each directive heading and each area heading gets an attribute, and the "Find a directive"
field goes above the first area. It adds no wrapper per directive or per area (hundreds of
wrappers cost layout time on phones); the script hides a directive's heading and the
elements after it up to the next heading; `src/components/DirectiveFinder.astro` carries the filter script and the
styles, and `src/lib/directive-match.ts` the rule that turns typed text into a name to
look for. Only the HTML page changes:
the Markdown source, `/docs/configuration.md` and `llms-full.txt` do not. Astro caches
rendered content, so after editing the plugin delete `.astro/data-store.json` and
`node_modules/.astro/data-store.json` before the next local build or dev run (CI builds
start clean). `test/reference/directive-blocks.test.ts`, `test/directive-match.test.ts` and
`tests/directive-finder.spec.ts` cover the markup, the matching rule and the finder.

## The agent files

`public/llms.txt` and `public/llms-full.txt` are generated by `scripts/generate-llms.mjs` from the docs collection (every page, grouped as the sidebar groups it), the release manifests and `src/data/product-facts.mjs`, with the summary, the about paragraphs and the non-docs page list in `src/data/agent-files.mjs`; the full file carries the Markdown of every page, rendered by `src/lib/docs-markdown.mjs`, which is also what `/docs/<slug>.md` (`src/pages/docs/[slug].md.ts`) serves.
A docs change regenerates them at prebuild; run `npm run gen:llms` and commit the result (the byte-equivalence gate in `test/sync/llms-generated.test.ts` rejects a stale or hand-edited file). `public/.well-known/ai-plugin.json` still renders from `scripts/llms-templates/ai-plugin.json.tmpl` and `LLMS_TOKENS`.
A new MDX component used in a docs page needs its text form in `docs-markdown.mjs` (the generator fails on an unknown one); prose about the product that no docs page carries goes in `agent-files.mjs`, never in a generated file.

## Copy rules

These bind every customer-facing string: pages, docs, blog, templates, metadata.

- The product is **mod_pagespeed** (lowercase, underscore), with no line number; versions
  appear only as release facts ("since 2.5.0"). "ModPageSpeed 2.0" and
  "mod_pagespeed 1.15" name the predecessor lines and appear only as predecessors;
  1.15 upgrades to the current release in place, so never frame them as two products to choose between.
- **"worker", never "daemon".** Also: optimize (not supercharge), self-hosted (not
  on-prem), variant (not copy), original (not unoptimized), serves (not delivers).
- Sentence-case headings. Short sentences, active voice, no filler; name trade-offs.
- No internal identifiers: no decision-record numbers, machine names, private
  repository names or work-item ids. `tools/ci/check-public-hygiene.sh` (run from the
  repository root) is the gate and CI fails on a hit.
- Provenance: mod_pagespeed is an open-source project originally developed at Google;
  the current mod_pagespeed is developed by We-Amp B.V. and is not affiliated with or endorsed
  by Google. Say nothing beyond that about the relationship, and never disparage the
  original authors' work.
- Support tiers, license and publication claims come from `src/data/product-facts.mjs`
  (`SUPPORT_TIERS`, `PRICING_LINE`, `COMMERCIAL_EMAIL`, `LICENSE_CLAUSE`,
  `SOURCE_PUBLICATION`), never from a literal. Support prices (euro, per year, excl. VAT),
  first-response targets and the checkout links live in `SUPPORT_TIERS`; the hosting-partner
  rate stays unpublished. No "free trial" and no license keys anywhere on the site. Commercial mail goes to `COMMERCIAL_EMAIL`;
  `security@modpagespeed.com` is for vulnerability reports only.
- Dark mode: critical CSS is computed per page and carries the `dark:` variant rules, but it is
  not guaranteed to carry the `.dark` token rule, so the `html.dark` token block in the
  `<style is:inline>` of `BaseLayout.astro` stays (kept in sync with `global.css` by a test).
  Do not add per-element `!important` dark overrides there.

## CI and deploys

The `Website` workflow runs on every push and pull request that touches `website/**`:
the two content guards, `npm ci`, `npx vitest run`, a production build with
`PRICING_ALLOW_STALE=1`, and, in a second job, the Playwright suite against the dev
server. A manual workflow (`refresh-examples-gallery.yml`) re-measures the examples
gallery against the live demo and files an issue on drift. Two tools outside this
directory write into it: `tools/examples-gallery/generate.mjs` (writes
`src/data/examples-data.json`) and `deploy/helm/package-chart.sh` (writes
`public/charts/`). Deploys are the operator's step, from this tree, never from CI.
