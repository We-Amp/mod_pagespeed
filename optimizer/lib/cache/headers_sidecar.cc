// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/cache/headers_sidecar.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lib/cache/vary_storability.h"
#include "lib/classify/alternate_id.h"

// PAYLOAD LAYOUT (format version 1)
//
//   [1B  format version]                    == kHeadersSidecarFormatVersion
//   [2B  field count, big-endian]
//   repeated, field-count times:
//     [1B  name length]                     1..255
//     [2B  value length, big-endian]        0..65535
//     [name bytes]                          verbatim, as received
//     [value bytes]                         verbatim, as received
//
// LENGTH-PREFIXED, not delimited, and that is the whole point of the encoding:
// a value may legally contain leading, trailing and internal whitespace, may
// be empty, and may carry 8-bit bytes (obs-text), none of which survive a
// delimiter-and-trim format.  What the encoding cannot carry is a value with a
// line terminator in it, because such a value cannot be re-emitted as one
// header field at all — the gate refuses those rather than storing bytes whose
// replay would be a different message.
//
// The count is redundant with the byte stream and is kept anyway: it makes a
// truncated payload detectable as truncation rather than as a shorter block.

namespace pagespeed {

// The payload's first byte must never be mistakable for an entry-metadata
// version byte.  Nothing depends on that today — the read path takes the RAW
// blob rather than the metadata-stripped view, precisely so it does not — but
// a future format version chosen inside the metadata range would make the two
// shapes ambiguous to anyone who did reach for the stripped view, and the
// cheapest place to stop that is here.  Metadata versions start at 3
// (AlternateMetadata::Deserialize refuses anything lower).
static_assert(kHeadersSidecarFormatVersion > 0 &&
                  kHeadersSidecarFormatVersion < 3,
              "the sidecar payload format version must stay outside the "
              "entry-metadata version range");
static_assert(kHeadersSidecarPairedFormatVersion > 0 &&
                  kHeadersSidecarPairedFormatVersion < 3 &&
                  kHeadersSidecarPairedFormatVersion !=
                      kHeadersSidecarFormatVersion,
              "the paired sidecar format version must stay outside the "
              "entry-metadata version range and differ from format 1");

namespace {

// ASCII case-insensitive equality.  Folds only A-Z, so a field-name
// comparison cannot change meaning with the process locale — the same fold
// the store-side Vary predicate applies.
bool AsciiEqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    auto ca = static_cast<unsigned char>(a[i]);
    auto cb = static_cast<unsigned char>(b[i]);
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca | 0x20);
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb | 0x20);
    if (ca != cb) return false;
  }
  return true;
}

std::string AsciiLower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    auto u = static_cast<unsigned char>(c);
    if (u >= 'A' && u <= 'Z') c = static_cast<char>(u | 0x20);
  }
  return out;
}

bool Contains(std::span<const std::string_view> set, std::string_view name) {
  for (std::string_view candidate : set) {
    if (AsciiEqualsIgnoreCase(candidate, name)) return true;
  }
  return false;
}

// RFC 9110 §5.6.2 tchar.
bool IsTokenChar(unsigned char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9')) {
    return true;
  }
  switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

