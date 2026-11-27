// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/webbotauth_warmer.h"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "lib/base/atomic_file_writer.h"
#include "lib/net/fetch_policy.h"
#include "lib/net/ssrf_guard.h"
#include "src/browser/agent_fetcher.h"

namespace pagespeed {

std::string WebBotAuthKeysFilePath(const std::string& cache_path) {
  if (cache_path.empty()) {
    return {};
  }
  std::filesystem::path p(cache_path);
  return (p.parent_path() / "pagespeed-webbotauth-keys.conf").string();
}

std::string RslCapKeysFilePath(const std::string& cache_path) {
  if (cache_path.empty()) {
    return {};
  }
  std::filesystem::path p(cache_path);
  return (p.parent_path() / "pagespeed-rslcap-keys.conf").string();
}

std::vector<std::string> WebBotAuthDirectoryHosts(
    const std::vector<std::string>& urls) {
  std::vector<std::string> hosts;
  for (const std::string& url : urls) {
    std::optional<UrlAuthority> auth = ParseHttpUrl(url);
    if (!auth.has_value() || auth->host.empty()) {
      continue;  // fail-closed: an unparseable URL contributes nothing
    }
    // ParseHttpUrl already lower-cases the host.
    bool seen = false;
    for (const std::string& h : hosts) {
      if (h == auth->host) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      hosts.push_back(auth->host);
    }
  }
  return hosts;
}

int64_t ParseMaxAgeSeconds(std::string_view cache_control) {
  // Scan comma-separated directives for a well-formed "max-age=N" token.
  size_t pos = 0;
  while (pos <= cache_control.size()) {
    size_t comma = cache_control.find(',', pos);
    if (comma == std::string_view::npos) comma = cache_control.size();
    std::string_view token = cache_control.substr(pos, comma - pos);
    pos = comma + 1;
    // Trim surrounding whitespace.
    while (!token.empty() && absl::ascii_isspace(token.front())) {
      token.remove_prefix(1);
    }
    while (!token.empty() && absl::ascii_isspace(token.back())) {
      token.remove_suffix(1);
    }
    constexpr std::string_view kPrefix = "max-age=";
    if (token.size() <= kPrefix.size()) continue;
    bool prefix_match = true;
    for (size_t i = 0; i < kPrefix.size(); ++i) {
      if (absl::ascii_tolower(token[i]) != kPrefix[i]) {
        prefix_match = false;
        break;
      }
    }
    if (!prefix_match) continue;
    std::string_view digits = token.substr(kPrefix.size());
    int64_t value = 0;
    auto [ptr, ec] =
        std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (ec != std::errc{} || ptr != digits.data() + digits.size() ||
        value < 0) {
      continue;
    }
    return value;
  }
  return 0;
}

int RefreshWebBotAuthKeys(const WebBotAuthWarmerConfig& config,
                          const JwksFetchFn& fetch, int64_t now_unix_sec,
                          WebBotAuthWarmerState* state,
                          WebBotAuthRefreshFetchStats* fetch_stats) {
  if (fetch_stats != nullptr) {
    *fetch_stats = {};
  }
  if (state == nullptr || !fetch) {
    return 0;
  }

  // Bound the store before this cycle's writes: Put/PutNegative only ever
  // insert/overwrite, so without compaction a kid-rotating directory would
  // grow the map (and the per-cycle state copy) without bound.
  state->store.CompactExpired(now_unix_sec);

  // Phase 1: fetch + warm every directory URL, recording which kids each
  // SUCCESSFULLY-fetched URL currently publishes.  Tombstoning is deferred
  // to phase 2 so two URLs on the SAME host can never kill each other's
  // just-written keys (the store's host namespace is shared per host).
  int total_written = 0;
  std::map<std::string, std::set<std::string>> fetched_kids_by_url;
  std::map<std::string, std::set<std::string>> current_kids_by_host;
  for (const std::string& url : config.directory_urls) {
    std::optional<UrlAuthority> auth = ParseHttpUrl(url);
    if (!auth.has_value() || auth->host.empty()) {
      continue;  // unparseable URL: nothing to warm, nothing to prune
    }
    const std::string& host = auth->host;

    if (fetch_stats != nullptr) {
      ++fetch_stats->attempted;
    }
    JwksFetchResult result = fetch(url);
    if (result.ok && fetch_stats != nullptr) {
      ++fetch_stats->fetched_ok;
    }
    if (!result.ok) {
      // Strict expiry: a failed refresh writes nothing —
      // no keys, no tombstones.  Previously warmed keys for this URL lapse
      // on their own TTL, and its last-kids bookkeeping stays untouched.
      continue;
    }

    int64_t ttl = result.ttl_hint_sec > 0 ? result.ttl_hint_sec
                                          : config.default_positive_ttl_sec;
    std::set<std::string> kids;
    total_written += webbotauth::WarmKeyDirectoryCache(
        &state->store, config.realm, host, result.body, now_unix_sec, ttl,
        &kids);
    current_kids_by_host[host].insert(kids.begin(), kids.end());
    fetched_kids_by_url[url] = std::move(kids);
  }

  // Phase 2: negative caching (the 1.15 kNegativeTtlSec semantics,
  // reimplemented in the storage layer).  A kid that a successfully
  // re-fetched URL published last cycle but no longer does gets a short
  // tombstone — UNLESS any other URL on the same host still publishes it
  // this cycle (the merged per-host set), in which case the key is simply
  // still valid under that host.
  for (auto& [url, kids] : fetched_kids_by_url) {
    std::optional<UrlAuthority> auth = ParseHttpUrl(url);
    const std::string& host = auth->host;  // parseable: fetched in phase 1
    const std::set<std::string>& host_kids = current_kids_by_host[host];
    auto it = state->last_kids_by_url.find(url);
    if (it != state->last_kids_by_url.end()) {
      for (const std::string& old_kid : it->second) {
        if (kids.find(old_kid) == kids.end() &&
            host_kids.find(old_kid) == host_kids.end()) {
          state->store.PutNegative(
              webbotauth::WarmedKeyStore::StoreKey(config.realm, host, old_kid),
              now_unix_sec + webbotauth::kNegativeTtlSec);
        }
      }
    }
    state->last_kids_by_url[url] = std::move(kids);
  }

  // Publish for nginx.  Serialize drops expired entries, so this rewrite is
  // also the file's garbage collection.  0644: nginx workers run unprivileged
  // and must read it.  World-readable is fine HERE and only here — these are
  // public keys, nothing secret is ever written to this file.  (It is
  // deliberately NOT the mode of pagespeed-shared.conf, which went to 0640
  // with the daemon's privilege drop.)
  if (!config.keys_file_path.empty()) {
    AtomicWriteFile(config.keys_file_path, state->store.Serialize(now_unix_sec),
                    0644);
  }
  return total_written;
}

JwksFetchFn MakeWebBotAuthJwksFetcher(const WebBotAuthWarmerConfig& config) {
  // The fetch allowlist is exactly the operator-configured directory hosts;
  // a redirect anywhere else is denied per hop by the fetcher's G2
  // re-adjudication.
  FetchPolicy policy;
  policy.allow_hosts = WebBotAuthDirectoryHosts(config.directory_urls);

  AgentFetcherDeps deps;
  deps.spawn = RealCurlSpawn();
  deps.resolve = RealHostResolver();
  // Trust-anchor key material: require https on EVERY hop, not just the
  // operator-validated initial URL — an http redirect hop would let an
  // on-path attacker serve the JWKS body.
  deps.require_https = true;
  deps.curl_opts.max_time_sec = config.fetch_timeout_sec;
  deps.curl_opts.max_response_bytes = config.max_response_bytes;
  deps.curl_opts.user_agent =
      "Mozilla/5.0 (compatible; mod_pagespeed webbotauth-key-warmer)";
  deps.curl_opts.accept =
      "application/http-message-signatures-directory+json, "
      "application/jwk-set+json;q=0.9, application/json;q=0.8, */*;q=0.1";

  return [policy, deps](const std::string& url) -> JwksFetchResult {
    JwksFetchResult out;
    AgentFetchOutcome outcome =
        FetchSubresource(url, ResourceClass::kFetch, policy, deps);
    if (!outcome.fulfilled || outcome.status != 200) {
      return out;  // ok == false
    }
    for (const HttpHeader& h : outcome.headers) {
      if (absl::AsciiStrToLower(h.name) == "cache-control") {
        out.ttl_hint_sec = ParseMaxAgeSeconds(h.value);
        break;
      }
    }
    out.body = std::move(outcome.body);
    out.ok = true;
    return out;
  };
}

}  // namespace pagespeed
