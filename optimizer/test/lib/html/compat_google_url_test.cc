// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Behavioral coverage for lib/html/compat/google_url.h (#1130): the default
// NonEmptyUrlPolicy must reproduce 2.0's historical behavior byte-identically
// (non-empty = valid, identity normalization), and the seam must let a
// custom policy take over validation and normalization without any
// canonical-code change.

#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "gtest/gtest.h"
#include "lib/html/compat/google_url.h"

namespace net_instaweb {
namespace {

// Restores the default NonEmptyUrlPolicy after every test, even on failure.
class GoogleUrlPolicyTest : public testing::Test {
 protected:
  void TearDown() override { GoogleUrl::SetPolicy(nullptr); }
};

TEST_F(GoogleUrlPolicyTest, DefaultConstructedIsInvalid) {
  GoogleUrl url;
  EXPECT_FALSE(url.IsValid());
  EXPECT_TRUE(url.Spec().empty());
}

TEST_F(GoogleUrlPolicyTest, DefaultPolicyEmptySpecIsInvalid) {
  GoogleUrl url("");
  EXPECT_FALSE(url.IsValid());
  EXPECT_TRUE(url.Spec().empty());
}

TEST_F(GoogleUrlPolicyTest, DefaultPolicyNonEmptySpecIsValidAndRoundTrips) {
  // The default policy validates on non-emptiness and normalizes with the
  // identity, so Spec() must round-trip byte-identical — this documents the
  // "no normalization" behavior that keeps 2.0 byte-identical to itself.
  const std::string kSpec = "htTP://Example.COM:8080/a b/../c?q=1#frag";
  GoogleUrl url(kSpec);
  EXPECT_TRUE(url.IsValid());
  EXPECT_EQ(url.Spec(), kSpec);
}

TEST_F(GoogleUrlPolicyTest, ResetRerunsValidation) {
  GoogleUrl url("http://example.com/");
  ASSERT_TRUE(url.IsValid());
  EXPECT_FALSE(url.Reset(""));
  EXPECT_FALSE(url.IsValid());
  EXPECT_TRUE(url.Reset("http://example.org/"));
  EXPECT_EQ(url.Spec(), "http://example.org/");
}

TEST_F(GoogleUrlPolicyTest, SwapExchangesSpecAndValidity) {
  GoogleUrl valid("http://example.com/");
  GoogleUrl invalid;
  ASSERT_TRUE(valid.IsValid());
  ASSERT_FALSE(invalid.IsValid());
  valid.Swap(&invalid);
  EXPECT_FALSE(valid.IsValid());
  EXPECT_TRUE(valid.Spec().empty());
  EXPECT_TRUE(invalid.IsValid());
  EXPECT_EQ(invalid.Spec(), "http://example.com/");
}

TEST_F(GoogleUrlPolicyTest, IsAnyValidAliasesIsValid) {
  GoogleUrl empty;
  GoogleUrl non_empty("x");
  EXPECT_EQ(empty.IsAnyValid(), empty.IsValid());
  EXPECT_EQ(non_empty.IsAnyValid(), non_empty.IsValid());
  EXPECT_TRUE(non_empty.IsAnyValid());
  EXPECT_FALSE(empty.IsAnyValid());
}

// A policy that rejects specs containing "bad" and normalizes by appending
// a trailing slash — proves IsValid/Spec follow the installed policy.
class RejectingNormalizingPolicy final : public UrlValidationPolicy {
 public:
  bool IsValid(std::string_view spec) const override {
    return spec.find("bad") == std::string_view::npos;
  }
  std::string Normalize(std::string_view spec) const override {
    return std::string(spec) + "/";
  }
};

TEST_F(GoogleUrlPolicyTest, CustomPolicyDrivesValidityAndNormalization) {
  GoogleUrl::SetPolicy(std::make_shared<RejectingNormalizingPolicy>());
  GoogleUrl good("http://good.example");
  EXPECT_TRUE(good.IsValid());
  EXPECT_EQ(good.Spec(), "http://good.example/");
  GoogleUrl bad("http://bad.example");
  EXPECT_FALSE(bad.IsValid());
  EXPECT_EQ(bad.Spec(), "http://bad.example/");
}

TEST_F(GoogleUrlPolicyTest, SetPolicyNullptrRestoresDefault) {
  GoogleUrl::SetPolicy(std::make_shared<RejectingNormalizingPolicy>());
  GoogleUrl::SetPolicy(nullptr);
  GoogleUrl url("bad but non-empty");
  EXPECT_TRUE(url.IsValid());
  EXPECT_EQ(url.Spec(), "bad but non-empty");
}

// Smoke test for the policy-slot mutex: one thread hammers SetPolicy while
// another hammers Reset/IsValid/Spec. No assertion beyond no-crash — under
// the tsan config the sanitizer is the oracle for the race-freedom claim.
TEST_F(GoogleUrlPolicyTest, ConcurrentSetPolicyAndResetSmoke) {
  std::thread writer([] {
    for (int i = 0; i < 200; ++i) {
      GoogleUrl::SetPolicy(std::make_shared<RejectingNormalizingPolicy>());
      GoogleUrl::SetPolicy(nullptr);
    }
  });
  std::thread reader([] {
    for (int i = 0; i < 2000; ++i) {
      GoogleUrl url(i % 2 == 0 ? "http://example.com/" : "bad");
      (void)url.IsValid();
      (void)url.Spec();
    }
  });
  writer.join();
  reader.join();
}

}  // namespace
}  // namespace net_instaweb
