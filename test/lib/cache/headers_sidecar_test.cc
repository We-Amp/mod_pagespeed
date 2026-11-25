// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The response-header sidecar's admission gate and its payload codec.
//
// The properties these tests exist to hold, in the order the class depends on
// them:
//
//   1. what goes in comes back out BYTE FOR BYTE — case, whitespace, empty
//      values, 8-bit bytes, duplicate lines, order;
//   2. a nonce-bearing CSP is never carried, under every spelling, and the
//      refusal is the whole response rather than "drop that one header";
//   3. a header whose value depends on the request is never carried, and the
//      allowlist is a closed set rather than a default-allow with exceptions;
//   4. the Vary the origin actually sent is what is stored, and whether the
//      response is storable at all is decided by the shared store-side
//      predicate rather than by a second copy of it here;
//   5. a payload this build cannot read is a miss, not a guess.

#include "lib/cache/headers_sidecar.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "lib/cache/vary_storability.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"

namespace pagespeed {
namespace {

using Fields = std::vector<HeaderField>;

SidecarClassification Classify(const Fields& fields) {
  return ClassifyForHeadersSidecar(fields);
}

// Round-trip helper: classify, then parse the payload back.
std::vector<std::pair<std::string, std::string>> RoundTrip(
    const SidecarClassification& classified) {
  std::vector<std::pair<std::string, std::string>> out;
  auto parsed = ParseHeadersSidecar(classified.payload);
  if (!parsed.has_value()) return out;
  for (const HeaderField& field : *parsed) {
    out.emplace_back(std::string(field.name), std::string(field.value));
  }
  return out;
}

// ---------------------------------------------------------------- byte
// fidelity

TEST(HeadersSidecarTest, EveryAllowlistedClassRoundTripsByteForByte) {
  // One member of every allowlisted class, with values chosen to be awkward
  // rather than representative: mixed-case names, folding whitespace inside a
  // value, an empty value, and 8-bit bytes.
  const Fields fields = {
      {"Content-Security-Policy", "default-src 'self';   img-src  *"},
      {"access-control-allow-origin", "*"},
      {"X-Content-Type-Options", "nosniff"},
      {"Referrer-Policy", ""},
      {"Cross-Origin-Opener-Policy", "same-origin"},
      {"Cross-Origin-Embedder-Policy", "require-corp"},
      {"Cross-Origin-Resource-Policy", "cross-origin"},
      {"Permissions-Policy", "geolocation=(), camera=(\"\xC3\xA9\")"},
      {"Vary", "Accept-Encoding"},
  };
  const auto result = Classify(fields);
  ASSERT_EQ(result.verdict, SidecarVerdict::kSwapEligible) << result.reasons[0];
  EXPECT_EQ(result.fields, fields.size());

  const auto parsed = RoundTrip(result);
  ASSERT_EQ(parsed.size(), fields.size());
  for (size_t i = 0; i < fields.size(); ++i) {
    EXPECT_EQ(parsed[i].first, fields[i].name) << "at " << i;
    EXPECT_EQ(parsed[i].second, fields[i].value) << "at " << i;
  }
}

TEST(HeadersSidecarTest, DuplicateHeaderLinesAreBothKeptInOrder) {
  // The origin sent two lines; a relay that merges them into one has changed
  // the message. Order is part of what is stored.
  const Fields fields = {
      {"Content-Security-Policy", "default-src 'self'"},
      {"Content-Security-Policy", "frame-ancestors 'none'"},
  };
  const auto result = Classify(fields);
  ASSERT_EQ(result.verdict, SidecarVerdict::kSwapEligible);
  const auto parsed = RoundTrip(result);
  ASSERT_EQ(parsed.size(), 2u);
  EXPECT_EQ(parsed[0].second, "default-src 'self'");
  EXPECT_EQ(parsed[1].second, "frame-ancestors 'none'");
}

TEST(HeadersSidecarTest, WhitespaceAndEmptyValuesSurviveUntouched) {
  const Fields fields = {
      {"Referrer-Policy", " \tno-referrer \t"},
      {"X-Content-Type-Options", ""},
  };
  const auto result = Classify(fields);
  ASSERT_EQ(result.verdict, SidecarVerdict::kSwapEligible);
  const auto parsed = RoundTrip(result);
  ASSERT_EQ(parsed.size(), 2u);
  EXPECT_EQ(parsed[0].second, " \tno-referrer \t");
  EXPECT_EQ(parsed[1].second, "");
}

TEST(HeadersSidecarTest, EightBitBytesInAValueAreCarriedUnchanged) {
  std::string value("a\x80\xFE\x7F  b");
  const Fields fields = {{"Referrer-Policy", value}};
  const auto result = Classify(fields);
  ASSERT_EQ(result.verdict, SidecarVerdict::kSwapEligible);
  const auto parsed = RoundTrip(result);
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0].second, value);
}

