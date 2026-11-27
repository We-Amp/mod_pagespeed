// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Ported from mod_pagespeed pagespeed/kernel/webbotauth/key_directory.h
// @ 8e15a19ce (2026-07-02). Keep logic in sync manually; the RFC 9421/8941
// core is standards-frozen.
//
// KeyDirectoryProvider: the injectable abstraction that maps a (host, keyid)
// to a raw 32-byte Ed25519 public key. The verifier depends ONLY on this
// interface, so the hermetic unit test substitutes a fake provider and never
// touches the network.
//
// TODO(webbotauth-A2): mod_pagespeed 1.15 backs key storage with a Cyclone
// CacheInterface (CachedKeyDirectoryProvider + a serialized TTL codec with
// negative caching; see the 1.15 key_directory.{h,cc}). PageSpeed 2.0 has no
// CacheInterface abstraction, so that layer is deliberately NOT ported here.
// The shared, cross-process key storage lands together with the background
// key-directory warmer + net-fetch provider in the next phase, built against
// 2.0's own lib/net + cache. Until then, WarmedKeyStore below is a
// process-local, in-memory stand-in that exercises the same population and
// strict-expiry read semantics (and it is NOT thread-safe).

#ifndef PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_KEY_DIRECTORY_H_
#define PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_KEY_DIRECTORY_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pagespeed {
namespace webbotauth {

struct KeyLookupResult {
  enum Status : std::uint8_t {
    kFound,     // raw_key_32 is a valid 32-byte Ed25519 public key
    kNotFound,  // host reachable + allowlisted but keyid not present
    kError,     // off-allowlist, SSRF-blocked, fetch/parse failure, timeout
  };
  Status status = kError;  // default FAIL-CLOSED
  std::string raw_key_32;  // valid only when status == kFound

