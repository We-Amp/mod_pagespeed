// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Web Bot Auth key-directory warmer (the 2.0 analog of
// 1.15's KeyDirectoryWarmer).  The request path (nginx) must stay strictly
// synchronous, so ALL network I/O happens here in the worker, off the request
// path: a periodic timer (cloned from the license-renewal pattern in
// worker.cc) queues one refresh cycle onto the libuv work pool, which fetches
// each operator-configured key-directory JWKS over HTTPS, warms an in-memory
// WarmedKeyStore, and atomically writes the store to a world-readable file
// next to pagespeed-shared.conf.  nginx re-reads that file on its existing
// 1-second shared-config cadence — the same worker→nginx propagation
// mechanism as agent_optimize_entitled, extended with a payload file.
//
// Fetching reuses the hardened agent_optimize fetcher (src/browser
// agent_fetcher): DNS resolve → lib/net SSRF guard on every resolved address
// → IP-pinned spawn-curl (NO libcurl) with a scrubbed environment → manual
// redirects re-adjudicated per hop.  Directory hosts come ONLY from operator
// config; the fetch allowlist is exactly those hosts.
//
// Failure semantics (strict-expiry): a failed fetch writes
// NOTHING for that directory — previously warmed keys stay until their own
// TTL lapses, and a kid observed to have VANISHED from a successfully
// re-fetched directory is replaced by a short negative entry
// (webbotauth::kNegativeTtlSec) so removals propagate promptly.
//
// The refresh core is pure with respect to the injected fetch function, so
// unit tests drive full cycles hermetically (no network, no threads).

#ifndef SRC_WORKER_WEBBOTAUTH_WARMER_H_
#define SRC_WORKER_WEBBOTAUTH_WARMER_H_

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "src/crypto/webbotauth/key_directory.h"

namespace pagespeed {

struct WebBotAuthWarmerConfig {
  // Trust-domain realm the warmed keys are stored under (WarmedKeyStore is
  // realm-namespaced so distinct trust domains never collide under one host).
  // "wba" = the observe-only Web-Bot-Auth verifier; "rsl" = the RSL-CAP
  // enforcement layer.  Defaults to "wba" so existing A1 call sites are
  // unchanged.
  std::string realm = "wba";
  // Key-directory JWKS URLs (https), e.g.
  // "https://example.com/.well-known/http-message-signatures-directory".
  std::vector<std::string> directory_urls;
  // Destination for the serialized key store ("" = keep in-memory only).
  std::string keys_file_path;
  // Positive-entry TTL when the directory response carries no usable
  // Cache-Control max-age hint.  Clamped to [kTtlMinSec, kTtlMaxSec] by the
  // storage core either way.
  int64_t default_positive_ttl_sec = 3600;
  // Fetch bounds (mirror the 1.15 warmer defaults).
  int fetch_timeout_sec = 10;
  size_t max_response_bytes = size_t{256} * 1024;
};

// One directory fetch result. `ok` only for an HTTP 200 within the size cap;
// `ttl_hint_sec` is the response's Cache-Control max-age (0 = no hint).
struct JwksFetchResult {
  bool ok = false;
  std::string body;
  int64_t ttl_hint_sec = 0;
};

// Injected fetch effect so the refresh core is hermetically testable.  The
// production implementation (MakeWebBotAuthJwksFetcher) is the SSRF-guarded
// IP-pinned spawn-curl path.
using JwksFetchFn = std::function<JwksFetchResult(const std::string& url)>;

// Warmer state carried across refresh cycles.  Owned by the worker; a cycle
// operates on a copy on the work pool and the result is swapped back on the
// loop thread, so the state itself needs no locking.
struct WebBotAuthWarmerState {
  webbotauth::WarmedKeyStore store;
  // Kids seen in the previous successful refresh of each directory URL, so
  // a refresh that observes a key's removal can write its negative entry.
  // Keyed per URL (not per host): two URLs on one host each track their own
  // kid set, and tombstoning consults the merged per-host view so same-host
  // directories can never evict each other's live keys.
  std::map<std::string, std::set<std::string>> last_kids_by_url;
};

// "{parent_of(cache_path)}/pagespeed-webbotauth-keys.conf" ("" on empty
// input) — same placement rule as SharedConfigFilePath.
std::string WebBotAuthKeysFilePath(const std::string& cache_path);

// "{parent_of(cache_path)}/pagespeed-rslcap-keys.conf" ("" on empty input) —
// the RSL-CAP enforcement keys file (realm "rsl"), kept separate from the
// observe-only keys file above so the two trust domains never share a file.
std::string RslCapKeysFilePath(const std::string& cache_path);

// The HOST of each parseable https URL in `urls` (order-preserving,
// de-duplicated, lower-cased).  This is both the fetch allowlist and the
// store-key host namespace, and it is what the worker publishes to nginx via
// shared config (web_bot_auth_directory_hosts).
std::vector<std::string> WebBotAuthDirectoryHosts(
    const std::vector<std::string>& urls);

// Parse a Cache-Control header value's max-age directive (seconds).
// Returns 0 when absent/unparseable (caller falls back to the default TTL).
int64_t ParseMaxAgeSeconds(std::string_view cache_control);

// Per-cycle fetch outcome counts, for the caller's retry policy (#876).  A
// cycle where every parseable directory fetch failed (attempted > 0 &&
// fetched_ok == 0) is total failure — characteristic of boot ordering, e.g.
// a same-host directory served by an nginx that starts after the worker —
// and is distinct from a directory legitimately serving an empty key set
// (fetched_ok > 0 with keys possibly 0).
struct WebBotAuthRefreshFetchStats {
  int attempted = 0;   // parseable directory URLs this cycle
  int fetched_ok = 0;  // of those, fetched successfully (HTTP 200 in bounds)
};

// One refresh cycle: fetch every configured directory via `fetch`, warm
// `state->store` (positive entries; vanished kids get negative entries),
// then atomically write the serialized store to config.keys_file_path
// (0644, world-readable for nginx).  Returns the number of positive keys
// written this cycle (0 also when every fetch failed — in which case the
// store is left untouched apart from the rewrite dropping expired entries).
// `fetch_stats`, when non-null, receives the per-cycle fetch outcome counts.
int RefreshWebBotAuthKeys(const WebBotAuthWarmerConfig& config,
                          const JwksFetchFn& fetch, int64_t now_unix_sec,
                          WebBotAuthWarmerState* state,
                          WebBotAuthRefreshFetchStats* fetch_stats = nullptr);

// Production fetch function: resolve + SSRF-validate + IP-pinned spawn-curl
// GET via the agent_optimize fetcher, allowlisted to exactly the hosts of
// `config.directory_urls`.  BLOCKING — call on the libuv work pool only.
JwksFetchFn MakeWebBotAuthJwksFetcher(const WebBotAuthWarmerConfig& config);

}  // namespace pagespeed

#endif  // SRC_WORKER_WEBBOTAUTH_WARMER_H_
