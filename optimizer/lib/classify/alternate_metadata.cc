// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/classify/alternate_metadata.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

#include "lib/classify/alternate_id.h"
#include "lib/classify/capability_mask.h"

namespace pagespeed {
namespace {

// Strip CR, LF and NUL from a header-derived string.  Applied on write AND on
// read to every stored header value, so the stored bytes and the value handed
// to a caller are the same set — a stored CRLF would otherwise reach whatever
// forwards the value as a response header.
std::string StripHeaderControlChars(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    if (c != '\r' && c != '\n' && c != '\0') {
      out.push_back(c);
    }
  }
  return out;
}

// Parse cache-control suffix (14 bytes, present in all versions).
void ParseCacheControlFields(AlternateMetadata& meta,
                             std::span<const std::byte> data,
                             size_t suffix_offset) {
  std::memcpy(&meta.cache_inserted_at, &data[suffix_offset], 4);
  std::memcpy(&meta.origin_max_age, &data[suffix_offset + 4], 4);
  std::memcpy(&meta.origin_s_maxage, &data[suffix_offset + 8], 4);
  std::memcpy(&meta.origin_cc_flags, &data[suffix_offset + 12], 2);
}

// Parse V4 quality + classification fields (3 bytes after cache-control).
void ParseV4Fields(AlternateMetadata& meta, std::span<const std::byte> data,
                   size_t suffix_offset) {
  std::memcpy(&meta.ssimulacra2_score_x100, &data[suffix_offset + 14], 2);
  // Clamp out-of-range scores to N/A (valid: 0-10000 or kScoreNA).  A gzip/brotli
  // coded entry keeps its raw value: there it is an encoded-size code, not a score
  // (see AlternateMetadata::kEncodedRatioBase).
  if (meta.ssimulacra2_score_x100 > AlternateMetadata::kMaxScoreX100 &&
      meta.ssimulacra2_score_x100 != AlternateMetadata::kScoreNA &&
      !meta.IsEncodedVariant()) {
    meta.ssimulacra2_score_x100 = AlternateMetadata::kScoreNA;
  }
  meta.content_class = static_cast<uint8_t>(data[suffix_offset + 16]);
  // Clamp unknown content_class values to prevent OOB if used as index.
  if (meta.content_class > AlternateMetadata::kContentClassUnknown) {
    meta.content_class = AlternateMetadata::kContentClassUnknown;
  }
}

// Parse V5 conditional revalidation fields (last_modified + etag).
// Returns false if etag_len exceeds remaining data.
// On success, out_etag_len is set to the wire etag length for V6 parsing.
bool ParseV5Fields(AlternateMetadata& meta, std::span<const std::byte> data,
                   size_t suffix_offset, uint16_t ct_len,
                   uint16_t& out_etag_len) {
  std::memcpy(&meta.origin_last_modified, &data[suffix_offset + 17], 4);
  uint16_t etag_len = 0;
  std::memcpy(&etag_len, &data[suffix_offset + 21], 2);
  size_t fixed_suffix =
      (meta.version >= 8)   ? AlternateMetadata::kV8FixedSuffixSize
      : (meta.version >= 7) ? AlternateMetadata::kV7FixedSuffixSize
      : (meta.version >= 6) ? AlternateMetadata::kV6FixedSuffixSize
                            : AlternateMetadata::kV5FixedSuffixSize;
  size_t min_size =
      AlternateMetadata::kFixedPrefixSize + ct_len + fixed_suffix + etag_len;
  if (data.size() < min_size) {
    return false;
  }
  if (etag_len > 0) {
    meta.origin_etag.assign(
        reinterpret_cast<const char*>(&data[suffix_offset + 23]), etag_len);
  }
  out_etag_len = etag_len;
  return true;
}

// Parse V6 origin content length (4 bytes after etag).
// Requires ParseV5Fields to have run first (etag_len from wire).
void ParseV6Fields(AlternateMetadata& meta, std::span<const std::byte> data,
                   size_t suffix_offset, uint16_t etag_len) {
  std::memcpy(&meta.origin_content_length, &data[suffix_offset + 23 + etag_len],
              4);
}

// Parse V7 content-binding hashes (2 x 32 bytes after origin_content_length).
// Requires ParseV5Fields to have run first (etag_len from wire); the size
// (incl. etag) was already validated by ParseV5Fields' v7 min_size branch.
void ParseV7Fields(AlternateMetadata& meta, std::span<const std::byte> data,
                   size_t suffix_offset, uint16_t etag_len) {
  size_t base =
      suffix_offset + 23 + etag_len + 4;  // after origin_content_length
  std::memcpy(meta.origin_html_hash.data(), &data[base],
              AlternateMetadata::kHashSize);
  std::memcpy(meta.render_source_hash.data(),
              &data[base + AlternateMetadata::kHashSize],
              AlternateMetadata::kHashSize);
}

// Parse V8 fields: the per-URL epoch and the raw origin Cache-Control string
// (after the two v7 hashes).  Requires ParseV5Fields to have run first
// (etag_len from wire); everything up to and including cc_len was already
// size-validated there via the v8 fixed suffix.  Returns false when cc_len is
// over the cap or runs past the end of the blob.
bool ParseV8Fields(AlternateMetadata& meta, std::span<const std::byte> data,
                   size_t suffix_offset, uint16_t ct_len, uint16_t etag_len) {
  // after origin_content_length + the two hashes
  size_t base =
      suffix_offset + 23 + etag_len + 4 + 2 * AlternateMetadata::kHashSize;
  std::memcpy(&meta.origin_epoch, &data[base], 8);
  uint16_t cc_len = 0;
  std::memcpy(&cc_len, &data[base + 8], 2);
  if (cc_len > AlternateMetadata::kMaxOriginCcLen) {
    return false;
  }
  size_t min_size = AlternateMetadata::kFixedPrefixSize + ct_len +
                    AlternateMetadata::kV8FixedSuffixSize + etag_len + cc_len;
  if (data.size() < min_size) {
    return false;
  }
  meta.wire_cc_len = cc_len;
  if (cc_len > 0) {
    meta.origin_cache_control = StripHeaderControlChars(std::string_view(
        reinterpret_cast<const char*>(&data[base + 10]), cc_len));
  }
  return true;
}

// Append-only cursor over a pre-sized output buffer.  Each Write* call emits
// the next field at the running offset, so the wire layout is determined by
// call order rather than by re-deriving per-field magic offsets.  The buffer
// is sized exactly by the caller; writes past the end are a programming error
// (the same out-of-bounds condition the previous manual indexing had).
class SerializeCursor {
 public:
  explicit SerializeCursor(std::vector<std::byte>& out) : out_(out) {}

