// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Hermetic unit tests for the design record A2 cache layer: WarmKeyDirectoryCache
// (whole-directory population), CachedKeyDirectoryProvider read-through vs
// cache-only (the non-blocking request path), ChainedKeyDirectoryProvider, and
// ExtractAllEd25519Keys. No network, no nginx, no threads -- a real in-memory
// blocking LRUCache and explicit unix-second clocks.

#include "pagespeed/kernel/webbotauth/key_directory.h"

#include "gtest/gtest.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/cache/lru_cache.h"
#include "pagespeed/kernel/webbotauth/base64.h"
#include "pagespeed/kernel/webbotauth/jwks.h"
#include "pagespeed/kernel/webbotauth/static_key_directory.h"

namespace net_instaweb {
namespace webbotauth {
namespace {

// base64url (no pad) of 32 bytes. kX -> all-zero; kX2 -> first byte 0x04 (a
// distinct, valid 32-byte key; the trailing char stays 'A' so the 2-byte tail
// stays well-formed). See static_key_directory_test.cc for the same trick.
const char kX[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
const char kX2[] = "BAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

GoogleString TwoKeyDoc() {
  return StrCat("{\"keys\":[",
                "{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k1\",\"x\":\"",
                kX, "\"},",
                "{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k2\",\"x\":\"",
                kX2, "\"}]}");
}

// JWKS array with exactly one keyed entry "only".
GoogleString OneKeyDoc() {
  return StrCat(
      "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"only\",\"x\":"
      "\"",
      kX, "\"}]}");
}

GoogleString Raw(const char* x) {
  GoogleString out;
  EXPECT_TRUE(Base64UrlDecode(x, &out));
  return out;
}

// A KeyDirectoryProvider that records how many times it was consulted, so a
// cache-only read can be proven to NOT fall through to it.
class CountingProvider : public KeyDirectoryProvider {
 public:
  explicit CountingProvider(const KeyLookupResult& result) : result_(result) {}
  KeyLookupResult GetKey(StringPiece, StringPiece, int64_t) override {
    ++calls_;
    return result_;
  }
  int calls() const { return calls_; }

 private:
  KeyLookupResult result_;
  int calls_ = 0;
};

// ---- ExtractAllEd25519Keys -------------------------------------------------

TEST(ExtractAllEd25519KeysTest, EnumeratesEveryKeyedEntry) {
  std::vector<std::pair<GoogleString, GoogleString> > out;
  EXPECT_EQ(2u, ExtractAllEd25519Keys(TwoKeyDoc(), &out));
  ASSERT_EQ(2u, out.size());
  EXPECT_EQ("k1", out[0].first);
  EXPECT_EQ(Raw(kX), out[0].second);
  EXPECT_EQ("k2", out[1].first);
  EXPECT_EQ(Raw(kX2), out[1].second);
}

TEST(ExtractAllEd25519KeysTest, SkipsKidlessArrayEntryButKeepsKeyed) {
  GoogleString doc =
      StrCat("{\"keys\":[", "{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"x\":\"", kX,
             "\"},",  // no kid
             "{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k2\",\"x\":\"",
             kX2, "\"}]}");
  std::vector<std::pair<GoogleString, GoogleString> > out;
  EXPECT_EQ(1u, ExtractAllEd25519Keys(doc, &out));
  ASSERT_EQ(1u, out.size());
  EXPECT_EQ("k2", out[0].first);
}

TEST(ExtractAllEd25519KeysTest, SingleJwkWithKidIsEnumerated) {
  GoogleString doc =
      StrCat("{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"solo\",\"x\":\"",
             kX, "\"}");
  std::vector<std::pair<GoogleString, GoogleString> > out;
  EXPECT_EQ(1u, ExtractAllEd25519Keys(doc, &out));
  ASSERT_EQ(1u, out.size());
  EXPECT_EQ("solo", out[0].first);
}

TEST(ExtractAllEd25519KeysTest, KidlessSingleJwkIsSkipped) {
  GoogleString doc =
      StrCat("{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"x\":\"", kX, "\"}");
  std::vector<std::pair<GoogleString, GoogleString> > out;
  EXPECT_EQ(0u, ExtractAllEd25519Keys(doc, &out));
  EXPECT_TRUE(out.empty());
}

TEST(ExtractAllEd25519KeysTest, MalformedAndEmptyYieldNothing) {
  std::vector<std::pair<GoogleString, GoogleString> > out;
  EXPECT_EQ(0u, ExtractAllEd25519Keys("not json", &out));
  EXPECT_EQ(0u, ExtractAllEd25519Keys("{\"keys\":[]}", &out));
  EXPECT_EQ(0u, ExtractAllEd25519Keys("", &out));
  EXPECT_TRUE(out.empty());
}

// ---- WarmKeyDirectoryCache + cache-only request path -----------------------

TEST(WarmKeyDirectoryCacheTest, PopulatesEveryKidAndRequestPathResolves) {
  LRUCache cache(1 << 16);
  ASSERT_TRUE(cache.IsBlocking());
  const int64_t now = 1000;
  EXPECT_EQ(2, WarmKeyDirectoryCache(&cache, "wba", "keys.example.com",
                                     TwoKeyDoc(), now, 3600));

  // Cache-only request path: inner must NEVER be consulted.
  CountingProvider trap(KeyLookupResult::Error());
  CachedKeyDirectoryProvider cache_only(&trap, &cache, "wba",
                                        /*read_through=*/false);

  KeyLookupResult k1 = cache_only.GetKey("keys.example.com", "k1", now);
  EXPECT_EQ(KeyLookupResult::kFound, k1.status);
  EXPECT_EQ(Raw(kX), k1.raw_key_32);
  KeyLookupResult k2 = cache_only.GetKey("keys.example.com", "k2", now);
  EXPECT_EQ(KeyLookupResult::kFound, k2.status);
  EXPECT_EQ(Raw(kX2), k2.raw_key_32);
  EXPECT_EQ(0, trap.calls());  // resolved purely from cache, zero fetch
}

TEST(WarmKeyDirectoryCacheTest, CacheOnlyMissFailsClosedWithoutInner) {
  LRUCache cache(1 << 16);
  CountingProvider trap(KeyLookupResult::Found("x"));
  CachedKeyDirectoryProvider cache_only(&trap, &cache, "wba",
                                        /*read_through=*/false);

  // Cold cache: unknown host/kid -> kNotFound, never calls inner.
  KeyLookupResult r = cache_only.GetKey("keys.example.com", "k1", 1000);
  EXPECT_EQ(KeyLookupResult::kNotFound, r.status);
  EXPECT_EQ(0, trap.calls());
}

TEST(WarmKeyDirectoryCacheTest, CacheOnlyExpiresStrictly) {
  LRUCache cache(1 << 16);
  const int64_t now = 1000;
  // ttl below the floor clamps to kTtlMinSec (300s).
  EXPECT_EQ(1, WarmKeyDirectoryCache(&cache, "wba", "h", OneKeyDoc(), now, 10));
  CachedKeyDirectoryProvider cache_only(nullptr, &cache, "wba",
                                        /*read_through=*/false);

  EXPECT_EQ(KeyLookupResult::kFound,
            cache_only.GetKey("h", "only", now + 299).status);
  // At/after the (clamped 300s) expiry -> fail closed.
  EXPECT_EQ(KeyLookupResult::kNotFound,
            cache_only.GetKey("h", "only", now + 300).status);
}

// Distinct realms never share a cache entry, even for the same host+kid -- the
// guard against A1 (web-bot-auth) <-> A3 (RSL-CAP) cross-feature key confusion.
TEST(WarmKeyDirectoryCacheTest, RealmsAreIsolated) {
  LRUCache cache(1 << 16);
  const int64_t now = 1000;
  EXPECT_EQ(1, WarmKeyDirectoryCache(&cache, "wba", "shared.example.com",
                                     OneKeyDoc(), now, 3600));

  CachedKeyDirectoryProvider wba_reader(nullptr, &cache, "wba",
                                        /*read_through=*/false);
  CachedKeyDirectoryProvider rsl_reader(nullptr, &cache, "rsl",
                                        /*read_through=*/false);
  EXPECT_EQ(KeyLookupResult::kFound,
            wba_reader.GetKey("shared.example.com", "only", now).status);
  EXPECT_EQ(KeyLookupResult::kNotFound,
            rsl_reader.GetKey("shared.example.com", "only", now).status);
}

// ---- read-through (the background warm path) --------------------------------

TEST(CachedKeyDirectoryProviderTest, ReadThroughStoresAndServesPositive) {
  LRUCache cache(1 << 16);
  CountingProvider inner(KeyLookupResult::Found(Raw(kX)));
  CachedKeyDirectoryProvider rt(&inner, &cache, "wba", /*read_through=*/true);

  EXPECT_EQ(KeyLookupResult::kFound, rt.GetKey("h", "k", 1000).status);
  EXPECT_EQ(1, inner.calls());
  // Second call within TTL served from cache; inner not consulted again.
  EXPECT_EQ(KeyLookupResult::kFound, rt.GetKey("h", "k", 1001).status);
  EXPECT_EQ(1, inner.calls());
}

TEST(CachedKeyDirectoryProviderTest, ReadThroughNegativeCacheSuppressesStorms) {
  LRUCache cache(1 << 16);
  CountingProvider inner(KeyLookupResult::NotFound());
  CachedKeyDirectoryProvider rt(&inner, &cache, "wba", /*read_through=*/true);

  EXPECT_EQ(KeyLookupResult::kNotFound, rt.GetKey("h", "k", 1000).status);
  EXPECT_EQ(1, inner.calls());
  // Within the negative TTL the inner is not re-consulted.
  EXPECT_EQ(KeyLookupResult::kNotFound, rt.GetKey("h", "k", 1010).status);
  EXPECT_EQ(1, inner.calls());
}

// ---- ChainedKeyDirectoryProvider -------------------------------------------

TEST(ChainedKeyDirectoryProviderTest, RemoteCacheWinsOverLocalFallback) {
  LRUCache cache(1 << 16);
  WarmKeyDirectoryCache(&cache, "wba", "h", TwoKeyDoc(), 1000, 3600);  // k1->kX
  CachedKeyDirectoryProvider remote(nullptr, &cache, "wba",
                                    /*read_through=*/false);
  // Local file maps k1 to a DIFFERENT key (kX2) -- remote-first must win.
  StaticKeyDirectory local(
      StrCat("{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k1\","
             "\"x\":\"",
             kX2, "\"}]}"));

  std::vector<KeyDirectoryProvider*> chain;
  chain.push_back(&remote);
  chain.push_back(&local);
  ChainedKeyDirectoryProvider provider(chain);

  KeyLookupResult r = provider.GetKey("h", "k1", 1000);
  EXPECT_EQ(KeyLookupResult::kFound, r.status);
  EXPECT_EQ(Raw(kX), r.raw_key_32);  // the warm-fetched (remote) key
}

TEST(ChainedKeyDirectoryProviderTest, FallsBackToLocalWhenCacheCold) {
  LRUCache cache(1 << 16);  // empty
  CachedKeyDirectoryProvider remote(nullptr, &cache, "wba",
                                    /*read_through=*/false);
  StaticKeyDirectory local(
      StrCat("{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k1\","
             "\"x\":\"",
             kX2, "\"}]}"));
  std::vector<KeyDirectoryProvider*> chain;
  chain.push_back(&remote);
  chain.push_back(&local);
  ChainedKeyDirectoryProvider provider(chain);

  KeyLookupResult r = provider.GetKey("h", "k1", 1000);
  EXPECT_EQ(KeyLookupResult::kFound, r.status);
  EXPECT_EQ(Raw(kX2), r.raw_key_32);  // from the local file
}

TEST(ChainedKeyDirectoryProviderTest, UnknownKidIsNotFoundAndEmptyChainErrors) {
  StaticKeyDirectory local(
      StrCat("{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k1\","
             "\"x\":\"",
             kX, "\"}]}"));
  std::vector<KeyDirectoryProvider*> one;
  one.push_back(&local);
  ChainedKeyDirectoryProvider provider(one);
  EXPECT_EQ(KeyLookupResult::kNotFound,
            provider.GetKey("h", "unknown", 1000).status);

  ChainedKeyDirectoryProvider empty((std::vector<KeyDirectoryProvider*>()));
  EXPECT_EQ(KeyLookupResult::kError, empty.GetKey("h", "k1", 1000).status);
}

}  // namespace
}  // namespace webbotauth
}  // namespace net_instaweb
