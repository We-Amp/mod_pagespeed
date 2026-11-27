---
title: 'mod_pagespeed 2.1: one product again'
description: 'The mod_pagespeed lineage and the ModPageSpeed 2.0 engine converge into one open-source product: the module you know plus an optimizer worker. Apache-2.0.'
date: 2026-09-17
author: 'Otto van der Schaaf'
tags: ['announcement', 'release']
draft: false
lastUpdated: 2026-10-22
---

> Editor's note (22 October 2026): this post is kept as written for the 2.1.0 launch. The product is now called mod_pagespeed, with no version in its name; "2.1" below refers to the 2.1.0 release. For the current release, start with [getting started](/docs/getting-started/).

For the past year we shipped two products: mod_pagespeed 1.15, the maintained
continuation of the classic module, and ModPageSpeed 2.0, a ground-up rewrite
of the optimization engine. Today they are one product again.

**mod_pagespeed 2.1** is the converged line: the native web-server module for
Apache and nginx, with the 2.0 engine running alongside it as a separate
optimizer worker. It is open source under the Apache-2.0 license — the same
license the original codebase carried — and it is a drop-in for your existing
configuration: 1.14 and 1.15 directives and filter names carry over.

## What actually converged

The module stays. If you run mod_pagespeed today, 2.1 keeps the
`pagespeed`-directive surface and the 1.14/1.15 filter names on its Core
surface — swap the packages and your `pagespeed.conf` carries over. On the
supported Linux deb/rpm channels the module and the `pagespeed-optimizer`
worker install from the same signed repository at packages.modpagespeed.com.

The optimizer moved out. The heavy work — image transcoding and the rest of
the rewrite pipeline — now runs in the worker, off the request path, writing
optimized variants back to the Cyclone cache that the Apache module, and
nginx in the reverse-proxy deployment, serve from. That
worker _is_ the ModPageSpeed 2.0 engine: the image pipeline with ML-predicted
quality and SSIMULACRA2 verification, variant-aware caching, and the
optimizations the rewrite was built for, now behind the module everyone
already runs.

## Why the architecture is worth having

A web optimizer decodes untrusted bytes: every image it recompresses and
every page it parses arrived from somewhere else. In mod_pagespeed 2.1 that
optimization work runs in a dedicated `pagespeed-optimizer` process, under
its own unprivileged system user with an empty capability set, and with the
Apache module or the reverse-proxy deployment the module reaches its results
through the shared cache. Browser-based analysis runs in that same worker
process, with the Chrome sandbox required by default.

## Browser analysis and the agentic web

Browser-based analysis (critical-CSS validation, waterfall capture, visual
comparison) and the agent-readability tooling that makes your pages legible
to AI assistants as well as browsers run in the optimizer worker, and reach
you with the Apache module or the reverse-proxy deployment.

## Open source, Apache-2.0

mod_pagespeed 2.1 is open source under the Apache-2.0 license, like the
original codebase — and the relicensing is retroactive, covering the
predecessor lines as well. The source lives at
[github.com/We-Amp/mod_pagespeed](https://github.com/We-Amp/mod_pagespeed).
There are no editions and no usage registration, and the standard signed
packages are free. What's for sale is support: [plans with first-response targets and a
hoster partner program](/pricing/) from the people who build it; the software
and its signed packages are the same for everyone.

## What this means for you

- **Running mod_pagespeed 1.15?** The line continues — 2.1 is its next
  release, and the upgrade is drop-in. The IIS package ships from the 1.15
  packaging channel. The [upgrade guide](/docs/migrating-to-2-1/#upgrading-from-1-15) has the
  details.
- **Running ModPageSpeed 2.0?** The Docker/Helm line moves via the
  [migration guide](/docs/migrating-to-2-1/). The ASP.NET Core middleware
  continues unchanged.
- **New here?** [Install it](/docs/getting-started/) — two packages, your
  existing web server, free in development and in production.

---

mod_pagespeed is an open-source project originally developed at Google.
We-Amp helped build ngx_pagespeed, maintained mod_pagespeed, and drove the
project's Apache incubation throughout the Google era; mod_pagespeed 2.1 is
developed by We-Amp B.V. and is not affiliated with or endorsed by Google.
