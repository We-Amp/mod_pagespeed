// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Ported from mod_pagespeed
// test/pagespeed/kernel/webbotauth/key_directory_test.cc @ 8e15a19ce
// (2026-07-02). Keep logic in sync manually; the RFC 9421/8941 core is
// standards-frozen. The 1.15 CachedKeyDirectoryProvider read-through cases
// are not ported (that layer is CacheInterface-backed); its negative-cache
// and TTL-codec semantics ARE covered below against WarmedKeyStore, where
// the 2.0 wiring phase reimplemented them (PutNegative +
// Serialize/Deserialize).
//
// Hermetic unit tests for the key-store layer:
// WarmKeyDirectoryCache (whole-directory population), the store-only request
// path (WarmedKeyDirectoryProvider), ChainedKeyDirectoryProvider,
// MultiHostWarmedKeyDirectoryProvider, the cross-process store codec, and
// ExtractAllEd25519Keys. No network, no server runtime, no threads -- an
// in-memory store and explicit unix-second clocks.

#include "src/crypto/webbotauth/key_directory.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "src/crypto/webbotauth/base64.h"
#include "src/crypto/webbotauth/jwks.h"
#include "src/crypto/webbotauth/static_key_directory.h"

namespace pagespeed {
namespace webbotauth {
namespace {

// base64url (no pad) of 32 bytes. kX -> all-zero; kX2 -> first byte 0x04 (a
// distinct, valid 32-byte key; the trailing char stays 'A' so the 2-byte tail
// stays well-formed). See static_key_directory_test.cc for the same trick.
const char kX[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
const char kX2[] = "BAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

std::string TwoKeyDoc() {
  return absl::StrCat(
      "{\"keys\":[",
      "{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k1\",\"x\":\"", kX,
      "\"},", "{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k2\",\"x\":\"",
      kX2, "\"}]}");
}

// JWKS array with exactly one keyed entry "only".
std::string OneKeyDoc() {
  return absl::StrCat(
      "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"only\",\"x\":"
      "\"",
      kX, "\"}]}");
}

std::string Raw(const char* x) {
  std::string out;
  EXPECT_TRUE(Base64UrlDecode(x, &out));
  return out;
}

// ---- ExtractAllEd25519Keys -------------------------------------------------

TEST(ExtractAllEd25519KeysTest, EnumeratesEveryKeyedEntry) {
  std::vector<std::pair<std::string, std::string>> out;
  EXPECT_EQ(2u, ExtractAllEd25519Keys(TwoKeyDoc(), &out));
  ASSERT_EQ(2u, out.size());
  EXPECT_EQ("k1", out[0].first);
  EXPECT_EQ(Raw(kX), out[0].second);
  EXPECT_EQ("k2", out[1].first);
  EXPECT_EQ(Raw(kX2), out[1].second);
}

TEST(ExtractAllEd25519KeysTest, SkipsKidlessArrayEntryButKeepsKeyed) {
  std::string doc = absl::StrCat(
      "{\"keys\":[", "{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"x\":\"", kX,
      "\"},",  // no kid
      "{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k2\",\"x\":\"", kX2,
      "\"}]}");
  std::vector<std::pair<std::string, std::string>> out;
  EXPECT_EQ(1u, ExtractAllEd25519Keys(doc, &out));
  ASSERT_EQ(1u, out.size());
  EXPECT_EQ("k2", out[0].first);
}

TEST(ExtractAllEd25519KeysTest, SingleJwkWithKidIsEnumerated) {
  std::string doc = absl::StrCat(
      "{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"solo\",\"x\":\"", kX,
      "\"}");
  std::vector<std::pair<std::string, std::string>> out;
  EXPECT_EQ(1u, ExtractAllEd25519Keys(doc, &out));
  ASSERT_EQ(1u, out.size());
  EXPECT_EQ("solo", out[0].first);
}

TEST(ExtractAllEd25519KeysTest, KidlessSingleJwkIsSkipped) {
  std::string doc =
      absl::StrCat("{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"x\":\"", kX, "\"}");
  std::vector<std::pair<std::string, std::string>> out;
  EXPECT_EQ(0u, ExtractAllEd25519Keys(doc, &out));
  EXPECT_TRUE(out.empty());
}

TEST(ExtractAllEd25519KeysTest, MalformedAndEmptyYieldNothing) {
  std::vector<std::pair<std::string, std::string>> out;
  EXPECT_EQ(0u, ExtractAllEd25519Keys("not json", &out));
  EXPECT_EQ(0u, ExtractAllEd25519Keys("{\"keys\":[]}", &out));
  EXPECT_EQ(0u, ExtractAllEd25519Keys("", &out));
  EXPECT_TRUE(out.empty());
}

// ---- WarmKeyDirectoryCache + store-only request path ------------------------

TEST(WarmKeyDirectoryCacheTest, PopulatesEveryKidAndRequestPathResolves) {
  WarmedKeyStore store;
  const int64_t now = 1000;
  EXPECT_EQ(2, WarmKeyDirectoryCache(&store, "wba", "keys.example.com",
                                     TwoKeyDoc(), now, 3600));

  // Store-only request path: resolves purely from the warmed store, zero
  // fetch (there is no inner provider to fall through to by construction).
  WarmedKeyDirectoryProvider reader(&store, "wba");

  KeyLookupResult k1 = reader.GetKey("keys.example.com", "k1", now);
  EXPECT_EQ(KeyLookupResult::kFound, k1.status);
  EXPECT_EQ(Raw(kX), k1.raw_key_32);
  KeyLookupResult k2 = reader.GetKey("keys.example.com", "k2", now);
  EXPECT_EQ(KeyLookupResult::kFound, k2.status);
  EXPECT_EQ(Raw(kX2), k2.raw_key_32);
}

TEST(WarmKeyDirectoryCacheTest, StoreOnlyMissFailsClosed) {
  WarmedKeyStore store;  // cold
  WarmedKeyDirectoryProvider reader(&store, "wba");

  // Cold store: unknown host/kid -> kNotFound (fail-closed, no fetch).
  KeyLookupResult r = reader.GetKey("keys.example.com", "k1", 1000);
  EXPECT_EQ(KeyLookupResult::kNotFound, r.status);
}

TEST(WarmKeyDirectoryCacheTest, StoreOnlyExpiresStrictly) {
  WarmedKeyStore store;
  const int64_t now = 1000;
  // ttl below the floor clamps to kTtlMinSec (300s).
  EXPECT_EQ(1, WarmKeyDirectoryCache(&store, "wba", "h", OneKeyDoc(), now, 10));
  WarmedKeyDirectoryProvider reader(&store, "wba");

  EXPECT_EQ(KeyLookupResult::kFound,
            reader.GetKey("h", "only", now + 299).status);
  // At/after the (clamped 300s) expiry -> fail closed.
  EXPECT_EQ(KeyLookupResult::kNotFound,
            reader.GetKey("h", "only", now + 300).status);
}

// Distinct realms never share a store entry, even for the same host+kid -- the
// guard against A1 (web-bot-auth) <-> A3 (RSL-CAP) cross-feature key
// confusion.
TEST(WarmKeyDirectoryCacheTest, RealmsAreIsolated) {
  WarmedKeyStore store;
  const int64_t now = 1000;
  EXPECT_EQ(1, WarmKeyDirectoryCache(&store, "wba", "shared.example.com",
                                     OneKeyDoc(), now, 3600));

  WarmedKeyDirectoryProvider wba_reader(&store, "wba");
  WarmedKeyDirectoryProvider rsl_reader(&store, "rsl");
  EXPECT_EQ(KeyLookupResult::kFound,
            wba_reader.GetKey("shared.example.com", "only", now).status);
  EXPECT_EQ(KeyLookupResult::kNotFound,
            rsl_reader.GetKey("shared.example.com", "only", now).status);
}

// ---- ChainedKeyDirectoryProvider -------------------------------------------

TEST(ChainedKeyDirectoryProviderTest, RemoteStoreWinsOverLocalFallback) {
  WarmedKeyStore store;
  WarmKeyDirectoryCache(&store, "wba", "h", TwoKeyDoc(), 1000, 3600);  // k1->kX
  WarmedKeyDirectoryProvider remote(&store, "wba");
  // Local file maps k1 to a DIFFERENT key (kX2) -- remote-first must win.
  StaticKeyDirectory local(absl::StrCat(
      "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k1\","
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

TEST(ChainedKeyDirectoryProviderTest, FallsBackToLocalWhenStoreCold) {
  WarmedKeyStore store;  // empty
  WarmedKeyDirectoryProvider remote(&store, "wba");
  StaticKeyDirectory local(absl::StrCat(
      "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k1\","
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
  StaticKeyDirectory local(absl::StrCat(
      "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k1\","
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

// ---- Negative caching (2.0 storage-layer reimplementation) ------------------

TEST(WarmedKeyStoreNegativeTest, LiveNegativeEntryFailsClosed) {
  WarmedKeyStore store;
  const int64_t now = 1000;
  const std::string key = WarmedKeyStore::StoreKey("wba", "h", "gone");
  store.PutNegative(key, now + kNegativeTtlSec);

  EXPECT_EQ(KeyLookupResult::kNotFound, store.Get(key, now).status);
  // Expired negative: still kNotFound (a miss), never a resurrected key.
  EXPECT_EQ(KeyLookupResult::kNotFound,
            store.Get(key, now + kNegativeTtlSec).status);
}

TEST(WarmedKeyStoreNegativeTest, NegativeOverwritesLivePositive) {
  // The load-bearing effect: a kid removed from the directory gets a
  // tombstone that OVERWRITES its still-live positive entry, so the removal
  // propagates immediately instead of waiting out the positive TTL.
  WarmedKeyStore store;
  const int64_t now = 1000;
  WarmKeyDirectoryCache(&store, "wba", "h", OneKeyDoc(), now, 3600);
  const std::string key = WarmedKeyStore::StoreKey("wba", "h", "only");
  ASSERT_EQ(KeyLookupResult::kFound, store.Get(key, now).status);

  store.PutNegative(key, now + kNegativeTtlSec);
  EXPECT_EQ(KeyLookupResult::kNotFound, store.Get(key, now).status);
}

// ---- Cross-process store codec (worker -> nginx propagation) ----------------

TEST(WarmedKeyStoreCodecTest, RoundTripsPositiveAndNegativeEntries) {
  WarmedKeyStore store;
  const int64_t now = 1000;
  WarmKeyDirectoryCache(&store, "wba", "h", TwoKeyDoc(), now, 3600);
  store.PutNegative(WarmedKeyStore::StoreKey("wba", "h", "gone"),
                    now + kNegativeTtlSec);

  WarmedKeyStore copy;
  ASSERT_TRUE(WarmedKeyStore::Deserialize(store.Serialize(now), &copy));

  KeyLookupResult k1 =
      copy.Get(WarmedKeyStore::StoreKey("wba", "h", "k1"), now);
  EXPECT_EQ(KeyLookupResult::kFound, k1.status);
  EXPECT_EQ(Raw(kX), k1.raw_key_32);
  EXPECT_EQ(KeyLookupResult::kFound,
            copy.Get(WarmedKeyStore::StoreKey("wba", "h", "k2"), now).status);
  // The negative entry crossed the boundary too (still fail-closed).
  EXPECT_EQ(KeyLookupResult::kNotFound,
            copy.Get(WarmedKeyStore::StoreKey("wba", "h", "gone"), now).status);
  // ... and expiry is preserved: past the positive TTL everything lapses.
  EXPECT_EQ(
      KeyLookupResult::kNotFound,
      copy.Get(WarmedKeyStore::StoreKey("wba", "h", "k1"), now + 3600).status);
}

TEST(WarmedKeyStoreCodecTest, SerializeDropsExpiredEntries) {
  WarmedKeyStore store;
  const int64_t now = 1000;
  WarmKeyDirectoryCache(&store, "wba", "h", OneKeyDoc(), now, 3600);

  // Serialize AFTER expiry: the entry is garbage-collected from the output.
  WarmedKeyStore copy;
  ASSERT_TRUE(WarmedKeyStore::Deserialize(store.Serialize(now + 7200), &copy));
  EXPECT_TRUE(copy.empty());
}

TEST(WarmedKeyStoreCodecTest, MalformedLinesAreSkippedFailClosed) {
  WarmedKeyStore store;
  const int64_t now = 1000;
  WarmKeyDirectoryCache(&store, "wba", "h", OneKeyDoc(), now, 3600);
  std::string blob = store.Serialize(now);
  blob += "k=P,notanumber,AAAA,AAAA\n";                // bad expiry
  blob += "k=X,2000,AAAA,AAAA\n";                      // bad status
  blob += "k=P,2000,!!notb64!!,AAAA\n";                // bad store key
  blob += "k=P,2000,AAAA," + std::string(kX) + "Z\n";  // raw key != 32 bytes
  blob += "garbage line with no equals\n";
  blob += "unknown_key=whatever\n";

  WarmedKeyStore copy;
  EXPECT_TRUE(WarmedKeyStore::Deserialize(blob, &copy));
  // Only the one well-formed entry survives.
  EXPECT_EQ(KeyLookupResult::kFound,
            copy.Get(WarmedKeyStore::StoreKey("wba", "h", "only"), now).status);
}

TEST(WarmedKeyStoreCodecTest, FutureVersionYieldsEmptyStore) {
  WarmedKeyStore store;
  WarmKeyDirectoryCache(&store, "wba", "h", OneKeyDoc(), 1000, 3600);
  WarmedKeyStore copy;
  EXPECT_FALSE(
      WarmedKeyStore::Deserialize("version=2\nk=P,2000,AAAA,AAAA\n", &copy));
  EXPECT_TRUE(copy.empty());
}

TEST(WarmedKeyStoreCodecTest, DeserializeReplacesExistingEntries) {
  WarmedKeyStore copy;
  WarmKeyDirectoryCache(&copy, "wba", "stale-host", OneKeyDoc(), 1000, 3600);
  // Deserializing an (empty-but-valid) blob replaces the whole store —
  // nginx's view always mirrors the latest worker-published file.
  ASSERT_TRUE(WarmedKeyStore::Deserialize("version=1\n", &copy));
  EXPECT_TRUE(copy.empty());
}

// ---- MultiHostWarmedKeyDirectoryProvider -------------------------------------

TEST(MultiHostProviderTest, ResolvesAcrossConfiguredHostsOnly) {
  WarmedKeyStore store;
  const int64_t now = 1000;
  WarmKeyDirectoryCache(&store, "wba", "a.example.com", OneKeyDoc(), now, 3600);
  WarmKeyDirectoryCache(&store, "wba", "b.example.com", TwoKeyDoc(), now, 3600);

  // The provider borrows the host list (no copy); it must outlive it.
  const std::vector<std::string> hosts = {"a.example.com", "b.example.com"};
  MultiHostWarmedKeyDirectoryProvider provider(&store, "wba", hosts);
  // "only" lives under a., "k2" under b. — both resolve regardless of the
  // (ignored) request-supplied host argument.
  EXPECT_EQ(KeyLookupResult::kFound,
            provider.GetKey("ignored.example.org", "only", now).status);
  EXPECT_EQ(KeyLookupResult::kFound,
            provider.GetKey("ignored.example.org", "k2", now).status);
  EXPECT_EQ(KeyLookupResult::kNotFound,
            provider.GetKey("ignored.example.org", "absent", now).status);
}

TEST(MultiHostProviderTest, UnconfiguredHostNeverConsulted) {
  WarmedKeyStore store;
  const int64_t now = 1000;
  WarmKeyDirectoryCache(&store, "wba", "evil.example.com", OneKeyDoc(), now,
                        3600);
  // The provider only consults operator-configured hosts; keys warmed under
  // any other host are unreachable from this lookup path.
  const std::vector<std::string> hosts = {"good.example.com"};
  MultiHostWarmedKeyDirectoryProvider provider(&store, "wba", hosts);
  EXPECT_EQ(KeyLookupResult::kNotFound,
            provider.GetKey("evil.example.com", "only", now).status);
}

TEST(MultiHostProviderTest, EmptyHostListFailsClosed) {
  WarmedKeyStore store;
  const std::vector<std::string> hosts;
  MultiHostWarmedKeyDirectoryProvider provider(&store, "wba", hosts);
  EXPECT_EQ(KeyLookupResult::kError, provider.GetKey("h", "k1", 1000).status);
}

// ---- CompactExpired ----------------------------------------------------------

TEST(WarmedKeyStoreCompactTest, ErasesExpiredKeepsLive) {
  WarmedKeyStore store;
  const int64_t now = 1000;
  store.Put(WarmedKeyStore::StoreKey("wba", "h", "live"), Raw(kX), now + 3600);
  store.Put(WarmedKeyStore::StoreKey("wba", "h", "dead"), Raw(kX2), now - 1);
  store.PutNegative(WarmedKeyStore::StoreKey("wba", "h", "dead-neg"), now - 1);
  ASSERT_EQ(3u, store.size());

  EXPECT_EQ(2, store.CompactExpired(now));
  EXPECT_EQ(1u, store.size());
  EXPECT_EQ(
      KeyLookupResult::kFound,
      store.Get(WarmedKeyStore::StoreKey("wba", "h", "live"), now).status);
}

TEST(WarmedKeyStoreCompactTest, KidRotationStaysBounded) {
  // A directory that mints a fresh kid every refresh must not grow the store
  // without bound: once old entries expire, compaction reclaims them.
  WarmedKeyStore store;
  const int64_t ttl = 3600;
  for (int cycle = 0; cycle < 50; ++cycle) {
    const int64_t now = 1000 + cycle * 7200;  // each cycle: prior kid expired
    store.CompactExpired(now);
    store.Put(WarmedKeyStore::StoreKey("wba", "h", absl::StrCat("kid", cycle)),
              Raw(kX), now + ttl);
    EXPECT_LE(store.size(), 1u) << "cycle " << cycle;
  }
}

// ---- Missing version declaration ----------------------------------------------

TEST(WarmedKeyStoreCodecTest, MissingVersionFailsClosed) {
  // A blob with entries but NO version= line is not a store file we
  // recognize — fail closed to an empty store, like a future version.
  WarmedKeyStore seed;
  WarmKeyDirectoryCache(&seed, "wba", "h", OneKeyDoc(), 1000, 3600);
  std::string blob = seed.Serialize(1000);
  // Strip the leading "version=1\n".
  ASSERT_TRUE(blob.starts_with("version=1\n"));
  blob = blob.substr(sizeof("version=1\n") - 1);

  WarmedKeyStore copy;
  EXPECT_FALSE(WarmedKeyStore::Deserialize(blob, &copy));
  EXPECT_TRUE(copy.empty());
}

}  // namespace
}  // namespace webbotauth
}  // namespace pagespeed
