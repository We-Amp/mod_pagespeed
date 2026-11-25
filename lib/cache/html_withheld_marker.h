// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CACHE_HTML_WITHHELD_MARKER_H_
#define PAGESPEED_LIB_CACHE_HTML_WITHHELD_MARKER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace pagespeed {

// The withheld marker: the record the deriver writes when it permanently
// refuses to build the Markdown copy of one stored HTML page, so the serve
// side can stop asking for it.
//
// A marker is bound to two things, and means nothing without either:
//   - the parent hash H, the content hash stamped on the stored HTML entry it
//     was derived from: a page whose content changed is a different page;
//   - the deriver version, which names the extractor and the fidelity floor
//     in force when the refusal was made: a deriver that could now succeed
//     must get to try.
// A marker whose H or deriver version differs from the current one is
// ignored, exactly as if it were absent.
//
// The size cap is a runtime setting, so it cannot be part of a constant
// version.  A `too_large` marker records the cap the page exceeded instead,
// and the deriver ignores it once its own cap is larger
// (HtmlWithheldMarkerBlocksDerivation), so raising the cap re-derives the
// page on its next notification.  The serve side cannot see the deriver's
// cap and keeps reading such a marker as withheld until the page is stored
// again, which removes the marker: the cost of a raised cap is at most one
// stored lifetime without the copy, never a copy served for other content.
//
// The marker is its own record, never a Markdown entry: no reader that
// selects or serves a copy may ever return it.  This file is only its payload
// format, so the writer and every reader agree on it byte for byte.
//
// PAYLOAD (kHtmlWithheldMarkerBytes bytes):
//   [1 byte  format version = kHtmlWithheldMarkerFormat]
//   [1 byte  reason, an HtmlWithheldReason value]
//   [4 bytes deriver version, little-endian]
//   [32 bytes parent hash H]
//   [8 bytes size cap, little-endian: for `too_large`, the cap the page
//            exceeded; 0 for every other reason]
// The format version is below every entry-metadata version, so a metadata
// parse can never consume a prefix of a marker.  A payload of any other
// length, with an unknown format version or reason, or with an all-zero H, is
// not a marker.

// Bump when the extractor or the fidelity floor changes in a way that could
// turn a refusal into a copy.  A marker written under any
// other value is ignored.
inline constexpr uint32_t kHtmlMarkdownDeriverVersion = 1;

// The fidelity floor: a Markdown copy whose body, after any front matter,
// holds fewer non-whitespace bytes than this is not kept.  Such a copy is a
// shell (a script-rendered page read without a browser, a redirect stub),
// and serving it in place of the page would tell an agent the page is empty.
// Part of what kHtmlMarkdownDeriverVersion names.
inline constexpr size_t kHtmlMarkdownFloorBytes = 200;

inline constexpr uint8_t kHtmlWithheldMarkerFormat = 1;
inline constexpr size_t kHtmlWithheldMarkerBytes = 46;
inline constexpr size_t kHtmlParentHashBytes = 32;

// Why the copy was withheld.  The numeric values are part of the stored
// format: append only.
enum class HtmlWithheldReason : uint8_t {
  kBelowFloor = 1,     // the extraction fell below the fidelity floor
  kTooLarge = 2,       // the stored page exceeds the size cap
  kEncoded = 3,        // the stored bytes carry a content coding
  kHashMismatch = 4,   // the bytes do not match the stored content hash
  kExtractFailed = 5,  // the extractor failed
};

inline constexpr uint8_t kHtmlWithheldReasonMax = 5;

struct HtmlWithheldMarker {
  HtmlWithheldReason reason = HtmlWithheldReason::kBelowFloor;
  uint32_t deriver_version = 0;
  std::array<uint8_t, kHtmlParentHashBytes> parent_hash{};
  // The size cap a `too_large` refusal was made under; 0 otherwise.
  uint64_t size_cap = 0;
};

// The payload for `marker`.  Always kHtmlWithheldMarkerBytes long.
std::string EncodeHtmlWithheldMarker(const HtmlWithheldMarker& marker);

// The marker a payload holds, or nullopt when it is not one (see above).
std::optional<HtmlWithheldMarker> DecodeHtmlWithheldMarker(
    std::span<const uint8_t> payload);

// True when `payload` is a marker for exactly this parent hash and the
// current deriver version.  `parent_hash` is kHtmlParentHashBytes bytes; an
// all-zero hash matches nothing.
bool HtmlWithheldMarkerApplies(std::span<const uint8_t> payload,
                               std::span<const uint8_t> parent_hash);

// The deriver's own question: does this marker still stop it from deriving
// the copy of `parent_hash` with a size cap of `max_html_size`?  As
// HtmlWithheldMarkerApplies, except that a `too_large` marker recorded under
// a smaller cap does not (see the size cap note above).
bool HtmlWithheldMarkerBlocksDerivation(std::span<const uint8_t> payload,
                                        std::span<const uint8_t> parent_hash,
                                        uint64_t max_html_size);

// True when `markdown`, after a leading front-matter block, holds fewer than
// kHtmlMarkdownFloorBytes non-whitespace bytes.
bool MarkdownBelowFidelityFloor(std::string_view markdown);

// below_floor, too_large, encoded, hash_mismatch, extract_failed; "unknown"
// for any other value.
std::string_view HtmlWithheldReasonName(HtmlWithheldReason reason);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CACHE_HTML_WITHHELD_MARKER_H_
