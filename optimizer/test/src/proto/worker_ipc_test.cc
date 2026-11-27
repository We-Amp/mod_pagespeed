// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Unit tests for CacheNotification serialization

#include "src/proto/worker_ipc.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

// ============================================================================
// Scheme validation
// ============================================================================

TEST(SchemeValidationTest, AcceptsHttp) { EXPECT_TRUE(ValidateScheme("http")); }

TEST(SchemeValidationTest, AcceptsHttps) {
  EXPECT_TRUE(ValidateScheme("https"));
}

TEST(SchemeValidationTest, RejectsUppercase) {
  EXPECT_FALSE(ValidateScheme("HTTP"));
  EXPECT_FALSE(ValidateScheme("HTTPS"));
  EXPECT_FALSE(ValidateScheme("Https"));
}

TEST(SchemeValidationTest, RejectsOther) {
  EXPECT_FALSE(ValidateScheme("ftp"));
  EXPECT_FALSE(ValidateScheme("ws"));
  EXPECT_FALSE(ValidateScheme(""));
  EXPECT_FALSE(ValidateScheme("http "));
  EXPECT_FALSE(ValidateScheme("https\n"));
}

TEST(SchemeWireTest, RoundTrip) {
  EXPECT_EQ("http", SchemeFromWire(SchemeToWire("http")));
  EXPECT_EQ("https", SchemeFromWire(SchemeToWire("https")));
}

TEST(SchemeWireTest, WireValues) {
  EXPECT_EQ(IpcScheme::kHttp, SchemeToWire("http"));
  EXPECT_EQ(IpcScheme::kHttps, SchemeToWire("https"));
  EXPECT_EQ(static_cast<uint8_t>(IpcScheme::kHttp), 0x01);
  EXPECT_EQ(static_cast<uint8_t>(IpcScheme::kHttps), 0x02);
}

// ============================================================================
// Round-trip tests
// ============================================================================

TEST(CacheNotificationTest, RoundTripBasic) {
  CacheNotification original;
  original.url = "/page.html";
  original.hostname = "example.com";
  original.scheme = "https";
  original.content_type = ContentType::kHtml;
  original.capability_mask = 0x08;

  std::vector<char> wire = original.Serialize();
  ASSERT_FALSE(wire.empty());

  CacheNotification decoded;
  ASSERT_TRUE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));

  EXPECT_EQ(original.url, decoded.url);
  EXPECT_EQ(original.hostname, decoded.hostname);
  EXPECT_EQ(original.scheme, decoded.scheme);
  EXPECT_EQ(original.content_type, decoded.content_type);
  EXPECT_EQ(original.capability_mask, decoded.capability_mask);
  EXPECT_FALSE(decoded.agent_request);  // default
}

TEST(CacheNotificationTest, RoundTripAgentRequest) {
  // Wire v4: the agent_request intent bit round-trips.
  CacheNotification original;
  original.url = "/page.html";
  original.hostname = "example.com";
  original.scheme = "https";
  original.content_type = ContentType::kHtml;
  original.capability_mask = 0x08;
  original.agent_request = true;

  std::vector<char> wire = original.Serialize();
  ASSERT_FALSE(wire.empty());

  CacheNotification decoded;
  ASSERT_TRUE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));
  EXPECT_TRUE(decoded.agent_request);
}

TEST(CacheNotificationTest, RoundTripHttp) {
  CacheNotification original;
  original.url = "/page.html";
  original.hostname = "example.com";
  original.scheme = "http";
  original.content_type = ContentType::kHtml;
  original.capability_mask = 0x08;

  std::vector<char> wire = original.Serialize();
  ASSERT_FALSE(wire.empty());

  CacheNotification decoded;
  ASSERT_TRUE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));

  EXPECT_EQ("http", decoded.scheme);
}

TEST(CacheNotificationTest, RoundTripAllContentTypes) {
  for (uint8_t ct = 0; ct <= static_cast<uint8_t>(ContentType::kOther); ++ct) {
    CacheNotification original;
    original.url = "/test";
    original.hostname = "h";
    original.scheme = "https";
    original.content_type = static_cast<ContentType>(ct);
    original.capability_mask = ct;

    std::vector<char> wire = original.Serialize();

    CacheNotification decoded;
    ASSERT_TRUE(CacheNotification::Deserialize(
        std::string_view(wire.data(), wire.size()), &decoded));
    EXPECT_EQ(original.content_type, decoded.content_type);
    EXPECT_EQ(original.capability_mask, decoded.capability_mask);
    EXPECT_EQ("https", decoded.scheme);
  }
}

TEST(CacheNotificationTest, RoundTripEmptyHostname) {
  CacheNotification original;
  original.url = "/img.png";
  original.hostname = "";
  original.scheme = "https";
  original.content_type = ContentType::kImage;
  original.capability_mask = 0x01;

  std::vector<char> wire = original.Serialize();

  CacheNotification decoded;
  ASSERT_TRUE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));
  EXPECT_EQ("", decoded.hostname);
  EXPECT_EQ(original.url, decoded.url);
  EXPECT_EQ("https", decoded.scheme);
}

TEST(CacheNotificationTest, RoundTripLargeCapabilityMask) {
  CacheNotification original;
  original.url = "/x";
  original.hostname = "h";
  original.scheme = "http";
  original.content_type = ContentType::kCss;
  original.capability_mask = 0xFFFFFFFF;

  std::vector<char> wire = original.Serialize();

  CacheNotification decoded;
  ASSERT_TRUE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));
  EXPECT_EQ(0xFFFFFFFF, decoded.capability_mask);
  EXPECT_EQ("http", decoded.scheme);
}

TEST(CacheNotificationTest, RoundTripLongUrl) {
  CacheNotification original;
  original.url = std::string(8192, 'x');
  original.hostname = "example.com";
  original.scheme = "https";
  original.content_type = ContentType::kOther;
  original.capability_mask = 0;

  std::vector<char> wire = original.Serialize();

  CacheNotification decoded;
  ASSERT_TRUE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));
  EXPECT_EQ(original.url, decoded.url);
  EXPECT_EQ("https", decoded.scheme);
}

// ============================================================================
// Scheme validation in Serialize
// ============================================================================

