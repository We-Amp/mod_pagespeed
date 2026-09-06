# Documentation

Developer- and operator-facing documentation for mod_pagespeed (the C++ Apache /
nginx / Envoy / IIS optimization engine). For an architecture overview and the
build/test workflow, start at the root [`CLAUDE.md`](../CLAUDE.md). Customer-facing
1.1 docs are not here — they live in the `pagespeed-optimizer` repo at
`website/src/content/docs-1.1/` (published to modpagespeed.com/1.1/docs/).

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
| [test-catalog.md](test-catalog.md) | Per-platform test pass/skip catalog. |
| [envoy-limitations.md](envoy-limitations.md) | Known Envoy limitations. |
| [iis-limitations.md](iis-limitations.md) | Known IIS limitations. |
| [operations-monitoring.md](operations-monitoring.md) | Operations and monitoring. |
| [autocreate-directive-snippets.md](autocreate-directive-snippets.md) | AutoCreateCachePath / AutoCreateLogDir directive snippets. |

## Plans and historical

These are point-in-time planning and release documents — read them as history,
not as current spec.

- [plans/](plans/) — historical planning documents (Apache/APR, Envoy, IIS, nginx, WASM, zero-copy cache).
- [PACKAGE_REPO_PLAN.md](PACKAGE_REPO_PLAN.md), [release-build-parallelization.md](release-build-parallelization.md), [release-plan-1.1.0-beta.1.md](release-plan-1.1.0-beta.1.md), [WINDOWS_SHARED_MEM_ALIGNMENT_PLAN.md](WINDOWS_SHARED_MEM_ALIGNMENT_PLAN.md) — release and feature plans.
- [ROADMAP.md](ROADMAP.md), [DECISIONS.md](DECISIONS.md), [BUSINESS_MODEL.md](BUSINESS_MODEL.md), [WASM_ARCHITECTURE.md](WASM_ARCHITECTURE.md), [WASM_HTML_PARSER.md](WASM_HTML_PARSER.md) — **HISTORICAL.** An aspirational "PageSpeed WASM" exploration that was never implemented in this repo. The paths they reference (`pagespeed/wasm/`, `pagespeed/service/`, `integrations/wordpress/`) do not exist. Each carries a banner at the top.
