// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// KeyDirectoryProvider: the injectable abstraction that maps a (host, keyid)
// to a raw 32-byte Ed25519 public key. The verifier depends ONLY on this
// interface, so the hermetic unit test substitutes a fake provider and never
// touches the network.
//
// CachedKeyDirectoryProvider memoizes an inner provider's results in a Cyclone
// CacheInterface -- the engine's own cache, deliberately not a second, parallel
// cache stack. Cyclone exposes only Get/Put/Delete (NO per-entry TTL), so the
// TTL/expiry is encoded INSIDE the serialized value and checked on Get;
// negative (not-found / error) results are cached with a shorter TTL to prevent
// fetch storms. Delete() invalidates.

#ifndef PAGESPEED_KERNEL_WEBBOTAUTH_KEY_DIRECTORY_H_
#define PAGESPEED_KERNEL_WEBBOTAUTH_KEY_DIRECTORY_H_

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include "pagespeed/kernel/base/cache_interface.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {
namespace webbotauth {

struct KeyLookupResult {
  enum Status {
    kFound,     // raw_key_32 is a valid 32-byte Ed25519 public key
    kNotFound,  // host reachable + allowlisted but keyid not present
    kError,     // off-allowlist, SSRF-blocked, fetch/parse failure, timeout
  };
  Status status = kError;   // default FAIL-CLOSED
  GoogleString raw_key_32;  // valid only when status == kFound

  static KeyLookupResult Found(GoogleString key) {
    KeyLookupResult r;
    r.status = kFound;
    r.raw_key_32 = std::move(key);
    return r;
  }
  static KeyLookupResult NotFound() {
    KeyLookupResult r;
    r.status = kNotFound;
    return r;
  }
  static KeyLookupResult Error() {
    KeyLookupResult r;
    r.status = kError;
    return r;
  }
};

// Injectable provider interface. Implementations MUST be total (never throw)
// and MUST fail closed (return kError) on any uncertainty. `now_unix_sec` is
// supplied so cache layers can evaluate expiry deterministically (testable).
class KeyDirectoryProvider {
 public:
  virtual ~KeyDirectoryProvider() = default;
  virtual KeyLookupResult GetKey(StringPiece host, StringPiece keyid,
                                 int64_t now_unix_sec) = 0;
};

// TTL clamps (seconds).
constexpr int64_t kTtlMinSec = 300;       // 5 minutes
constexpr int64_t kTtlMaxSec = 86400;     // 24 hours
constexpr int64_t kNegativeTtlSec = 120;  // negative-cache: 2 minutes

// Serialize/deserialize a cache entry. Exposed for unit testing the value
// codec (status + raw key + fetched_at + expires_at). Returns false on a
// malformed blob.
GoogleString SerializeCacheEntry(KeyLookupResult::Status status,
                                 StringPiece raw_key_32, int64_t fetched_at,
                                 int64_t expires_at);
bool DeserializeCacheEntry(StringPiece blob, KeyLookupResult::Status* status,
                           GoogleString* raw_key_32, int64_t* fetched_at,
                           int64_t* expires_at);

// Populate `cache` with every kid-bearing OKP/Ed25519 key found in `jwks_body`
// for directory `host` within trust `realm`, as POSITIVE entries that expire at
// now_unix_sec + clamp(ttl_sec, [kTtlMinSec, kTtlMaxSec]). Returns the number of
// keys written. `realm` namespaces the cache so distinct trust domains (A1
// Web-Bot-Auth vs A3 RSL-CAP) can never collide even under the same `host`.
// This is the cache-population core of the background warmer (A2), factored here
// (no fetcher/thread deps) so it is unit-testable with an in-memory cache and a
// literal JWKS document. `cache` must be a BLOCKING CacheInterface; a null or
// non-blocking cache writes nothing (the cache-only request reader can only read
// a blocking cache). Only positive entries are written -- the warmer never caches
// negatives or errors (strict-expiry: stale entries lapse on their own TTL).
// If `written_kids` is non-null it is filled with the set of kids written, so a
// caller (the warmer) can prune kids that have vanished from the directory since
// the previous refresh.
int WarmKeyDirectoryCache(CacheInterface* cache, StringPiece realm,
                          StringPiece host, StringPiece jwks_body,
                          int64_t now_unix_sec, int64_t ttl_sec,
                          std::set<GoogleString>* written_kids = nullptr);

// Memoizing provider. Wraps an inner provider and a CacheInterface (Cyclone).
// `cache_ttl_hint_sec` is the directory's HTTP cache hint (or 0 if none);
// positive results expire at fetched_at + clamp(hint, [min,max]); negative
// results use the shorter negative TTL.
class CachedKeyDirectoryProvider : public KeyDirectoryProvider {
 public:
  // Does NOT take ownership of inner or cache.
  //
  // read_through (default true): on a cache miss/expiry, consult `inner` and
  // store its result. This is the BACKGROUND warm path -- it may block on a
  // network fetch and so must NEVER run on the nginx event loop.
  //
  // read_through == false: CACHE-ONLY mode for the request path. A miss, an
  // expired entry, a corrupt entry, or a cached negative all return kNotFound
  // (fail-closed) WITHOUT consulting `inner` and WITHOUT mutating the cache (no
  // Put, no Delete) -- a pure, side-effect-free, non-blocking read that is safe
  // to call synchronously inside an nginx phase handler. `inner` may be nullptr.
  //
  // `realm` namespaces the cache keys (e.g. "wba" vs "rsl") so the two AgentPass
  // trust domains never share a key even under the same host.
  CachedKeyDirectoryProvider(KeyDirectoryProvider* inner, CacheInterface* cache,
                             StringPiece realm, bool read_through = true)
      : inner_(inner),
        cache_(cache),
        realm_(realm.data(), realm.size()),
        read_through_(read_through) {}