bool IsFieldName(std::string_view name) {
  if (name.empty()) return false;
  for (char c : name) {
    if (!IsTokenChar(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

// A value this encoding can hand back as ONE field value.  CR and LF would
// re-emit as a different message; NUL is not a field-value character and is a
// reliable sign the value came from somewhere it should not have.  Everything
// else — SP, HTAB, and every byte >= 0x80 (obs-text) — is carried unchanged.
bool IsStorableFieldValue(std::string_view value) {
  for (char c : value) {
    auto u = static_cast<unsigned char>(c);
    if (u == '\r' || u == '\n' || u == 0) return false;
  }
  return true;
}

void AppendBigEndian16(std::string* out, uint16_t v) {
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
  out->push_back(static_cast<char>(v & 0xFF));
}

uint16_t ReadBigEndian16(std::string_view s, size_t offset) {
  return static_cast<uint16_t>((static_cast<unsigned char>(s[offset]) << 8) |
                               static_cast<unsigned char>(s[offset + 1]));
}

// The sidecar's closed set of carried field names.
//
// A name is listed when its value describes the resource or the server's
// policy and is the same for every request.  Names whose value is stamped per
// request, connection or trace (a CDN request id, Set-Cookie, Server-Timing)
// are never listed.
//
// `vary` is carried, and its ADMISSION is decided by the store-side Vary
// predicate rather than by membership here: a response whose Vary the cache
// key cannot honour is not storable at all, so there is nothing to carry.
// What membership buys is that the origin's ACTUAL Vary string is relayed
// byte for byte instead of being re-synthesised from the tokens the cache
// happened to understand.
constexpr std::string_view kAllowlist[] = {
    "content-security-policy",            //
    "access-control-allow-origin",        //
    "x-content-type-options",             //
    "referrer-policy",                    //
    "cross-origin-opener-policy",         //
    "cross-origin-embedder-policy",       //
    "cross-origin-resource-policy",       //
    "permissions-policy",                 //
    "vary",                               //
    "link",                               //
    "strict-transport-security",          //
    "x-frame-options",                    //
    "x-xss-protection",                   //
    "x-permitted-cross-domain-policies",  //
    "origin-agent-cluster",               //
    "x-dns-prefetch-control",             //
    "timing-allow-origin",                //
    "access-control-expose-headers",      //
    "reporting-endpoints",                //
    "nel",                                //
    "x-powered-by",                       //
    "x-pingback",                         //
    "x-robots-tag",                       //
    "content-language",                   //
    "content-disposition",                //
    "alt-svc",                            //
    "allow",                              //
};

// Reproduced by the entry metadata itself, so the sidecar does not carry them
// and their presence is not a fall-through reason.
constexpr std::string_view kReproducedByEntryMetadata[] = {
    "cache-control", "content-type",  "content-length",
    "etag",          "last-modified", "expires",
};

// Stamped per response by whatever is serving, on every path, so they are
// never reproduced from a cache entry and never a reason to refuse one.
constexpr std::string_view kStampedPerResponse[] = {
    "date", "server",        "connection", "keep-alive", "transfer-encoding",
    "age",  "accept-ranges",
};

}  // namespace

bool CspCarriesNonce(std::string_view csp_value) {
  constexpr std::string_view kNeedle = "nonce-";
  if (csp_value.size() < kNeedle.size()) return false;
  const size_t last = csp_value.size() - kNeedle.size();
  for (size_t i = 0; i <= last; ++i) {
    if (AsciiEqualsIgnoreCase(csp_value.substr(i, kNeedle.size()), kNeedle)) {
      return true;
    }
  }
  return false;
}

bool LinkCarriesNonce(std::string_view link_value) {
  constexpr std::string_view kNeedle = "nonce";
  bool in_target = false;
  bool in_quotes = false;
  for (size_t i = 0; i < link_value.size(); ++i) {
    const char c = link_value[i];
    if (in_target) {
      in_target = c != '>';
    } else if (in_quotes) {
      if (c == '\\' && i + 1 < link_value.size()) {
        ++i;
      } else if (c == '"') {
        in_quotes = false;
      }
    } else if (c == '"') {
      in_quotes = true;
    } else if (c == '<') {
      in_target = true;
    }
    if (!in_target && i + kNeedle.size() <= link_value.size() &&
        AsciiEqualsIgnoreCase(link_value.substr(i, kNeedle.size()), kNeedle)) {
      return true;
    }
  }
  // An unclosed target is malformed: never carry it.
  return in_target;
}

std::span<const std::string_view> HeadersSidecarAllowlist() {
  return kAllowlist;
}

std::span<const std::string_view> HeadersReproducedByEntryMetadata() {
  return kReproducedByEntryMetadata;
}

std::span<const std::string_view> HeadersStampedPerResponse() {
  return kStampedPerResponse;
}

SidecarClassification ClassifyForHeadersSidecar(
    std::span<const HeaderField> headers) {
  SidecarClassification result;
  std::vector<HeaderField> carried;
  std::string combined_vary;
  bool saw_vary = false;
  bool never_optimized = false;

  auto add_reason = [&result](std::string reason) {
    result.reasons.push_back(std::move(reason));
  };

  for (const HeaderField& field : headers) {
    // THE NONCE IS CHECKED FIRST, before the field is checked for being
    // well-formed at all, and the order is the decision rather than an
    // accident.  A CSP that carries a nonce AND a line terminator is refused
    // either way, so nothing about what is stored turns on this — but the two
    // verdicts exist to keep a PERMANENT class out of the closable one, and a
    // nonce is the decisive fact about such a response no matter what else is
    // wrong with it.  Filed under malformed-field it would be counted as a
    // gap later work could close, which it is not.
    if (AsciiEqualsIgnoreCase(field.name, "content-security-policy") &&
        CspCarriesNonce(field.value)) {
      // Terminal for the response, and deliberately NOT "carry the others and
      // drop this one": the nonce is the policy.  Scanning continues so the
      // reasons stay complete.
      never_optimized = true;
      add_reason("nonce-csp");
      continue;
    }

    // A per-response nonce in a preload hint must never be replayed.
    if (AsciiEqualsIgnoreCase(field.name, "link") &&
        LinkCarriesNonce(field.value)) {
      never_optimized = true;
      add_reason("nonce-link");
      continue;
    }

    if (!IsFieldName(field.name) || !IsStorableFieldValue(field.value)) {
      // Not a field this gate is willing to reason about.  It is refused
      // rather than ignored: something produced it, and a response the gate
      // cannot fully account for is exactly the fall-through class.
      add_reason("malformed-field:" + AsciiLower(field.name));
      continue;
    }

    if (AsciiEqualsIgnoreCase(field.name, "vary")) {
      saw_vary = true;
      // RFC 9110 §5.3: several Vary lines are ONE list, and the store-side
      // predicate has to see the whole of it.
      if (!combined_vary.empty()) combined_vary.push_back(',');
      combined_vary.append(field.value);
    }

    if (Contains(kReproducedByEntryMetadata, field.name) ||
        Contains(kStampedPerResponse, field.name)) {
      continue;
    }

    if (!Contains(kAllowlist, field.name)) {
      add_reason("unreconstructible-header:" + AsciiLower(field.name));
      continue;
    }

    if (field.name.size() > kMaxHeadersSidecarNameBytes ||
        field.value.size() > kMaxHeadersSidecarValueBytes) {
      add_reason("field-too-large:" + AsciiLower(field.name));
      continue;
    }

    carried.push_back(field);
  }

  if (saw_vary && VaryUncacheable(combined_vary)) {
    // Not storable at all — so there is no entry for a sidecar to belong to.
    // This gate composes with that predicate rather than restating it.
    add_reason("vary-unstorable:" + combined_vary);
  }

  // Size the block before encoding it: the ceiling exists to bound what is
  // stored, and a block measured after the fact has already been built.
  size_t encoded_size = 3;
  for (const HeaderField& field : carried) {
    encoded_size += 3 + field.name.size() + field.value.size();
  }
  if (!carried.empty() && (carried.size() > 0xFFFF ||
                           encoded_size > kMaxHeadersSidecarPayloadBytes)) {
    add_reason("sidecar-too-large");
    carried.clear();
  }

  if (never_optimized) {
    result.verdict = SidecarVerdict::kNeverOptimized;
  } else if (!result.reasons.empty()) {
    result.verdict = SidecarVerdict::kFallThrough;
  }

  std::sort(result.reasons.begin(), result.reasons.end());
  result.reasons.erase(
      std::unique(result.reasons.begin(), result.reasons.end()),
      result.reasons.end());

  if (result.verdict != SidecarVerdict::kSwapEligible || carried.empty()) {
    // Nothing to store: either the response is not optimized in place, or
    // every header it carries is already reproduced by the entry metadata.
    return result;
  }

  std::string payload;
  payload.reserve(encoded_size);
  payload.push_back(static_cast<char>(kHeadersSidecarFormatVersion));
  AppendBigEndian16(&payload, static_cast<uint16_t>(carried.size()));
  for (const HeaderField& field : carried) {
    payload.push_back(static_cast<char>(field.name.size()));
    AppendBigEndian16(&payload, static_cast<uint16_t>(field.value.size()));
    payload.append(field.name);
    payload.append(field.value);
  }

  result.payload = std::move(payload);
  result.fields = carried.size();
  return result;
}

namespace {

// The field list of a payload of either format, starting at the 2-byte count
// at `offset`.
std::optional<std::vector<HeaderField>> ParseSidecarFields(
    std::string_view payload, size_t offset) {
  if (payload.size() < offset + 2) return std::nullopt;
  const size_t count = ReadBigEndian16(payload, offset);
  offset += 2;
  std::vector<HeaderField> fields;
  fields.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    if (offset + 3 > payload.size()) return std::nullopt;
    const size_t name_len = static_cast<unsigned char>(payload[offset]);
    const size_t value_len = ReadBigEndian16(payload, offset + 1);
    offset += 3;
    if (name_len == 0) return std::nullopt;
    if (payload.size() - offset < name_len) return std::nullopt;
    if (payload.size() - offset - name_len < value_len) return std::nullopt;
    fields.push_back(HeaderField{payload.substr(offset, name_len),
                                 payload.substr(offset + name_len, value_len)});
    offset += name_len + value_len;
  }
  // Bytes left over mean this is not the block it claims to be.
  if (offset != payload.size()) return std::nullopt;
  return fields;
}

}  // namespace

std::optional<HeadersSidecarBlock> ParseHeadersSidecarBlock(
    std::string_view payload) {
  if (payload.empty()) return std::nullopt;
  const auto version = static_cast<unsigned char>(payload[0]);
  HeadersSidecarBlock block;
  size_t fields_at = 1;
  if (version == kHeadersSidecarPairedFormatVersion) {
    if (payload.size() < 1 + kHeadersSidecarPairingHashBytes) {
      return std::nullopt;
    }
    std::array<std::byte, kHeadersSidecarPairingHashBytes> hash{};
    bool all_zero = true;
    for (size_t i = 0; i < kHeadersSidecarPairingHashBytes; ++i) {
      hash[i] = static_cast<std::byte>(payload[1 + i]);
      if (hash[i] != std::byte{0}) all_zero = false;
    }
    // All-zero is the reserved "no binding" value everywhere a content hash
    // is stamped; a writer never pairs with it, so a block carrying it is not
    // one this class wrote.
    if (all_zero) return std::nullopt;
    block.pairing_hash = hash;
    fields_at += kHeadersSidecarPairingHashBytes;
  } else if (version != kHeadersSidecarFormatVersion) {
    return std::nullopt;
  }
  auto fields = ParseSidecarFields(payload, fields_at);
  if (!fields.has_value()) return std::nullopt;
  block.fields = std::move(*fields);
  return block;
}

std::optional<std::vector<HeaderField>> ParseHeadersSidecar(
    std::string_view payload) {
  auto block = ParseHeadersSidecarBlock(payload);
  // Fields only, so this reader cannot check a pairing: a paired block is
  // absent to it (fail closed), and the caller's miss path runs.
  if (!block.has_value() || block->pairing_hash.has_value()) {
    return std::nullopt;
  }
  return std::move(block->fields);
}

std::string PairHeadersSidecarPayload(
    std::string_view format1_payload,
    std::span<const std::byte, kHeadersSidecarPairingHashBytes> hash) {
  std::string out;
  out.reserve(1 + kHeadersSidecarPairingHashBytes +
              (format1_payload.empty() ? 2 : format1_payload.size() - 1));
  out.push_back(static_cast<char>(kHeadersSidecarPairedFormatVersion));
  for (std::byte b : hash) out.push_back(static_cast<char>(b));
  if (format1_payload.empty()) {
    AppendBigEndian16(&out, 0);
  } else {
    // Everything after format 1's version byte: the count and the fields.
    out.append(format1_payload.substr(1));
  }
  return out;
}

}  // namespace pagespeed
