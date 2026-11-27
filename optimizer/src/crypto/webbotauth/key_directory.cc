// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Ported from mod_pagespeed
// pagespeed/kernel/webbotauth/key_directory.cc @ 8e15a19ce (2026-07-02).
// Keep logic in sync manually; the RFC 9421/8941 core is standards-frozen.
// The 1.15 CacheInterface-backed CachedKeyDirectoryProvider is deliberately
// NOT ported; its TTL codec + negative caching are reimplemented here on
// WarmedKeyStore (Serialize/Deserialize + PutNegative, 2.0 wiring phase).

#include "src/crypto/webbotauth/key_directory.h"

#include <charconv>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "src/crypto/webbotauth/base64.h"
#include "src/crypto/webbotauth/jwks.h"

namespace pagespeed {
namespace webbotauth {

void WarmedKeyStore::Put(std::string_view store_key,
                         std::string_view raw_key_32, int64_t expires_at) {
  Entry entry;
  entry.raw_key_32 = std::string(raw_key_32);
  entry.expires_at = expires_at;
  map_[std::string(store_key)] = std::move(entry);
}

void WarmedKeyStore::PutNegative(std::string_view store_key,
                                 int64_t expires_at) {
  Entry entry;
  entry.expires_at = expires_at;
  entry.negative = true;
  map_[std::string(store_key)] = std::move(entry);
}

KeyLookupResult WarmedKeyStore::Get(std::string_view store_key,
                                    int64_t now_unix_sec) const {
  auto it = map_.find(store_key);
  if (it == map_.end()) return KeyLookupResult::NotFound();
  if (now_unix_sec >= it->second.expires_at) {
    // Expired: fail closed. Do NOT mutate the store on the read path (the
    // background warmer's fresh write owns the entry's lifecycle).
    return KeyLookupResult::NotFound();
  }
  if (it->second.negative) {
    // Live negative entry: fresh knowledge of absence (kNegativeTtlSec).
    return KeyLookupResult::NotFound();
  }
  return KeyLookupResult::Found(it->second.raw_key_32);
}

int WarmedKeyStore::CompactExpired(int64_t now_unix_sec) {
  int erased = 0;
  for (auto it = map_.begin(); it != map_.end();) {
    if (now_unix_sec >= it->second.expires_at) {
      it = map_.erase(it);
      ++erased;
    } else {
      ++it;
    }
  }
  return erased;
}

std::string WarmedKeyStore::Serialize(int64_t now_unix_sec) const {
  std::string out;
  out += "version=1\n";
  for (const auto& [key, entry] : map_) {
    if (now_unix_sec >= entry.expires_at) {
      continue;  // drop expired entries (serialize-time garbage collection)
    }
    absl::StrAppend(&out, "k=", entry.negative ? "N" : "P", ",",
                    entry.expires_at, ",", absl::WebSafeBase64Escape(key), ",",
                    absl::WebSafeBase64Escape(entry.raw_key_32), "\n");
  }
  return out;
}

bool WarmedKeyStore::Deserialize(std::string_view content,
                                 WarmedKeyStore* out) {
  if (out == nullptr) return false;
  out->map_.clear();
  bool version_seen = false;
  size_t pos = 0;
  while (pos < content.size()) {
    size_t eol = content.find('\n', pos);
    if (eol == std::string_view::npos) eol = content.size();
    std::string_view line = content.substr(pos, eol - pos);
    pos = eol + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line[0] == '#') continue;
    size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    std::string_view k = line.substr(0, eq);
    std::string_view v = line.substr(eq + 1);
    if (k == "version") {
      int version = 0;
      auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), version);
      if (ec != std::errc{} || version >= 2) {
        // Unparseable or future format: fail closed with an EMPTY store (a
        // signed request then honestly classifies as unknown).
        out->map_.clear();
        return false;
      }
      version_seen = true;
      continue;
    }
    if (k != "k") continue;  // unknown keys silently ignored
    // k=<P|N>,<expires_at>,<b64url(store_key)>,<b64url(raw_key_32)>
    size_t c1 = v.find(',');
    if (c1 == std::string_view::npos) continue;
    size_t c2 = v.find(',', c1 + 1);
    if (c2 == std::string_view::npos) continue;
    size_t c3 = v.find(',', c2 + 1);
    if (c3 == std::string_view::npos) continue;
    std::string_view status = v.substr(0, c1);
    std::string_view expires_sv = v.substr(c1 + 1, c2 - c1 - 1);
    std::string_view key_b64 = v.substr(c2 + 1, c3 - c2 - 1);
    std::string_view raw_b64 = v.substr(c3 + 1);
    if (status != "P" && status != "N") continue;
    int64_t expires_at = 0;
    auto [ptr, ec] = std::from_chars(
        expires_sv.data(), expires_sv.data() + expires_sv.size(), expires_at);
    if (ec != std::errc{} || ptr != expires_sv.data() + expires_sv.size()) {
      continue;
    }
    std::string store_key;
    if (!Base64UrlDecode(key_b64, &store_key) || store_key.empty()) continue;
    if (status == "N") {
      out->PutNegative(store_key, expires_at);
      continue;
    }
    std::string raw_key;
    if (!Base64UrlDecode(raw_b64, &raw_key) || raw_key.size() != 32) continue;
    out->Put(store_key, raw_key, expires_at);
  }
  if (!version_seen) {
    // No version declaration at all: not a store file we recognize.  Fail
    // closed with an empty store, same as a future version.
    out->map_.clear();
    return false;
  }
  return true;
}