TEST(HeadersSidecarTest, ANonceOutranksAMalformedValue) {
  // Both facts are true of this field and both refuse it, so nothing about
  // what is stored turns on the order — but the verdicts are not
  // interchangeable: one names a class later work can close and the other
  // names one it cannot. The nonce is the decisive fact.
  const Fields fields = {
      {"Content-Security-Policy", "script-src 'nonce-a'\r\nX-Injected: 1"}};
  const auto result = Classify(fields);
  EXPECT_EQ(result.verdict, SidecarVerdict::kNeverOptimized);
  EXPECT_TRUE(result.payload.empty());
  ASSERT_FALSE(result.reasons.empty());
  EXPECT_EQ(result.reasons[0], "nonce-csp");
}

TEST(HeadersSidecarTest, AValueCarryingALineTerminatorIsRefused) {
  // A value that cannot be re-emitted as ONE header field is not stored: its
  // replay would be a different message, not a stale header.
  for (std::string_view value : std::initializer_list<std::string_view>{
           "same-origin\r\nX-Injected: 1", "same-origin\nX-Injected: 1",
           std::string_view("a\0b", 3)}) {
    const Fields fields = {{"Cross-Origin-Opener-Policy", value}};
    const auto result = Classify(fields);
    EXPECT_EQ(result.verdict, SidecarVerdict::kFallThrough);
    EXPECT_TRUE(result.payload.empty());
  }
}

// -------------------------------------------------------------- nonce CSP

TEST(HeadersSidecarTest, NonceBearingCspIsNeverStored) {
  for (std::string_view csp : {
           "default-src 'self'; script-src 'nonce-abc123'",
           "script-src 'NONCE-ABC123'",
           "script-src 'Nonce-Abc'",
           "script-src   'nonce-a' 'strict-dynamic'",
           // The needle at the very end, and at the very start.
           "script-src 'nonce-",
           "nonce-x",
       }) {
    const Fields fields = {{"Content-Security-Policy", csp}};
    const auto result = Classify(fields);
    EXPECT_EQ(result.verdict, SidecarVerdict::kNeverOptimized) << csp;
    EXPECT_TRUE(result.payload.empty()) << csp;
    EXPECT_TRUE(CspCarriesNonce(csp)) << csp;
  }
}

TEST(HeadersSidecarTest, NonceInsideAHostSourceIsTreatedAsANonce) {
  // Deliberately over-broad. `nonce-cdn.example` is not a nonce, but the two
  // are not reliably separable without a full CSP grammar and the error
  // directions are not symmetric: this costs one URL its optimization, the
  // other direction replays a per-response secret to every later client.
  const Fields fields = {
      {"Content-Security-Policy", "script-src https://nonce-cdn.example.com"}};
  EXPECT_EQ(Classify(fields).verdict, SidecarVerdict::kNeverOptimized);
}

TEST(HeadersSidecarTest, ANonceRefusesTheWholeResponseNotJustTheCsp) {
  // The temptation is to carry the other headers and drop the CSP. That would
  // serve an optimized entry with a security header removed, which is the bug
  // the class exists to prevent.
  const Fields fields = {
      {"X-Content-Type-Options", "nosniff"},
      {"Referrer-Policy", "no-referrer"},
      {"Content-Security-Policy", "script-src 'nonce-abc'"},
  };
  const auto result = Classify(fields);
  EXPECT_EQ(result.verdict, SidecarVerdict::kNeverOptimized);
  EXPECT_TRUE(result.payload.empty());
  EXPECT_EQ(result.fields, 0u);
}

