// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/classify/option_context.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "sha256.hpp"

namespace pagespeed {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

bool IsLowerHex(std::string_view s) {
  for (const char c : s) {
    const bool digit = c >= '0' && c <= '9';
    const bool lower = c >= 'a' && c <= 'f';
    if (!digit && !lower) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::string OptionContextSignature(std::string_view payload) {
  const auto digest = cyclone::crypto::SHA256::hash(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(payload.data()), payload.size()));
  std::string out;
  out.reserve(digest.size() * 2);
  for (const std::byte b : digest) {
    const auto value = static_cast<uint8_t>(b);
    out.push_back(kHexDigits[value >> 4]);
    out.push_back(kHexDigits[value & 0x0F]);
  }
  return out;
}

bool IsDefaultOptionContext(std::string_view signature) {
  return signature.empty() || signature == kDefaultOptionContextSignature;
}

OptionContextStatus ValidateOptionContext(std::string_view payload,
                                          std::string_view signature) {
  // "Neither present" is the ordinary case, not a degraded one: it is every
  // peer that does not send the field, and every request with nothing to
  // declare.  Checked first so that the error arms below are only ever reached
  // by a peer that meant to supply a context.
  if (payload.empty() && signature.empty()) {
    return OptionContextStatus::kEmpty;
  }

  if (payload.size() > kMaxOptionContextBytes) {
    return OptionContextStatus::kTooLarge;
  }

  // Format gate, and it is an equality test on the token rather than a prefix
  // sniff of the first byte: a payload from a later format version is refused
  // whole, exactly as the wire refuses a later protocol version, and for the
  // same reason.  Reading a psoc2 payload with a psoc1 reader would produce a
  // signature that means something other than what the peer said, and the
  // whole value of the signature is that both sides derive it the same way.
  const std::string expected_opening =
      std::string(kOptionContextFormatVersion) + "\n";
  if (payload.size() < expected_opening.size() ||
      payload.compare(0, expected_opening.size(), expected_opening) != 0) {
    return OptionContextStatus::kUnknownFormat;
  }

  if (signature.size() != kOptionContextSignatureChars ||
      !IsLowerHex(signature)) {
    return OptionContextStatus::kMalformedSignature;
  }

  // Re-derive rather than trust.  See the header: this is the arm that turns a
  // silent divergence between the two implementations of this format into a
  // counted refusal on the first message.
  if (OptionContextSignature(payload) != signature) {
    return OptionContextStatus::kSignatureMismatch;
  }

  return OptionContextStatus::kOk;
}

std::string_view OptionContextStatusName(OptionContextStatus status) {
  switch (status) {
    case OptionContextStatus::kOk:
      return "ok";
    case OptionContextStatus::kEmpty:
      return "empty";
    case OptionContextStatus::kTooLarge:
      return "too-large";
    case OptionContextStatus::kUnknownFormat:
      return "unknown-format";
    case OptionContextStatus::kMalformedSignature:
      return "malformed-signature";
    case OptionContextStatus::kSignatureMismatch:
      return "signature-mismatch";
  }
  return "unknown";
}

}  // namespace pagespeed