TEST(CacheNotificationTest, SerializeRejectsInvalidScheme) {
  CacheNotification n;
  n.url = "/x";
  n.hostname = "h";
  n.content_type = ContentType::kHtml;
  n.capability_mask = 0;

  n.scheme = "HTTP";
  EXPECT_TRUE(n.Serialize().empty());

  n.scheme = "ftp";
  EXPECT_TRUE(n.Serialize().empty());

  n.scheme = "";
  EXPECT_TRUE(n.Serialize().empty());

  n.scheme = "https ";
  EXPECT_TRUE(n.Serialize().empty());
}

TEST(CacheNotificationTest, SerializeRejectsEmptyScheme) {
  CacheNotification n;
  n.url = "/x";
  n.hostname = "h";
  n.scheme = "";
  n.content_type = ContentType::kHtml;
  n.capability_mask = 0;
  EXPECT_TRUE(n.Serialize().empty());
}

// ============================================================================
// Version handling
// ============================================================================

TEST(CacheNotificationTest, DeserializeRejectsV2Format) {
  // Simulate a v2 message (no version byte — starts with total_length).
  // A v2 message has no leading version byte, so the first byte
  // would be part of the 4-byte total_length field (likely 0x00).
  // v3 expects version byte == 3; anything else is rejected.
  CacheNotification n;
  n.url = "/x";
  n.hostname = "h";
  n.scheme = "https";
  n.content_type = ContentType::kHtml;
  n.capability_mask = 0;

  std::vector<char> wire = n.Serialize();
  ASSERT_FALSE(wire.empty());

  // Version byte is at offset 4 (after the 4-byte total_length).
  // Corrupt version byte to 0 (simulating v2 format).
  wire[4] = 0;
  CacheNotification decoded;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));

  // Corrupt version byte to 2.
  wire[4] = 2;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));

  // Corrupt version byte to 3, 4, then 5 (all superseded — v6 is current).
  wire[4] = 3;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));

  wire[4] = 4;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));

  wire[4] = 5;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));

  // Corrupt version byte to 7 (future).
  wire[4] = 7;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));
}

// Version skew is REPORTED as version skew, not as "something went wrong".
//
// The distinction is the point of the out-param: a peer on another wire
// version and a peer sending corrupt bytes both produce "false" and both
// produce "nothing gets optimized", and they have nothing in common as fixes.
// Every version other than the current one — below AND above — must land in
// the same arm, because a peer NEWER than this build is the case that will
// actually occur once notify senders ship on their own cadence, and it is the
// case where guessing at the bytes would be most tempting and most wrong.
TEST(CacheNotificationTest, DeserializeReportsVersionMismatchWithPeerVersion) {
  CacheNotification n;
  n.url = "/x";
  n.hostname = "h";
  n.scheme = "https";
  n.content_type = ContentType::kHtml;
  n.capability_mask = 0;

  const std::vector<char> good = n.Serialize();
  ASSERT_FALSE(good.empty());

  // Older, newer, and outright garbage version bytes.  0 stands in for a
  // pre-version-byte sender, 1 and 3 for superseded peers, 5 for the peer one
  // version ahead of this build, 0x7F and 0xFF for bytes that are not a
  // version at all.
  for (uint8_t peer : {uint8_t{0}, uint8_t{1}, uint8_t{3}, uint8_t{5},
                       uint8_t{0x7F}, uint8_t{0xFF}}) {
    if (peer == kIpcVersion) continue;
    SCOPED_TRACE(static_cast<unsigned>(peer));
    std::vector<char> wire = good;
    wire[4] = static_cast<char>(peer);

    CacheNotification decoded;
    IpcRejection rejection;
    EXPECT_FALSE(CacheNotification::Deserialize(
        std::string_view(wire.data(), wire.size()), &decoded, &rejection));
    EXPECT_EQ(rejection.reason, IpcRejectReason::kVersionMismatch);
    EXPECT_EQ(rejection.peer_version, peer)
        << "the receive path can only name the skewed peer if the version it "
           "sent is reported back";
    // Nothing may have been read past the version byte: a rejected
    // notification must not half-populate the output.
    EXPECT_TRUE(decoded.url.empty());
    EXPECT_TRUE(decoded.hostname.empty());
  }
}

// A version this build DOES speak, with a damaged body, is a different fault
// and says so.  Without this half, "rejected_version" could silently absorb
// every parse failure and mean nothing.
TEST(CacheNotificationTest, DeserializeReportsMalformedSeparately) {
  CacheNotification n;
  n.url = "/x";
  n.hostname = "h";
  n.scheme = "https";
  n.content_type = ContentType::kHtml;
  n.capability_mask = 0;

  std::vector<char> wire = n.Serialize();
  ASSERT_FALSE(wire.empty());
  // Corrupt the scheme byte (second from the end in v4) — version still v4.
  wire[wire.size() - 2] = static_cast<char>(0x7E);

  CacheNotification decoded;
  IpcRejection rejection;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded, &rejection));
  EXPECT_EQ(rejection.reason, IpcRejectReason::kMalformed);
  EXPECT_EQ(rejection.peer_version, kIpcVersion);

  // A frame too short to even carry a version byte is truncation, not skew:
  // there is no peer version to blame.
  IpcRejection short_rejection;
  CacheNotification short_decoded;
  EXPECT_FALSE(
      CacheNotification::Deserialize("", &short_decoded, &short_rejection));
  EXPECT_EQ(short_rejection.reason, IpcRejectReason::kTruncated);
  EXPECT_EQ(short_rejection.peer_version, 0);
}

// A well-formed current-version message reports no rejection at all, so the
// out-param cannot be read as "always populated with something".
TEST(CacheNotificationTest, DeserializeSuccessLeavesRejectionClear) {
  CacheNotification n;
  n.url = "/ok.css";
  n.hostname = "example.com";
  n.scheme = "https";
  n.content_type = ContentType::kCss;
  n.capability_mask = 0;

  std::vector<char> wire = n.Serialize();
  ASSERT_FALSE(wire.empty());

  CacheNotification decoded;
  IpcRejection rejection;
  rejection.reason = IpcRejectReason::kMalformed;  // Poison it first.
  rejection.peer_version = 99;
  EXPECT_TRUE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded, &rejection));
  EXPECT_EQ(rejection.reason, IpcRejectReason::kNone);
  EXPECT_EQ(decoded.url, "/ok.css");
}