TEST(HeadersSidecarTest, ANonceFreeCspIsCarried) {
  const Fields fields = {
      {"Content-Security-Policy", "default-src 'self'; script-src 'sha256-x'"}};
  const auto result = Classify(fields);
  ASSERT_EQ(result.verdict, SidecarVerdict::kSwapEligible);
  EXPECT_EQ(result.fields, 1u);
  EXPECT_FALSE(CspCarriesNonce("default-src 'self'; script-src 'sha256-x'"));
}

TEST(HeadersSidecarTest, NonceDetectionIsSubstringNotTokenPrefix) {
  EXPECT_FALSE(CspCarriesNonce(""));
  EXPECT_FALSE(CspCarriesNonce("nonce"));  // no hyphen: not the construct
  EXPECT_FALSE(CspCarriesNonce("noncex-a"));
  EXPECT_TRUE(CspCarriesNonce("'nonce-'"));
}

// ------------------------------------------------- request-dependent headers

TEST(HeadersSidecarTest, RequestDependentHeadersAreNeverCarried) {
  // Each of these varies with who is asking. The gate is fail-closed, so the
  // proof is not "they are on a denylist" — nothing is carried unless it is
  // positively recognised — but the class is worth pinning by name.
  for (std::string_view name : {
           "Set-Cookie",
           "Set-Cookie2",
           "WWW-Authenticate",
           "Proxy-Authenticate",
           "Authorization",
           "Content-Location",
           "X-Request-Id",
           "Content-Security-Policy-Report-Only",
           "Access-Control-Allow-Credentials",
           "CF-Ray",
       }) {
    const Fields fields = {{"Cache-Control", "max-age=600"},
                           {"Content-Type", "text/css"},
                           {name, "whatever"}};
    const auto result = Classify(fields);
    EXPECT_EQ(result.verdict, SidecarVerdict::kFallThrough) << name;
    EXPECT_TRUE(result.payload.empty()) << name;
  }
}

TEST(HeadersSidecarTest, AllowlistIsAClosedSetNotADefaultAllow) {
  const Fields fields = {{"X-Anything-At-All", "1"}};
  const auto result = Classify(fields);
  ASSERT_EQ(result.verdict, SidecarVerdict::kFallThrough);
  ASSERT_EQ(result.reasons.size(), 1u);
  EXPECT_EQ(result.reasons[0], "unreconstructible-header:x-anything-at-all");
}

TEST(HeadersSidecarTest, LinkAndStrictTransportSecurityAreCarried) {
  const Fields fields = {{"Cache-Control", "max-age=600"},
                         {"Content-Type", "text/css"},
                         {"Link", "</style.css>; rel=preload; as=style"},
                         {"Strict-Transport-Security", "max-age=63072000"}};
  const auto result = Classify(fields);
  EXPECT_EQ(result.verdict, SidecarVerdict::kSwapEligible);
  EXPECT_EQ(result.fields, 2u);
  EXPECT_TRUE(result.reasons.empty());
}

TEST(HeadersSidecarTest, LinkCarryingANonceIsNeverStored) {
  const Fields fields = {
      {"Cache-Control", "max-age=600"},
      {"Link", "</a.js>; rel=preload; as=script; Nonce=abc"}};
  const auto result = Classify(fields);
  EXPECT_EQ(result.verdict, SidecarVerdict::kNeverOptimized);
  EXPECT_TRUE(result.payload.empty());
  ASSERT_EQ(result.reasons.size(), 1u);
  EXPECT_EQ(result.reasons[0], "nonce-link");
}

TEST(HeadersSidecarTest, LinkNonceInAnySpellingIsNeverStored) {
  for (std::string_view value : {
           "</a.js>; rel=preload; as=script; nonce = \"x\"",
           "</a.js>; rel=preload; as=script; nonce*=UTF-8''x",
           "</a.js>; rel=preload; as=script; NONCE=x",
           "</a.js>; rel=preload; title=\"<\"; nonce=abc",
           "<x>; rel=preload; anchor=\"<y\"; nonce=z",
           "</a.js; nonce=abc",
       }) {
    const Fields fields = {{"Cache-Control", "max-age=600"}, {"Link", value}};
    const auto result = Classify(fields);
    EXPECT_EQ(result.verdict, SidecarVerdict::kNeverOptimized) << value;
    ASSERT_EQ(result.reasons.size(), 1u) << value;
    EXPECT_EQ(result.reasons[0], "nonce-link") << value;
  }
}

