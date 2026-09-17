# Glossary

Load-bearing terms used across `CLAUDE.md` and the per-subsystem
`pagespeed/*/CLAUDE.md` files, sourced from the code and existing docs. One line
each; follow the pointer for detail. Filter behaviour and customer-facing docs
live on modpagespeed.com (see [Related Repositories](../CLAUDE.md#related-repositories)),
not here.

| Term | Meaning | Source of truth |
|------|---------|-----------------|
| **PSOL** | PageSpeed Optimization Library — the platform-independent rewriting engine. Its rewrite workers run on their own thread pool ("PSOL threads"), which is why port code must never touch the server's request thread from a PSOL thread. | `pagespeed/nginx/CLAUDE.md` (thread model); `CLAUDE.md` Nginx Thread Model |
| **instaweb** | The legacy internal project name. Survives as the `net_instaweb::` C++ namespace and the Apache module `mod_instaweb`. Do not introduce new uses of the namespace in new code. | `pagespeed/kernel/CLAUDE.md` Namespace; `pagespeed/apache/mod_instaweb.cc` |
| **IPRO** | In-Place Resource Optimization — optimizes CSS/JS/image resources at their original URLs (no URL rewrite), recording and serving optimized bytes on later requests. | `pagespeed/system/CLAUDE.md`; `CLAUDE.md` Envoy Filter |
| **ProxyFetch** | The shared HTML rewriting engine used by all deployment modes. It bundles the writes/flushes that arrive on a fetcher thread onto a `QueuedWorkerPool::Sequence` worker thread. `ProxyFetchFactory` creates and starts them. | `pagespeed/automatic/proxy_fetch.h` |
| **RewriteDriver** | Per-request optimization coordinator: drives the `HtmlParse` → `HtmlFilter` chain for one request. Not thread-safe. | `net/instaweb/rewriter/public/rewrite_driver.h`; `pagespeed/kernel/CLAUDE.md` |
| **RewriteDriverFactory** | Creates `RewriteDriver`s and holds server-wide state. Each port subclasses it (e.g. the live `IisRewriteDriverFactory` in `pagespeed/iis/`). | `net/instaweb/rewriter/public/rewrite_driver_factory.h`; `CLAUDE.md` IIS Platform Internals |
| **RewriteOptions** | The configuration object (enabled filters, limits, domain rules). Once initialized from config it is frozen; mutating it after freeze silently fails. | `net/instaweb/rewriter/public/rewrite_options.h`; `CLAUDE.md` Frozen RewriteOptions |
| **ProcessContext** | Once-per-process singleton holding the domain registry and HTML keywords. Must be constructed exactly once per process (in `DllMain` on IIS, `ap_hook_pre_config` on Apache, lazily on Envoy). | `CLAUDE.md` Key Abstractions / DllMain Loader Lock |
| **DomainLawyer** | Domain-mapping and authorization policy: which domains may be rewritten, sharding, CDN/cookieless mapping. `LoopbackRouteFetcher` routes hosts not named in the DomainLawyer back to our own IP. | `net/instaweb/rewriter/public/domain_lawyer.h`; `pagespeed/system/loopback_route_fetcher.h` |
| **beacon** | Client-side instrumentation: `AddInstrumentationFilter` injects JavaScript that posts page load timing back to a beacon URL on the server. | `net/instaweb/rewriter/public/add_instrumentation_filter.h`; `net/instaweb/rewriter/add_instrumentation.js` |
| **CLFUS** | Clock with Low Inter-reference Recency First Unset Second chance — the scan-resistant cache eviction algorithm used by the Cyclone disk cache. | `pagespeed/kernel/cache/cyclone_cache.h` |
