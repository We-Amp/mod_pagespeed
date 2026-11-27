# Release-aware components

These components render release-version content from the manifest at
`src/content/releases-{1.1,2.0,2.1}/release.yaml` (synced from the release manifests
maintained outside this repository).

**Authoring rule:** every version-coupled string on the website must flow
through `getRelease()` (typed access) or one of these components. The
CI step "Guard against version-pinned URLs in non-frozen content" in
`.github/workflows/ci.yml` (Stage 7) rejects any `releases/vX.Y.Z/`
literal in `src/content/`, `src/pages/`, `src/components/`, or
`src/layouts/` (blog content is exempt — content).

## Available components

| Component               | Use for                                                                     |
| ----------------------- | --------------------------------------------------------------------------- |
| `<Version />`           | A single version label (`1.1.0`, `v1.1.0`, `v1.1.0+r9`) inline in prose.    |
| `<CompatMatrix />`      | The "Supported on …" block (nginx + Apache + IIS, per surface).             |
| `<NugetCmd />`          | `dotnet add package WeAmp.PageSpeed.AspNetCore [--prerelease]`.             |
| `<XPageSpeedExample />` | Response-header example: `X-Page-Speed:`/`X-Mod-Pagespeed:`/`X-PageSpeed:`. |
| `<ImageTag />`          | Docker / Helm image tag for 2.0 (`2.0.7` etc.).                             |

`<XPageSpeedExample />` takes `surface="middleware"` for the ASP.NET Core
middleware's `X-PageSpeed: MISS` form on any line. No form shows a version: the
modules send `on` by default.

## Usage in MDX

```mdx
---
title: 'My doc page'
description: '…'
---

import XPageSpeedExample from '../../components/release/XPageSpeedExample.astro';

Verify the response header:

<pre>
  <code>
    <XPageSpeedExample surface="nginx"  />
  </code>
</pre>
```

Plain `.md` cannot import or render Astro components — only `.mdx`. If a
new docs page needs version content, use the `.mdx` extension.

## Usage in `.astro` pages

`.astro` pages can import the components or call `getRelease()` directly:

```astro
---
import { getRelease } from '~/lib/release';
const rel20 = await getRelease('2.0');
const cmd = `dotnet add package ${rel20.artifacts.nuget.package}${
  rel20.display.nuget_install_flags ? ` ${rel20.display.nuget_install_flags}` : ''
}`;
---

<code>{cmd}</code>
```

For the canonical download-page pattern, see `src/pages/download.astro`.

## Why this is shaped this way

See `decisions/062-deterministic-version-bump.md`:

- **§1** — Manifest is the single source of truth.
- **§2** — Naming convention (`v{semver}` in URLs, `+rN` in tags only).
- **§4** — Render mechanism (build-time, components in markdown via MDX).
- **§9** — Frozen content (blog posts are byte-untouched on reships).