TEST(HeadersSidecarTest, ALinkTargetContainingTheWordNonceIsCarried) {
  const Fields fields = {
      {"Cache-Control", "max-age=600"},
      {"Link", "</nonce-helper.js>; rel=preload; as=script"}};
  const auto result = Classify(fields);
  EXPECT_EQ(result.verdict, SidecarVerdict::kSwapEligible);
  EXPECT_EQ(result.fields, 1u);
}

TEST(HeadersSidecarTest, ReportToIsRefused) {
  const Fields fields = {{"Cache-Control", "max-age=600"},
                         {"Report-To", "{\"group\":\"cf-nel\"}"}};
  const auto result = Classify(fields);
  EXPECT_EQ(result.verdict, SidecarVerdict::kFallThrough);
  ASSERT_EQ(result.reasons.size(), 1u);
  EXPECT_EQ(result.reasons[0], "unreconstructible-header:report-to");
}

TEST(HeadersSidecarTest, ACdnRequestIdIsRefused) {
  const Fields fields = {{"Cache-Control", "max-age=600"},
                         {"CF-Ray", "8a1b2c3d4e5f-AMS"}};
  const auto result = Classify(fields);
  EXPECT_EQ(result.verdict, SidecarVerdict::kFallThrough);
  ASSERT_EQ(result.reasons.size(), 1u);
  EXPECT_EQ(result.reasons[0], "unreconstructible-header:cf-ray");
}

TEST(HeadersSidecarTest, TheAllowlistIsExactlyTheEnumeratedSet) {
  // The list is normative; a change here is a change to what may be stored,
  // and should be as loud as changing an entry format.
  const std::vector<std::string_view> expected = {
      "content-security-policy",
      "access-control-allow-origin",
      "x-content-type-options",
      "referrer-policy",
      "cross-origin-opener-policy",
      "cross-origin-embedder-policy",
      "cross-origin-resource-policy",
      "permissions-policy",
      "vary",
      "link",
      "strict-transport-security",
      "x-frame-options",
      "x-xss-protection",
      "x-permitted-cross-domain-policies",
      "origin-agent-cluster",
      "x-dns-prefetch-control",
      "timing-allow-origin",
      "access-control-expose-headers",
      "reporting-endpoints",
      "nel",
      "x-powered-by",
      "x-pingback",
      "x-robots-tag",
      "content-language",
      "content-disposition",
      "alt-svc",
      "allow",
  };
  const auto actual = HeadersSidecarAllowlist();
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(actual[i], expected[i]);
  }
}

TEST(HeadersSidecarTest, HeadersTheEntryMetadataAlreadyHoldsAreNotCarried) {
  // Swap-eligible, but there is nothing for the sidecar to hold: the entry
  // metadata reproduces all of these, and an empty block would be an entry
  // asserting a class membership that means nothing.
  const Fields fields = {
      {"Cache-Control", "max-age=600"},
      {"Content-Type", "text/css"},
      {"Content-Length", "42"},
      {"ETag", "\"abc\""},
      {"Last-Modified", "Mon, 10 Aug 2026 00:00:00 GMT"},
      {"Expires", "Tue, 11 Aug 2026 00:00:00 GMT"},
      {"Date", "Mon, 10 Aug 2026 00:00:00 GMT"},
      {"Server", "nginx"},
  };
  const auto result = Classify(fields);
  EXPECT_EQ(result.verdict, SidecarVerdict::kSwapEligible);
  EXPECT_TRUE(result.payload.empty());
  EXPECT_EQ(result.fields, 0u);
}

TEST(HeadersSidecarTest, AgeAndAcceptRangesAreStampedNotFallThrough) {
  // Both are stamped by the serving stack on every response, so a CDN-fronted
  // origin (Age) or a static origin (Accept-Ranges) stays swap-eligible with
  // nothing to carry.
  const Fields fields = {
      {"Age", "37"},
      {"Accept-Ranges", "bytes"},
  };
  const auto result = Classify(fields);
  EXPECT_EQ(result.verdict, SidecarVerdict::kSwapEligible);
  EXPECT_TRUE(result.payload.empty());
  EXPECT_EQ(result.fields, 0u);
  EXPECT_TRUE(result.reasons.empty());

  bool age = false, ranges = false;
  for (std::string_view n : HeadersStampedPerResponse()) {
    age |= (n == "age");
    ranges |= (n == "accept-ranges");
  }
  EXPECT_TRUE(age);
  EXPECT_TRUE(ranges);
}

