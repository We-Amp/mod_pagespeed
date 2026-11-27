# Documentation Index

Canonical map of `docs/`. Cross-linked from the root `CLAUDE.md`. Status is taken
from each doc's own header where it states one, or bucketed from content into the
section that reflects its current-vs-historical state.

Status markers: **Active** = current reference, **Implemented** = built (historical
record of a shipped feature), **Pending/Draft** = proposed or not yet implemented,
**Historical** = superseded or done-and-archived.

## Active References
- [GLOSSARY.md](GLOSSARY.md) -- Load-bearing terms (PSOL, AlternateId/SentinelId, capability mask, …)
- [code-coverage.md](code-coverage.md) -- Code coverage reporting setup
- [multi-process-cache-stripes.md](multi-process-cache-stripes.md) -- How nginx + worker share one cache file (stripe/volume_size invariant)

## Implemented (shipped — historical record)
- [freshness.md](freshness.md) -- Cache freshness with conditional revalidation (Implemented)

## Pending / Draft
- [compression-flow-analysis.md](compression-flow-analysis.md) -- Compression flow analysis (Phases 1/2/4 complete, 3 pending)

## Historical / Archived
- [jpeg_xl_return.md](jpeg_xl_return.md) -- JPEG XL support plan (superseded by SVG auto-vectorization)
