// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/nginx/etag_util.h"

#include <array>
#include <cstdio>

namespace pagespeed {

namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

// Separates the origin-ETag bytes from the Last-Modified bytes so that
// ("", lm) can never alias an origin ETag whose raw bytes happen to encode
// the same timestamp.
constexpr uint8_t kFnvDomainSeparator = 0x1F;

inline uint64_t FnvMix(uint64_t h, uint8_t byte) {
  h ^= byte;
  h *= kFnvPrime;
  return h;
}

}  // namespace

std::optional<uint64_t> ContentIdentity64(const AlternateMetadata& meta) {
  // Tier 1: stored content hash (v7).  All-zero is the reserved
  // "no binding" value (alternate_metadata.h), never a real SHA-256 output
  // in practice.
  static constexpr std::array<std::byte, AlternateMetadata::kHashSize>
      kZeroHash{};
  if (meta.origin_html_hash != kZeroHash) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) {
      v = (v << 8) | std::to_integer<uint64_t>(meta.origin_html_hash[i]);
    }
    return v;
  }

  // Tier 2: origin validators (v5).  Present for any content type when the
  // origin sent ETag and/or Last-Modified at store time; refreshed by the
  // 304-restamp path when the origin revises them.
  if (!meta.origin_etag.empty() || meta.origin_last_modified != 0) {
    uint64_t h = kFnvOffset;
    for (char c : meta.origin_etag) {
      h = FnvMix(h, static_cast<uint8_t>(c));
    }
    h = FnvMix(h, kFnvDomainSeparator);
    const uint32_t lm = meta.origin_last_modified;
    for (int i = 0; i < 4; ++i) {
      h = FnvMix(h, static_cast<uint8_t>((lm >> (8 * i)) & 0xFF));
    }
    return h;
  }

  // Tier 3: no identity signal: caller emits no ETag.
  return std::nullopt;
}

size_t FormatHitETag(const AlternateMetadata& meta, size_t body_len, char* buf,
                     size_t buf_size) {
  if (buf == nullptr || buf_size == 0) {
    return 0;
  }
  const std::optional<uint64_t> identity = ContentIdentity64(meta);
  if (!identity.has_value()) {
    // No identity signal: emit no ETag at all.  A mask+flags+length-only
    // tag would validate any revision of the same optimized length and
    // answer 304 for changed bytes; an absent validator can never do
    // that (RFC 9110 Section 8.8: the client simply revalidates with a
    // full response).
    return 0;
  }
  int n = snprintf(buf, buf_size, "W/\"ps-%08x%02x-%016llx-%016llx\"",
                   static_cast<unsigned>(meta.full_mask),
                   static_cast<unsigned>(meta.flags),
                   static_cast<unsigned long long>(*identity),
                   static_cast<unsigned long long>(body_len));
  if (n <= 0 || static_cast<size_t>(n) >= buf_size) {
    return 0;
  }
  return static_cast<size_t>(n);
}

}  // namespace pagespeed