  // Emit a single byte (e.g. version, content_type, flags, content_class).
  void WriteByte(std::byte value) { out_[offset_++] = value; }

  // Emit `len` raw bytes from `src` (content-type string, etag, hashes).
  void WriteBytes(const void* src, size_t len) {
    if (len > 0) {
      std::memcpy(&out_[offset_], src, len);
    }
    offset_ += len;
  }

  // Emit a fixed-width little-endian scalar exactly as the in-memory bytes,
  // matching the previous `std::memcpy(&result[..], &field, sizeof)` calls.
  template <typename T>
  void WriteScalar(const T& value) {
    WriteBytes(&value, sizeof(T));
  }

 private:
  std::vector<std::byte>& out_;
  size_t offset_ = 0;
};

}  // namespace

std::vector<std::byte> AlternateMetadata::Serialize() const {
  // Strip \r, \n, \0 from origin_content_type on write so that the
  // stored ct_len matches the sanitized string.  This ensures
  // ParseMetadataPrefix() computes the correct content offset.
  std::string sanitized = StripHeaderControlChars(origin_content_type);

  // Same treatment for the raw Cache-Control string, with one difference: it
  // is stored as ABSENT rather than truncated when it is over the cap.  A
  // clipped content-type is still a usable hint; a clipped directive list is
  // a different caching policy from the one the origin sent.
  std::string sanitized_cc = StripHeaderControlChars(origin_cache_control);
  if (sanitized_cc.size() > kMaxOriginCcLen) {
    sanitized_cc.clear();
  }

  uint16_t ct_len = static_cast<uint16_t>(
      std::min(static_cast<size_t>(kMaxOriginCtLen), sanitized.size()));
  uint16_t etag_len = static_cast<uint16_t>(
      std::min(static_cast<size_t>(65535), origin_etag.size()));
  uint16_t cc_len = static_cast<uint16_t>(sanitized_cc.size());
  std::vector<std::byte> result(kFixedPrefixSize + ct_len + kV8FixedSuffixSize +
                                etag_len + cc_len);

  // Append fields in wire order; the cursor accumulates the running offset so
  // the layout matches the previous explicit offset arithmetic byte-for-byte.
  SerializeCursor cursor(result);

  // Fixed prefix (9 bytes: version, full_mask, content_type, flags, ct_len).
  cursor.WriteByte(static_cast<std::byte>(kCurrentVersion));
  cursor.WriteScalar(full_mask);  // 4 bytes, little-endian
  cursor.WriteByte(static_cast<std::byte>(content_type));
  cursor.WriteByte(static_cast<std::byte>(flags));
  cursor.WriteScalar(ct_len);  // 2 bytes, little-endian

  // Origin content-type string (already sanitized).
  cursor.WriteBytes(sanitized.data(), ct_len);

  // Cache-control suffix (14 bytes after ct string).
  cursor.WriteScalar(cache_inserted_at);  // 4
  cursor.WriteScalar(origin_max_age);     // 4
  cursor.WriteScalar(origin_s_maxage);    // 4
  cursor.WriteScalar(origin_cc_flags);    // 2

  // V4 quality + classification (3 bytes after cc_flags).
  cursor.WriteScalar(ssimulacra2_score_x100);  // 2
  cursor.WriteByte(static_cast<std::byte>(content_class));

  // V5 conditional revalidation fields (6 + etag_len bytes).
  cursor.WriteScalar(origin_last_modified);  // 4
  cursor.WriteScalar(etag_len);              // 2
  cursor.WriteBytes(origin_etag.data(), etag_len);

  // V6 origin content length (4 bytes after etag).
  cursor.WriteScalar(origin_content_length);  // 4

  // V7 content-binding hashes (64 bytes after origin_content_length).
  cursor.WriteBytes(origin_html_hash.data(), kHashSize);
  cursor.WriteBytes(render_source_hash.data(), kHashSize);

  // V8 per-URL epoch + raw origin Cache-Control (10 + cc_len bytes).
  cursor.WriteScalar(origin_epoch);  // 8
  cursor.WriteScalar(cc_len);        // 2
  cursor.WriteBytes(sanitized_cc.data(), cc_len);

  return result;
}