TEST(CacheNotificationTest, VersionByteAfterFramingHeader) {
  CacheNotification n;
  n.url = "/x";
  n.hostname = "h";
  n.scheme = "https";
  n.content_type = ContentType::kHtml;
  n.capability_mask = 0;

  std::vector<char> wire = n.Serialize();
  ASSERT_FALSE(wire.empty());
  // Version byte is at offset 4, after the 4-byte total_length header.
  EXPECT_EQ(static_cast<uint8_t>(wire[4]), kIpcVersion);
}

// ============================================================================
// Malformed input
// ============================================================================

TEST(CacheNotificationTest, DeserializeEmpty) {
  CacheNotification decoded;
  EXPECT_FALSE(CacheNotification::Deserialize("", &decoded));
}

TEST(CacheNotificationTest, DeserializeTooShort) {
  CacheNotification decoded;
  EXPECT_FALSE(CacheNotification::Deserialize(std::string_view("\x03\x00", 2),
                                              &decoded));
}

TEST(CacheNotificationTest, DeserializeTruncatedAfterTotalLen) {
  // 4-byte total_length says 100 bytes follow, version=3, but only 5 bytes
  // total (1 byte of payload). Fails because buffer is too short.
  char buf[5] = {0, 0, 0, 100, 3};
  CacheNotification decoded;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(buf, sizeof(buf)), &decoded));
}

TEST(CacheNotificationTest, DeserializeTruncatedAfterUrl) {
  // Serialize a valid notification, then truncate the buffer.
  CacheNotification original;
  original.url = "/page.html";
  original.hostname = "h";
  original.scheme = "https";
  original.content_type = ContentType::kHtml;
  original.capability_mask = 0;

  std::vector<char> wire = original.Serialize();
  // Truncate partway through hostname_length field.
  // v3: 4 (total_len) + 1 (version) + 4 (url_len) + url + 2 bytes into host_len
  size_t cut = 4 + 1 + 4 + original.url.size() + 2;

  CacheNotification decoded;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), cut), &decoded));
}

TEST(CacheNotificationTest, DeserializeHostnameTooLong) {
  // Serialize valid, then patch hostname_length to exceed limit.
  CacheNotification original;
  original.url = "/x";
  original.hostname = "h";
  original.scheme = "https";
  original.content_type = ContentType::kHtml;
  original.capability_mask = 0;

  std::vector<char> wire = original.Serialize();

  // Hostname length field is at offset: 4 (total_len) + 1 (version) +
  //   4 (url_len) + url_len
  size_t host_len_offset = 4 + 1 + 4 + original.url.size();
  // Write 513 (> kMaxHostnameLength=512) in big-endian
  wire[host_len_offset + 0] = 0;
  wire[host_len_offset + 1] = 0;
  wire[host_len_offset + 2] = 2;
  wire[host_len_offset + 3] = 1;  // 0x0201 = 513

  CacheNotification decoded;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));
}

TEST(CacheNotificationTest, DeserializeInvalidContentType) {
  CacheNotification original;
  original.url = "/x";
  original.hostname = "h";
  original.scheme = "https";
  original.content_type = ContentType::kHtml;
  original.capability_mask = 0;

  std::vector<char> wire = original.Serialize();

  // Content type byte: 4 (total_len) + 1 (version) + 4 (url_len) +
  //   url_len + 4 (host_len) + host_len
  size_t ct_offset =
      4 + 1 + 4 + original.url.size() + 4 + original.hostname.size();
  wire[ct_offset] =
      static_cast<char>(static_cast<uint8_t>(ContentType::kOther) + 1);

  CacheNotification decoded;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));
}

TEST(CacheNotificationTest, DeserializeInvalidScheme) {
  CacheNotification original;
  original.url = "/x";
  original.hostname = "h";
  original.scheme = "https";
  original.content_type = ContentType::kHtml;
  original.capability_mask = 0;

  std::vector<char> wire = original.Serialize();

  // Tail after the scheme byte: agent_request(1) + option_context_length(4)
  // + option_context(0 here) + option_signature_length(1) + signature(0 here)
  // + site_length(1, v6) + site(0 here) + flags(1, v6).
  const size_t scheme_idx = wire.size() - 9;
  ASSERT_EQ(0x02u, static_cast<unsigned char>(wire[scheme_idx]))
      << "scheme byte is not where this test thinks it is";
  wire[scheme_idx] = 0x00;  // Invalid scheme byte

  CacheNotification decoded;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));

  wire[scheme_idx] = 0x03;  // Invalid scheme byte
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));

  wire[scheme_idx] = static_cast<char>(0xFF);  // Invalid scheme byte
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));
}

TEST(CacheNotificationTest, DeserializeTruncatedBeforeScheme) {
  CacheNotification original;
  original.url = "/x";
  original.hostname = "h";
  original.scheme = "https";
  original.content_type = ContentType::kHtml;
  original.capability_mask = 0;

  std::vector<char> wire = original.Serialize();
  // Remove last byte (scheme).
  CacheNotification decoded;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size() - 1), &decoded));
}

// ============================================================================
// Wire format verification
// ============================================================================

