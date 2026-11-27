// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The option-context cache key: exact separation, an unmoved default, and what
// it costs against the alternate budget.
//
// NOTE what this covers and what it does not. The context-keyed overload is
// FUTURE SURFACE: nothing in the engine calls it, because no optimized output
// currently varies with a resolved configuration, so the worker accepts a valid
// non-default context and files the work under the default context (see
// lib/classify/option_context.h and Worker::AcceptOptionContext). This file is
// kept, and kept green, because the choice it records — key, do not filter — is
// the expensive one to get wrong and the arithmetic behind it does not change.
//
// The last of those is why this file exists at all rather than being three
// assertions bolted onto cache_test. Signature separation was keyed rather than
// filtered at selection time for an arithmetic reason (see
// lib/classify/option_context.h), and an arithmetic reason is worth a test:
// a per-alternate context dimension would multiply the alternates on one key by
// the number of live contexts, against a ceiling of 64 that the published
// budget already spends 44 of. Keyed separation costs zero chain depth, and
// "zero" is asserted here rather than asserted in a comment.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cyclone/key.hpp"
#include "gtest/gtest.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/classify/option_context.h"
#include "test/test_util/cache_test_peer.h"
#include "test/test_util/temp_dir.h"

namespace pagespeed {
namespace {

constexpr std::string_view kUrl = "/style.css";
constexpr std::string_view kHost = "example.com";
constexpr std::string_view kScheme = "https";

std::string SignatureOf(std::string_view payload) {
  return OptionContextSignature(payload);
}

// Two distinct, valid, non-default contexts.
std::string ContextA() {
  return SignatureOf("psoc1\nf:hw\no:ImageInlineMaxBytes=3072\n");
}
std::string ContextB() {
  return SignatureOf("psoc1\nf:hw\no:ImageInlineMaxBytes=4096\n");
}

// ---------------------------------------------------------------------------
// The default context does not move anything.
// ---------------------------------------------------------------------------

TEST(OptionContextKeyTest, DefaultContextYieldsTheHistoricalKeyExactly) {
  const cyclone::CacheKey historical =
      PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme);

  // Both spellings of "no context".
  EXPECT_EQ(historical,
            PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme, ""));
  EXPECT_EQ(historical,
            PageSpeedCache::ComposeKeyPreNormalized(
                kUrl, kHost, kScheme, kDefaultOptionContextSignature));
}

TEST(OptionContextKeyTest, TheUnmovedKeyIsWhatKeepsAnExistingCacheWarm) {
  // Stated as its own case because it is the property an upgrade depends on:
  // every entry already on disk is addressed by the historical string, and a
  // build that keyed default-context requests differently would read as a
  // cold cache on first start with no other symptom.
  const cyclone::CacheKey historical =
      PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme);
  const cyclone::CacheKey after_upgrade =
      PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme, "");
  EXPECT_EQ(historical.to_hex(), after_upgrade.to_hex());
}

// ---------------------------------------------------------------------------
// Separation is exact.
// ---------------------------------------------------------------------------

TEST(OptionContextKeyTest, DifferentContextsProduceDifferentKeys) {
  const cyclone::CacheKey a =
      PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme, ContextA());
  const cyclone::CacheKey b =
      PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme, ContextB());
  const cyclone::CacheKey base =
      PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme);
  EXPECT_NE(a, b);
  EXPECT_NE(a, base);
  EXPECT_NE(b, base);
}

TEST(OptionContextKeyTest, ASingleCharacterOfSignatureIsAWholeDifferentKey) {
  // No nearest fit, no bucketing: the discrimination is the full signature.
  std::string near = ContextA();
  near[0] = (near[0] == 'a') ? 'b' : 'a';
  EXPECT_NE(
      PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme, ContextA()),
      PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme, near));
}

TEST(OptionContextKeyTest, TheSameContextIsTheSameKeyEveryTime) {
  EXPECT_EQ(
      PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme, ContextA()),
      PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme,
                                              ContextA()));
}

TEST(OptionContextKeyTest, ContextDoesNotCollapseTheUrlHostOrSchemeAxes) {
  const std::string ctx = ContextA();
  std::set<std::string> hexes;
  hexes.insert(
      PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme, ctx)
          .to_hex());
  hexes.insert(
      PageSpeedCache::ComposeKeyPreNormalized("/other.css", kHost, kScheme, ctx)
          .to_hex());
  hexes.insert(
      PageSpeedCache::ComposeKeyPreNormalized(kUrl, "other.com", kScheme, ctx)
          .to_hex());
  hexes.insert(PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, "http", ctx)
                   .to_hex());
  EXPECT_EQ(4u, hexes.size()) << "an axis was lost when the context was added";
}