  KeyLookupResult GetKey(StringPiece host, StringPiece keyid,
                         int64_t now_unix_sec) override;

  // Explicit invalidation (operator action / detected staleness).
  void Invalidate(StringPiece host, StringPiece keyid);

  // The directory HTTP cache hint to apply to the NEXT positive store. The
  // production fetch provider sets this from the fetched response; tests may
  // leave it at the default.
  void set_cache_ttl_hint_sec(int64_t s) { cache_ttl_hint_sec_ = s; }

  static GoogleString CacheKey(StringPiece realm, StringPiece host,
                               StringPiece keyid);

 private:
  KeyDirectoryProvider* inner_;
  CacheInterface* cache_;
  GoogleString realm_;
  int64_t cache_ttl_hint_sec_ = 0;
  bool read_through_ = true;
};

// Resolves a key by trying each provider in order and returning the first
// kFound. Used by the nginx request path to layer a cache-only remote directory
// (warm-fetched keys) in FRONT of the operator-local StaticKeyDirectory file:
// a managed remote directory wins, the static file is the seed / fallback, and
// with no remote layer configured the chain is just the local file (behavior
// identical to the original local-file-only verifier). Does NOT own the
// providers.
//
// If no provider returns kFound, the chain returns kNotFound when ANY provider
// reported kNotFound (the host was reachable but the key is absent) and kError
// only when every provider errored -- so the most-informative non-found status
// wins. An empty chain returns kError (fail-closed).
class ChainedKeyDirectoryProvider : public KeyDirectoryProvider {
 public:
  explicit ChainedKeyDirectoryProvider(
      std::vector<KeyDirectoryProvider*> providers)
      : providers_(std::move(providers)) {}

  KeyLookupResult GetKey(StringPiece host, StringPiece keyid,
                         int64_t now_unix_sec) override;

 private:
  std::vector<KeyDirectoryProvider*> providers_;  // not owned
};

}  // namespace webbotauth
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_WEBBOTAUTH_KEY_DIRECTORY_H_