TEST(CacheNotificationTest, BigEndianEncoding) {
  CacheNotification original;
  original.url = "AB";
  original.hostname = "C";
  original.scheme = "https";
  original.content_type = ContentType::kImage;  // 3
  original.capability_mask = 0x01020304;

  std::vector<char> wire = original.Serialize();

  auto u = [](char c) { return static_cast<unsigned char>(c); };

  // Bytes 0-3: total_length (not including itself)
  // = 1(version) + 4(url_len) + 2(url) + 4(host_len) + 1(host) +
  //   1(ct) + 4(mask) + 1(scheme) + 1(agent_request, v4)
  //   + 4(option_context_len, v5) + 0(option_context)
  //   + 1(option_signature_len, v5) + 0(option_signature)
  //   + 1(site_len, v6) + 0(site) + 1(flags, v6) = 26
  EXPECT_EQ(0u, u(wire[0]));
  EXPECT_EQ(0u, u(wire[1]));
  EXPECT_EQ(0u, u(wire[2]));
  EXPECT_EQ(26u, u(wire[3]));

  // Byte 4: version = 6
  EXPECT_EQ(6u, u(wire[4]));

  // url_length = 2
  EXPECT_EQ(0u, u(wire[5]));
  EXPECT_EQ(0u, u(wire[6]));
  EXPECT_EQ(0u, u(wire[7]));
  EXPECT_EQ(2u, u(wire[8]));

  // url = "AB"
  EXPECT_EQ('A', wire[9]);
  EXPECT_EQ('B', wire[10]);

  // hostname_length = 1
  EXPECT_EQ(0u, u(wire[11]));
  EXPECT_EQ(0u, u(wire[12]));
  EXPECT_EQ(0u, u(wire[13]));
  EXPECT_EQ(1u, u(wire[14]));

  // hostname = "C"
  EXPECT_EQ('C', wire[15]);

  // content_type = 3 (kImage)
  EXPECT_EQ(3u, u(wire[16]));

  // capability_mask = 0x01020304
  EXPECT_EQ(0x01u, u(wire[17]));
  EXPECT_EQ(0x02u, u(wire[18]));
  EXPECT_EQ(0x03u, u(wire[19]));
  EXPECT_EQ(0x04u, u(wire[20]));

  // scheme = 0x02 (kHttps)
  EXPECT_EQ(0x02u, u(wire[21]));

  // agent_request = 0 (v4; default false — not set on `original`)
  EXPECT_EQ(0u, u(wire[22]));

  // option_context_length = 0 (v5; no context on `original`)
  EXPECT_EQ(0u, u(wire[23]));
  EXPECT_EQ(0u, u(wire[24]));
  EXPECT_EQ(0u, u(wire[25]));
  EXPECT_EQ(0u, u(wire[26]));

  // option_signature_length = 0 (v5)
  EXPECT_EQ(0u, u(wire[27]));

  // site_length = 0 (v6), flags = 0 (v6)
  EXPECT_EQ(0u, u(wire[28]));
  EXPECT_EQ(0u, u(wire[29]));

  // Total wire size = 4 (framing) + 26 (payload) = 30
  EXPECT_EQ(30u, wire.size());
}

// ============================================================================
// Serialize bounds validation
// ============================================================================

TEST(CacheNotificationTest, SerializeUrlTooLong) {
  CacheNotification n;
  n.url = std::string(16385, 'x');  // > kMaxUrlLength (16384)
  n.hostname = "h";
  n.scheme = "https";
  n.content_type = ContentType::kHtml;
  n.capability_mask = 0;
  EXPECT_TRUE(n.Serialize().empty());
}

TEST(CacheNotificationTest, SerializeHostnameTooLong) {
  CacheNotification n;
  n.url = "/x";
  n.hostname = std::string(513, 'h');  // > kMaxHostnameLength (512)
  n.scheme = "https";
  n.content_type = ContentType::kHtml;
  n.capability_mask = 0;
  EXPECT_TRUE(n.Serialize().empty());
}

TEST(CacheNotificationTest, SerializeAtMaxLengths) {
  CacheNotification n;
  n.url = std::string(16384, 'u');     // Exactly kMaxUrlLength
  n.hostname = std::string(512, 'h');  // Exactly kMaxHostnameLength
  n.scheme = "https";
  n.content_type = ContentType::kHtml;
  n.capability_mask = 0;
  EXPECT_FALSE(n.Serialize().empty());  // Should succeed

  CacheNotification decoded;
  auto wire = n.Serialize();
  ASSERT_TRUE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));
  EXPECT_EQ(decoded.url, n.url);
  EXPECT_EQ(decoded.hostname, n.hostname);
  EXPECT_EQ(decoded.scheme, "https");
}

// ============================================================================
// Per-request option context (v5)
// ============================================================================

namespace {

CacheNotification BaseNotification() {
  CacheNotification n;
  n.url = "/a.css";
  n.hostname = "example.com";
  n.scheme = "https";
  n.content_type = ContentType::kCss;
  n.capability_mask = 0x08;
  return n;
}

// Builds a frame the way a peer one wire version BEHIND this build would: the
// v4 field set, and a v4 version byte. Hand-assembled on purpose -- the point
// is to have bytes that this build's Serialize can no longer produce.
std::vector<char> BuildV4Frame() {
  const std::string url = "/a.css";
  const std::string host = "example.com";
  std::string payload;
  auto put_be32 = [&payload](uint32_t v) {
    payload.push_back(static_cast<char>((v >> 24) & 0xFF));
    payload.push_back(static_cast<char>((v >> 16) & 0xFF));
    payload.push_back(static_cast<char>((v >> 8) & 0xFF));
    payload.push_back(static_cast<char>(v & 0xFF));
  };
  payload.push_back(static_cast<char>(4));  // version
  put_be32(static_cast<uint32_t>(url.size()));
  payload.append(url);
  put_be32(static_cast<uint32_t>(host.size()));
  payload.append(host);
  payload.push_back(static_cast<char>(ContentType::kCss));
  put_be32(0x08);
  payload.push_back(static_cast<char>(0x02));  // https
  payload.push_back(0);                        // agent_request

  std::vector<char> wire;
  wire.resize(4);
  const auto total = static_cast<uint32_t>(payload.size());
  wire[0] = static_cast<char>((total >> 24) & 0xFF);
  wire[1] = static_cast<char>((total >> 16) & 0xFF);
  wire[2] = static_cast<char>((total >> 8) & 0xFF);
  wire[3] = static_cast<char>(total & 0xFF);
  wire.insert(wire.end(), payload.begin(), payload.end());
  return wire;
}

}  // namespace

TEST(CacheNotificationTest, ThisBuildSpeaksVersionSix) {
  EXPECT_EQ(6, kIpcVersion)
      << "the site and flags are v6 fields; if the version moved again, the "
         "skew cases below need revisiting rather than renumbering";
}

TEST(CacheNotificationTest, OptionContextRoundTrips) {
  CacheNotification n = BaseNotification();
  n.option_context = "psoc1\nf:ce\no:ImageInlineMaxBytes=3072\n";
  n.option_signature = OptionContextSignature(n.option_context);

  const std::vector<char> wire = n.Serialize();
  ASSERT_FALSE(wire.empty());

  CacheNotification decoded;
  IpcRejection rejection;
  ASSERT_TRUE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded, &rejection));
  EXPECT_EQ(rejection.reason, IpcRejectReason::kNone);
  EXPECT_EQ(decoded.option_context, n.option_context);
  EXPECT_EQ(decoded.option_signature, n.option_signature);
  // The fields that were already there are unmoved.
  EXPECT_EQ(decoded.url, n.url);
  EXPECT_EQ(decoded.hostname, n.hostname);
  EXPECT_EQ(decoded.capability_mask, n.capability_mask);
}