TEST(HeadersSidecarTest, AgeDoesNotDisturbCarriedHeaders) {
  const Fields fields = {
      {"Age", "5"},
      {"Access-Control-Allow-Origin", "*"},
      {"Accept-Ranges", "bytes"},
  };
  const auto result = Classify(fields);
  EXPECT_EQ(result.verdict, SidecarVerdict::kSwapEligible);
  EXPECT_EQ(result.fields, 1u);
}

// -------------------------------------------------------------- Vary, F4

TEST(HeadersSidecarTest, TheOriginsActualVaryStringIsWhatIsStored) {
  // Not the tokens the cache understood, and not a re-synthesised list: the
  // bytes the origin sent, spacing and case included.
  const Fields fields = {
      {"Vary", "Accept-Encoding,   ACCEPT-ENCODING ,accept-encoding"}};
  const auto result = Classify(fields);
  ASSERT_EQ(result.verdict, SidecarVerdict::kSwapEligible) << result.reasons[0];
  const auto parsed = RoundTrip(result);
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0].first, "Vary");
  EXPECT_EQ(parsed[0].second,
            "Accept-Encoding,   ACCEPT-ENCODING ,accept-encoding");
}

TEST(HeadersSidecarTest,
     SeveralVaryLinesAreCarriedSeparatelyAndJudgedTogether) {
  // RFC 9110 §5.3: they are one list for the storability question, and two
  // lines for the relay question. Both halves matter, so both are checked.
  const Fields storable = {{"Vary", "Accept-Encoding"},
                           {"vary", "accept-encoding"}};
  const auto ok = Classify(storable);
  ASSERT_EQ(ok.verdict, SidecarVerdict::kSwapEligible) << ok.reasons[0];
  const auto parsed = RoundTrip(ok);
  ASSERT_EQ(parsed.size(), 2u);
  EXPECT_EQ(parsed[0].first, "Vary");
  EXPECT_EQ(parsed[1].first, "vary");

  // The combination is what is judged: each line alone is storable, the pair
  // is not, and a per-line check would wave it through.
  const Fields combined = {{"Vary", "Accept-Encoding"}, {"Vary", "Cookie"}};
  EXPECT_EQ(Classify(combined).verdict, SidecarVerdict::kFallThrough);
}

TEST(HeadersSidecarTest, VaryStarIsRefusedByTheSharedStorePredicate) {
  const Fields fields = {{"Vary", "*"}};
  const auto result = Classify(fields);
  EXPECT_EQ(result.verdict, SidecarVerdict::kFallThrough);
  EXPECT_TRUE(result.payload.empty());
  // The refusal is the shared predicate's, not a second copy of it here.
  EXPECT_TRUE(VaryUncacheable("*"));
}

TEST(HeadersSidecarTest, VaryOriginClosesTheReflectedAcaoCase) {
  // An ACAO reflected from the request Origin is request-dependent, and a
  // response that reflects declares it. That response is not storable at all,
  // so the sidecar never sees the reflected value — the two gates compose
  // rather than each having to recognise reflection on its own.
  const Fields fields = {{"Access-Control-Allow-Origin", "https://a.example"},
                         {"Vary", "Origin"}};
  const auto result = Classify(fields);
  EXPECT_EQ(result.verdict, SidecarVerdict::kFallThrough);
  EXPECT_TRUE(result.payload.empty());
  EXPECT_TRUE(VaryUncacheable("Origin"));
}

TEST(HeadersSidecarTest, MixedCaseVaryNameIsStillTheVaryHeader) {
  const Fields fields = {{"vArY", "Cookie"}};
  EXPECT_EQ(Classify(fields).verdict, SidecarVerdict::kFallThrough);
}

// ------------------------------------------------------------------ bounds

TEST(HeadersSidecarTest, AnOversizedBlockIsAFallThroughNotATruncation) {
  const std::string huge(kMaxHeadersSidecarPayloadBytes, 'x');
  const Fields fields = {{"Content-Security-Policy", huge},
                         {"Referrer-Policy", "no-referrer"}};
  const auto result = Classify(fields);
  EXPECT_EQ(result.verdict, SidecarVerdict::kFallThrough);
  EXPECT_TRUE(result.payload.empty());
  ASSERT_FALSE(result.reasons.empty());
  EXPECT_EQ(result.reasons.back(), "sidecar-too-large");
}

