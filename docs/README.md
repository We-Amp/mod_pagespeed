# Documentation

Developer- and operator-facing documentation for mod_pagespeed (the C++ Apache /
nginx / Envoy / IIS optimization engine). For an architecture overview and the
build/test workflow, start at the root [`CLAUDE.md`](../CLAUDE.md). Customer-facing
docs are not here: they live in the `pagespeed-optimizer` repo at
`website/src/content/docs/` (published to modpagespeed.com/docs/).

## Orientation

| Document | Purpose |
|----------|---------|
| [GLOSSARY.md](GLOSSARY.md) | Load-bearing terms (PSOL, IPRO, instaweb, ProxyFetch, DomainLawyer, beacon, CLFUS). |
| [../CLAUDE.md](../CLAUDE.md) | Architecture, build system, two source trees, testing, IIS internals. |
| [../DEVELOPER.md](../DEVELOPER.md) | Contributor setup: Docker dev container, native Linux deps, single-test examples. |

## Install

| Document | Purpose |
|----------|---------|
| [install-apache.md](install-apache.md) | Installing mod_pagespeed for Apache. |
| [install-nginx.md](install-nginx.md) | Installing PageSpeed for nginx. |
| [install-envoy.md](install-envoy.md) | Installing PageSpeed for Envoy. |
| [install-iis.md](install-iis.md) | Installing PageSpeed for IIS. |
| [recommended-configuration.md](recommended-configuration.md) | Recommended baseline configuration. |

## Reference

| Document | Purpose |
|----------|---------|
| [filter-reference.md](filter-reference.md) | Filter reference. |
| [rule-packs.md](rule-packs.md) | Rule packs: operator-written head rules (canonical, title, description, hreflang, JSON-LD), report-first workflow, decision log. |
| [test-catalog.md](test-catalog.md) | Per-platform test pass/skip catalog. |
| [envoy-limitations.md](envoy-limitations.md) | Known Envoy limitations. |
| [iis-limitations.md](iis-limitations.md) | Known IIS limitations. |
| [operations-monitoring.md](operations-monitoring.md) | Operations and monitoring. |
| [autocreate-directive-snippets.md](autocreate-directive-snippets.md) | AutoCreateCachePath / AutoCreateLogDir directive snippets. |