TEST(CacheNotificationTest, AbsentOptionContextRoundTripsAsAbsent) {
  const CacheNotification n = BaseNotification();
  const std::vector<char> wire = n.Serialize();
  ASSERT_FALSE(wire.empty());

  CacheNotification decoded;
  ASSERT_TRUE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));
  EXPECT_TRUE(decoded.option_context.empty());
  EXPECT_TRUE(decoded.option_signature.empty());
}

TEST(CacheNotificationTest, OptionContextPayloadIsCarriedNotInterpreted) {
  // Bytes that are not a valid canonical payload at all still round-trip: the
  // wire is a carrier. Whether the context is USABLE is decided later, by
  // ValidateOptionContext, so that a divergence is reported as itself rather
  // than as a corrupt frame.
  CacheNotification n = BaseNotification();
  n.option_context = std::string("\x00not-a-payload\xFF", 15);
  n.option_signature = OptionContextSignature(n.option_context);

  const std::vector<char> wire = n.Serialize();
  ASSERT_FALSE(wire.empty());
  CacheNotification decoded;
  ASSERT_TRUE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));
  EXPECT_EQ(decoded.option_context, n.option_context);
  EXPECT_EQ(
      OptionContextStatus::kUnknownFormat,
      ValidateOptionContext(decoded.option_context, decoded.option_signature));
}

// --- Version negotiation, proven in both directions -------------------------

// OLD PEER -> NEW READER. A sender one version behind emits a v4 frame; this
// build refuses it whole and names the peer, rather than parsing the prefix it
// happens to understand and acting on a notification with no option context
// attached when the peer may have had one.
TEST(CacheNotificationTest, AV4FrameIsRefusedByThisBuildAndTheSkewIsNamed) {
  const std::vector<char> wire = BuildV4Frame();

  CacheNotification decoded;
  IpcRejection rejection;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded, &rejection));
  EXPECT_EQ(rejection.reason, IpcRejectReason::kVersionMismatch);
  EXPECT_EQ(rejection.peer_version, 4);
  // Nothing was read past the version byte.
  EXPECT_TRUE(decoded.url.empty());
  EXPECT_TRUE(decoded.hostname.empty());
  EXPECT_TRUE(decoded.option_context.empty());
}

// NEW SENDER -> OLD READER. The reverse direction cannot be run against a v4
// binary from here, so it is proven the only way that stays true forever: the
// version byte a v4 reader would gate on is not 4. Its gate is an equality
// test at a fixed offset (see the v3->v4 precedent, unchanged since), so a
// frame carrying anything else is refused before a field is read.
TEST(CacheNotificationTest, AFrameFromThisBuildFailsAV4ReadersVersionGate) {
  CacheNotification n = BaseNotification();
  n.option_context = "psoc1\n";
  n.option_signature = OptionContextSignature(n.option_context);
  const std::vector<char> wire = n.Serialize();
  ASSERT_GT(wire.size(), 5u);
  EXPECT_NE(static_cast<uint8_t>(wire[4]), 4)
      << "a v4 reader would accept this frame and silently drop the option "
         "context it carries";
  EXPECT_EQ(static_cast<uint8_t>(wire[4]), kIpcVersion);
}

// The bump is what makes the two cases above refusals instead of silent
// misreads. Without it, a v4 reader would parse the v4 prefix of a v5 frame
// and IGNORE the appended tail -- Deserialize does not require the frame to be
// fully consumed -- so the notification would be acted on under the default
// context regardless of what the sender declared.
TEST(CacheNotificationTest, AV5FrameIsLongerThanItsV4PrefixWouldBe) {
  CacheNotification n = BaseNotification();
  const std::vector<char> without = n.Serialize();
  n.option_context = "psoc1\no:ImageInlineMaxBytes=3072\n";
  n.option_signature = OptionContextSignature(n.option_context);
  const std::vector<char> with = n.Serialize();
  ASSERT_FALSE(without.empty());
  ASSERT_FALSE(with.empty());
  EXPECT_GT(with.size(), without.size())
      << "the context rides in the frame, so length alone cannot distinguish "
         "the versions -- which is why the version byte must";
}

// --- Size bound, and the half-context refusals ------------------------------

TEST(CacheNotificationTest, AnOversizedOptionContextIsNotSerialized) {
  CacheNotification n = BaseNotification();
  n.option_context = std::string(kMaxOptionContextBytes + 1, 'x');
  n.option_signature = OptionContextSignature(n.option_context);
  EXPECT_TRUE(n.Serialize().empty())
      << "the sender refuses to build a frame the receiver would reject, so "
         "the failure lands where the value came from";
}

TEST(CacheNotificationTest, AnOptionContextExactlyAtTheBoundIsSerialized) {
  CacheNotification n = BaseNotification();
  n.option_context = std::string(kMaxOptionContextBytes, 'x');
  n.option_signature = OptionContextSignature(n.option_context);
  const std::vector<char> wire = n.Serialize();
  ASSERT_FALSE(wire.empty());
  CacheNotification decoded;
  ASSERT_TRUE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));
  EXPECT_EQ(decoded.option_context.size(), kMaxOptionContextBytes);
}

TEST(CacheNotificationTest, APayloadWithoutItsSignatureIsNotSerialized) {
  CacheNotification n = BaseNotification();
  n.option_context = "psoc1\n";
  EXPECT_TRUE(n.Serialize().empty())
      << "a payload with no signature carries nothing the receiver can check "
         "it against";
}

TEST(CacheNotificationTest, ASignatureWithoutItsPayloadIsNotSerialized) {
  CacheNotification n = BaseNotification();
  n.option_signature = OptionContextSignature("psoc1\n");
  EXPECT_TRUE(n.Serialize().empty())
      << "a signature with no payload describes content the receiver never "
         "saw, so there is nothing to re-derive it from";
}

TEST(CacheNotificationTest, ASignatureOfTheWrongLengthIsNotSerialized) {
  CacheNotification n = BaseNotification();
  n.option_context = "psoc1\n";
  n.option_signature = "deadbeef";
  EXPECT_TRUE(n.Serialize().empty());
}