  static KeyLookupResult Found(std::string key) {
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
// supplied so store layers can evaluate expiry deterministically (testable).
class KeyDirectoryProvider {
 public:
  virtual ~KeyDirectoryProvider() = default;
  virtual KeyLookupResult GetKey(std::string_view host, std::string_view keyid,
                                 int64_t now_unix_sec) = 0;
};

// TTL clamps (seconds).
constexpr int64_t kTtlMinSec = 300;       // 5 minutes
constexpr int64_t kTtlMaxSec = 86400;     // 24 hours
constexpr int64_t kNegativeTtlSec = 120;  // negative-cache: 2 minutes

// In-memory warmed-key store: maps StoreKey(realm, host, kid) -> (raw 32-byte
// key, absolute expiry). This is the process-local stand-in for the 1.15
// Cyclone-backed key cache (see the TODO above); the population and
// strict-expiry read semantics match 1.15's WarmKeyDirectoryCache +
// cache-only CachedKeyDirectoryProvider. NOT thread-safe.
class WarmedKeyStore {
 public:
  WarmedKeyStore() = default;

  // Store/overwrite a positive entry. A failed refresh still writes nothing
  // (strict-expiry: stale entries lapse on their own TTL).
  void Put(std::string_view store_key, std::string_view raw_key_32,
           int64_t expires_at);

  // Store/overwrite a NEGATIVE entry: fresh knowledge that the kid is absent
  // from its directory. Restores the 1.15 CachedKeyDirectoryProvider negative
  // caching (kNegativeTtlSec) in the 2.0 storage layer: the warmer writes a
  // short-lived tombstone for a kid that vanished from the directory, so the
  // removal propagates promptly (overwriting the still-live positive entry)
  // instead of waiting out the positive TTL.
  void PutNegative(std::string_view store_key, int64_t expires_at);

  // Strict-expiry read: kFound only when a POSITIVE entry exists and
  // now_unix_sec < expires_at; kNotFound otherwise (miss, expired, or a live
  // negative entry -- all fail-closed). Never kError: there is no fetch/SSRF
  // surface to fail. A pure read -- expired entries are not erased (no
  // mutation on the request path, mirroring the 1.15 cache-only mode).
  KeyLookupResult Get(std::string_view store_key, int64_t now_unix_sec) const;

  // The store key format, realm-namespaced so distinct trust domains (A1
  // Web-Bot-Auth vs A3 RSL-CAP) can never collide even under the same host:
  // "wba:dir:<realm>:<host>/<kid>".
  static std::string StoreKey(std::string_view realm, std::string_view host,
                              std::string_view keyid);

  bool empty() const { return map_.empty(); }
  size_t size() const { return map_.size(); }

  // Erase every entry (positive or negative) whose expiry has passed and
  // return the number erased. Put/PutNegative only ever insert/overwrite, so
  // without this a kid-rotating directory would grow the map without bound;
  // the warmer calls it once per refresh cycle (never on the request path --
  // the read path stays mutation-free).
  int CompactExpired(int64_t now_unix_sec);

  // --- Cross-process propagation codec (2.0 wiring phase) ---
  // The warmer runs in the worker; the nginx module verifies. Keys cross the
  // process boundary the same way the rest of the worker->nginx state does:
  // an atomically-written world-readable file next to pagespeed-shared.conf.
  //
  // Serialize emits every UNEXPIRED entry (positive and negative) as
  // versioned key=value lines; expired entries are dropped (this doubles as
  // the store's garbage collection). Deserialize replaces *out with the
  // parsed entries; malformed lines are skipped (fail-closed per line) and a
  // missing or future format version yields an empty store (fail-closed,
  // mirroring the shared-config version guard). Binary fields (store key,
  // raw key) are base64url-encoded so arbitrary kid bytes can never break
  // the line format.
  std::string Serialize(int64_t now_unix_sec) const;
  static bool Deserialize(std::string_view content, WarmedKeyStore* out);

 private:
  struct Entry {
    std::string raw_key_32;  // empty for a negative entry
    int64_t expires_at = 0;
    bool negative = false;
  };
  std::map<std::string, Entry, std::less<>> map_;
};

// Populate `store` with every kid-bearing OKP/Ed25519 key found in `jwks_body`
// for directory `host` within trust `realm`, as POSITIVE entries that expire
// at now_unix_sec + clamp(ttl_sec, [kTtlMinSec, kTtlMaxSec]). Returns the
// number of keys written. `realm` namespaces the store so distinct trust
// domains can never collide even under the same `host`. This is the
// store-population core of the background warmer (A2), factored here (no
// fetcher/thread deps) so it is unit-testable with a literal JWKS document.
// If `written_kids` is non-null it is filled with the set of kids written, so
// a caller (the warmer) can prune kids that have vanished from the directory
// since the previous refresh.
int WarmKeyDirectoryCache(WarmedKeyStore* store, std::string_view realm,
                          std::string_view host, std::string_view jwks_body,
                          int64_t now_unix_sec, int64_t ttl_sec,
                          std::set<std::string>* written_kids = nullptr);

// Store-only request-path provider over a WarmedKeyStore, scoped to a realm.
// The 2.0 analog of 1.15's cache-only CachedKeyDirectoryProvider
// (read_through=false): a miss or an expired entry returns kNotFound
// (fail-closed) WITHOUT any fetch and WITHOUT mutating the store -- a pure,
// side-effect-free read that is safe on the request path. Does NOT own the
// store.
class WarmedKeyDirectoryProvider : public KeyDirectoryProvider {
 public:
  WarmedKeyDirectoryProvider(const WarmedKeyStore* store,
                             std::string_view realm)
      : store_(store), realm_(realm) {}

  KeyLookupResult GetKey(std::string_view host, std::string_view keyid,
                         int64_t now_unix_sec) override;

 private:
  const WarmedKeyStore* store_;  // not owned
  std::string realm_;
};

// Store-only request-path provider that resolves a keyid across EVERY
// operator-configured directory host (2.0 wiring phase). The
// verifier's RequestView carries a single directory_host, but 2.0 lets the
// operator configure several key-directory URLs; this provider IGNORES the
// request-supplied host and instead tries each configured host in order,
// returning the first kFound. The host list comes ONLY from operator config,
// never from the request. Same most-informative-status rule as
// ChainedKeyDirectoryProvider; an empty host list returns kError
// (fail-closed). Does NOT own the store.
class MultiHostWarmedKeyDirectoryProvider : public KeyDirectoryProvider {
 public:
  // Does NOT own or copy `hosts` -- the caller's vector must outlive the
  // provider. The intended use is a short-lived stack provider over a
  // long-lived operator-config list, constructed per signed request, so a
  // deep copy here would be pure hot-path waste.
  MultiHostWarmedKeyDirectoryProvider(const WarmedKeyStore* store,
                                      std::string_view realm,
                                      const std::vector<std::string>& hosts)
      : store_(store), realm_(realm), hosts_(&hosts) {}

  KeyLookupResult GetKey(std::string_view host, std::string_view keyid,
                         int64_t now_unix_sec) override;

 private:
  const WarmedKeyStore* store_;  // not owned
  std::string realm_;
  const std::vector<std::string>* hosts_;  // not owned
};

// Resolves a key by trying each provider in order and returning the first
// kFound. Used by the request path to layer a store-only remote directory
// (warm-fetched keys) in FRONT of the operator-local StaticKeyDirectory file
// itself: a managed remote directory wins, the static file is the seed /
// fallback, and with no remote layer configured the chain is just the local
// file (behavior identical to A1 v1). Does NOT own the providers.
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

  KeyLookupResult GetKey(std::string_view host, std::string_view keyid,
                         int64_t now_unix_sec) override;

 private:
  std::vector<KeyDirectoryProvider*> providers_;  // not owned
};

}  // namespace webbotauth
}  // namespace pagespeed

#endif  // PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_KEY_DIRECTORY_H_
