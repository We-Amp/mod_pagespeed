// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "lib/cache/html_withheld_marker.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace pagespeed {
namespace {

bool AllZero(std::span<const uint8_t> bytes) {
  return std::all_of(bytes.begin(), bytes.end(),
                     [](uint8_t b) { return b == 0; });
}

}  // namespace

std::string EncodeHtmlWithheldMarker(const HtmlWithheldMarker& marker) {
  std::string out(kHtmlWithheldMarkerBytes, '\0');
  out[0] = static_cast<char>(kHtmlWithheldMarkerFormat);
  out[1] = static_cast<char>(marker.reason);
  for (int i = 0; i < 4; ++i) {
    out[2 + i] = static_cast<char>((marker.deriver_version >> (8 * i)) & 0xFF);
  }
  for (size_t i = 0; i < kHtmlParentHashBytes; ++i) {
    out[6 + i] = static_cast<char>(marker.parent_hash[i]);
  }
  return out;
}

std::optional<HtmlWithheldMarker> DecodeHtmlWithheldMarker(
    std::span<const uint8_t> payload) {
  if (payload.size() != kHtmlWithheldMarkerBytes) return std::nullopt;
  if (payload[0] != kHtmlWithheldMarkerFormat) return std::nullopt;
  if (payload[1] == 0 || payload[1] > kHtmlWithheldReasonMax) {
    return std::nullopt;
  }
  HtmlWithheldMarker marker;
  marker.reason = static_cast<HtmlWithheldReason>(payload[1]);
  for (int i = 0; i < 4; ++i) {
    marker.deriver_version |= static_cast<uint32_t>(payload[2 + i]) << (8 * i);
  }
  std::copy_n(payload.begin() + 6, kHtmlParentHashBytes,
              marker.parent_hash.begin());
  if (AllZero(marker.parent_hash)) return std::nullopt;
  return marker;
}

bool HtmlWithheldMarkerApplies(std::span<const uint8_t> payload,
                               std::span<const uint8_t> parent_hash) {
  if (parent_hash.size() != kHtmlParentHashBytes || AllZero(parent_hash)) {
    return false;
  }
  const std::optional<HtmlWithheldMarker> marker =
      DecodeHtmlWithheldMarker(payload);
  if (!marker.has_value()) return false;
  if (marker->deriver_version != kHtmlMarkdownDeriverVersion) return false;
  return std::equal(parent_hash.begin(), parent_hash.end(),
                    marker->parent_hash.begin());
}

std::string_view HtmlWithheldReasonName(HtmlWithheldReason reason) {
  switch (reason) {
    case HtmlWithheldReason::kBelowFloor:
      return "below_floor";
    case HtmlWithheldReason::kTooLarge:
      return "too_large";
    case HtmlWithheldReason::kEncoded:
      return "encoded";
    case HtmlWithheldReason::kHashMismatch:
      return "hash_mismatch";
    case HtmlWithheldReason::kExtractFailed:
      return "extract_failed";
  }
  return "unknown";
}

}  // namespace pagespeed
