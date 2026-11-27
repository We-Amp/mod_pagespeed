// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 — Cyclone StorageAlternateSelector implementation.
//
// Selects the best-fit cache alternate for a client request by scoring
// each alternate's stored metadata against the client's capability mask.
//
// Scoring weights (viewport=2000 > format=1000 > original_fallback=100
// > encoding=60 > density=40 > save-data=20) intentionally differ
// from the old FallbackMasks() probe order.  The ordering prefers
// serving an image at the client's size class first (a phone never gets
// the desktop-size image when a phone-size one exists), then the correct
// image format (e.g. AVIF to an AVIF-capable client), then correct
// transfer encoding (brotli/gzip/identity).
// SVG alternates receive a +1200 universal format bonus (higher than
// the +1000 exact match) since SVG is resolution-independent and
// universally supported.  Viewport and density bonuses are always
// awarded for SVG.  Save-Data gets +50 (SVG is inherently lightweight).
// Mismatched non-identity encoding is a hard disqualification (score 0)
// because the client cannot decode content in an unsupported encoding.

#ifndef PAGESPEED_LIB_CLASSIFY_PAGESPEED_SELECTOR_H_
#define PAGESPEED_LIB_CLASSIFY_PAGESPEED_SELECTOR_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "cyclone/alternate.hpp"

namespace pagespeed {

// Scores how well a stored alternate matches the client's request.
// Higher = better match.  Returns 0 if the alternate is a sentinel or
// its metadata is corrupt.
//
// Dimensions and weights:
//   Format match   : +1000 (exact match)
//   SVG format     : +1200 (universal bonus — SVG serves all devices)
//   Original format: +100  (fallback bonus when stored is Original
//                           and client requests a different format)
//   Viewport match : +2000 (exact match; SVG always awarded).  Larger
//                           than all other dimensions combined, so a
//                           variant at the client's size class beats
//                           one in its format at another size.
//   Density match  : +40   (exact match; SVG always awarded)
//   Save-Data match: +20   (exact match)
//   Save-Data SVG  : +50   (SVG is inherently lightweight)
//   Save-Data clash: -980  (stored is Save-Data, client did not ask for it;
//                           not for SVG; the total is floored at 1, so the
//                           variant stays selectable as a last resort)
//   Encoding match : +60   (exact match)
//   Encoding ident.: +5    (stored is identity, client wants compressed)
//   Encoding clash : 0     (stored is non-identity, doesn't match client)
int ScoreAlternate(uint32_t client_mask, uint32_t stored_mask);

// StorageAlternateSelector that picks the best alternate for a PageSpeed
// client capability mask.
//
// The client mask is serialized as 4-byte LE in the selection context's
// request_metadata field.
class PageSpeedSelector : public cyclone::StorageAlternateSelector {
 public:
  std::optional<size_t> select(
      std::span<const cyclone::AlternateInfo> alternates,
      const cyclone::AlternateSelectionContext& ctx) const override;
};

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CLASSIFY_PAGESPEED_SELECTOR_H_