TEST(HeadersSidecarTest, AValueOverTheEncodingsFieldLimitIsAFallThrough) {
  const std::string huge(kMaxHeadersSidecarValueBytes + 1, 'x');
  const Fields fields = {{"Permissions-Policy", huge}};
  const auto result = Classify(fields);
  EXPECT_EQ(result.verdict, SidecarVerdict::kFallThrough);
  ASSERT_EQ(result.reasons.size(), 1u);
  EXPECT_EQ(result.reasons[0], "field-too-large:permissions-policy");
}

TEST(HeadersSidecarTest, ABlockAtTheCeilingStillStores) {
  // The ceiling is a ceiling, not an off-by-one refusal of everything large.
  const size_t overhead = 3 + 3 + std::string_view("Permissions-Policy").size();
  const std::string value(kMaxHeadersSidecarPayloadBytes - overhead, 'x');
  const Fields fields = {{"Permissions-Policy", value}};
  const auto result = Classify(fields);
  ASSERT_EQ(result.verdict, SidecarVerdict::kSwapEligible) << result.reasons[0];
  EXPECT_EQ(result.payload.size(), kMaxHeadersSidecarPayloadBytes);
}

// ------------------------------------------------------------------- codec

TEST(HeadersSidecarTest, AnEmptyOrShortPayloadIsAMiss) {
  EXPECT_FALSE(ParseHeadersSidecar("").has_value());
  EXPECT_FALSE(ParseHeadersSidecar(std::string_view("\x01", 1)).has_value());
  EXPECT_FALSE(
      ParseHeadersSidecar(std::string_view("\x01\x00", 2)).has_value());
}

TEST(HeadersSidecarTest, AnUnknownFormatVersionStopsTheReaderRatherThanGuess) {
  const Fields fields = {{"Referrer-Policy", "no-referrer"}};
  std::string payload = Classify(fields).payload;
  ASSERT_FALSE(payload.empty());
  ASSERT_TRUE(ParseHeadersSidecar(payload).has_value());
  payload[0] = static_cast<char>(kHeadersSidecarPairedFormatVersion + 1);
  EXPECT_FALSE(ParseHeadersSidecar(payload).has_value());
  payload[0] = 0;
  EXPECT_FALSE(ParseHeadersSidecar(payload).has_value());
}

TEST(HeadersSidecarTest, TruncatedAndOverlongPayloadsAreMisses) {
  const Fields fields = {{"Referrer-Policy", "no-referrer"},
                         {"X-Content-Type-Options", "nosniff"}};
  const std::string payload = Classify(fields).payload;
  ASSERT_FALSE(payload.empty());
  for (size_t cut = 1; cut < payload.size(); ++cut) {
    EXPECT_FALSE(ParseHeadersSidecar(payload.substr(0, cut)).has_value())
        << "truncated to " << cut;
  }
  EXPECT_FALSE(ParseHeadersSidecar(payload + "trailing").has_value());
}

TEST(HeadersSidecarTest, AZeroLengthNameIsAMiss) {
  // 1 field, name length 0 — a shape the writer never produces, so a reader
  // that accepts it is accepting somebody else's bytes.
  const std::string payload("\x01\x00\x01\x00\x00\x00", 6);
  EXPECT_FALSE(ParseHeadersSidecar(payload).has_value());
}

TEST(HeadersSidecarTest, TheClassCostsNoEntryMetadataVersionOfItsOwn) {
  // The class carries its OWN payload version, which is the whole point of
  // that byte: adding it did not move the entry-metadata format version, and
  // must not. An entry format version is a cache-wide event — every entry on
  // every volume — and a new entry class is not a reason to spend one.
  EXPECT_EQ(AlternateMetadata::kCurrentVersion, 9);
  EXPECT_EQ(kHeadersSidecarFormatVersion, 1);
}

TEST(HeadersSidecarTest, ThePayloadStartsWithItsFormatVersion) {
  const Fields fields = {{"Referrer-Policy", "no-referrer"}};
  const std::string payload = Classify(fields).payload;
  ASSERT_FALSE(payload.empty());
  EXPECT_EQ(static_cast<unsigned char>(payload[0]),
            kHeadersSidecarFormatVersion);
}

