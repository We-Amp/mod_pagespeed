# CLAUDE.md

> **ARCHIVE — DO NOT EDIT. This is enforced; see below.** This directory is the
> mod_pagespeed 1.0 documentation site inherited from Apache, published
> unchanged as the `/1.0/` archive on modpagespeed.com. It describes what that
> release did. It is therefore **not** updated when behaviour changes — an
> archive that tracks the current product is a record of nothing. Last
> substantive content edit: 2023 (ASF incubator exit).

## Where documentation actually lives

Both live in the **pagespeed-optimizer** repository, not here:

| Product | Source | Published at |
|---|---|---|
| 2.0 | `website/src/content/docs/` | modpagespeed.com/docs/ |
| 1.15 | `website/src/content/docs-1.1/` | modpagespeed.com/1.1/docs/ |

## Why this is enforced rather than asked

The paragraph above has said "legacy — do not invest effort" since 2023, and it
did not work. The failure mode does not involve reading this file: someone
changes product behaviour, greps the tree for the prose describing the old
behaviour, finds it in `doc/system.html`, and edits it there. The edit looks
right in review — the sentence really did describe the behaviour, and it really
was out of date — but nothing a user reads has changed, the real documentation
still says the old thing, and the archive has quietly stopped being accurate
about 1.0.

That happened in the change that removed the two-pool thread split.
So the notice now sits at the top of **every file here**, where a grep lands,
and `tools/ci/check_legacy_docs.sh` fails any pull request that touches
`html/`. To change the archive itself — fixing link rot, or retiring it — put
`Legacy-Docs: <reason>` in a commit message.

## Current State

Legacy static HTML documentation. No longer deployed as a standalone site.

## Structure

- `index.html` - Main landing page
- `doc/` - Documentation pages (filters, configuration, downloads, release notes)
- `doc/doc.css` - Shared stylesheet
- `doc/_header.html`, `doc/_footer.html`, `doc/_navline.html` - Common includes
- Images: `*.png`, `*.svg`, `*.jpg` in root; `doc/images/` for doc assets

## Content

Static HTML documentation for mod_pagespeed (Apache) and ngx_pagespeed (Nginx). Includes:
- Filter documentation (`doc/filter-*.html`)
- Configuration guides (`doc/configuration.html`, `doc/config_filters.html`)
- System administration (`doc/system.html`, `doc/admin.html`)
- Release notes (`doc/release_notes.html`)
- Security advisories (`doc/CVE-*.html`, `doc/announce-*.html`)

## Style

- Uses `doc/doc.css` for consistent styling
- Links: We-Amp GitHub (`github.com/we-amp/mod_pagespeed`, `github.com/we-amp/ngx_pagespeed`)
- Apache License 2.0 header in HTML comments
