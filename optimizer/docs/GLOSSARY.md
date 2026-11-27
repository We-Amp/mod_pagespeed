# Glossary

Load-bearing terms used across the mod_pagespeed code and docs. Each definition
is one line plus a pointer to its source-of-truth header — read the header before
changing the behaviour, not this file.

| Term | Definition | Source of truth |
|------|------------|-----------------|
| **PSOL** | PageSpeed Optimization Libraries — the underlying mod_pagespeed C++ components (HTML parser, image/CSS/JS optimizers) curated into `lib/`; 2.0 uses them directly and skips RewriteDriver. | `lib/` (`lib/html/`, `lib/image/`, `lib/css/`, `lib/js/`); `src/worker/worker.h` ("PSOL Factory Worker") |
| **Capability mask** | 32-bit value describing a request's served form (image format, viewport class, pixel density, save-data, transfer-encoding); drives variant selection. | `lib/classify/capability_mask.h` |
| **AlternateId** | Opaque `uint8_t` that is the low byte of `CapabilityMask::Encode()`, used as the Cyclone `AlternateId` at the cache boundary. Direct cast, no offset. | `lib/classify/alternate_id.h` |
| **SentinelId** | An `AlternateId` reserved for non-content slots (Original, Early Hints, Warmup, content hash, agent-markdown, llms.txt, …); all have viewport bits = 3, which `FromHeaders()` never produces. | `lib/classify/alternate_id.h` (`enum class SentinelId`) |
| **kIdentity** | The no-transfer-encoding value (`TransferEncoding::kIdentity = 0`). The worker strips encoding bits to `kIdentity` before writing the uncompressed variant (→ `0x08` identity AlternateId). | `lib/classify/capability_mask.h` (`enum class TransferEncoding`) |
| **Default alternate (0x08 vs 0x00)** | `CapabilityMask()` default = Desktop/Identity = **0x08** (what nginx writes for original content); `CapabilityMask::Decode(0)` = Mobile/Identity = **0x00**. The two are distinct — do not conflate. | `lib/classify/capability_mask.h`; `lib/classify/alternate_id.h` (static_asserts on 0x08/0x00) |
| **alternate == variant** | "Alternate" is Cyclone's term and "variant" is the PageSpeed term for the same thing: one cached form of a URL keyed by an AlternateId. Used interchangeably in docs. | `lib/classify/capability_mask.h:21` ("Variants stored as Cyclone alternates") |
| **`net_instaweb::`** | Namespace for legacy ported HTML-parser code (HtmlParse, HtmlElement, HtmlNode, …). | `lib/html/*.h`; CLAUDE.md → Namespaces |
| **`pagespeed::`** | Namespace for new mod_pagespeed code; legacy image processing lives in the nested `pagespeed::image_compression::`. | `lib/classify/*.h`; CLAUDE.md → Namespaces |
| **Cyclone stripe** | A partition of the on-disk cache volume; Cyclone derives the stripe count from the volume size, so nginx and the worker must agree on size or the same key resolves to different stripes (cross-process reads/writes go invisible). | `docs/multi-process-cache-stripes.md`; `src/nginx/ngx_pagespeed_module.cc` (`MakeNginxCacheConfig`) |
| **volume_size** | Cache volume byte size in `PageSpeedCacheConfig`. The worker sets it via `--cache-size`; **nginx must set `volume_size = 0`** to auto-detect from the existing file (matching size keeps the stripe layout consistent). | `lib/cache/cache.h` (`PageSpeedCacheConfig::volume_size`); CLAUDE.md → Cross-Process Cache Sharing |
| **RewriteDriver** | The mod_pagespeed filter orchestrator (2000+ LOC, 60+ filters) that mod_pagespeed 2 deliberately does **not** port — 2.0 calls the underlying PSOL components directly. | CLAUDE.md → Project Overview (key design decision) |
