# Browser Sanity Test Findings

**Date**: 2026-02-22
**Stack**: workbench-demo2 Docker stack (localhost)

## Test Results Summary

| Suite | Tests | Passed | Skipped | Failed |
|-------|-------|--------|---------|--------|
| test_sanity.py (HTTP-level) | 39 | 39 | 0 | 0 |
| test_browser_sanity.py (Playwright) | 46 | 45 | 1 | 0 |
| **Total** | **85** | **84** | **1** | **0** |

## Skipped Tests

### test_savedata_variants_lower_quality (SKIPPED)
- **Reason**: No matching normal/save-data WebP desktop pairs with SSIMULACRA2 scores
- **Root cause**: Cache entries for `localhost` hostname don't have SSIMULACRA2 scores yet (worker still processing inflight items). Older entries cached through a hosted instance do have scores.
- **Severity**: Low - not a bug, just timing. The test will pass once the worker finishes processing all variants.

## Observations

### Demo Sites
- All 3 demo sites (news, ecommerce, portfolio) load cleanly with no JS errors or 404s
- All main website pages (/, /docs/, /pricing/, /features/) render correctly
- Product images on ecommerce have proper width/height attributes (CLS prevention)
- No broken images detected across all demo pages
- JavaScript interactivity works (cart add-to-cart functionality confirmed)

### Image Optimization
- Chromium receives modern format images (WebP/AVIF) as expected
- Format negotiation works correctly:
  - `Accept: image/webp` -> WebP response (confirmed via HTTP and Playwright)
  - `Accept: image/avif` -> AVIF response
  - `Accept: */*` -> Original JPEG
- Image dimensions preserved in HTML attributes

### Console UI
- Dashboard loads and renders stat cards with non-zero values
- Health indicator shows status
- Breakdown tables render with Type, Format, SSIMULACRA2, etc.
- No JavaScript errors on dashboard
- URLs page lists cached entries with variant counts
- URL detail view shows alternate metadata (format, viewport, encoding, etc.)
- URL search/filter is functional
- Logs page shows filter controls (Worker, Debug, Info, Warning, Error)
- Config page shows quality settings and export/import controls

### Cache Metadata
- 14 URLs cached (mix of images, HTML, CSS, JS)
- Images have up to 36 alternates
- Multiple formats present (webp, avif, original)
- Multiple viewports present (mobile, tablet, desktop)
- Content classification working (photo, noisy detected)
- Origin content-type preserved (image/jpeg)
- Identity encoding present on all images

### Worker Stats
- Notifications received: 90+
- Images processed: 100+
- Variants written: 126+
- SSIMULACRA2 checks: 81
- Learned quality predictions: 43
- Content analysis active (photo: 3, noisy: 3, denoised: 20)

## Issues Found

No blocking issues found. The stack is healthy and all optimizations are functioning correctly.

### Minor Observations
1. **SSIMULACRA2 scores missing on newer cache entries**: When accessing via `localhost` (vs a hosted instance), newly cached entries may not have SSIMULACRA2 scores until the worker finishes processing all inflight items. This is expected behavior - scores are written when the quality verification pass completes.

2. **Worker ready=false during initial processing**: The worker health endpoint reports `ready: false` while there are inflight items being processed. This doesn't affect serving - cached variants are still served correctly.

3. **SVG vectorization**: All 6 SVG candidates were evaluated but rejected (candidates_rejected: 6). This is expected for the demo images (photos don't vectorize well).