bool AlternateMetadata::IsEncodedVariant() const {
  const auto enc = CapabilityMask::Decode(full_mask).transfer_encoding();
  return enc == CapabilityMask::TransferEncoding::kGzip ||
         enc == CapabilityMask::TransferEncoding::kBrotli;
}

std::optional<uint32_t> AlternateMetadata::EncodedOriginLength() const {
  if (origin_content_length == 0) return std::nullopt;
  if (!IsEncodedVariant()) return origin_content_length;
  if (ssimulacra2_score_x100 < kEncodedRatioBase ||
      ssimulacra2_score_x100 == kScoreNA) {
    return std::nullopt;
  }
  const uint64_t code = ssimulacra2_score_x100 - kEncodedRatioBase;
  const uint64_t encoded =
      (code * origin_content_length + kEncodedRatioScale / 2) /
      kEncodedRatioScale;
  if (encoded > UINT32_MAX) return std::nullopt;
  return static_cast<uint32_t>(encoded);
}

void AlternateMetadata::ClearEncodedOriginLength() {
  if (IsEncodedVariant()) ssimulacra2_score_x100 = kScoreNA;
}

void AlternateMetadata::SetEncodedOriginLength(uint64_t encoded_bytes) {
  if (!IsEncodedVariant()) return;
  ssimulacra2_score_x100 = kScoreNA;
  if (origin_content_length == 0) return;
  const uint64_t code =
      (encoded_bytes * kEncodedRatioScale + origin_content_length / 2) /
      origin_content_length;
  if (code <= static_cast<uint64_t>(kScoreNA - 1 - kEncodedRatioBase)) {
    ssimulacra2_score_x100 = static_cast<uint16_t>(kEncodedRatioBase + code);
  }
}

bool AlternateMetadata::IsRetired(std::span<const std::byte> data) {
  // version, full_mask (4 bytes LE), content_type, flags.
  if (data.size() < 7) return false;
  const auto version = static_cast<uint8_t>(data[0]);
  if (version < 3 || version >= 9) return false;
  // Every HTML entry, whoever wrote it.
  const auto content_type = static_cast<uint8_t>(data[5]);
  if (content_type == static_cast<uint8_t>(ContentType::kHtml)) return true;
  // The Markdown version of a page, written by the worker.
  const auto mask_low_byte = static_cast<uint8_t>(data[1]);
  return (static_cast<uint8_t>(data[6]) & kFlagWorkerProcessed) != 0 &&
         mask_low_byte == static_cast<uint8_t>(SentinelId::kAgentMarkdown);
}