std::string WarmedKeyStore::StoreKey(std::string_view realm,
                                     std::string_view host,
                                     std::string_view keyid) {
  return absl::StrCat("wba:dir:", realm, ":", host, "/", keyid);
}

int WarmKeyDirectoryCache(WarmedKeyStore* store, std::string_view realm,
                          std::string_view host, std::string_view jwks_body,
                          int64_t now_unix_sec, int64_t ttl_sec,
                          std::set<std::string>* written_kids) {
  if (store == nullptr) {
    return 0;
  }
  if (ttl_sec < kTtlMinSec) ttl_sec = kTtlMinSec;
  if (ttl_sec > kTtlMaxSec) ttl_sec = kTtlMaxSec;

  std::vector<std::pair<std::string, std::string>> keys;
  ExtractAllEd25519Keys(jwks_body, &keys);

  int written = 0;
  for (size_t i = 0; i < keys.size(); ++i) {
    const std::string& kid = keys[i].first;
    const std::string& raw_key = keys[i].second;
    store->Put(WarmedKeyStore::StoreKey(realm, host, kid), raw_key,
               now_unix_sec + ttl_sec);
    if (written_kids != nullptr) {
      written_kids->insert(kid);
    }
    ++written;
  }
  return written;
}

KeyLookupResult WarmedKeyDirectoryProvider::GetKey(std::string_view host,
                                                   std::string_view keyid,
                                                   int64_t now_unix_sec) {
  if (store_ == nullptr) return KeyLookupResult::NotFound();
  return store_->Get(WarmedKeyStore::StoreKey(realm_, host, keyid),
                     now_unix_sec);
}

KeyLookupResult MultiHostWarmedKeyDirectoryProvider::GetKey(
    std::string_view host, std::string_view keyid, int64_t now_unix_sec) {
  (void)host;  // operator-configured hosts only; never the request's host
  if (store_ == nullptr || hosts_ == nullptr || hosts_->empty()) {
    return KeyLookupResult::Error();
  }
  bool any_not_found = false;
  for (const std::string& h : *hosts_) {
    KeyLookupResult r =
        store_->Get(WarmedKeyStore::StoreKey(realm_, h, keyid), now_unix_sec);
    if (r.status == KeyLookupResult::kFound) return r;
    if (r.status == KeyLookupResult::kNotFound) any_not_found = true;
  }
  return any_not_found ? KeyLookupResult::NotFound() : KeyLookupResult::Error();
}

KeyLookupResult ChainedKeyDirectoryProvider::GetKey(std::string_view host,
                                                    std::string_view keyid,
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
  // No provider had the key. Prefer the more-informative "reachable but
  // absent" (kNotFound) over "all errored" (kError); both fail closed
  // downstream.
  return any_not_found ? KeyLookupResult::NotFound() : KeyLookupResult::Error();
}

}  // namespace webbotauth
}  // namespace pagespeed