// ---------------------------------------------------------------- pairing
// (payload format 2)

std::array<std::byte, kHeadersSidecarPairingHashBytes> TestHash(uint8_t seed) {
  std::array<std::byte, kHeadersSidecarPairingHashBytes> h{};
  for (size_t i = 0; i < h.size(); ++i) {
    h[i] = static_cast<std::byte>(seed + i);
  }
  return h;
}

TEST(HeadersSidecarTest, APairedBlockRoundTripsItsFieldsAndItsHash) {
  const Fields fields = {{"Referrer-Policy", "  no-referrer "},
                         {"X-Content-Type-Options", "nosniff"}};
  const std::string format1 = Classify(fields).payload;
  ASSERT_FALSE(format1.empty());
  const auto hash = TestHash(7);
  const std::string paired = PairHeadersSidecarPayload(format1, hash);

  EXPECT_EQ(static_cast<unsigned char>(paired[0]),
            kHeadersSidecarPairedFormatVersion);
  EXPECT_EQ(paired.size(), format1.size() + kHeadersSidecarPairingHashBytes);
  auto block = ParseHeadersSidecarBlock(paired);
  ASSERT_TRUE(block.has_value());
  ASSERT_TRUE(block->pairing_hash.has_value());
  EXPECT_EQ(*block->pairing_hash, hash);
  ASSERT_EQ(block->fields.size(), 2u);
  EXPECT_EQ(block->fields[0].name, "Referrer-Policy");
  EXPECT_EQ(block->fields[0].value, "  no-referrer ");
  EXPECT_EQ(block->fields[1].name, "X-Content-Type-Options");
  // The fields-only reader cannot check a pairing, so it reads a paired
  // block as absent (fail closed); format 1 still parses there.
  EXPECT_FALSE(ParseHeadersSidecar(paired).has_value());
  EXPECT_TRUE(ParseHeadersSidecar(format1).has_value());
}

TEST(HeadersSidecarTest, AFormat1BlockParsesWithNoPairingHash) {
  const std::string format1 =
      Classify(Fields{{"Referrer-Policy", "no-referrer"}}).payload;
  auto block = ParseHeadersSidecarBlock(format1);
  ASSERT_TRUE(block.has_value());
  EXPECT_FALSE(block->pairing_hash.has_value());
  EXPECT_EQ(block->fields.size(), 1u);
}

TEST(HeadersSidecarTest, APairedBlockMayCarryNoField) {
  const auto hash = TestHash(1);
  const std::string paired = PairHeadersSidecarPayload("", hash);
  EXPECT_EQ(paired.size(), 1 + kHeadersSidecarPairingHashBytes + 2);
  auto block = ParseHeadersSidecarBlock(paired);
  ASSERT_TRUE(block.has_value());
  EXPECT_TRUE(block->fields.empty());
  ASSERT_TRUE(block->pairing_hash.has_value());
  EXPECT_EQ(*block->pairing_hash, hash);
}

TEST(HeadersSidecarTest, AMalformedPairedBlockIsAMiss) {
  const std::string format1 =
      Classify(Fields{{"Referrer-Policy", "no-referrer"}}).payload;
  const std::string paired = PairHeadersSidecarPayload(format1, TestHash(3));
  for (size_t cut = 1; cut < paired.size(); ++cut) {
    EXPECT_FALSE(ParseHeadersSidecarBlock(paired.substr(0, cut)).has_value())
        << "truncated to " << cut;
  }
  EXPECT_FALSE(ParseHeadersSidecarBlock(paired + "x").has_value());
  // An all-zero hash is the unbound value; no writer pairs with it.
  std::string zero = paired;
  for (size_t i = 1; i <= kHeadersSidecarPairingHashBytes; ++i) zero[i] = 0;
  EXPECT_FALSE(ParseHeadersSidecarBlock(zero).has_value());
  EXPECT_FALSE(ParseHeadersSidecar(zero).has_value());
}

TEST(HeadersSidecarTest, ThePairedFormatStaysOutsideTheMetadataRange) {
  EXPECT_EQ(kHeadersSidecarPairedFormatVersion, 2);
  EXPECT_NE(kHeadersSidecarPairedFormatVersion, kHeadersSidecarFormatVersion);
  EXPECT_LT(kHeadersSidecarPairedFormatVersion, 3);
}

}  // namespace
}  // namespace pagespeed