std::optional<AlternateMetadata> AlternateMetadata::Deserialize(
    std::span<const std::byte> data) {
  // Require at least the fixed prefix.
  if (data.size() < kFixedPrefixSize) {
    return std::nullopt;
  }

  AlternateMetadata meta;

  // Version check — accept v3 through kCurrentVersion.  Anything above it was
  // written by a peer that knows a field this build does not, and the only
  // safe answer is "I cannot read this" (the caller turns that into a MISS
  // and re-records), never a partial parse of the prefix it happens to
  // recognise.
  meta.version = static_cast<uint8_t>(data[0]);
  if (meta.version < 3 || meta.version > kCurrentVersion) {
    return std::nullopt;
  }

  // Full mask (little-endian).
  std::memcpy(&meta.full_mask, &data[1], 4);

  // Content type -- validate range.
  auto ct_raw = static_cast<uint8_t>(data[5]);
  if (ct_raw > static_cast<uint8_t>(ContentType::kOther)) {
    return std::nullopt;
  }
  meta.content_type = static_cast<ContentType>(ct_raw);

  // Flags byte.
  meta.flags = static_cast<uint8_t>(data[6]);

  // HTML pages and Markdown versions of pages below version 9 read as a MISS,
  // so every reader rebuilds them; CSS, images and the rest stay readable.
  if (IsRetired(data)) {
    return std::nullopt;
  }

  // Origin content-type length (little-endian).
  uint16_t ct_len = 0;
  std::memcpy(&ct_len, &data[7], 2);

  // Cap at maximum.
  if (ct_len > kMaxOriginCtLen) {
    return std::nullopt;
  }

  // Store wire ct_len for ParseMetadataPrefix.
  meta.wire_ct_len = ct_len;

  // Total size check: prefix + ct_len + version-dependent fixed suffix.
  // v5+ has a variable-length etag after the fixed suffix; checked below.
  // v6 adds 4 bytes (origin_content_length) after etag; v7 adds 64 bytes
  // (origin_html_hash + render_source_hash) after origin_content_length.
  size_t suffix_size = (meta.version >= 8)   ? kV8FixedSuffixSize
                       : (meta.version >= 7) ? kV7FixedSuffixSize
                       : (meta.version >= 6) ? kV6FixedSuffixSize
                       : (meta.version >= 5) ? kV5FixedSuffixSize
                       : (meta.version >= 4) ? 17
                                             : 14;
  if (data.size() < kFixedPrefixSize + ct_len + suffix_size) {
    return std::nullopt;
  }

  // Extract origin content-type string, stripping \r, \n, \0.
  meta.origin_content_type = StripHeaderControlChars(std::string_view(
      reinterpret_cast<const char*>(&data[kFixedPrefixSize]), ct_len));

  // Parse version-specific suffix fields.
  // Older versions leave fields at struct defaults (kScoreNA, empty etag, etc.).
  size_t suffix_offset = kFixedPrefixSize + ct_len;
  ParseCacheControlFields(meta, data, suffix_offset);
  uint16_t etag_len = 0;
  if (meta.version >= 4) {
    ParseV4Fields(meta, data, suffix_offset);
  }
  if (meta.version >= 5) {
    if (!ParseV5Fields(meta, data, suffix_offset, ct_len, etag_len)) {
      return std::nullopt;
    }
  }
  if (meta.version >= 6) {
    ParseV6Fields(meta, data, suffix_offset, etag_len);
  }
  if (meta.version >= 7) {
    ParseV7Fields(meta, data, suffix_offset, etag_len);
  }
  if (meta.version >= 8) {
    if (!ParseV8Fields(meta, data, suffix_offset, ct_len, etag_len)) {
      return std::nullopt;
    }
  }

  // Compute consumed byte count by summing individual field sizes.
  // Deliberately avoids the kV*FixedSuffixSize constants so that this
  // value is independently derived from WireSize().  ParseMetadataPrefix
  // cross-checks the two as a regression canary: a mismatch means a
  // code change broke a constant or omitted a field.
  size_t cursor = 1 + 4 + 1 + 1 + 2;  // version, mask, ct, flags, ct_len
  cursor += ct_len;                   // origin content-type string
  cursor += 4 + 4 + 4 + 2;            // cc fields (14 bytes, all versions)
  if (meta.version >= 4) {
    cursor += 2 + 1;  // score_x100, content_class
  }
  if (meta.version >= 5) {
    cursor += 4 + 2;                    // last_modified, etag_len
    cursor += meta.origin_etag.size();  // etag data
  }
  if (meta.version >= 6) {
    cursor += 4;  // origin_content_length
  }
  if (meta.version >= 7) {
    cursor += 64;  // origin_html_hash (32) + render_source_hash (32)
  }
  if (meta.version >= 8) {
    cursor += 8 + 2;             // origin_epoch, origin_cc_len
    cursor += meta.wire_cc_len;  // raw Cache-Control bytes (pre-sanitize)
  }
  meta.consumed_size = cursor;

  return meta;
}

}  // namespace pagespeed
