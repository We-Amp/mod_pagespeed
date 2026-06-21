// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").

#include "pagespeed/kernel/webbotauth/key_directory.h"

#include <cstdint>
#include <utility>
#include <vector>

#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/webbotauth/jwks.h"

namespace net_instaweb {
namespace webbotauth {

namespace {

// Synchronous collector callback. Valid only with a blocking cache
// (CycloneCache::IsBlocking() == true): the callbacks fire before Get()
// returns, so we can read the result inline.
class SyncCollector : public CacheInterface::Callback {
 public:
  CacheInterface::KeyState state = CacheInterface::kNotFound;
  bool done = false;
  GoogleString data;

  void Done(CacheInterface::KeyState s) override {
    state = s;
    if (s == CacheInterface::kAvailable) {
      // value() returns a const MappedSharedString& (NOT a pointer);
      // value_as_string_piece() is the zero-copy StringPiece accessor.
      StringPiece v = value_as_string_piece();
      data.assign(v.data(), v.size());
    }
    done = true;
  }
};

// A simple textual codec for the cache entry:
//   "<status>\n<fetched_at>\n<expires_at>\n<keylen>\n<raw_key_bytes>"
// status: 0=found 1=not_found. raw bytes follow verbatim (binary-safe because
// length-prefixed).
}  // namespace

GoogleString SerializeCacheEntry(KeyLookupResult::Status status,
                                 StringPiece raw_key_32, int64_t fetched_at,
                                 int64_t expires_at) {
  int code = (status == KeyLookupResult::kFound) ? 0 : 1;
  GoogleString out =
      StrCat(IntegerToString(code), "\n", Integer64ToString(fetched_at), "\n",
             Integer64ToString(expires_at), "\n");
  StrAppend(&out, Integer64ToString(static_cast<int64_t>(raw_key_32.size())),
            "\n");
  out.append(raw_key_32.data(), raw_key_32.size());
  return out;
}

bool DeserializeCacheEntry(StringPiece blob, KeyLookupResult::Status* status,
                           GoogleString* raw_key_32, int64_t* fetched_at,
                           int64_t* expires_at) {
  // Parse four newline-terminated header fields then the raw key tail.
  auto read_line = [](StringPiece* s, StringPiece* line) -> bool {
    size_t nl = s->find('\n');
    if (nl == StringPiece::npos) return false;
    *line = s->substr(0, nl);
    *s = s->substr(nl + 1);
    return true;
  };
  StringPiece s = blob;
  StringPiece code_s, fetched_s, expires_s, len_s;
  if (!read_line(&s, &code_s)) return false;
  if (!read_line(&s, &fetched_s)) return false;
  if (!read_line(&s, &expires_s)) return false;
  if (!read_line(&s, &len_s)) return false;

  int code = 0;
  int64_t fetched = 0, expires = 0, keylen = 0;
  if (!StringToInt(code_s, &code)) return false;
  if (!StringToInt64(fetched_s, &fetched)) return false;
  if (!StringToInt64(expires_s, &expires)) return false;
  if (!StringToInt64(len_s, &keylen)) return false;
  if (keylen < 0 || static_cast<size_t>(keylen) != s.size()) return false;

  *status = (code == 0) ? KeyLookupResult::kFound : KeyLookupResult::kNotFound;
  *fetched_at = fetched;
  *expires_at = expires;
  raw_key_32->assign(s.data(), s.size());
  return true;
}

int WarmKeyDirectoryCache(CacheInterface* cache, StringPiece realm,
                          StringPiece host, StringPiece jwks_body,
                          int64_t now_unix_sec, int64_t ttl_sec,
                          std::set<GoogleString>* written_kids) {
  // The cache-only request reader can only read a BLOCKING cache, so writing to
  // a null or non-blocking cache would be silently unreadable -- skip it.
  if (cache == nullptr || !cache->IsBlocking()) {
    return 0;
  }
  if (ttl_sec < kTtlMinSec) ttl_sec = kTtlMinSec;
  if (ttl_sec > kTtlMaxSec) ttl_sec = kTtlMaxSec;

  std::vector<std::pair<GoogleString, GoogleString> > keys;
  webbotauth::ExtractAllEd25519Keys(jwks_body, &keys);

  int written = 0;
  for (size_t i = 0; i < keys.size(); ++i) {
    const GoogleString& kid = keys[i].first;
    const GoogleString& raw_key = keys[i].second;
    GoogleString blob = SerializeCacheEntry(
        KeyLookupResult::kFound, raw_key, now_unix_sec, now_unix_sec + ttl_sec);
    SharedString sv(blob);
    cache->Put(CachedKeyDirectoryProvider::CacheKey(realm, host, kid), sv);
    if (written_kids != nullptr) {
      written_kids->insert(kid);
    }
    ++written;
  }
  return written;
}

GoogleString CachedKeyDirectoryProvider::CacheKey(StringPiece realm,
                                                  StringPiece host,
                                                  StringPiece keyid) {
  return StrCat("wba:dir:", realm, ":", host, "/", keyid);
}

void CachedKeyDirectoryProvider::Invalidate(StringPiece host,
                                            StringPiece keyid) {
  if (cache_ != nullptr) cache_->Delete(CacheKey(realm_, host, keyid));
}

KeyLookupResult CachedKeyDirectoryProvider::GetKey(StringPiece host,
                                                   StringPiece keyid,
                                                   int64_t now_unix_sec) {
  const GoogleString key = CacheKey(realm_, host, keyid);

  // 1. Try cache (only meaningful with a blocking cache).
  if (cache_ != nullptr && cache_->IsBlocking()) {
    SyncCollector collector;
    cache_->Get(key, &collector);
    if (collector.done && collector.state == CacheInterface::kAvailable) {
      KeyLookupResult::Status st;
      GoogleString raw;
      int64_t fetched_at = 0, expires_at = 0;
      if (DeserializeCacheEntry(collector.data, &st, &raw, &fetched_at,
                                &expires_at)) {
        if (now_unix_sec < expires_at) {
          if (st == KeyLookupResult::kFound) {
            return KeyLookupResult::Found(raw);
          }
          // Negative cache hit still in TTL: fail closed without re-fetching.
          return KeyLookupResult::NotFound();
        }
        // Expired. In cache-only mode (request path) do NOT mutate the cache:
        // a Delete is a disk write we must not do on the nginx event loop, and
        // it could race the background warmer's fresh write. Just fail closed.
        if (read_through_) {
          cache_->Delete(key);  // invalidate, fall through to a fresh lookup
        }
      } else if (read_through_) {
        // Corrupt entry -> drop it (background path only; see above).
        cache_->Delete(key);
      }
    }
  }

  // 2a. Cache-only mode (request path): never consult `inner` (it could block on
  // a network fetch) and never store. A miss/expiry/corrupt entry fails closed.
  if (!read_through_) {
    return KeyLookupResult::NotFound();
  }

  // 2b. Read-through: miss / expired -> ask the inner provider.
  KeyLookupResult result = inner_->GetKey(host, keyid, now_unix_sec);

  // 3. Store the result (positive and negative) with an encoded expiry.
  if (cache_ != nullptr) {
    if (result.status == KeyLookupResult::kFound) {
      int64_t ttl = cache_ttl_hint_sec_;
      if (ttl < kTtlMinSec) ttl = kTtlMinSec;
      if (ttl > kTtlMaxSec) ttl = kTtlMaxSec;
      GoogleString blob =
          SerializeCacheEntry(KeyLookupResult::kFound, result.raw_key_32,
                              now_unix_sec, now_unix_sec + ttl);
      SharedString sv(blob);
      cache_->Put(key, sv);
    } else if (result.status == KeyLookupResult::kNotFound) {
      // Cache negatives to prevent fetch storms; do NOT cache hard errors
      // (off-allowlist / SSRF) -- those should re-evaluate each time.
      GoogleString blob =
          SerializeCacheEntry(KeyLookupResult::kNotFound, StringPiece(),
                              now_unix_sec, now_unix_sec + kNegativeTtlSec);
      SharedString sv(blob);
      cache_->Put(key, sv);
    }
  }
  return result;
}

KeyLookupResult ChainedKeyDirectoryProvider::GetKey(StringPiece host,
                                                    StringPiece keyid,
                                                    int64_t now_unix_sec) {
  bool any_not_found = false;
  for (KeyDirectoryProvider* p : providers_) {
    if (p == nullptr) {
      continue;
    }
    KeyLookupResult r = p->GetKey(host, keyid, now_unix_sec);
    if (r.status == KeyLookupResult::kFound) {
      return r;
    }
    if (r.status == KeyLookupResult::kNotFound) {
      any_not_found = true;
    }
  }
  // No provider had the key. Prefer the more-informative "reachable but absent"
  // (kNotFound) over "all errored" (kError); both fail closed downstream.
  return any_not_found ? KeyLookupResult::NotFound() : KeyLookupResult::Error();
}

}  // namespace webbotauth
}  // namespace net_instaweb