TEST(OptionContextKeyTest, AContextKeyCannotCollideWithADefaultContextKey) {
  // The two forms share one hash input space, so injectivity is a property
  // worth asserting rather than reasoning about once. A default-context key
  // always begins with the scheme; a context key always begins with "oc1:".
  // The adversarial case is a URL crafted to look like the context form.
  const std::string ctx = ContextA();
  const cyclone::CacheKey context_key =
      PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme, ctx);

  // A default-context request whose URL contains the whole context prefix.
  const std::string sneaky_url =
      "/oc1:" + ctx + "@https://" + std::string(kHost) + std::string(kUrl);
  const cyclone::CacheKey sneaky =
      PageSpeedCache::ComposeKeyPreNormalized(sneaky_url, kHost, kScheme);
  EXPECT_NE(context_key, sneaky);

  // And the same URL under the same context is different again.
  EXPECT_NE(context_key, PageSpeedCache::ComposeKeyPreNormalized(
                             sneaky_url, kHost, kScheme, ctx));
}

TEST(OptionContextKeyTest, ManyDistinctContextsProduceManyDistinctKeys) {
  std::set<std::string> hexes;
  constexpr int kContexts = 200;
  for (int i = 0; i < kContexts; ++i) {
    const std::string payload =
        "psoc1\no:ImageInlineMaxBytes=" + std::to_string(1000 + i) + "\n";
    hexes.insert(PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme,
                                                         SignatureOf(payload))
                     .to_hex());
  }
  EXPECT_EQ(static_cast<size_t>(kContexts), hexes.size());
}

// ---------------------------------------------------------------------------
// The alternate-count budget.
// ---------------------------------------------------------------------------

class OptionContextBudgetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    PageSpeedCacheConfig config;
    config.volume_path = temp_dir_ + "/cache.vol";
    config.volume_size = static_cast<uint64_t>(16 * 1024 * 1024);
    auto result = PageSpeedCache::Create(config);
    ASSERT_TRUE(result.has_value());
    cache_ = std::move(*result);
  }

  void TearDown() override {
    cache_.reset();
    std::filesystem::remove_all(temp_dir_);
  }

  // Writes one ordinary content alternate under `signature`.
  bool WriteVariant(std::string_view signature, AlternateId id,
                    std::string_view body) {
    AlternateMetadata meta;
    meta.full_mask = static_cast<uint32_t>(id);
    meta.content_type = ContentType::kCss;
    const cyclone::CacheKey key = PageSpeedCache::ComposeKeyPreNormalized(
        kUrl, kHost, kScheme, signature);
    auto wh = cache_->WriteAlternateByKey(key, id, body.size(), meta, kUrl);
    if (!wh.has_value()) return false;
    auto written = wh->write_sync(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(body.data()), body.size()));
    if (!written.has_value()) return false;
    return wh->close_sync().has_value();
  }

  // How many alternates hang off one key. Read through the test peer because
  // the production surface deliberately does not publish a by-key listing --
  // and reading it rather than inferring it is the point of this file.
  size_t AlternatesOn(std::string_view signature) {
    const cyclone::CacheKey key = PageSpeedCache::ComposeKeyPreNormalized(
        kUrl, kHost, kScheme, signature);
    auto listed =
        PageSpeedCacheTestPeer::Cyclone(*cache_).list_alternates_sync(key);
    return listed.has_value() ? listed->size() : 0;
  }

  std::string temp_dir_;
  std::unique_ptr<PageSpeedCache> cache_;
};

TEST_F(OptionContextBudgetTest, ContextsCostKeyCardinalityNotChainDepth) {
  // THE BUDGET STATEMENT, asserted rather than asserted-in-prose.
  //
  // Ten option contexts each store the SAME four alternate ids for the same
  // URL. If the signature were a per-alternate dimension, that would be 40
  // alternates on one key against a ceiling of 64 with 44 already accounted
  // for -- over budget with six contexts, and the whole key would start
  // refusing writes. Because it is a key dimension, each key sees exactly the
  // four it was given and the chain depth of every one of them is identical.
  constexpr int kContexts = 10;
  const AlternateId kIds[] = {0x00, 0x01, 0x02, 0x03};

  std::vector<std::string> signatures;
  signatures.reserve(kContexts);
  for (int i = 0; i < kContexts; ++i) {
    signatures.push_back(SignatureOf(
        "psoc1\no:ImageInlineMaxBytes=" + std::to_string(2000 + i) + "\n"));
  }

  for (const std::string& signature : signatures) {
    for (const AlternateId id : kIds) {
      ASSERT_TRUE(WriteVariant(signature, id, "body-bytes"))
          << "write refused for id " << static_cast<int>(id);
    }
  }

  for (const std::string& signature : signatures) {
    EXPECT_EQ(std::size(kIds), AlternatesOn(signature))
        << "a context key carries only its own alternates; if this grew, the "
           "signature stopped being a key dimension";
  }
}

TEST_F(OptionContextBudgetTest,
       TheDefaultContextKeyIsUnaffectedByOtherContexts) {
  const AlternateId kId = 0x00;
  ASSERT_TRUE(WriteVariant("", kId, "default-context-body"));

  for (int i = 0; i < 8; ++i) {
    const std::string signature = SignatureOf(
        "psoc1\no:ImageInlineMaxBytes=" + std::to_string(3000 + i) + "\n");
    ASSERT_TRUE(WriteVariant(signature, kId, "other-context-body"));
  }

  EXPECT_EQ(1u, AlternatesOn(""))
      << "eight other contexts added alternates to the default context's key, "
         "so the separation is not actually by key";
}

}  // namespace
}  // namespace pagespeed