TEST(CacheNotificationTest, AHalfContextOnTheWireIsRefusedOnRead) {
  // A sender that never emits a half context does not make a receiver that
  // would accept one safe, so the read side checks it too. Built by hand from
  // a good frame with the signature length byte zeroed.
  CacheNotification n = BaseNotification();
  n.option_context = "psoc1\n";
  n.option_signature = OptionContextSignature(n.option_context);
  std::vector<char> wire = n.Serialize();
  ASSERT_FALSE(wire.empty());

  // The signature length byte is the one immediately before the 64-character
  // signature, which is followed only by the v6 site length and flags bytes.
  const size_t sig_len_index =
      wire.size() - 2 - kOptionContextSignatureChars - 1;
  ASSERT_EQ(static_cast<uint8_t>(wire[sig_len_index]),
            kOptionContextSignatureChars);
  wire[sig_len_index] = 0;
  // Drop the now-unreferenced signature bytes and fix the framing length.
  wire.erase(
      wire.begin() + static_cast<std::ptrdiff_t>(sig_len_index + 1),
      wire.begin() + static_cast<std::ptrdiff_t>(sig_len_index + 1 +
                                                 kOptionContextSignatureChars));
  const auto total = static_cast<uint32_t>(wire.size() - 4);
  wire[0] = static_cast<char>((total >> 24) & 0xFF);
  wire[1] = static_cast<char>((total >> 16) & 0xFF);
  wire[2] = static_cast<char>((total >> 8) & 0xFF);
  wire[3] = static_cast<char>(total & 0xFF);

  CacheNotification decoded;
  IpcRejection rejection;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded, &rejection));
  EXPECT_EQ(rejection.reason, IpcRejectReason::kMalformed);
  EXPECT_EQ(rejection.peer_version, kIpcVersion);
}

TEST(CacheNotificationTest, AnOversizedDeclaredContextLengthIsRefusedOnRead) {
  CacheNotification n = BaseNotification();
  n.option_context = "psoc1\n";
  n.option_signature = OptionContextSignature(n.option_context);
  std::vector<char> wire = n.Serialize();
  ASSERT_FALSE(wire.empty());

  // Overwrite the declared context length with something past the bound. The
  // reader must reject on the DECLARED size, before it tries to read that many
  // bytes out of a frame that does not contain them.
  const size_t ctx_len_index = wire.size() - 2 - kOptionContextSignatureChars -
                               1 - n.option_context.size() - 4;
  const uint32_t huge = kMaxOptionContextBytes + 1;
  wire[ctx_len_index + 0] = static_cast<char>((huge >> 24) & 0xFF);
  wire[ctx_len_index + 1] = static_cast<char>((huge >> 16) & 0xFF);
  wire[ctx_len_index + 2] = static_cast<char>((huge >> 8) & 0xFF);
  wire[ctx_len_index + 3] = static_cast<char>(huge & 0xFF);

  CacheNotification decoded;
  IpcRejection rejection;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded, &rejection));
  EXPECT_EQ(rejection.reason, IpcRejectReason::kMalformed);
}

TEST(CacheNotificationTest, ATruncatedOptionContextIsRefusedOnRead) {
  CacheNotification n = BaseNotification();
  n.option_context = "psoc1\no:ImageInlineMaxBytes=3072\n";
  n.option_signature = OptionContextSignature(n.option_context);
  std::vector<char> wire = n.Serialize();
  ASSERT_GT(wire.size(), 10u);
  wire.resize(wire.size() - 10);

  CacheNotification decoded;
  IpcRejection rejection;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded, &rejection));
  // Truncated below what the framing header claims: reported as truncation,
  // not as a malformed field.
  EXPECT_EQ(rejection.reason, IpcRejectReason::kTruncated);
}

TEST(CacheNotificationTest, FieldsAreReadWithinTheFrameNotWithinTheBuffer) {
  // Two notifications concatenated, handed over as ONE buffer. The reader must
  // decode the first frame using its own declared length and must not reach
  // into the second -- which, now that the frame ends in length-prefixed
  // fields, is where an unclamped bound would find an option context.
  CacheNotification first = BaseNotification();
  first.url = "/first.css";
  CacheNotification second = BaseNotification();
  second.url = "/second.css";
  second.option_context = "psoc1\no:ImageInlineMaxBytes=3072\n";
  second.option_signature = OptionContextSignature(second.option_context);

  std::vector<char> both = first.Serialize();
  ASSERT_FALSE(both.empty());
  const std::vector<char> tail = second.Serialize();
  ASSERT_FALSE(tail.empty());
  both.insert(both.end(), tail.begin(), tail.end());

  CacheNotification decoded;
  IpcRejection rejection;
  ASSERT_TRUE(CacheNotification::Deserialize(
      std::string_view(both.data(), both.size()), &decoded, &rejection));
  EXPECT_EQ(decoded.url, "/first.css");
  EXPECT_TRUE(decoded.option_context.empty())
      << "the second notification's option context leaked into the first";
  EXPECT_TRUE(decoded.option_signature.empty());
}

// The case above is reassuring but NOT separating: two well-formed frames never
// come near either bound, so it passes with or without the clamp. This one is
// the separating case, and it is the shape the hazard actually has -- a frame
// whose own length header UNDER-DECLARES the fields that follow it, in a buffer
// that holds more bytes after that point.
//
// With the bound taken from the frame: refused, because the option-context
// length field lies past the end this frame declared.
// With the bound taken from the buffer: ACCEPTED, and the notification comes
// back carrying an option context that its own length header says is not part
// of it -- a context attributed to a sender that did not send it, which is the
// one thing the signature exists to make impossible.
TEST(CacheNotificationTest, AnUnderDeclaredLengthCannotReachPastItsOwnFrame) {
  CacheNotification n = BaseNotification();
  n.option_context = "psoc1\no:ImageInlineMaxBytes=3072\n";
  n.option_signature = OptionContextSignature(n.option_context);

  const std::vector<char> honest = n.Serialize();
  ASSERT_FALSE(honest.empty());

  // Everything the v5 and v6 tail occupies: the context length field, the
  // payload, the signature length byte, the signature, the site length byte
  // (no site here) and the flags byte.
  const size_t tail_len =
      4 + n.option_context.size() + 1 + n.option_signature.size() + 1 + 1;
  ASSERT_GT(honest.size(), 4 + tail_len);

  // Control first: the identical bytes with an HONEST length are accepted and
  // do carry the context. This is what isolates total_length as the only thing
  // that differs below -- without it, a rejection could just mean the frame was
  // malformed for some other reason.
  {
    CacheNotification decoded;
    ASSERT_TRUE(CacheNotification::Deserialize(
        std::string_view(honest.data(), honest.size()), &decoded));
    EXPECT_EQ(decoded.option_context, n.option_context);
  }

  // Now under-declare: the frame claims to end where the option context begins,
  // while every one of those bytes is still sitting in the buffer.
  std::vector<char> lying = honest;
  const auto shrunk = static_cast<uint32_t>(honest.size() - 4 - tail_len);
  lying[0] = static_cast<char>((shrunk >> 24) & 0xFF);
  lying[1] = static_cast<char>((shrunk >> 16) & 0xFF);
  lying[2] = static_cast<char>((shrunk >> 8) & 0xFF);
  lying[3] = static_cast<char>(shrunk & 0xFF);

  CacheNotification decoded;
  IpcRejection rejection;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(lying.data(), lying.size()), &decoded, &rejection))
      << "the reader read past the length this frame declared and accepted an "
         "option context the frame does not contain";
  EXPECT_EQ(rejection.reason, IpcRejectReason::kMalformed);
  EXPECT_EQ(rejection.peer_version, kIpcVersion);
  EXPECT_TRUE(decoded.option_context.empty());
  EXPECT_TRUE(decoded.option_signature.empty());
}

