# CLAUDE.md

> **Legacy -- do not invest significant effort.** This directory contains the
> original mod_pagespeed documentation site inherited from Apache. It has been
> superseded by the Astro-based site at `pagespeed-optimizer/website/` which is
> deployed to modpagespeed.com. Last substantive content edit: 2023 (ASF
> incubator exit). Kept for historical reference and redirect mapping only.

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
