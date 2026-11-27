// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/classify/pagespeed_selector.h"

#include <cstring>

#include "lib/classify/alternate_id.h"

namespace pagespeed {

namespace {

// Bit field extraction helpers (mirror CapabilityMask layout).
constexpr int kFormatShift = 0;
constexpr int kViewportShift = 2;
constexpr int kDensityShift = 4;
constexpr int kSaveDataShift = 5;
constexpr int kEncodingShift = 6;

constexpr uint32_t kFormatMask = 0x03;
constexpr uint32_t kViewportMask = 0x03;
constexpr uint32_t kDensityMask = 0x01;
constexpr uint32_t kSaveDataMask = 0x01;
constexpr uint32_t kEncodingMask = 0x03;

// SVG format value (bits 0-1 = 11 = 3).
constexpr int kSvgFormat = 3;

// Score for the agent_optimize markdown sentinel when an entitled agent request
// asks for it — strictly greater than any capability score (max realistic ~1200),
// so an entitled agent request always prefers it.
constexpr int kAgentMarkdownScore = 100000;

// Penalty for a stored Save-Data variant when the client did not ask for
// Save-Data (see ScoreAlternate).
constexpr int kSaveDataMismatchPenalty = 980;

// The one selectable sentinel and the never-selectable one are different
// classes.  If they were ever given the same id, the skip in select() below
// would silently become unreachable and the durable original would inherit
// the markdown exception.
static_assert(static_cast<uint8_t>(SentinelId::kAgentMarkdown) !=
                  static_cast<uint8_t>(SentinelId::kOriginalContent),
              "the selectable sentinel and the durable original must stay "
              "distinct ids");

int ExtractFormat(uint32_t mask) {
  return static_cast<int>((mask >> kFormatShift) & kFormatMask);
}
int ExtractViewport(uint32_t mask) {
  return static_cast<int>((mask >> kViewportShift) & kViewportMask);
}
int ExtractDensity(uint32_t mask) {
  return static_cast<int>((mask >> kDensityShift) & kDensityMask);
}
int ExtractSaveData(uint32_t mask) {
  return static_cast<int>((mask >> kSaveDataShift) & kSaveDataMask);
}
int ExtractEncoding(uint32_t mask) {
  return static_cast<int>((mask >> kEncodingShift) & kEncodingMask);
}

}  // namespace

int ScoreAlternate(uint32_t client_mask, uint32_t stored_mask) {
  // Check for sentinel — stored mask's low byte has viewport=3.
  auto stored_byte = static_cast<uint8_t>(stored_mask & 0xFF);
  if (IsSentinel(stored_byte)) {
    return 0;
  }

  int score = 0;
  bool is_svg = (ExtractFormat(stored_mask) == kSvgFormat);

  // Format scoring:
  //   SVG stored: +1200 universal bonus (SVG serves all devices)
  //   Exact format match: +1000
  //   Original fallback: +100
  //   Any other mismatch: hard disqualify (total score 0).
  if (is_svg) {
    score += 1200;
  } else if (ExtractFormat(client_mask) == ExtractFormat(stored_mask)) {
    score += 1000;
  } else if (ExtractFormat(stored_mask) == 0) {
    score += 100;  // Original format fallback bonus
  } else {
    // Stored is a negotiated raster format (WebP/AVIF) that differs from the
    // format the client mask negotiated.  The format field is a negotiated
    // PREFERENCE, not a capability set (CapabilityMask::FromHeaders): a kWebP
    // client may in fact decode AVIF (its Accept may have carried image/* or
    // */*), and a kAvif client almost certainly decodes WebP — but the mask
    // cannot prove either.  Serving a format the request gave no evidence it
    // can decode hands the client undecodable bytes, so mirror the
    // transfer-encoding disqualify below.  The asymmetry justifies the bias:
    // a false disqualify costs a cache miss / re-optimization, a false
    // accept serves a broken image.  (Issue #1331.)
    //
    // Cross-reference: this is the rule that makes the ORIGINAL-format slot
    // the universally reachable one -- it is the only stored format every
    // client mask can select, via the fallback bonus above.  Two decisions
    // rest on that and should move with it: unconverted bytes are stored at
    // the original-format id rather than a converted one (#1374), and an
    // absent Accept classifies as */* (#1377), which is monotone here because
    // the wider mask keeps the fallback bonus and gains the exact matches.
    // The cost of this rule -- a client that also advertises WebP not being
    // offered a smaller WebP sibling when its negotiated format is AVIF -- is
    // tracked in #1376.
    return 0;
  }

  // Viewport: exact match = +2000, deliberately larger than the format
  // bonus.  The viewport is the image's size class: a variant at the
  // client's size outranks a variant in the client's negotiated format at
  // another size.  With +80 (below the +900 gap between an exact format
  // match and the original-format fallback) a phone whose Accept named WebP
  // but whose slot had no WebP (refused as not smaller than the phone-size
  // JPEG) was handed the full-size desktop WebP, many times the bytes of
  // the phone JPEG it could have had; and a desktop could be handed a
  // phone-size image.  2000 exceeds every other dimension combined
  // (1000 + 40 + 20 + 60 = 1120), so a size-class match always wins (a
  // Save-Data variant penalized for a client without Save-Data included:
  // see the penalty below), and the SVG tier (which always takes this bonus) stays above every raster.
  // SVG is resolution-independent — always award the viewport bonus.
  if (is_svg || ExtractViewport(client_mask) == ExtractViewport(stored_mask)) {
    score += 2000;
  }

  // Density: exact match = +40.
  // SVG is resolution-independent — always award the density bonus.
  if (is_svg || ExtractDensity(client_mask) == ExtractDensity(stored_mask)) {
    score += 40;
  }

  // Save-Data: SVG gets +50 bonus (inherently lightweight vector format),
  // otherwise exact match = +20.
  if (is_svg && ExtractSaveData(client_mask) == 1) {
    score += 50;
  } else if (ExtractSaveData(client_mask) == ExtractSaveData(stored_mask)) {
    score += 20;
  } else if (!is_svg && ExtractSaveData(stored_mask) == 1) {
    // A Save-Data variant offered to a client that did not ask for Save-Data
    // carries deliberately reduced quality.  The format bonus alone outranks
    // the +20 match above, so with the client's own normal slot empty a
    // normal client was handed the reduced-quality sibling of its format
    // instead of the normal variant of another format at the same size.
    // The penalty is 980 so that the orderings hold for every mask:
    //   * a penalized variant (at most 1000 + 2000 + 40 + 60 - 980 = 2120)
    //     stays below every normal variant at the client's viewport (at
    //     least 100 + 2000 + 20 + 5 = 2125);
    //   * a penalized variant at the client's viewport (at least
    //     100 + 2000 + 5 - 980 = 1125) stays above every normal variant at
    //     another viewport (at most 1000 + 40 + 20 + 60 = 1120).
    // Normal bytes for a Save-Data client stay allowed: that direction is
    // never penalized.  Text variants are scored the same way; they carry the
    // Save-Data bit only when written for it, and a fallback hit asks the
    // worker for the client's own slot, so they self-heal.  The floor below
    // keeps a penalized variant selectable as a last resort.
    score -= kSaveDataMismatchPenalty;
  }

  // Encoding: exact match +60, identity fallback +5, mismatched
  // non-identity = hard disqualify (total score 0).
  int client_enc = ExtractEncoding(client_mask);
  int stored_enc = ExtractEncoding(stored_mask);
  if (client_enc == stored_enc) {
    score += 60;
  } else if (stored_enc == 0) {
    // Identity stored — nginx can dynamically compress at serve time.
    score += 5;
  } else {
    // Stored is non-identity (gzip/brotli) but doesn't match client.
    // Can't serve compressed content the client can't decode.
    return 0;
  }

  return score > 0 ? score : 1;
}

std::optional<size_t> PageSpeedSelector::select(
    std::span<const cyclone::AlternateInfo> alternates,
    const cyclone::AlternateSelectionContext& ctx) const {
  if (alternates.empty()) {
    return std::nullopt;
  }

  // Extract client mask from request_metadata (first 4 bytes LE).
  uint32_t client_mask = 0;
  if (ctx.request_metadata.size() >= 4) {
    std::memcpy(&client_mask, ctx.request_metadata.data(), 4);
  }

  // Optional 5th byte: the agent_optimize "entitled agent request" flag.
  // Absent (legacy 4-byte metadata) => false. Only such a request may ever
  // select the kAgentMarkdown sentinel; every other request leaves it skipped.
  bool agent_request = ctx.request_metadata.size() >= 5 &&
                       static_cast<unsigned char>(ctx.request_metadata[4]) != 0;

  std::optional<size_t> best_idx;
  int best_score = 0;
  uint8_t best_mask = 0;

  for (size_t i = 0; i < alternates.size(); ++i) {
    const auto& alt = alternates[i];

    // Sentinel AlternateIds are not content variants and are skipped — EXCEPT the
    // agent-markdown sentinel, which an entitled agent request selects on purpose.
    auto alt_byte = static_cast<uint8_t>(alt.id);
    if (IsSentinel(alt_byte)) {
      // The durable original-content class is NEVER selectable, by any
      // request, entitled or not.  It is read by exact id and only by a
      // caller that asked for it by name; scoring must not be able to reach
      // it, because "the original" competing with the optimized variants is
      // precisely the outcome the class is stored to avoid.
      //
      // Written as an unconditional skip AHEAD of the one sentinel exception
      // below, rather than left to the fact that the exception names a
      // different id: that way widening the exception cannot reach this class
      // by accident, and the guarantee is a statement in the selection path
      // instead of a property of the code that happens to hold today.
      if (alt_byte == static_cast<uint8_t>(SentinelId::kOriginalContent)) {
        continue;
      }
      if (agent_request &&
          alt_byte == static_cast<uint8_t>(SentinelId::kAgentMarkdown) &&
          kAgentMarkdownScore > best_score) {
        best_score = kAgentMarkdownScore;
        best_idx = i;
      }
      continue;
    }

    // Derive the stored mask from the AlternateId.  All 8 capability
    // bits (format, viewport, density, save-data, encoding) fit in the
    // low byte, which IS the AlternateId.  This avoids depending on
    // Cyclone's set_header(), which has a known bug in multi-process
    // mode that corrupts the alternate chain.
    auto stored_mask = static_cast<uint32_t>(AlternateIdToMask(alt_byte));
    int score = ScoreAlternate(client_mask, stored_mask);
    // Tie-break (issue #1331): equal-scoring alternates must never resolve
    // by listing order.  Positive-score ties between DISTINCT stored masks
    // are real and routine: per-dimension bonuses are binary, so two
    // variants that match the client on the same dimensions but differ in a
    // mismatching one (e.g. a Tablet and a Desktop variant for a Mobile
    // client) tie, as do resolution-independent SVG entries that differ
    // only in viewport/density.  Rule: on an equal positive score, prefer
    // the stored mask with the higher format value, then the numerically
    // larger mask byte.  The format arm mirrors the system's own preference
    // order — CapabilityMask::FromHeaders negotiates AVIF > WebP > Original
    // (capability_mask.cc) and the enum values kOriginal=0 < kWebP=1 <
    // kAvif=2 < kSvg=3 encode exactly that, with worker-only universal SVG
    // on top.  (Cross-format positive ties cannot actually occur today —
    // the SVG tier scores >= 3245, viewport-matched exact formats are
    // 3005-3120 and Original fallbacks 2105-2220, and without a viewport
    // match exact formats are 1005-1120 and Original fallbacks 105-220;
    // a Save-Data variant offered to a client without Save-Data sits lower
    // still: viewport-matched exact 2025-2120, Original fallbacks 1125-1220,
    // exact 25-120 without a viewport match, and Original fallbacks there
    // floored at 1 (all one format, so the byte arm decides),
    // and every one of these totals differs between an exact match and a
    // fallback by the format bonus alone (900), which no combination of
    // the smaller dimensions (at most 120) can close; so the byte arm is
    // the live one; the format arm is
    // stated so a future scoring change cannot silently reintroduce
    // order-dependent selection.)  Byte-identical duplicates are
    // interchangeable, so keeping the first of those is not a listing-order
    // dependency in any observable sense.
    bool wins = score > best_score;
    if (!wins && best_idx.has_value() && score == best_score) {
      int cand_fmt = ExtractFormat(stored_mask);
      int best_fmt = ExtractFormat(best_mask);
      wins =
          cand_fmt > best_fmt || (cand_fmt == best_fmt && alt_byte > best_mask);
    }
    if (wins) {
      best_score = score;
      best_idx = i;
      best_mask = alt_byte;
    }
  }

  return best_idx;
}

}  // namespace pagespeed