// ============================================================================
// Site and flags (v6)
// ============================================================================

namespace {

const std::string kSite(kSiteIdChars, 'c');

// Rewrites the site field of a frame that carries a 64-character site,
// keeping the framing length honest. `site` may be any length up to 255.
std::vector<char> WithSiteBytes(const std::vector<char>& wire,
                                const std::string& site) {
  // The site is the last field but one: [site_len][site][flags].
  const size_t site_len_index = wire.size() - 1 - kSiteIdChars - 1;
  std::vector<char> out(
      wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(site_len_index));
  out.push_back(static_cast<char>(site.size()));
  out.insert(out.end(), site.begin(), site.end());
  out.push_back(wire.back());
  const auto total = static_cast<uint32_t>(out.size() - 4);
  out[0] = static_cast<char>((total >> 24) & 0xFF);
  out[1] = static_cast<char>((total >> 16) & 0xFF);
  out[2] = static_cast<char>((total >> 8) & 0xFF);
  out[3] = static_cast<char>(total & 0xFF);
  return out;
}

IpcRejection Rejection(const std::vector<char>& wire) {
  CacheNotification decoded;
  IpcRejection rejection;
  EXPECT_FALSE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded, &rejection));
  return rejection;
}

}  // namespace

TEST(CacheNotificationTest, SiteAndFlagsRoundTrip) {
  CacheNotification n = BaseNotification();
  n.site = kSite;
  n.host_vouched = true;
  n.from_module = false;
  n.legacy_ex_caller = true;
  n.option_context = "psoc1\n";
  n.option_signature = OptionContextSignature(n.option_context);
  const std::vector<char> wire = n.Serialize();
  ASSERT_FALSE(wire.empty());

  CacheNotification decoded;
  ASSERT_TRUE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));
  EXPECT_EQ(decoded.site, kSite);
  EXPECT_TRUE(decoded.host_vouched);
  EXPECT_FALSE(decoded.from_module);
  EXPECT_TRUE(decoded.legacy_ex_caller);
  EXPECT_EQ(decoded.option_context, n.option_context);
  EXPECT_EQ(decoded.url, n.url);

  // The last two bytes are the site's tail and the flags byte.
  EXPECT_EQ(0x05u, static_cast<uint8_t>(wire.back()));
}

TEST(CacheNotificationTest, NoSiteAndNoFlagsRoundTripAsUnsitedAndUnvouched) {
  const std::vector<char> wire = BaseNotification().Serialize();
  ASSERT_FALSE(wire.empty());
  EXPECT_EQ(0u, static_cast<uint8_t>(wire[wire.size() - 2]));  // site_length
  EXPECT_EQ(0u, static_cast<uint8_t>(wire.back()));            // flags
  CacheNotification decoded;
  decoded.host_vouched = true;  // must be overwritten
  ASSERT_TRUE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));
  EXPECT_TRUE(decoded.site.empty());
  EXPECT_FALSE(decoded.host_vouched);
  EXPECT_FALSE(decoded.from_module);
  EXPECT_FALSE(decoded.legacy_ex_caller);
}

TEST(CacheNotificationTest, FromModuleFlagIsBitOne) {
  CacheNotification n = BaseNotification();
  n.site = kSite;
  n.from_module = true;
  const std::vector<char> wire = n.Serialize();
  ASSERT_FALSE(wire.empty());
  EXPECT_EQ(0x02u, static_cast<uint8_t>(wire.back()));
}

TEST(CacheNotificationTest, ASiteOfAnyOtherShapeIsNotSerialized) {
  for (const std::string& bad :
       {std::string(1, 'a'), std::string(63, 'a'), std::string(65, 'a'),
        std::string(64, 'A'), std::string(63, 'a') + "g",
        std::string(63, 'a') + " "}) {
    CacheNotification n = BaseNotification();
    n.site = bad;
    EXPECT_TRUE(n.Serialize().empty()) << "site of length " << bad.size();
  }
}

TEST(CacheNotificationTest, ASiteOfAnyOtherShapeIsMalformedOnRead) {
  CacheNotification n = BaseNotification();
  n.site = kSite;
  const std::vector<char> good = n.Serialize();
  ASSERT_FALSE(good.empty());
  for (const std::string& bad :
       {std::string(1, 'a'), std::string(63, 'a'), std::string(65, 'a'),
        std::string(64, 'A'), std::string(63, 'a') + "g",
        std::string(63, 'a') + std::string(1, '\0')}) {
    const IpcRejection r = Rejection(WithSiteBytes(good, bad));
    EXPECT_EQ(r.reason, IpcRejectReason::kMalformed)
        << "site of length " << bad.size();
    EXPECT_EQ(r.peer_version, kIpcVersion);
  }
  // Control: the same rewrite with a valid site is accepted.
  const std::vector<char> rewritten =
      WithSiteBytes(good, std::string(kSiteIdChars, 'd'));
  CacheNotification decoded;
  ASSERT_TRUE(CacheNotification::Deserialize(
      std::string_view(rewritten.data(), rewritten.size()), &decoded));
  EXPECT_EQ(decoded.site, std::string(kSiteIdChars, 'd'));
}

TEST(CacheNotificationTest, AReservedFlagBitIsMalformedOnRead) {
  for (int bit = 4; bit < 8; ++bit) {
    std::vector<char> wire = BaseNotification().Serialize();
    ASSERT_FALSE(wire.empty());
    wire.back() = static_cast<char>(1u << bit);
    const IpcRejection r = Rejection(wire);
    EXPECT_EQ(r.reason, IpcRejectReason::kMalformed) << "bit " << bit;
  }
  // Each known bit alone is accepted (the HTML-stored bit only in its own
  // shape; see the tests below).
  for (int bit = 0; bit < 3; ++bit) {
    std::vector<char> wire = BaseNotification().Serialize();
    wire.back() = static_cast<char>(1u << bit);
    CacheNotification decoded;
    EXPECT_TRUE(CacheNotification::Deserialize(
        std::string_view(wire.data(), wire.size()), &decoded))
        << "bit " << bit;
  }
}

// The frame the ASP.NET Core middleware writes for a host AllowedHosts names
// (WorkerNotificationTests in samples/aspnetcore): no options context, no
// site, flags HOST_VOUCHED only.  The reader takes it as vouched, unsited and
// not from the module; a reserved bit next to it is still refused.
TEST(CacheNotificationTest, TheMiddlewaresVouchedFrameIsReadAsVouched) {
  const std::string url = "http://example.com/vouched";
  const std::string host = "example.com";
  std::vector<char> wire = {0, 0, 0, 60, 6, 0, 0, 0, 26};
  wire.insert(wire.end(), url.begin(), url.end());
  wire.insert(wire.end(), {0, 0, 0, 11});
  wire.insert(wire.end(), host.begin(), host.end());
  wire.insert(wire.end(), {0, 0, 0, 0, 8, 2, 1, 0, 0, 0, 0, 0, 0, 1});
  ASSERT_EQ(wire.size(), 64u);

  CacheNotification decoded;
  ASSERT_TRUE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));
  EXPECT_EQ(decoded.url, url);
  EXPECT_EQ(decoded.hostname, host);
  EXPECT_TRUE(decoded.agent_request);
  EXPECT_TRUE(decoded.site.empty());
  EXPECT_TRUE(decoded.host_vouched);
  EXPECT_FALSE(decoded.from_module);
  EXPECT_FALSE(decoded.legacy_ex_caller);

  wire.back() = static_cast<char>(0x01 | 0x10);
  EXPECT_EQ(Rejection(wire).reason, IpcRejectReason::kMalformed);
}

// A frame in the one shape the HTML-stored bit is valid in.
CacheNotification StoredPageNotification() {
  CacheNotification n = BaseNotification();
  n.url = "/page.html";
  n.content_type = ContentType::kHtml;
  n.site = kSite;
  n.from_module = true;
  n.host_vouched = true;
  n.html_stored = true;
  return n;
}

// Every frame that carries the HTML-stored bit outside its shape (not from
// the module, not vouched, unsited, not HTML): Serialize refuses it, and the
// same frame forged on the wire is malformed.
TEST(CacheNotificationTest, HtmlStoredOutsideItsShapeIsRefusedBothWays) {
  std::vector<std::pair<const char*, CacheNotification>> shapes;
  {
    CacheNotification n = StoredPageNotification();
    n.from_module = false;
    shapes.emplace_back("not from the module", n);
  }
  {
    CacheNotification n = StoredPageNotification();
    n.host_vouched = false;
    shapes.emplace_back("not vouched", n);
  }
  {
    CacheNotification n = StoredPageNotification();
    n.site.clear();
    shapes.emplace_back("unsited", n);
  }
  {
    CacheNotification n = StoredPageNotification();
    n.content_type = ContentType::kCss;
    shapes.emplace_back("not HTML", n);
  }
  {
    CacheNotification n = StoredPageNotification();
    n.from_module = false;
    n.host_vouched = false;
    n.site.clear();
    n.content_type = ContentType::kCss;
    shapes.emplace_back("nothing else", n);
  }
  for (const auto& [name, shape] : shapes) {
    EXPECT_TRUE(shape.Serialize().empty()) << name;
    CacheNotification without = shape;
    without.html_stored = false;
    std::vector<char> wire = without.Serialize();
    ASSERT_FALSE(wire.empty()) << name;
    CacheNotification decoded;
    EXPECT_TRUE(CacheNotification::Deserialize(
        std::string_view(wire.data(), wire.size()), &decoded))
        << name << ": the frame without the bit is fine";
    wire.back() = static_cast<char>(wire.back() | kNotifyFlagHtmlStored);
    const IpcRejection r = Rejection(wire);
    EXPECT_EQ(r.reason, IpcRejectReason::kMalformed) << name;
    EXPECT_EQ(r.peer_version, kIpcVersion) << name;
  }
}

// The HTML-stored bit (C API 1.14) is bit three, rides beside from_module
// and round-trips in its shape; the wire version does not move for it.
TEST(CacheNotificationTest, HtmlStoredFlagIsBitThree) {
  CacheNotification n = StoredPageNotification();
  const std::vector<char> wire = n.Serialize();
  ASSERT_FALSE(wire.empty());
  EXPECT_EQ(0x0Bu, static_cast<uint8_t>(wire.back()));
  EXPECT_EQ(kIpcVersion, static_cast<uint8_t>(wire[4]));
  CacheNotification decoded;
  ASSERT_TRUE(CacheNotification::Deserialize(
      std::string_view(wire.data(), wire.size()), &decoded));
  EXPECT_TRUE(decoded.html_stored);
  EXPECT_TRUE(decoded.from_module);
  EXPECT_TRUE(decoded.host_vouched);
  EXPECT_EQ(kIpcVersion, 6);
}

TEST(CacheNotificationTest, AFrameWithoutTheFlagsByteIsMalformed) {
  std::vector<char> wire = BaseNotification().Serialize();
  wire.pop_back();
  const auto total = static_cast<uint32_t>(wire.size() - 4);
  wire[3] = static_cast<char>(total & 0xFF);
  EXPECT_EQ(Rejection(wire).reason, IpcRejectReason::kMalformed);
}

TEST(CacheNotificationTest, V5AndV7FramesAreVersionMismatchesWithPeerVersion) {
  CacheNotification n = BaseNotification();
  n.site = kSite;
  for (uint8_t peer : {uint8_t{5}, uint8_t{7}}) {
    std::vector<char> wire = n.Serialize();
    wire[4] = static_cast<char>(peer);
    const IpcRejection r = Rejection(wire);
    EXPECT_EQ(r.reason, IpcRejectReason::kVersionMismatch);
    EXPECT_EQ(r.peer_version, peer);
  }
}

}  // namespace
}  // namespace pagespeed
