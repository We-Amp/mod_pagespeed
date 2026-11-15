// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// ModPageSpeed 2.0 - Nginx Interceptor Module
//
// C++ nginx module for zero-copy cache serving with record-on-miss.
// Classifies requests, composes cache keys, performs lookups.
// On cache miss, records origin response to cache and notifies worker.
// Uses C++ cache/capability APIs directly.
//
// Build: This module is built as a dynamic nginx module.

extern "C" {
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
}

#include <sys/stat.h>
#include <unistd.h>  // dup()

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lib/cache/cache.h"
#include "lib/cache/cache_control_header.h"
#include "lib/cache/freshness.h"
#include "lib/cache/headers_sidecar.h"
#include "lib/cache/origin_cache_control.h"
#include "lib/cache/vary_emission.h"
#include "lib/cache/vary_storability.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/classify/url_normalizer.h"
#include "src/crypto/webbotauth/classifier.h"
#include "src/crypto/webbotauth/key_directory.h"
#include "src/crypto/webbotauth/rsl_cap_token.h"
#include "src/crypto/webbotauth/rsl_cap_validator.h"
#include "src/crypto/webbotauth/verifier.h"
#include "src/nginx/agent_self_fetch.h"
#include "src/nginx/authz_cache_gate.h"
#include "src/nginx/cache_generation_check.h"
#include "src/nginx/cookie_cache_gate.h"
#include "src/nginx/early_hints.h"
#include "src/nginx/early_hints_util.h"
#include "src/nginx/etag_util.h"
#include "src/nginx/header_key_util.h"
#include "src/nginx/headers_sidecar_record.h"
#include "src/nginx/headers_sidecar_replay.h"
#include "src/nginx/host_vouch.h"
#include "src/nginx/mime_util.h"
#include "src/nginx/revalidation_storability.h"
#include "src/nginx/zerocopy_barrier.h"
#include "src/product_version/version.h"
#include "src/proto/notification_sender.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/async_css_loader.h"
#include "src/worker/serve_stats.h"

using pagespeed::AlternateId;
using pagespeed::AlternateMetadata;
using pagespeed::BuildCacheControlHeader;
using pagespeed::CacheControlInput;
using pagespeed::CacheMode;
using pagespeed::CacheNotification;
using pagespeed::CapabilityMask;
using pagespeed::ClosePersistentConnection;
using pagespeed::ContentType;
using pagespeed::EvaluateFreshness;
using pagespeed::FreshnessConfig;
using pagespeed::FreshnessInput;
using pagespeed::FreshnessVerdict;
using pagespeed::MaskToAlternateId;
using pagespeed::PageSpeedCache;
using pagespeed::PageSpeedCacheConfig;
using pagespeed::ReadResult;
using pagespeed::ResetPersistentConnection;
using pagespeed::SendNotificationPersistent;
using pagespeed::SentinelId;
using pagespeed::WantsAgentMarkdown;

// Saved next filters in the body/header filter chains
static ngx_http_output_body_filter_pt ngx_http_next_body_filter;
static ngx_http_output_header_filter_pt ngx_http_next_header_filter;

// Singleton cache instance shared across all requests in this worker
// process.  Cyclone is configured with mmap'd directories so the
// worker process can also read/write the same volume file.
//
// LIFETIME (cache-generation safety): held by shared_ptr, not unique_ptr.
// On a cache HIT the response buffer points either INTO the mmap-backed
// ReadResult span (b->memory) or sendfiles from g_cache_sendfile_fd
// (b->in_file).  ngx_http_output_filter may DEFER the socket write for a
// slow client (NGX_AGAIN), so nginx re-reads that span / re-sendfiles from
// that fd across later event cycles AFTER the handler returns.  A cache
// generation change (worker purge/reset) replaces g_cache with a fresh
// instance and unmaps the old volume on destruction — see cache.h: "All
// ReadHandles must be destroyed before the PageSpeedCache is destroyed."
// Previously only ONE prior generation was retired (g_old_cache), so two
// back-to-back generation changes during a single slow in-flight response
// destroyed the gen the response still references → use-after-free /
// cross-tenant disclosure.  We now anchor the gen the request reads from
// to the request's own lifetime: each HIT stashes a shared_ptr<PageSpeedCache>
// (and a dup()'d sendfile fd) in its per-request ctx, so the mapping/fd
// outlive the deferred write no matter how many generation changes occur.
static std::shared_ptr<PageSpeedCache> g_cache;
static std::mutex g_cache_mutex;
static uint64_t g_cache_generation = 0;
static std::string g_cache_gen_path;
// File descriptor for sendfile-based cache serving.  Opened once when
// the cache is first opened; the persistent mmap ensures content pointers
// have known file offsets (ptr - mmap_base), enabling kernel-level
// zero-copy via sendfile instead of user-space writev.
//
// In-flight requests that sendfile from this volume dup() this fd into
// their per-request ctx (see the HIT path), so generation changes can
// close g_cache_sendfile_fd immediately on the next reopen without
// affecting deferred writes — the request owns its own descriptor.
static ngx_fd_t g_cache_sendfile_fd = NGX_INVALID_FILE;
static std::atomic<time_t> g_last_generation_check{0};
static constexpr time_t kGenerationCheckIntervalSec = 1;

// =================================================================
// Shared config read from pagespeed-shared.conf (written by worker)
// =================================================================

// Keep in sync with src/worker/shared_config.h — same fields and defaults.
struct SharedConfig {
  std::string socket_path = "/run/pagespeed-optimizer/notify.sock";
  bool disable_html = false;
  // Serve-side markdown gate.  Since 2.1 this is
  // the operator's agent_optimize flag as published by the worker; the wire
  // key keeps its historical name.
  bool agent_optimize_entitled = false;
  bool agent_optimize_llms_txt_enabled = false;  // /llms.txt serve gate
  // Observe-only Web Bot Auth classify toggle (default off — no
  // signature work at all when unset), the operator verified-bot registry
  // ("kid=name,..."), and the directory hosts keyids resolve under.
  bool web_bot_auth = false;
  std::string web_bot_auth_verified_bots;
  std::string web_bot_auth_directory_hosts;
  // Experimental: opt-in verified-crawl counter mode
  // (off/private/public; default off) gating /.well-known/webbotauth-counter.
  // NON-secret; the gating bearer token rides an env var, never shared config.
  std::string web_bot_auth_public_counter = "off";
  // Experimental: RSL-CAP capability-token enforcement toggle
  // (default off — zero token work when unset), the required license/scope, an
  // optional issuer pin, and the RSL issuer directory hosts (realm "rsl").
  bool rsl_cap_enforcement = false;
  std::string rsl_cap_directory_hosts;
  std::string rsl_cap_requested_license;
  std::string rsl_cap_requested_scope;
  std::string rsl_cap_issuer;
  // The volume size the worker opened its cache with; 0 = not stated (an
  // older worker, or one that could not tell).  Mirrored here only to keep
  // this struct a faithful copy of the one it is documented to track — THIS
  // module never consults it, because it opens the cache with volume_size = 0
  // and lets Cyclone resolve the existing file (see GetCache).  A peer that
  // cannot do that, such as an out-of-process reader on another product's
  // release train, needs the real number and reads it through the C API.
  uint64_t volume_size = 0;
  // The cache-directory generation the optimizer was compiled with; 0 = not
  // stated (an optimizer predating the field).  Compared with this module's
  // own generation by the cache-directory generation check (see
  // src/nginx/cache_generation_check.h).
  int cache_dir_generation = 0;
  std::string cache_mode;              // "safe" or "aggressive" (empty = safe)
  std::string strip_query_extensions;  // Comma-separated
  std::string strip_query_params;      // Comma-separated
};

// Global shared config.  Written under g_cache_mutex (in GetCache /
// CheckGenerationAndGetCache).  Read at request time without mutex —
// safe because nginx workers are single-threaded.
static SharedConfig g_shared_config;

// URL normalization config, populated from shared config + host aliases file.
static pagespeed::UrlNormalizationConfig g_url_norm_config;

// Shared mmap'd serve-time bandwidth stats.  Opened lazily in
// RefreshSharedConfigIfChanged (piggybacks on the ~1s poll interval).
// nullptr until the worker creates the file.
static pagespeed::ServeStats* g_serve_stats = nullptr;

// Web Bot Auth opt-in counter (experimental) — the SECRET
// bearer token gating the EXACT counter doc.  Read ONCE from the environment
// (PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN) at worker init — NOT via shared config
// (pagespeed-shared.conf is world-readable by design).  Empty = no token
// configured (the exact doc is then unreachable; only the coarse/404 surfaces
// exist).  NEVER logged, echoed, or exposed on any surface.
static std::string g_web_bot_auth_counter_token;

// =================================================================
// Web Bot Auth — observe-only classify state
// =================================================================
// The worker warm-fetches the operator-configured key directories and
// publishes them via pagespeed-webbotauth-keys.conf (written next to
// pagespeed-shared.conf); nginx re-reads that file on the same 1s
// mtime-based cadence as the shared config.  All state below is written on
// the single nginx worker thread (RefreshSharedConfigIfChanged /
// merge_loc_conf) and read at request time — same single-threaded safety
// argument as g_shared_config.
static pagespeed::webbotauth::WarmedKeyStore g_webbotauth_store;
static time_t g_webbotauth_keys_mtime = 0;
static pagespeed::webbotauth::VerifiedBotRegistry g_webbotauth_registry;
static std::vector<std::string> g_webbotauth_dir_hosts;

// =================================================================
// RSL-CAP enforcement (experimental) — enforcement state
// =================================================================
// The worker warm-fetches the RSL issuer key directories into realm "rsl" and
// publishes them via pagespeed-rslcap-keys.conf (separate file from the
// observe-only keys, a file-level trust-domain split).  Same single-nginx-
// worker-thread safety argument as g_shared_config / g_webbotauth_store.
static pagespeed::webbotauth::WarmedKeyStore g_rslcap_store;
static time_t g_rslcap_keys_mtime = 0;
static std::vector<std::string> g_rslcap_dir_hosts;

// Read pagespeed-shared.conf from disk (inline parser, no dependency on
// //src/worker:shared_config).  Returns defaults on any error.
static SharedConfig ReadSharedConfig(std::string_view cache_path,
                                     ngx_log_t* log) {
  SharedConfig config;

  if (cache_path.empty()) {
    return config;
  }

  // Derive path: parent_of(cache_path) / "pagespeed-shared.conf"
  std::filesystem::path conf_path =
      std::filesystem::path(cache_path).parent_path() / "pagespeed-shared.conf";

  std::ifstream f(conf_path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    ngx_log_error(NGX_LOG_WARN, log, 0,
                  "pagespeed: shared config not found at %s "
                  "(using defaults — worker may not have started yet)",
                  conf_path.c_str());
    return config;
  }

  static constexpr size_t kMaxSize = size_t{64} * 1024;
  auto size = f.tellg();
  if (size < 0 || static_cast<size_t>(size) > kMaxSize) {
    ngx_log_error(NGX_LOG_WARN, log, 0,
                  "pagespeed: shared config too large or unreadable at %s",
                  conf_path.c_str());
    return config;
  }

  f.seekg(0, std::ios::beg);
  std::string content(static_cast<size_t>(size), '\0');
  if (!f.read(content.data(), size)) {
    ngx_log_error(NGX_LOG_WARN, log, 0,
                  "pagespeed: failed to read shared config at %s",
                  conf_path.c_str());
    return config;
  }

  // Parse key=value lines (same logic as ParseSharedConfig in
  // src/worker/shared_config.cc).
  int version = -1;
  std::string_view sv(content);
  size_t pos = 0;
  while (pos < sv.size()) {
    size_t eol = sv.find('\n', pos);
    if (eol == std::string_view::npos) {
      eol = sv.size();
    }
    std::string_view line = sv.substr(pos, eol - pos);
    pos = eol + 1;

    // Strip trailing \r.
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    // Skip empty lines and comments.
    if (line.empty() || line[0] == '#') {
      continue;
    }

    // Find first '='.
    size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }

    std::string_view key = line.substr(0, eq);
    std::string_view value = line.substr(eq + 1);

    if (key == "version") {
      int v = 0;
      auto [ptr, ec] =
          std::from_chars(value.data(), value.data() + value.size(), v);
      if (ec == std::errc{}) {
        version = v;
      }
      if (version >= 2) {
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "pagespeed: shared config version %d not supported, "
                      "using defaults",
                      version);
        return SharedConfig{};
      }
    } else if (key == "pid") {
      // Informational only.
    } else if (key == "socket_path") {
      if (!value.empty()) {
        config.socket_path = std::string(value);
      }
    } else if (key == "disable_html") {
      config.disable_html = (value == "true" || value == "1");
    } else if (key == "agent_optimize_entitled") {
      config.agent_optimize_entitled = (value == "true" || value == "1");
    } else if (key == "agent_optimize_llms_txt_enabled") {
      config.agent_optimize_llms_txt_enabled =
          (value == "true" || value == "1");
    } else if (key == "web_bot_auth") {
      config.web_bot_auth = (value == "true" || value == "1");
    } else if (key == "web_bot_auth_verified_bots") {
      config.web_bot_auth_verified_bots = std::string(value);
    } else if (key == "web_bot_auth_directory_hosts") {
      config.web_bot_auth_directory_hosts = std::string(value);
    } else if (key == "web_bot_auth_public_counter") {
      if (value == "off" || value == "private" || value == "public") {
        config.web_bot_auth_public_counter = std::string(value);
      }
    } else if (key == "rsl_cap_enforcement") {
      config.rsl_cap_enforcement = (value == "true" || value == "1");
    } else if (key == "rsl_cap_directory_hosts") {
      config.rsl_cap_directory_hosts = std::string(value);
    } else if (key == "rsl_cap_requested_license") {
      config.rsl_cap_requested_license = std::string(value);
    } else if (key == "rsl_cap_requested_scope") {
      config.rsl_cap_requested_scope = std::string(value);
    } else if (key == "rsl_cap_issuer") {
      config.rsl_cap_issuer = std::string(value);
    } else if (key == "volume_size") {
      uint64_t v = 0;
      auto [ptr, ec] =
          std::from_chars(value.data(), value.data() + value.size(), v);
      if (ec == std::errc{} && ptr == value.data() + value.size()) {
        config.volume_size = v;
      }
    } else if (key == "cache_dir_generation") {
      // Same rule as the optimizer's own parser: anything but a positive
      // integer leaves 0, "not stated".
      int v = 0;
      auto [ptr, ec] =
          std::from_chars(value.data(), value.data() + value.size(), v);
      if (ec == std::errc{} && ptr == value.data() + value.size() && v > 0) {
        config.cache_dir_generation = v;
      }
    } else if (key == "cache_mode") {
      if (value == "safe" || value == "aggressive") {
        config.cache_mode = std::string(value);
      }
    } else if (key == "strip_query_extensions") {
      config.strip_query_extensions = std::string(value);
    } else if (key == "strip_query_params") {
      config.strip_query_params = std::string(value);
    }
    // Unknown keys silently ignored.
  }

  ngx_log_error(
      NGX_LOG_NOTICE, log, 0,
      "pagespeed: shared config loaded from %s "
      "(socket=%s, disable_html=%s, agent_optimize=%s, cache_mode=%s)",
      conf_path.c_str(), config.socket_path.c_str(),
      config.disable_html ? "true" : "false",
      config.agent_optimize_entitled ? "true" : "false",
      config.cache_mode.empty() ? "safe" : config.cache_mode.c_str());

  return config;
}

// Build URL normalization config from shared config + host aliases file.
// Called when shared config changes or cache is reopened.
static void RebuildUrlNormConfig(std::string_view cache_path,
                                 const SharedConfig& sc) {
  pagespeed::UrlNormalizationConfig cfg;

  // Parse comma-separated extensions.
  if (!sc.strip_query_extensions.empty()) {
    std::string_view sv(sc.strip_query_extensions);
    size_t pos = 0;
    while (pos < sv.size()) {
      size_t comma = sv.find(',', pos);
      if (comma == std::string_view::npos) comma = sv.size();
      std::string_view ext = sv.substr(pos, comma - pos);
      pos = comma + 1;
      if (!ext.empty()) {
        cfg.strip_query_extensions.emplace(ext);
      }
    }
  }

  // Parse comma-separated param keys.
  if (!sc.strip_query_params.empty()) {
    std::string_view sv(sc.strip_query_params);
    size_t pos = 0;
    while (pos < sv.size()) {
      size_t comma = sv.find(',', pos);
      if (comma == std::string_view::npos) comma = sv.size();
      std::string_view param = sv.substr(pos, comma - pos);
      pos = comma + 1;
      if (!param.empty()) {
        cfg.strip_query_params.emplace(param);
      }
    }
  }

  // Load host aliases from separate file.
  if (!cache_path.empty()) {
    std::filesystem::path hosts_path =
        std::filesystem::path(cache_path).parent_path() /
        "pagespeed-hosts.conf";
    std::ifstream f(hosts_path, std::ios::binary | std::ios::ate);
    if (f.is_open()) {
      static constexpr size_t kMaxSize = size_t{64} * 1024;
      auto size = f.tellg();
      if (size >= 0 && static_cast<size_t>(size) <= kMaxSize) {
        f.seekg(0, std::ios::beg);
        std::string content(static_cast<size_t>(size), '\0');
        if (f.read(content.data(), size)) {
          // Parse key=value lines (same format as host_aliases.cc).
          std::string_view sv(content);
          size_t pos = 0;
          while (pos < sv.size()) {
            size_t eol = sv.find('\n', pos);
            if (eol == std::string_view::npos) eol = sv.size();
            std::string_view line = sv.substr(pos, eol - pos);
            pos = eol + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty() || line[0] == '#') continue;
            size_t eq = line.find('=');
            if (eq == std::string_view::npos) continue;
            std::string_view k = line.substr(0, eq);
            std::string_view v_sv = line.substr(eq + 1);
            // Reject entries with embedded null bytes.
            if (k.find('\0') != std::string_view::npos ||
                v_sv.find('\0') != std::string_view::npos) {
              continue;
            }
            if (k == "version") {
              int v = 0;
              auto [ptr, ec] =
                  std::from_chars(v_sv.data(), v_sv.data() + v_sv.size(), v);
              if (ec == std::errc{} && v >= 2) {
                cfg.host_aliases.clear();
                break;
              }
              continue;
            }
            if (!k.empty() && !v_sv.empty()) {
              cfg.host_aliases[std::string(k)] = std::string(v_sv);
            }
          }
        }
      }
    }
  }

  g_url_norm_config = std::move(cfg);
}

// Trim ASCII whitespace from both ends of a string_view.
static std::string_view wba_trim(std::string_view sv) {
  while (!sv.empty() &&
         (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r')) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() &&
         (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r')) {
    sv.remove_suffix(1);
  }
  return sv;
}

// Rebuild the parsed verified-bot registry + directory host list
// from a freshly loaded shared config.  Called wherever g_shared_config is
// (re)read.  Both specs are operator config; entries are whitespace-trimmed
// ("kid1=a, kid2=b" registers "kid2"), and pairs with an empty keyid or an
// empty bot name are skipped with a notice so a typo'd registry is visible.
static void RebuildWebBotAuthConfig(const SharedConfig& sc, ngx_log_t* log) {
  pagespeed::webbotauth::VerifiedBotRegistry registry;
  {
    std::string_view spec(sc.web_bot_auth_verified_bots);
    size_t pos = 0;
    while (pos < spec.size()) {
      size_t comma = spec.find(',', pos);
      if (comma == std::string_view::npos) comma = spec.size();
      std::string_view pair = wba_trim(spec.substr(pos, comma - pos));
      pos = comma + 1;
      if (pair.empty()) continue;
      size_t eq = pair.find('=');
      std::string_view keyid = eq == std::string_view::npos
                                   ? std::string_view{}
                                   : wba_trim(pair.substr(0, eq));
      std::string_view name = eq == std::string_view::npos
                                  ? std::string_view{}
                                  : wba_trim(pair.substr(eq + 1));
      if (keyid.empty() || name.empty()) {
        if (log != nullptr) {
          ngx_log_error(NGX_LOG_NOTICE, log, 0,
                        "pagespeed: web_bot_auth_verified_bots entry \"%*s\" "
                        "is not keyid=name, skipped",
                        pair.size(), pair.data());
        }
        continue;
      }
      registry.Register(keyid, name);
    }
  }
  g_webbotauth_registry = std::move(registry);

  std::vector<std::string> hosts;
  {
    std::string_view sv(sc.web_bot_auth_directory_hosts);
    size_t pos = 0;
    while (pos < sv.size()) {
      size_t comma = sv.find(',', pos);
      if (comma == std::string_view::npos) comma = sv.size();
      std::string_view host = wba_trim(sv.substr(pos, comma - pos));
      pos = comma + 1;
      if (!host.empty()) {
        hosts.emplace_back(host);
      }
    }
  }
  if (sc.web_bot_auth && hosts.empty() && log != nullptr) {
    ngx_log_error(NGX_LOG_WARN, log, 0,
                  "pagespeed: web_bot_auth is enabled but no key-directory "
                  "hosts are published — signed requests will classify as "
                  "unknown until the worker warms a directory");
  }
  g_webbotauth_dir_hosts = std::move(hosts);
}

// Re-read the worker-published warmed key file when its mtime
// changes.  Runs on the 1-second poll cadence; disabled feature = zero work
// beyond one bool check.  A missing file keeps the current store (entries
// lapse on their own TTL — strict expiry); an unrecognized/future-format
// file yields an empty store (fail-closed: signed requests classify as
// unknown).  The last-seen mtime is recorded only AFTER a successful parse,
// so a transient open/read failure or an oversize file is retried on the
// next poll instead of being silently skipped until the next worker write.
static void RefreshWebBotAuthKeysIfChanged(std::string_view cache_path,
                                           ngx_log_t* log) {
  if (!g_shared_config.web_bot_auth || cache_path.empty()) {
    return;
  }
  std::filesystem::path keys_path =
      std::filesystem::path(cache_path).parent_path() /
      "pagespeed-webbotauth-keys.conf";
  struct stat st;
  if (::stat(keys_path.c_str(), &st) != 0) {
    return;  // Not written yet (or removed) — keep current store.
  }
  if (st.st_mtime == g_webbotauth_keys_mtime) {
    return;  // Unchanged.
  }

  static constexpr size_t kMaxKeysFileSize = size_t{1} * 1024 * 1024;
  std::ifstream f(keys_path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    ngx_log_error(NGX_LOG_WARN, log, 0,
                  "pagespeed: cannot open web-bot-auth keys file at %s "
                  "(will retry)",
                  keys_path.c_str());
    return;
  }
  auto size = f.tellg();
  if (size < 0 || static_cast<size_t>(size) > kMaxKeysFileSize) {
    ngx_log_error(NGX_LOG_WARN, log, 0,
                  "pagespeed: web-bot-auth keys file at %s is oversize or "
                  "unreadable (limit %uz bytes), keeping current keys",
                  keys_path.c_str(), kMaxKeysFileSize);
    return;
  }
  f.seekg(0, std::ios::beg);
  std::string content(static_cast<size_t>(size), '\0');
  if (!f.read(content.data(), size)) {
    ngx_log_error(NGX_LOG_WARN, log, 0,
                  "pagespeed: failed to read web-bot-auth keys file at %s "
                  "(will retry)",
                  keys_path.c_str());
    return;
  }
  if (!pagespeed::webbotauth::WarmedKeyStore::Deserialize(
          content, &g_webbotauth_store)) {
    // Deserialize fail-closed to an EMPTY store (unknown verdicts) — that is
    // the correct trust posture, but tell the operator why.
    ngx_log_error(NGX_LOG_WARN, log, 0,
                  "pagespeed: web-bot-auth keys file at %s has an "
                  "unrecognized format, key store cleared (fail-closed)",
                  keys_path.c_str());
  }
  g_webbotauth_keys_mtime = st.st_mtime;
}

// Parse the worker-published RSL issuer directory hosts (realm
// "rsl") into the lookup-order list the enforcement handler resolves keyids
// against.  Same comma-split shape as the observe-only host list.
static void RebuildRslCapConfig(const SharedConfig& sc, ngx_log_t* log) {
  std::vector<std::string> hosts;
  std::string_view sv(sc.rsl_cap_directory_hosts);
  size_t pos = 0;
  while (pos < sv.size()) {
    size_t comma = sv.find(',', pos);
    if (comma == std::string_view::npos) comma = sv.size();
    std::string_view host = wba_trim(sv.substr(pos, comma - pos));
    pos = comma + 1;
    if (!host.empty()) {
      hosts.emplace_back(host);
    }
  }
  if (sc.rsl_cap_enforcement && hosts.empty() && log != nullptr) {
    ngx_log_error(NGX_LOG_WARN, log, 0,
                  "pagespeed: rsl_cap_enforcement is enabled but no key-"
                  "directory hosts are published — every token will be "
                  "rejected as an unknown issuer until the worker warms a "
                  "directory");
  }
  g_rslcap_dir_hosts = std::move(hosts);
}

// Re-read the worker-published RSL-CAP key file when its mtime
// changes.  Mirrors RefreshWebBotAuthKeysIfChanged exactly, but over the
// separate pagespeed-rslcap-keys.conf and the enforcement toggle.
static void RefreshRslCapKeysIfChanged(std::string_view cache_path,
                                       ngx_log_t* log) {
  if (!g_shared_config.rsl_cap_enforcement || cache_path.empty()) {
    return;
  }
  std::filesystem::path keys_path =
      std::filesystem::path(cache_path).parent_path() /
      "pagespeed-rslcap-keys.conf";
  struct stat st;
  if (::stat(keys_path.c_str(), &st) != 0) {
    return;  // Not written yet (or removed) — keep current store.
  }
  if (st.st_mtime == g_rslcap_keys_mtime) {
    return;  // Unchanged.
  }

  static constexpr size_t kMaxKeysFileSize = size_t{1} * 1024 * 1024;
  std::ifstream f(keys_path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    ngx_log_error(NGX_LOG_WARN, log, 0,
                  "pagespeed: cannot open rsl-cap keys file at %s (will retry)",
                  keys_path.c_str());
    return;
  }
  auto size = f.tellg();
  if (size < 0 || static_cast<size_t>(size) > kMaxKeysFileSize) {
    ngx_log_error(NGX_LOG_WARN, log, 0,
                  "pagespeed: rsl-cap keys file at %s is oversize or "
                  "unreadable (limit %uz bytes), keeping current keys",
                  keys_path.c_str(), kMaxKeysFileSize);
    return;
  }
  f.seekg(0, std::ios::beg);
  std::string content(static_cast<size_t>(size), '\0');
  if (!f.read(content.data(), size)) {
    ngx_log_error(NGX_LOG_WARN, log, 0,
                  "pagespeed: failed to read rsl-cap keys file at %s "
                  "(will retry)",
                  keys_path.c_str());
    return;
  }
  if (!pagespeed::webbotauth::WarmedKeyStore::Deserialize(content,
                                                          &g_rslcap_store)) {
    // Fail-closed to an EMPTY store (unknown-issuer -> 401) — the correct
    // trust posture, but tell the operator why.
    ngx_log_error(NGX_LOG_WARN, log, 0,
                  "pagespeed: rsl-cap keys file at %s has an unrecognized "
                  "format, key store cleared (fail-closed)",
                  keys_path.c_str());
  }
  g_rslcap_keys_mtime = st.st_mtime;
}

// =================================================================
// Cache-directory generation check
// =================================================================
// Compares the generation the optimizer published in pagespeed-shared.conf
// (cache_dir_generation=) with this module's own, on every read of that file:
// at configuration time (start and reload), and on the ~1s mtime poll, so a
// completed rolling upgrade re-enables the cache without a reload.  On a
// mismatch the module runs with the shared cache disabled (pass-through);
// when the optimizer states no generation it carries on as before.  Why
// pass-through and not "keep using our own volume":
// src/nginx/cache_generation_check.h.
//
// Written under g_cache_mutex; read on the request path without it — same
// single-threaded nginx worker argument as g_shared_config.
static pagespeed::CacheGenerationMonitor g_cache_dir_gen(
    pagespeed::ModuleCacheDirGeneration());
// The shared config the verdict was read from, for the log line.
static std::string g_cache_dir_gen_conf_path;

static std::string SharedConfigPathFor(std::string_view cache_path) {
  return (std::filesystem::path(cache_path).parent_path() /
          "pagespeed-shared.conf")
      .string();
}

// Log the verdict if it has not been logged yet (a change, or a new
// configuration cycle).  A mismatch is an error, "unknown" a warning, a
// match a notice.
static void LogCacheDirGenerationIfPending(ngx_log_t* log) {
  if (!g_cache_dir_gen.log_pending()) {
    return;
  }
  const pagespeed::CacheGenerationVerdict& v = g_cache_dir_gen.verdict();
  const std::string line = pagespeed::DescribeCacheGenerationVerdict(
      v, g_cache_dir_gen.logged_state(), g_cache_dir_gen_conf_path);
  ngx_uint_t level = NGX_LOG_NOTICE;
  if (v.state == pagespeed::CacheGenerationState::kMismatch) {
    level = NGX_LOG_ERR;
  } else if (v.state == pagespeed::CacheGenerationState::kUnknown) {
    level = NGX_LOG_WARN;
  }
  ngx_log_error(level, log, 0, "%s", line.c_str());
  g_cache_dir_gen.MarkLogged();
}

// Re-evaluate the generation check against a freshly read g_shared_config.
// Call after EVERY assignment of g_shared_config from ReadSharedConfig, with
// g_cache_mutex held.  `log` == nullptr defers the log line (configuration
// time: the error log is not open yet, so init_module logs it instead).
static void ApplyCacheDirGeneration(std::string_view cache_path,
                                    ngx_log_t* log) {
  g_cache_dir_gen_conf_path = SharedConfigPathFor(cache_path);
  g_cache_dir_gen.Update(g_shared_config.cache_dir_generation);
  if (!g_cache_dir_gen.cache_enabled() && g_cache) {
    // Mismatch: stop holding the volume.  In-flight requests keep the
    // instance (and its mmap) alive through their own shared_ptr; GetCache
    // reopens once the generations match again.
    g_cache.reset();
    if (g_cache_sendfile_fd != NGX_INVALID_FILE) {
      ngx_close_file(g_cache_sendfile_fd);
      g_cache_sendfile_fd = NGX_INVALID_FILE;
    }
  }
  if (log != nullptr) {
    LogCacheDirGenerationIfPending(log);
  }
}

// Mtime of the last successfully loaded shared config file.
// Used to avoid re-reading an unchanged file every second.
static time_t g_shared_config_mtime = 0;

// Re-read pagespeed-shared.conf if the file mtime has changed.
// Called on the 1-second generation-check cadence.  Caller must NOT
// hold g_cache_mutex.
static void RefreshSharedConfigIfChanged(std::string_view cache_path,
                                         ngx_log_t* log) {
  if (cache_path.empty()) {
    return;
  }

  // Lazily open serve-stats mmap (created by worker).  Retry on every poll
  // because the worker may create the file after the shared config is written.
  if (g_serve_stats == nullptr) {
    std::string stats_path = pagespeed::ServeStatsPath(std::string(cache_path));
    g_serve_stats = pagespeed::OpenServeStats(stats_path);
  }

  std::filesystem::path conf_path =
      std::filesystem::path(cache_path).parent_path() / "pagespeed-shared.conf";
  struct stat st;
  if (::stat(conf_path.c_str(), &st) == 0 &&
      st.st_mtime != g_shared_config_mtime) {
    g_shared_config_mtime = st.st_mtime;
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_shared_config = ReadSharedConfig(cache_path, log);
    RebuildUrlNormConfig(cache_path, g_shared_config);
    RebuildWebBotAuthConfig(g_shared_config, log);
    RebuildRslCapConfig(g_shared_config, log);
    ApplyCacheDirGeneration(cache_path, log);
  }

  // The warmed-key file has its own writer cadence (the worker's
  // refresh timer), so check it on every poll independently of the
  // shared-config mtime — but AFTER the config read above, so the poll that
  // first observes web_bot_auth=true also loads the published keys instead
  // of skipping them behind the still-false toggle.
  RefreshWebBotAuthKeysIfChanged(cache_path, log);
  // Same independent-cadence poll for the RSL-CAP enforcement
  // keys file (separate writer, separate file).
  RefreshRslCapKeysIfChanged(cache_path, log);
}

// Build the cache config used by EVERY nginx-side cache open (initial open
// in GetCache and the reopen in CheckGenerationAndGetCache).  Single
// construction point so the two paths can never drift (issue #652: the
// reopen path used to rebuild the config inline — any override added only
// to GetCache silently vanished after the first generation change).
static PageSpeedCacheConfig MakeNginxCacheConfig(std::string_view cache_path) {
  PageSpeedCacheConfig config;
  config.volume_path = std::string(cache_path);
  // Auto-detect volume size from the existing file rather than using the
  // 1GB default.  The worker creates the volume at its configured size
  // (e.g. 2GB via --cache-size); using a mismatched size here causes
  // Cyclone to compute a different stripe layout, making nginx writes
  // invisible to the worker and vice versa.
  config.volume_size = 0;
  // CRC32 verification enabled — Cyclone's validation cache skips the
  // CRC on repeat reads of already-verified entries, so the first-read
  // cost is amortized to near zero on hot content.
  config.verify_checksum_on_read = true;
  // Disable the per-process RAM tier in the nginx serve process
  // (issue #652).  It is keyed (CacheKey, AlternateId) with no validation
  // against the on-disk document and is only evicted by the *removing*
  // process — a worker-side purge followed by a re-write at the same slot
  // would serve pre-purge bytes+metadata from here indefinitely.  Serving
  // already uses the dup()'d sendfile FD / mmap path; the RAM tier only
  // short-circuits docs <= 32KB, and Cyclone's validation cache amortizes
  // CRC on repeat reads, so the cost is a shared-mmap memory read.
  // Re-enable once Cyclone validates RAM hits against the directory entry
  // (tracked as a Cyclone follow-up).
  config.ram_cache_size = 0;
  return config;
}

// Returns a shared_ptr to the current cache so callers (the request
// handler) can anchor the instance to the request's lifetime, keeping the
// mmap-backed ReadHandles valid across deferred (slow-client) writes even
// if a later generation change replaces g_cache.
static std::shared_ptr<PageSpeedCache> GetCache(std::string_view cache_path,
                                                ngx_log_t* log) {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  if (g_cache) {
    return g_cache;
  }
  // Cache-directory generation mismatch: run uncached.  The volume at
  // cache_path is not the one the optimizer writes, so do not open (or
  // create, or "recover") anything there.
  if (!g_cache_dir_gen.cache_enabled()) {
    return nullptr;
  }

  PageSpeedCacheConfig config = MakeNginxCacheConfig(cache_path);
  auto result = PageSpeedCache::Create(config);
  if (!result.has_value()) {
    // Brief backoff before retry — handles the window where the old worker
    // process is still releasing Cyclone locks during exit_process.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    result = PageSpeedCache::Create(config);
  }
  if (!result.has_value()) {
    // Auto-recovery is for a damaged volume only: delete the volume file(s)
    // and recreate once.  A volume another live process (normally the
    // optimizer worker) holds, a refused open (ownership, permission,
    // symlink), or an I/O or configuration error is NOT damage: deleting
    // would leave the other process writing a file this one never reads (a
    // silent split cache).  Those leave every file in place, are logged
    // with the reason, and requests run without the cache (degraded).  The
    // classification is the one the worker uses (#428).
    bool deleted_damaged_volume = false;
    cyclone::CacheError first_error = cyclone::CacheError::Success;
    result = PageSpeedCache::CreateRecreatingDamagedVolume(
        config, &deleted_damaged_volume, &first_error);
    if (deleted_damaged_volume) {
      ngx_log_error(NGX_LOG_WARN, log, 0,
                    "pagespeed: cache open failed for %*s (%s), damaged "
                    "volume deleted and recreated",
                    cache_path.size(), cache_path.data(),
                    make_error_code(first_error).message().c_str());
    } else if (!result.has_value()) {
      ngx_log_error(NGX_LOG_WARN, log, 0,
                    "pagespeed: cache open failed for %*s (%s); the volume "
                    "is left in place (not damaged, or held by another "
                    "process) and requests run without the cache (degraded)",
                    cache_path.size(), cache_path.data(),
                    make_error_code(result.error()).message().c_str());
    }
  }
  if (result.has_value()) {
    g_cache = std::move(*result);
    g_cache_gen_path = std::string(cache_path) + ".gen";
    g_cache_generation = PageSpeedCache::ReadGenerationFile(g_cache_gen_path);
    g_shared_config = ReadSharedConfig(cache_path, log);
    RebuildUrlNormConfig(cache_path, g_shared_config);

    // Open a separate FD for sendfile-based cache serving.  This is the
    // first open in this worker (GetCache returns early once g_cache
    // exists), so there is no prior FD to retire here.  In-flight requests
    // that sendfile dup() this FD into their ctx, so subsequent generation
    // changes can close it without affecting deferred writes.
    // Cyclone's structural-fingerprint naming means the on-disk file is
    // not the configured cache_path — take the real path from the live
    // cache instance.
    const std::string path_str = g_cache->VolumeFilePath();
    g_cache_sendfile_fd =
        ngx_open_file(path_str.c_str(), NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (g_cache_sendfile_fd == NGX_INVALID_FILE) {
      ngx_log_error(NGX_LOG_WARN, log, ngx_errno,
                    "pagespeed: sendfile FD open failed for %*s, "
                    "falling back to mmap serving",
                    path_str.size(), path_str.data());
    }

    ngx_log_error(NGX_LOG_NOTICE, log, 0, "pagespeed: cache opened at %*s",
                  cache_path.size(), cache_path.data());
    // Last: a mismatch found by this re-read releases the cache just opened.
    ApplyCacheDirGeneration(cache_path, log);
  } else {
    ngx_log_error(NGX_LOG_ERR, log, 0,
                  "pagespeed: PageSpeedCache::Create failed for %*s, "
                  "running without the cache",
                  cache_path.size(), cache_path.data());
  }
  return g_cache;
}

// Check if the cache generation has changed (worker purge/reset) and
// reopen the cache if so.  Returns a shared_ptr to the current cache, which
// may be a freshly opened instance.  Caller must not hold g_cache_mutex.
//
// The retired generation is NOT explicitly kept in a global; instead, any
// in-flight request that took a HIT against it holds a shared_ptr to it in
// its ctx (see the handler), so the old instance — and its mmap — survive
// exactly as long as some deferred (slow-client) write still references it.
// This replaces the old fixed one-deep g_old_cache retirement, which could
// destroy gen N out from under an in-flight response after two back-to-back
// generation changes.
static std::shared_ptr<PageSpeedCache> CheckGenerationAndGetCache(
    std::string_view cache_path, ngx_log_t* log) {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  if (!g_cache || g_cache_gen_path.empty()) {
    return g_cache;
  }
  uint64_t gen = PageSpeedCache::ReadGenerationFile(g_cache_gen_path);
  if (gen == g_cache_generation) {
    return g_cache;
  }
  ngx_log_error(NGX_LOG_NOTICE, log, 0,
                "pagespeed: cache generation changed %" PRIu64 " -> %" PRIu64
                ", reopening",
                g_cache_generation, gen);
  g_cache_generation = gen;

  // Shared construction with GetCache — keeps the ram_cache_size=0
  // override (and any future override) in effect after a generation
  // change reopen (issue #652).
  PageSpeedCacheConfig config = MakeNginxCacheConfig(cache_path);
  auto result = PageSpeedCache::Create(config);
  if (result.has_value()) {
    // Drop our owning reference to the retired instance.  It stays alive
    // (and its mmap mapped) as long as any in-flight request's ctx holds a
    // shared_ptr to it; the instance is destroyed when the last such
    // request completes.
    g_cache = std::move(*result);
    g_shared_config = ReadSharedConfig(cache_path, log);
    RebuildUrlNormConfig(cache_path, g_shared_config);

    // Reopen the sendfile FD for the new cache volume.  The previous FD can
    // be closed immediately: in-flight requests that sendfile dup() it into
    // their ctx, so their deferred writes read from their own descriptors,
    // not this one.
    if (g_cache_sendfile_fd != NGX_INVALID_FILE) {
      ngx_close_file(g_cache_sendfile_fd);
      g_cache_sendfile_fd = NGX_INVALID_FILE;
    }
    // Take the real (fingerprint-named) path from the fresh instance.
    const std::string path_str = g_cache->VolumeFilePath();
    g_cache_sendfile_fd =
        ngx_open_file(path_str.c_str(), NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (g_cache_sendfile_fd == NGX_INVALID_FILE) {
      ngx_log_error(NGX_LOG_WARN, log, ngx_errno,
                    "pagespeed: sendfile FD reopen failed for %*s, "
                    "falling back to mmap serving",
                    path_str.size(), path_str.data());
    }
    // Last: a mismatch found by this re-read releases the reopened cache.
    ApplyCacheDirGeneration(cache_path, log);
  } else {
    // Reopen failed: drop the stale instance so the GetCache() fallback
    // recreates the cache from scratch (matches prior behavior, where
    // g_cache had been moved out before the failed Create()).
    g_cache.reset();
    if (g_cache_sendfile_fd != NGX_INVALID_FILE) {
      ngx_close_file(g_cache_sendfile_fd);
      g_cache_sendfile_fd = NGX_INVALID_FILE;
    }
    ngx_log_error(NGX_LOG_ERR, log, 0,
                  "pagespeed: cache reopen failed for %*s after generation "
                  "change",
                  cache_path.size(), cache_path.data());
  }
  return g_cache;
}

// Time-throttled shared-state poll: at most once per
// kGenerationCheckIntervalSec, detect a worker cache purge/reset
// (CheckGenerationAndGetCache) and re-read the shared config + published key
// files (RefreshSharedConfigIfChanged).  Returns the (re)opened cache when
// this call won the throttle slot, nullptr otherwise (callers that need the
// cache fall back to GetCache()).
//
// Single throttle point shared by BOTH request-path callers — the PREACCESS
// RSL-CAP enforcement handler and the PRECONTENT optimization handler — so
// the poll fires even when enforcement denials (401/402) finalize requests
// before the PRECONTENT phase runs.  Without that, an enforcement-enabled
// nginx whose key store was empty (cold start inside the worker's initial
// warm delay, or a rotation transiently emptying the directory) would 401
// every request in PREACCESS, the PRECONTENT-phase poll would never fire, and
// the store could never re-warm: a self-perpetuating lockout.  The atomic CAS
// makes the two callers idempotent within one interval: whichever runs first
// does the work, the other is a no-op.
static std::shared_ptr<PageSpeedCache> MaybePollSharedState(
    std::string_view cache_path, ngx_log_t* log) {
  if (cache_path.empty()) {
    return nullptr;
  }
  time_t now = ngx_time();
  time_t last = g_last_generation_check.load(std::memory_order_relaxed);
  if (now - last >= kGenerationCheckIntervalSec) {
    if (g_last_generation_check.compare_exchange_strong(
            last, now, std::memory_order_relaxed)) {
      std::shared_ptr<PageSpeedCache> cache =
          CheckGenerationAndGetCache(cache_path, log);
      RefreshSharedConfigIfChanged(cache_path, log);
      return cache;
    }
  }
  return nullptr;
}

// =================================================================
// Hot URL tracking for proactive variant warmup
// =================================================================

// Fixed-size hash map tracking fallback-hit counts per URL hash.
// When a URL crosses the threshold, a warmup notification is sent.
static constexpr size_t kHotUrlMapSize = 4096;  // Must be power of 2
static constexpr int kDefaultHotThreshold = 5;

struct HotUrlEntry {
  std::atomic<uint64_t> url_hash{0};
  std::atomic<int> count{0};
};

static HotUrlEntry g_hot_url_map[kHotUrlMapSize];

// FNV-1a hash for URL strings (fast, good distribution).
static uint64_t FNV1a(std::string_view s) {
  uint64_t hash = 14695981039346656037ULL;
  for (char c : s) {
    hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
    hash *= 1099511628211ULL;
  }
  return hash;
}

// Module configuration
typedef struct {
  ngx_flag_t enable;
  ngx_str_t cache_path;
  ngx_flag_t hot_threshold;        // Fallback hits before warmup (0=disabled)
  ngx_array_t* disallow_patterns;  // Array of ngx_str_t patterns
  ngx_int_t max_age_cap;           // Upper bound on max-age (default 86400)
  ngx_int_t
      immutable_max_age_cap;  // Upper bound for immutable (default 604800)
  ngx_flag_t synthesize_swr;  // Add stale-while-revalidate (default on)
  ngx_flag_t conditional_revalidation;  // Conditional revalidation (default on)
  ngx_int_t html_max_age;   // Default max-age for HTML (no CC, default: 0)
  ngx_int_t css_max_age;    // Default max-age for CSS/JS (no CC, default: 300)
  ngx_int_t image_max_age;  // Default max-age for images (no CC, default: 3600)
  ngx_flag_t
      force_refresh_html;  // Force revalidation on Ctrl+F5 for HTML (default: on)
  ngx_flag_t
      force_refresh;  // Force revalidation on Ctrl+F5 for all types (default: off)
  ngx_flag_t
      trust_x_forwarded_proto;  // Trust X-Forwarded-Proto header (default: off)
  ngx_uint_t cache_mode;        // 0 = safe (default), 1 = aggressive
  // Stale-if-error window in seconds (issue #652 review, RFC 9111 §4.2.4):
  // when a freshness-driven re-fetch fails upstream (>= 500), the stashed
  // stale entry is served instead of the error for up to this long past
  // its freshness lifetime.  0 disables.  Default: 86400 (matches the
  // stale-if-error directive synthesized downstream in aggressive mode).
  ngx_int_t stale_if_error_max_age;
} ngx_http_pagespeed_loc_conf_t;

// Per server block: a copy of the block's server names.  nginx keeps its own
// list (ngx_http_core_srv_conf_t::server_names) in the configuration's
// temporary pool, which is released once the configuration is read, so the
// request path cannot use it.  The names and compiled regexes it points to
// live in the configuration pool and stay valid.
typedef struct {
  ngx_http_server_name_t* server_names;
  ngx_uint_t server_names_count;
} ngx_http_pagespeed_srv_conf_t;

// Maximum response body size to record to cache (10MB).  Responses
// exceeding this limit are still served to the client but are not
// cached — this guards against unbounded memory allocation in the
// body filter when buffering upstream responses.
static constexpr size_t kMaxRecordingSize = 10 * 1024 * 1024;

// Conditional revalidation stats (per-worker process).
static std::atomic<uint64_t> g_conditional_revalidations{0};
static std::atomic<uint64_t> g_conditional_304s{0};
static std::atomic<uint64_t> g_conditional_200s{0};
static std::atomic<uint64_t> g_etag_too_long{0};
// Issue #652 review: bounded stale-while-revalidate serves (refresh
// coalesced onto another request's in-flight re-fetch) and stale-if-error
// serves (upstream >= 500 answered with the stashed stale entry).  Exported on
// /v1/metrics via the shared ServeStats mmap (Record{Swr,StaleIfError}* below).

// Zero-copy serve barrier counters, exported on /v1/metrics via the
// shared ServeStats mmap (Record* helpers below), the same cross-process path
// as RecordServeHit.  Torn aborts additionally emit a distinctive WARN log at
// the abort site so an operator sees them in the error log too:
//   * torn_aborts        — strict renew returned kTorn (a wrap overwrote the
//                          borrowed region): the serve was failed closed so a
//                          client never receives foreign bytes.
//   * proactive_copyouts — the aliased tail was copied into request-owned
//                          memory ahead of a wrap (kCopyNow, leases-off, or a
//                          forced-wrap deadline within the safety margin).
//   * copy_then_verify_discards — a wrap raced the de-alias memcpy and the
//                          post-copy re-check saw kTorn: the copy was discarded
//                          and the serve failed closed.

// =====================================================================
// Per-URL single-flight refresh (issue #652 review): when an age-expired
// entry triggers a re-fetch (kRevalidate), only ONE request per URL per
// grace window actually goes to origin; concurrent requests within the
// window serve the cached (stale) entry — true bounded
// stale-while-revalidate instead of an N-way origin stampede at every TTL
// expiry (and, for born-stale upstreams, a permanent per-request fetch).
// Per-worker-process state: with W nginx workers the origin sees at most
// W coalesced re-fetches per window, which is the accepted bound.
// =====================================================================
static constexpr uint32_t kRefreshGraceWindowSecs = 10;
static constexpr size_t kMaxRefreshSlotEntries = 1024;
static std::mutex g_refresh_slot_mutex;
static std::unordered_map<std::string, time_t> g_refresh_slots;

// Try to become the request that performs the origin re-fetch for `key`.
// Returns true when the caller owns the refresh (no other refresh started
// within `grace` seconds); false when a refresh is already in flight /
// recently completed and the caller should serve the cached stale entry.
// Overflow policy mirrors the worker's origin-refresh limiter: sweep only
// EXPIRED slots; if every slot is live, refresh WITHOUT tracking (fail
// toward correctness — an untracked URL stampedes, it never serves
// indefinitely-stale).
static bool ngx_http_pagespeed_acquire_refresh_slot(const std::string& key,
                                                    time_t now,
                                                    uint32_t grace) {
  std::lock_guard<std::mutex> lock(g_refresh_slot_mutex);
  auto it = g_refresh_slots.find(key);
  if (it != g_refresh_slots.end() &&
      now - it->second < static_cast<time_t>(grace)) {
    return false;  // refresh in flight / within grace — serve stale
  }
  if (it != g_refresh_slots.end()) {
    it->second = now;
    return true;
  }
  if (g_refresh_slots.size() >= kMaxRefreshSlotEntries) {
    std::erase_if(g_refresh_slots, [now](const auto& kv) {
      return now - kv.second >= static_cast<time_t>(kRefreshGraceWindowSecs);
    });
    if (g_refresh_slots.size() >= kMaxRefreshSlotEntries) {
      return true;  // cannot track without dropping a live window
    }
  }
  g_refresh_slots.emplace(key, now);
  return true;
}

// Rate-limited missing-CC warning log: one warning per (hostname,
// content-type-category) per nginx worker lifetime.  Capped at 1024
// entries (~64KB) to bound memory regardless of tenant count.
static constexpr size_t kMaxCCWarnings = 1024;
static std::unordered_set<std::string> g_missing_cc_warned;

// Per-request context
typedef struct {
  CapabilityMask mask;            // Stored by value (5 bytes, no heap alloc)
  ngx_chain_t* buffered_body;     // Buffered upstream response body
  ngx_chain_t** buffered_last;    // Pointer to last link for appending
  ReadResult* read_result;        // Cache read result (mmap'd, with metadata)
  ReadResult* stale_read_result;  // Stale entry during conditional revalidation
  // Owning reference to the cache GENERATION that read_result/
  // stale_read_result point into.  Keeps that PageSpeedCache instance (and
  // its mmap) alive for the whole request — including any deferred
  // slow-client write — even across back-to-back generation changes.
  // Heap-allocated because the ctx is nginx-pool-allocated (no C++ dtor);
  // freed in ngx_http_pagespeed_cleanup.  nullptr until a cache HIT.
  std::shared_ptr<PageSpeedCache>* cache_anchor;
  // Request-owned dup() of the sendfile FD when serving a HIT via sendfile.
  // The deferred write reads from this descriptor, so a generation change
  // closing the global FD cannot make it read a closed/recycled FD.  Closed
  // in ngx_http_pagespeed_cleanup.  Initialized to NGX_INVALID_FILE at ctx
  // creation (NGX_INVALID_FILE is -1, not 0, so ngx_pcalloc zeroing is not
  // a valid sentinel).
  ngx_fd_t sendfile_fd;
  // Lease renewal timer.  A cache HIT parks the ReadResult (a
  // Cyclone mmap borrow) on this ctx until request cleanup; a slow client
  // (or a slow upstream during stale revalidation) keeps the borrow alive
  // far past the read lease's 3T/4 protection floor.  This request-scoped
  // timer re-stamps the lease every kLeaseRenewIntervalMs while
  // read_result / stale_read_result are held.  Pool-allocated on first
  // arm; the pending timer is deleted in ngx_http_pagespeed_cleanup.
  ngx_event_t* lease_renew_event;
  // Zero-copy barrier: the emitted output buffer that still aliases
  // read_result's mmap span (b->memory) or sendfiles from the cache volume
  // (b->in_file).  Non-NULL only while an aliased HIT/llms/stale serve is
  // draining; the lease-renewal timer re-checks the borrow in this buffer's
  // stack and, on a wrap verdict, de-aliases its unsent tail in place (or
  // finalizes the request if the region is already torn).  Reset to NULL once
  // the buffer is owned (copied out), fully drained, or the serve is aborted.
  ngx_buf_t* aliased_buf;
  ngx_table_elt_t* saved_client_if_none_match;      // Client's original INM
  ngx_table_elt_t* saved_client_if_modified_since;  // Client's original IMS
  size_t recorded_bytes;   // Accumulated bytes buffered for recording
  unsigned recording : 1;  // Whether we're recording this response
  unsigned done : 1;       // Whether recording is complete
  unsigned waiting_for_worker : 1;
  unsigned served_from_cache : 1;  // Whether response was served from cache
  unsigned uncacheable : 1;        // no-store, private, or unsupported Vary
  unsigned no_transform : 1;       // Cache-Control: no-transform
  // Origin Vary names Accept: the origin does its own content negotiation.
  // Stamped onto the stored original as kFlagOriginVariesAccept and, like
  // no_transform, keeps the entry out of the optimizer entirely.
  unsigned origin_varies_accept : 1;
  unsigned head_request : 1;           // Method is HEAD
  unsigned stale_revalidation : 1;     // Conditional revalidation in progress
  unsigned serve_stale_body : 1;       // Body filter: serve cached content
  unsigned mask_valid : 1;             // Whether mask has been classified
  unsigned force_refresh : 1;          // Client sent force-refresh signal
  unsigned agent_wants_markdown : 1;   // Accept: text/markdown
  unsigned served_agent_markdown : 1;  // served the 0x7C variant
  // Issue #652: this request re-fetched expired origin content (stale
  // verdict full re-fetch, or conditional revalidation answered 200) while
  // stale optimized variants may still exist in cache.  record_response
  // sends the worker a kOriginRefreshedSentinel notification instead of a
  // normal one so the stale variant set is purged and rebuilt.
  unsigned origin_refreshed : 1;
  // Issue #652 review (stale-if-error): a full re-fetch of an age-expired
  // entry is in flight and the stale entry is stashed in stale_read_result.
  // If the upstream answers >= 500, the header filter serves the stashed
  // stale body (RFC 9111 §4.2.4) instead of propagating the error.
  unsigned stale_full_refetch : 1;
  // Set when the header filter rewrote an upstream error into a stale
  // cached response: the body filter must swallow the upstream (error
  // page) body chunks instead of appending them after the cached body.
  unsigned discard_upstream_body : 1;
  // Observe-only: the Web Bot Auth verdict computed at classify
  // time ("human" / "signed-agent" / "verified-bot" / "unknown", or
  // "<bot-name>, ed25519-verified"), pool-allocated.  len == 0 when the
  // feature is off or the request was never classified.  Surfaced as the
  // x-verified-bot response header by the header filter; NEVER consulted by
  // any caching / serving / notification decision.
  ngx_str_t webbotauth_verdict;
  // Response-header sidecar (issue #208).  The ORIGIN's header lines,
  // snapshotted at the top of the header filter before the module adds any
  // of its own (X-PageSpeed, Accept-CH, x-verified-bot, its own Vary): an
  // ngx_array_t of ngx_http_pagespeed_header_pair_t pointing at the
  // pool-owned list entries.  nullptr when the response was served from
  // cache or carried no lines.  The classification in the MISS block and
  // the sidecar write in record_response both read this, never the live
  // list.
  ngx_array_t* origin_headers;
  // Set by the MISS block when the snapshot is swap-eligible: record_response
  // then records through the sidecar path (block + flag bits).
  unsigned sidecar_classified : 1;
  // Response-header sidecar, serve side (issue #208).  The URL's stored
  // header lines, copied into the request pool by the handler when the
  // served entry requires them (headers_sidecar_replay.h): an ngx_array_t of
  // ngx_http_pagespeed_header_pair_t, in stored order.  nullptr when the
  // entry requires no block.  Read once, before the freshness decision, and
  // replayed by whichever serving path the request ends up on (fresh hit,
  // no-cache hit, coalesced stale serve, stale-if-error serve).
  ngx_array_t* sidecar_fields;
  // The stored lines include a Vary / an X-Content-Type-Options line.
  unsigned sidecar_carried_vary : 1;
  unsigned sidecar_carried_nosniff : 1;
} ngx_http_pagespeed_ctx_t;

// One snapshotted origin header line (see ctx->origin_headers).
typedef struct {
  ngx_str_t key;
  ngx_str_t value;
} ngx_http_pagespeed_header_pair_t;

// Forward declarations - these are nginx callbacks that need C linkage
extern "C" {
static ngx_int_t ngx_http_pagespeed_init(ngx_conf_t* cf);
static void* ngx_http_pagespeed_create_srv_conf(ngx_conf_t* cf);
static char* ngx_http_pagespeed_merge_srv_conf(ngx_conf_t* cf, void* parent,
                                               void* child);
static void* ngx_http_pagespeed_create_loc_conf(ngx_conf_t* cf);
static char* ngx_http_pagespeed_merge_loc_conf(ngx_conf_t* cf, void* parent,
                                               void* child);
static ngx_int_t ngx_http_pagespeed_handler(ngx_http_request_t* r);
static ngx_int_t ngx_http_pagespeed_body_filter(ngx_http_request_t* r,
                                                ngx_chain_t* in);
static ngx_int_t ngx_http_pagespeed_header_filter(ngx_http_request_t* r);
static ngx_int_t ngx_http_pagespeed_init_process(ngx_cycle_t* cycle);
static void ngx_http_pagespeed_exit_process(ngx_cycle_t* cycle);
static ngx_int_t ngx_http_pagespeed_init_module(ngx_cycle_t* cycle);
static ngx_int_t ngx_http_pagespeed_add_variables(ngx_conf_t* cf);
}

// Handler for pagespeed_disallow directive
static char* ngx_http_pagespeed_disallow(ngx_conf_t* cf, ngx_command_t* cmd,
                                         void* conf_ptr) {
  auto* conf = static_cast<ngx_http_pagespeed_loc_conf_t*>(conf_ptr);
  auto* value = static_cast<ngx_str_t*>(cf->args->elts);

  if (conf->disallow_patterns == nullptr) {
    conf->disallow_patterns = ngx_array_create(cf->pool, 4, sizeof(ngx_str_t));
    if (conf->disallow_patterns == nullptr) {
      return const_cast<char*>("failed to allocate disallow array");
    }
  }

  auto* pattern =
      static_cast<ngx_str_t*>(ngx_array_push(conf->disallow_patterns));
  if (pattern == nullptr) {
    return const_cast<char*>("failed to push disallow pattern");
  }
  *pattern = value[1];  // value[0] is the directive name

  return NGX_CONF_OK;
}

// Check if a URL matches any disallow pattern.
// Pattern starting with '/' is a prefix match.
// Pattern starting with '*' is a suffix match.
// Otherwise, substring match.
static bool ngx_http_pagespeed_url_disallowed(
    ngx_http_pagespeed_loc_conf_t* conf, ngx_str_t* uri) {
  if (conf->disallow_patterns == nullptr) {
    return false;
  }

  auto* patterns = static_cast<ngx_str_t*>(conf->disallow_patterns->elts);
  for (ngx_uint_t i = 0; i < conf->disallow_patterns->nelts; ++i) {
    std::string_view pat(reinterpret_cast<const char*>(patterns[i].data),
                         patterns[i].len);
    std::string_view path(reinterpret_cast<const char*>(uri->data), uri->len);

    if (pat.starts_with('/')) {
      if (path.starts_with(pat)) return true;
    } else if (pat.starts_with('*')) {
      if (path.ends_with(pat.substr(1))) return true;
    } else {
      if (path.find(pat) != std::string_view::npos) return true;
    }
  }
  return false;
}

// Cache mode enum values for pagespeed_cache_mode directive.
static ngx_conf_enum_t ngx_http_pagespeed_cache_mode_enum[] = {
    {ngx_string("safe"), 0},
    {ngx_string("aggressive"), 1},
    {ngx_null_string, 0}};

// Module directives
static ngx_command_t ngx_http_pagespeed_commands[] = {
    {ngx_string("pagespeed"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_pagespeed_loc_conf_t, enable), nullptr},

    {ngx_string("pagespeed_cache_path"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_str_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_pagespeed_loc_conf_t, cache_path), nullptr},

    {ngx_string("pagespeed_disallow"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_TAKE1,
     ngx_http_pagespeed_disallow, NGX_HTTP_LOC_CONF_OFFSET, 0, nullptr},

    {ngx_string("pagespeed_hot_threshold"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_num_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_pagespeed_loc_conf_t, hot_threshold), nullptr},

    {ngx_string("pagespeed_max_age"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_num_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_pagespeed_loc_conf_t, max_age_cap), nullptr},

    {ngx_string("pagespeed_immutable_max_age"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_num_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_pagespeed_loc_conf_t, immutable_max_age_cap), nullptr},

    {ngx_string("pagespeed_synthesize_swr"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_pagespeed_loc_conf_t, synthesize_swr), nullptr},

    {ngx_string("pagespeed_conditional_revalidation"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_pagespeed_loc_conf_t, conditional_revalidation),
     nullptr},

    {ngx_string("pagespeed_stale_if_error_max_age"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_num_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_pagespeed_loc_conf_t, stale_if_error_max_age), nullptr},

    {ngx_string("pagespeed_html_max_age"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_num_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_pagespeed_loc_conf_t, html_max_age), nullptr},

    {ngx_string("pagespeed_css_max_age"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_num_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_pagespeed_loc_conf_t, css_max_age), nullptr},

    {ngx_string("pagespeed_image_max_age"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_num_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_pagespeed_loc_conf_t, image_max_age), nullptr},

    {ngx_string("pagespeed_force_refresh_html"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_pagespeed_loc_conf_t, force_refresh_html), nullptr},

    {ngx_string("pagespeed_force_refresh"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_pagespeed_loc_conf_t, force_refresh), nullptr},

    {ngx_string("pagespeed_trust_x_forwarded_proto"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_pagespeed_loc_conf_t, trust_x_forwarded_proto), nullptr},

    {ngx_string("pagespeed_cache_mode"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_enum_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_pagespeed_loc_conf_t, cache_mode),
     ngx_http_pagespeed_cache_mode_enum},

    ngx_null_command};

// Module context
static ngx_http_module_t ngx_http_pagespeed_module_ctx = {
    ngx_http_pagespeed_add_variables,    // preconfiguration
    ngx_http_pagespeed_init,             // postconfiguration
    nullptr,                             // create main configuration
    nullptr,                             // init main configuration
    ngx_http_pagespeed_create_srv_conf,  // create server configuration
    ngx_http_pagespeed_merge_srv_conf,   // merge server configuration
    ngx_http_pagespeed_create_loc_conf,  // create location configuration
    ngx_http_pagespeed_merge_loc_conf    // merge location configuration
};

// Module definition — extern "C" is required so that nginx can find the
// symbol via dlsym() when loading the module dynamically.
extern "C" {
ngx_module_t ngx_http_pagespeed_module = {
    NGX_MODULE_V1,
    &ngx_http_pagespeed_module_ctx,   // module context
    ngx_http_pagespeed_commands,      // module directives
    NGX_HTTP_MODULE,                  // module type
    nullptr,                          // init master
    ngx_http_pagespeed_init_module,   // init module
    ngx_http_pagespeed_init_process,  // init process
    nullptr,                          // init thread
    nullptr,                          // exit thread
    ngx_http_pagespeed_exit_process,  // exit process
    nullptr,                          // exit master
    NGX_MODULE_V1_PADDING};

// Dynamic module metadata — nginx's load_module uses dlsym() to find these.
ngx_module_t* ngx_modules[] = {&ngx_http_pagespeed_module, nullptr};

char* ngx_module_names[] = {const_cast<char*>("ngx_http_pagespeed_module"),
                            nullptr};

char* ngx_module_order[] = {nullptr};
}

// Create location configuration
static void* ngx_http_pagespeed_create_srv_conf(ngx_conf_t* cf) {
  return ngx_pcalloc(cf->pool, sizeof(ngx_http_pagespeed_srv_conf_t));
}

// Runs after the core module's own merge, so the block's name list is
// complete here (including the empty name nginx adds to a block without one).
static char* ngx_http_pagespeed_merge_srv_conf(ngx_conf_t* cf, void* parent,
                                               void* child) {
  auto* conf = static_cast<ngx_http_pagespeed_srv_conf_t*>(child);
  const auto* cscf = static_cast<const ngx_http_core_srv_conf_t*>(
      ngx_http_conf_get_module_srv_conf(cf, ngx_http_core_module));
  conf->server_names = nullptr;
  conf->server_names_count = 0;
  if (cscf == nullptr || cscf->server_names.nelts == 0) {
    return NGX_CONF_OK;
  }
  const size_t bytes =
      cscf->server_names.nelts * sizeof(ngx_http_server_name_t);
  conf->server_names =
      static_cast<ngx_http_server_name_t*>(ngx_palloc(cf->pool, bytes));
  if (conf->server_names == nullptr) {
    return static_cast<char*>(NGX_CONF_ERROR);
  }
  ngx_memcpy(conf->server_names, cscf->server_names.elts, bytes);
  conf->server_names_count = cscf->server_names.nelts;
  return NGX_CONF_OK;
}

static void* ngx_http_pagespeed_create_loc_conf(ngx_conf_t* cf) {
  ngx_http_pagespeed_loc_conf_t* conf;

  conf = static_cast<ngx_http_pagespeed_loc_conf_t*>(
      ngx_pcalloc(cf->pool, sizeof(ngx_http_pagespeed_loc_conf_t)));
  if (conf == nullptr) {
    return nullptr;
  }

  conf->enable = NGX_CONF_UNSET;
  conf->hot_threshold = NGX_CONF_UNSET;
  conf->max_age_cap = NGX_CONF_UNSET;
  conf->immutable_max_age_cap = NGX_CONF_UNSET;
  conf->synthesize_swr = NGX_CONF_UNSET;
  conf->conditional_revalidation = NGX_CONF_UNSET;
  conf->html_max_age = NGX_CONF_UNSET;
  conf->css_max_age = NGX_CONF_UNSET;
  conf->image_max_age = NGX_CONF_UNSET;
  conf->force_refresh_html = NGX_CONF_UNSET;
  conf->force_refresh = NGX_CONF_UNSET;
  conf->trust_x_forwarded_proto = NGX_CONF_UNSET;
  conf->cache_mode = NGX_CONF_UNSET_UINT;
  conf->stale_if_error_max_age = NGX_CONF_UNSET;

  return conf;
}

// Merge location configuration
static char* ngx_http_pagespeed_merge_loc_conf(ngx_conf_t* cf, void* parent,
                                               void* child) {
  ngx_http_pagespeed_loc_conf_t* prev =
      static_cast<ngx_http_pagespeed_loc_conf_t*>(parent);
  ngx_http_pagespeed_loc_conf_t* conf =
      static_cast<ngx_http_pagespeed_loc_conf_t*>(child);

  ngx_conf_merge_value(conf->enable, prev->enable, 0);
  ngx_conf_merge_str_value(conf->cache_path, prev->cache_path, "");
  ngx_conf_merge_value(conf->hot_threshold, prev->hot_threshold,
                       kDefaultHotThreshold);
  ngx_conf_merge_value(conf->max_age_cap, prev->max_age_cap, 86400);
  ngx_conf_merge_value(conf->immutable_max_age_cap, prev->immutable_max_age_cap,
                       604800);
  ngx_conf_merge_value(conf->synthesize_swr, prev->synthesize_swr, 1);
  ngx_conf_merge_value(conf->conditional_revalidation,
                       prev->conditional_revalidation, 1);
  // Stale-if-error window (issue #652 review, RFC 9111 §4.2.4).  Default
  // matches the stale-if-error=86400 synthesized for downstream caches in
  // aggressive mode — the module now implements the semantics it
  // advertises.  0 disables.
  ngx_conf_merge_value(conf->stale_if_error_max_age,
                       prev->stale_if_error_max_age, 86400);
  ngx_conf_merge_value(conf->html_max_age, prev->html_max_age, 0);
  // Merge cache_mode before css/image max-age so we can select defaults
  // based on the resolved mode.  Default is NGX_CONF_UNSET_UINT so that
  // the shared config (workbench) can provide the value at runtime when
  // no directive is set.
  ngx_conf_merge_uint_value(conf->cache_mode, prev->cache_mode,
                            NGX_CONF_UNSET_UINT);
  // Read shared config from disk before resolving cache_mode defaults.
  // merge_loc_conf runs before postconfiguration, so g_shared_config may
  // be default-constructed or stale from the previous config cycle.
  // Read once per cycle using cf->cycle as the cycle identifier.
  {
    static ngx_cycle_t* s_merge_cycle = nullptr;
    if (cf->cycle != s_merge_cycle && conf->cache_path.len > 0) {
      s_merge_cycle = cf->cycle;
      std::string_view cp(reinterpret_cast<const char*>(conf->cache_path.data),
                          conf->cache_path.len);
      std::lock_guard<std::mutex> lock(g_cache_mutex);
      g_shared_config = ReadSharedConfig(cp, cf->log);
      RebuildUrlNormConfig(cp, g_shared_config);
      RebuildWebBotAuthConfig(g_shared_config, cf->log);
      RebuildRslCapConfig(g_shared_config, cf->log);
      // A new configuration cycle (start or reload) reports the
      // cache-directory generation verdict again, even if unchanged.  The
      // line itself is written by init_module, once the error log is open.
      g_cache_dir_gen.Rearm();
      ApplyCacheDirGeneration(cp, nullptr);
      // Warm the web-bot-auth key store at config time too.  The runtime
      // poll (RefreshSharedConfigIfChanged) fires on the request path AFTER
      // classify has already run, so without this the very first signed
      // request after startup would classify against an empty store
      // (kUnknown) even though the worker had published keys.
      RefreshWebBotAuthKeysIfChanged(cp, cf->log);
      // Same config-time warm for the RSL-CAP enforcement store.
      RefreshRslCapKeysIfChanged(cp, cf->log);
    }
  }
  // Resolve cache_mode: explicit directive > shared config > safe (0).
  {
    ngx_uint_t resolved_mode = conf->cache_mode;
    if (resolved_mode == NGX_CONF_UNSET_UINT) {
      // No explicit directive — check shared config.
      if (g_shared_config.cache_mode == "aggressive") {
        resolved_mode = 1;
      } else {
        resolved_mode = 0;  // safe
      }
    }
    // Store resolved mode for css/image default selection.
    // Note: conf->cache_mode stays NGX_CONF_UNSET_UINT when no directive
    // was set, so at runtime we can distinguish "directive" vs "default".
    ngx_int_t css_default = (resolved_mode == 1) ? 86400 : 300;
    ngx_int_t img_default = (resolved_mode == 1) ? 86400 : 1800;
    ngx_conf_merge_value(conf->css_max_age, prev->css_max_age, css_default);
    ngx_conf_merge_value(conf->image_max_age, prev->image_max_age, img_default);
  }
  ngx_conf_merge_value(conf->force_refresh_html, prev->force_refresh_html, 1);
  ngx_conf_merge_value(conf->force_refresh, prev->force_refresh, 0);
  ngx_conf_merge_value(conf->trust_x_forwarded_proto,
                       prev->trust_x_forwarded_proto, 0);

  if (conf->disallow_patterns == nullptr) {
    conf->disallow_patterns = prev->disallow_patterns;
  }

  return NGX_CONF_OK;
}

// Extract header value from request
static ngx_str_t ngx_http_pagespeed_get_header(ngx_http_request_t* r,
                                               ngx_str_t name) {
  ngx_str_t value = ngx_null_string;
  ngx_list_part_t* part;
  ngx_table_elt_t* h;
  ngx_uint_t i;

  part = &r->headers_in.headers.part;
  h = static_cast<ngx_table_elt_t*>(part->elts);

  for (i = 0;; i++) {
    if (i >= part->nelts) {
      if (part->next == nullptr) {
        break;
      }
      part = part->next;
      h = static_cast<ngx_table_elt_t*>(part->elts);
      i = 0;
    }

    if (h[i].key.len == name.len &&
        ngx_strncasecmp(h[i].key.data, name.data, name.len) == 0) {
      value = h[i].value;
      break;
    }
  }

  return value;
}

// Determine ContentType from Content-Type response header
static ContentType ngx_http_pagespeed_detect_content_type(
    ngx_http_request_t* r) {
  if (r->headers_out.content_type.len == 0) {
    return ContentType::kOther;
  }

  std::string_view ct(
      reinterpret_cast<const char*>(r->headers_out.content_type.data),
      r->headers_out.content_type.len);

  if (ct.starts_with("text/html")) return ContentType::kHtml;
  if (ct.starts_with("text/css")) return ContentType::kCss;
  if (ct.starts_with("application/javascript") ||
      ct.starts_with("text/javascript")) {
    return ContentType::kJs;
  }
  if (ct.starts_with("image/")) return ContentType::kImage;

  return ContentType::kOther;
}

// MimeFromUrl is in src/nginx/mime_util.h (shared with tests).

// Defined after the hostname helper below; used by classify.
static void ngx_http_pagespeed_webbotauth_classify(
    ngx_http_request_t* r, ngx_http_pagespeed_ctx_t* ctx);

// Classify request: derive capability mask from request headers.
static ngx_int_t ngx_http_pagespeed_classify(ngx_http_request_t* r,
                                             ngx_http_pagespeed_ctx_t* ctx) {
  // Header names
  static ngx_str_t accept_name = ngx_string("Accept");
  static ngx_str_t save_data_name = ngx_string("Save-Data");
  static ngx_str_t accept_encoding_name = ngx_string("Accept-Encoding");
  static ngx_str_t sec_ch_dpr_name = ngx_string("Sec-CH-DPR");

  // Get headers
  ngx_str_t accept = ngx_http_pagespeed_get_header(r, accept_name);
  ngx_str_t empty_str = ngx_null_string;
  ngx_str_t user_agent =
      r->headers_in.user_agent ? r->headers_in.user_agent->value : empty_str;
  ngx_str_t save_data = ngx_http_pagespeed_get_header(r, save_data_name);
  ngx_str_t accept_enc = ngx_http_pagespeed_get_header(r, accept_encoding_name);
  ngx_str_t sec_ch_dpr = ngx_http_pagespeed_get_header(r, sec_ch_dpr_name);

  // Create capability mask from headers
  std::string_view accept_sv(
      accept.data ? reinterpret_cast<const char*>(accept.data) : "",
      accept.data ? accept.len : 0);
  std::string_view ua_sv(
      user_agent.data ? reinterpret_cast<const char*>(user_agent.data) : "",
      user_agent.data ? user_agent.len : 0);
  std::string_view sd_sv(
      save_data.data ? reinterpret_cast<const char*>(save_data.data) : "",
      save_data.data ? save_data.len : 0);
  std::string_view ae_sv(
      accept_enc.data ? reinterpret_cast<const char*>(accept_enc.data) : "",
      accept_enc.data ? accept_enc.len : 0);
  std::string_view dpr_sv(
      sec_ch_dpr.data ? reinterpret_cast<const char*>(sec_ch_dpr.data) : "",
      sec_ch_dpr.data ? sec_ch_dpr.len : 0);

  ctx->mask =
      CapabilityMask::FromHeaders(accept_sv, ua_sv, sd_sv, ae_sv, dpr_sv);
  ctx->mask_valid = 1;
  // Does this request ask for the agent_optimize markdown variant?
  // ANDed with the serve-side entitlement (g_shared_config) at the cache read.
  ctx->agent_wants_markdown = WantsAgentMarkdown(accept_sv) ? 1 : 0;

  // Observe-only Web Bot Auth classification.  Default off —
  // one bool check when the operator has not enabled it.  The verdict is a
  // label (response header) + counter ONLY; it never affects any decision
  // this module makes about the request.
  if (g_shared_config.web_bot_auth) {
    ngx_http_pagespeed_webbotauth_classify(r, ctx);
  }

  return NGX_OK;
}

// Check if character is a Cache-Control token delimiter (comma, space,
// tab, or end-of-value).  Used to verify that a substring match is a
// complete token rather than a prefix of a longer directive.
static bool is_cc_delimiter(char c) {
  return c == ',' || c == ' ' || c == '\t' || c == ';';
}

// Case-insensitive search for a Cache-Control directive token in the
// header value.  Verifies the match is preceded by a delimiter (or is
// at the start) and followed by a delimiter or end-of-string.
static bool cc_has_directive(std::string_view cc_sv, std::string_view dir) {
  for (size_t pos = 0;;) {
    // Case-insensitive find: scan manually.
    size_t found = std::string_view::npos;
    for (size_t i = pos; i + dir.size() <= cc_sv.size(); ++i) {
      if (ngx_strncasecmp(
              const_cast<u_char*>(
                  reinterpret_cast<const u_char*>(cc_sv.data() + i)),
              const_cast<u_char*>(reinterpret_cast<const u_char*>(dir.data())),
              dir.size()) == 0) {
        found = i;
        break;
      }
    }
    if (found == std::string_view::npos) return false;

    // Check token boundaries.
    bool start_ok = (found == 0) || is_cc_delimiter(cc_sv[found - 1]);
    size_t end_pos = found + dir.size();
    bool end_ok = (end_pos >= cc_sv.size()) || is_cc_delimiter(cc_sv[end_pos]);
    if (start_ok && end_ok) return true;

    pos = found + 1;
  }
}

// Detect browser force-refresh: Cache-Control: no-cache, max-age=0,
// or Pragma: no-cache.  Returns true if the client is requesting a
// forced revalidation (e.g., Ctrl+F5).
static bool ngx_http_pagespeed_is_force_refresh(ngx_http_request_t* r) {
  // Check Cache-Control header for no-cache or max-age=0.
  static ngx_str_t cc_name = ngx_string("Cache-Control");
  ngx_str_t cc = ngx_http_pagespeed_get_header(r, cc_name);
  if (cc.data != nullptr && cc.len > 0) {
    std::string_view cc_sv(reinterpret_cast<const char*>(cc.data), cc.len);

    if (cc_has_directive(cc_sv, "no-cache") ||
        cc_has_directive(cc_sv, "max-age=0")) {
      return true;
    }
  }

  // Fallback: Pragma: no-cache (HTTP/1.0 force-refresh).
  // Per RFC 9111 S5.4, only honored when no Cache-Control is present.
  if (cc.data == nullptr || cc.len == 0) {
    static ngx_str_t pragma_name = ngx_string("Pragma");
    ngx_str_t pragma = ngx_http_pagespeed_get_header(r, pragma_name);
    if (pragma.data != nullptr && pragma.len == 8 &&
        ngx_strncasecmp(
            pragma.data,
            reinterpret_cast<u_char*>(const_cast<char*>("no-cache")), 8) == 0) {
      return true;
    }
  }

  return false;
}

// Lease renewal for parked mmap borrows.
//
// Cyclone stamps a per-stripe read lease (default T=5s) on every disk-borrow
// read; writers defer wraps while the lease is live.  The re-stamp avoidance
// guard makes 3T/4 (3.75s) the guaranteed floor, so any holder keeping a
// borrow longer must renew at a cadence <= 3T/4.  3s stays under that floor
// with margin.  NOTE: renewal is bounded by Cyclone's anti-starvation
// ceiling (lease_wrap_ceiling, default 60s) — a transfer that keeps a borrow
// past the ceiling loses protection (the wrap is forced, counted in
// wraps_forced_past_lease).  That residual is the same exposure as
// pre-lease behavior, now bounded and observable.
static constexpr ngx_msec_t kLeaseRenewIntervalMs = 3000;

// Zero-copy barrier — nginx buffer surgery.  The pure keep/copy/abort
// decision lives in src/nginx/zerocopy_barrier.h (unit-tested); these helpers
// apply it to nginx buffers.  Single-threaded worker: nginx never runs the
// output writer and a timer/handler on the same request concurrently, so
// mutating an in-flight buffer's unsent span here is race-free.

// Copy a borrowed result's whole content into request-pool memory and point
// `b` at it, dropping the mmap borrow.  copy-then-verify: re-check the lease
// AFTER the memcpy — a ceiling-forced wrap ignores the fresh lease and can
// race a large copy, so a kTorn verdict here means the copy may be torn.
// Returns true on a clean owned copy (b now serves owned memory, borrow
// released); false if torn (caller must fail the serve closed).
static bool ps_zerocopy_copy_out(ngx_http_request_t* r, ReadResult* rr,
                                 ngx_buf_t* b) {
  auto content = rr->content();
  const size_t size = content.size();
  u_char* owned =
      static_cast<u_char*>(ngx_pnalloc(r->pool, size == 0 ? 1 : size));
  if (owned == nullptr) {
    return false;  // cannot de-alias safely: fail closed (rare).
  }
  const bool ok = pagespeed::ps_barrier::CopyThenVerify(
      content.data(), size, owned, [rr] { return rr->renew_lease_strict(); });
  rr->release();  // copied out: drop the pin regardless of the verdict.
  if (!ok) {
    pagespeed::RecordZerocopyCopyThenVerifyDiscard(g_serve_stats);
    return false;  // region overwritten mid/pre-copy: torn body.
  }
  pagespeed::ps_barrier::RepointToOwned(b, owned, size);
  return true;
}

// De-alias the UNSENT tail of an in-flight aliased buffer into request-pool
// memory (timer-driven, for a slow drain approaching a forced wrap).  Handles
// both the mmap-alias form (b->memory: unsent span is [b->pos, b->last)) and
// the sendfile form (b->in_file: unsent span [b->file_pos, b->file_last) maps
// back into the content() mmap at file_pos - content_file_offset()).  Same
// copy-then-verify protocol as ps_zerocopy_copy_out.  Returns true if the tail
// is now owned (borrow released); false if torn (caller fails closed).
static bool ps_zerocopy_dealias_tail(ngx_http_request_t* r, ReadResult* rr,
                                     ngx_buf_t* b) {
  size_t rem = 0;
  const u_char* src = nullptr;
  if (b->in_file) {
    auto content = rr->content();
    const auto span = pagespeed::ps_barrier::MapSendfileTail(
        static_cast<uint64_t>(b->file_pos), static_cast<uint64_t>(b->file_last),
        rr->content_file_offset(), content.size());
    if (!span.valid) {
      return false;  // offsets do not map into the borrow: fail closed.
    }
    rem = span.length;
    src = reinterpret_cast<const u_char*>(content.data()) + span.offset;
  } else {
    rem = static_cast<size_t>(b->last - b->pos);
    src = b->pos;
  }
  u_char* owned =
      static_cast<u_char*>(ngx_pnalloc(r->pool, rem == 0 ? 1 : rem));
  if (owned == nullptr) {
    return false;
  }
  const bool ok = pagespeed::ps_barrier::CopyThenVerify(
      src, rem, owned, [rr] { return rr->renew_lease_strict(); });
  if (!ok) {
    pagespeed::RecordZerocopyCopyThenVerifyDiscard(g_serve_stats);
    return false;
  }
  pagespeed::ps_barrier::RepointToOwned(b, owned, rem);
  rr->release();  // tail owned: drop the pin so no further renewal is needed.
  return true;
}

// Emit-time barrier decision for an aliased serve site.  Stamps a fresh lease
// (the intent-checked strict renew) and classifies the borrow.  Called in the
// same call stack as the ngx_http_output_filter / next_body_filter that first
// exposes the aliased bytes — the port of mod_pagespeed 1.1's emit-time
// intent-checked decision to 2.0's serve-once architecture.
static pagespeed::ps_barrier::BarrierAction ps_zerocopy_emit_barrier(
    ReadResult* rr) {
  const pagespeed::LeaseRenewal lr = rr->renew_lease_strict();
  const uint64_t ns = rr->ns_until_forced_wrap();
  return pagespeed::ps_barrier::ClassifyRenewal(
      lr, ns, pagespeed::ps_barrier::kBarrierMarginNs);
}

// Emit-time copy gate (the port of 1.1's must_copy eligibility walk).
// Aliasing is safe ONLY when the buf we hand to the output filter is provably
// the SOLE reference into the borrow — the barrier's drained check and tail
// de-alias operate on that buf's cursor, and a copy-out releases the pin, so
// any downstream consumer holding its OWN reference into the mmap breaks both.
// Force the whole-body copy path (never alias, never track) when:
//   * filter_need_in_memory / filter_need_temporary (sub_filter, ssi,
//     addition, charset) or main_filter_need_in_memory (gzip/gunzip,
//     image_filter, xslt): these read, re-slice, or RETAIN raw pointers into
//     the body across drains (gzip drives zlib from a raw next_in a slow
//     client suspends);
//   * a Range request: the range filter emits multipart sub-range bufs
//     aliasing the mmap;
//   * any non-HTTP/1.x version: ngx_http_v2_send_chain splits our buf into
//     per-frame SHADOW bufs aliasing the mmap, drained asynchronously as the
//     flow-control window opens WITHOUT advancing our buf's cursor; HTTP/3
//     QUIC framing has the same shape, so gate on > NGX_HTTP_VERSION_11
//     rather than depend on build-specific copies-synchronously invariants;
//   * a subrequest: it does not own the connection's body-send loop.
static bool ps_zerocopy_must_copy(const ngx_http_request_t* r) {
  return pagespeed::ps_barrier::MustCopyForEmit(
      r->http_version > NGX_HTTP_VERSION_11, r->headers_in.range != nullptr,
      r != r->main, r->filter_need_in_memory, r->filter_need_temporary,
      r->main_filter_need_in_memory);
}

// Timer handler: re-stamp + intent-check the lease(s) for the borrows parked
// on the ctx.  ev->data is the request (so the barrier can de-alias
// its buffer / finalize the request); the ctx is fetched from it.  Re-arms
// only while at least one lease was actually renewed — RAM-cache hits and
// lease-disabled reads have nothing to protect, so the timer stops after one
// no-op.  For the aliased, still-draining borrow (aliased_buf set) this is the
// ONLY strict re-check between the emit-time barrier and the final drain, so
// it enforces the same keep/copy/abort verdict in this stack: kTorn while
// mid-send finalizes the request (the in-flight bytes may be foreign — never
// let nginx drain them); a forced-wrap deadline within the margin or a
// kCopyNow de-aliases the unsent tail in place; a clean kOk keeps aliasing.
static void ngx_http_pagespeed_lease_renew_handler(ngx_event_t* ev) {
  auto* r = static_cast<ngx_http_request_t*>(ev->data);
  auto* ctx = static_cast<ngx_http_pagespeed_ctx_t*>(
      ngx_http_get_module_ctx(r, ngx_http_pagespeed_module));
  if (ctx == nullptr) {
    return;
  }
  bool renewed = false;

  if (ctx->read_result != nullptr && ctx->aliased_buf != nullptr) {
    // The emitted borrow is still aliased and draining: enforce the strict
    // barrier in this stack (the send loop re-reads the same buffer).
    ngx_buf_t* b = ctx->aliased_buf;
    const bool drained =
        b->in_file ? (b->file_pos >= b->file_last) : (b->pos >= b->last);
    if (drained) {
      ctx->aliased_buf =
          nullptr;  // fully sent while aliased: nothing to protect.
    } else if (r->connection == nullptr || r->connection->error) {
      ctx->aliased_buf =
          nullptr;  // client gone: cleanup will release the borrow.
    } else {
      const pagespeed::LeaseRenewal lr = ctx->read_result->renew_lease_strict();
      const uint64_t ns = ctx->read_result->ns_until_forced_wrap();
      switch (pagespeed::ps_barrier::ClassifyRenewal(
          lr, ns, pagespeed::ps_barrier::kBarrierMarginNs)) {
        case pagespeed::ps_barrier::BarrierAction::kAbort: {
          // A wrap committed: the bytes still on the wire may be foreign.
          ctx->aliased_buf = nullptr;
          pagespeed::RecordZerocopyTornAbort(g_serve_stats);
          ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                        "pagespeed: zero-copy serve torn mid-send (cache "
                        "entry recycled under the response), finalizing %V",
                        &r->uri);
          ngx_http_finalize_request(r, NGX_ERROR);
          return;  // do not re-arm; cleanup disarms the timer.
        }
        case pagespeed::ps_barrier::BarrierAction::kCopyOut: {
          if (ps_zerocopy_dealias_tail(r, ctx->read_result, b)) {
            ctx->aliased_buf = nullptr;  // now owned; borrow released.
            pagespeed::RecordZerocopyProactiveCopyout(g_serve_stats);
            // read_result released: fall through, no re-arm needed for it.
          } else {
            ctx->aliased_buf = nullptr;
            pagespeed::RecordZerocopyTornAbort(g_serve_stats);
            ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                          "pagespeed: zero-copy serve torn while detaching "
                          "from the cache entry, finalizing %V",
                          &r->uri);
            ngx_http_finalize_request(r, NGX_ERROR);
            return;
          }
          break;
        }
        case pagespeed::ps_barrier::BarrierAction::kAlias:
          renewed = true;  // provably safe to keep aliasing.
          break;
      }
    }
  } else if (ctx->read_result != nullptr) {
    // Parked but not aliased (copied-out serve already released it, or a
    // borrow held without a live send): epoch renew keeps the lease alive.
    renewed = ctx->read_result->renew_lease() || renewed;
  }

  // Stale stash (issue #652): a stale entry held across an upstream fetch but
  // NOT yet on the wire.  Keep its lease alive; when it is actually served it
  // moves to read_result and comes under the aliased-buf barrier above.
  if (ctx->stale_read_result != nullptr) {
    renewed = ctx->stale_read_result->renew_lease() || renewed;
  }

  if (renewed) {
    ngx_add_timer(ev, kLeaseRenewIntervalMs);
  }
}

// Arm the per-request lease renewal timer (idempotent).  Called wherever a
// ReadResult is parked on the ctx (HIT serve, llms.txt serve).  Best-effort:
// on pool-alloc failure the borrow still has the initial lease's >= 3T/4
// floor — the same exposure as pre-lease behavior past that point.
static void ngx_http_pagespeed_ensure_lease_renew_timer(
    ngx_http_request_t* r, ngx_http_pagespeed_ctx_t* ctx) {
  if (ctx->lease_renew_event == nullptr) {
    auto* ev =
        static_cast<ngx_event_t*>(ngx_pcalloc(r->pool, sizeof(ngx_event_t)));
    if (ev == nullptr) {
      return;
    }
    ev->handler = ngx_http_pagespeed_lease_renew_handler;
    ev->data = r;  // the barrier needs the request to de-alias / finalize.
    ev->log = r->connection->log;
    ctx->lease_renew_event = ev;
  }
  if (!ctx->lease_renew_event->timer_set) {
    ngx_add_timer(ctx->lease_renew_event, kLeaseRenewIntervalMs);
  }
}

// Request cleanup handler
static void ngx_http_pagespeed_cleanup(void* data) {
  ngx_http_pagespeed_ctx_t* ctx = static_cast<ngx_http_pagespeed_ctx_t*>(data);
  // Disarm the lease renewal timer BEFORE the handles it renews
  // are destroyed (the event lives in the request pool, so it must leave
  // nginx's timer tree before the pool is freed).
  if (ctx->lease_renew_event != nullptr && ctx->lease_renew_event->timer_set) {
    ngx_del_timer(ctx->lease_renew_event);
  }
  // ORDER MATTERS: ReadHandles (read_result/stale_read_result) reference
  // mmap regions owned by the PageSpeedCache instance and MUST be destroyed
  // before that instance (see cache.h: "All ReadHandles must be destroyed
  // before the PageSpeedCache is destroyed").  cache_anchor may hold the
  // last shared_ptr to that instance, so it is released LAST.
  if (ctx->read_result) {
    delete ctx->read_result;
    ctx->read_result = nullptr;
  }
  // Clean up stale entry on ALL exit paths — upstream timeout, 5xx,
  // client disconnect, nginx shutdown.
  if (ctx->stale_read_result) {
    delete ctx->stale_read_result;
    ctx->stale_read_result = nullptr;
  }
  // Close the request-owned sendfile FD dup (if any) before dropping the
  // cache anchor.  NGX_INVALID_FILE (-1) is the not-taken sentinel.
  if (ctx->sendfile_fd != NGX_INVALID_FILE) {
    ngx_close_file(ctx->sendfile_fd);
    ctx->sendfile_fd = NGX_INVALID_FILE;
  }
  // Release our owning reference to the cache generation last.  When this
  // drops the final reference, the retired PageSpeedCache instance (and its
  // mmap) is destroyed here — after the ReadHandles above are already gone.
  if (ctx->cache_anchor) {
    delete ctx->cache_anchor;
    ctx->cache_anchor = nullptr;
  }
}

// Extract hostname from the request's Host header.
static std::string_view ngx_http_pagespeed_hostname(ngx_http_request_t* r) {
  if (r->headers_in.host && r->headers_in.host->value.len > 0) {
    return std::string_view(
        reinterpret_cast<const char*>(r->headers_in.host->value.data),
        r->headers_in.host->value.len);
  }
  // HTTP/3 carries the host in the ":authority" pseudo-header and nginx does
  // not synthesize a Host header from it (HTTP/1.x and HTTP/2 have one), so
  // without this fallback every HTTP/3 request would look up the cache under
  // an empty hostname.
  if (r->host_end != nullptr && r->host_end > r->host_start) {
    return std::string_view(reinterpret_cast<const char*>(r->host_start),
                            static_cast<size_t>(r->host_end - r->host_start));
  }
  return {};
}

// Append one HeaderField (with the canonical lowercase `canonical_name`) for
// EVERY occurrence of request header `name` — RFC 9421 field canonicalization
// joins repeated field lines, so all instances must be wired, not just the
// first.  Values are the raw wire bytes (string_views into the request pool;
// valid for the duration of the classify call).
static void ngx_http_pagespeed_collect_header_fields(
    ngx_http_request_t* r, ngx_str_t name, std::string_view canonical_name,
    std::vector<pagespeed::webbotauth::HeaderField>* out) {
  ngx_list_part_t* part = &r->headers_in.headers.part;
  auto* h = static_cast<ngx_table_elt_t*>(part->elts);
  for (ngx_uint_t i = 0;; i++) {
    if (i >= part->nelts) {
      if (part->next == nullptr) {
        break;
      }
      part = part->next;
      h = static_cast<ngx_table_elt_t*>(part->elts);
      i = 0;
    }
    if (h[i].key.len == name.len &&
        ngx_strncasecmp(h[i].key.data, name.data, name.len) == 0) {
      out->push_back(
          {canonical_name,
           std::string_view(h[i].value.data
                                ? reinterpret_cast<const char*>(h[i].value.data)
                                : "",
                            h[i].value.data ? h[i].value.len : 0)});
    }
  }
}

// The comma-joined value of every occurrence of request header `name`
// (RFC 9110: repeated list-typed field lines are semantically the ", "-join).
// RFC 9421 section 4.1 explicitly contemplates an intermediary adding its
// signature as a SEPARATE Signature-Input/Signature field line, so parsing
// only the first line would either hide the bot's tagged signature behind a
// CDN's line or split a label from its signature bytes.  Allocates — call it
// only on the signed-request path.
static std::string ngx_http_pagespeed_join_header_values(ngx_http_request_t* r,
                                                         ngx_str_t name) {
  std::vector<pagespeed::webbotauth::HeaderField> lines;
  ngx_http_pagespeed_collect_header_fields(r, name, /*canonical_name=*/"",
                                           &lines);
  std::string joined;
  for (const pagespeed::webbotauth::HeaderField& line : lines) {
    if (!joined.empty()) joined += ", ";
    joined.append(line.value);
  }
  return joined;
}

// Observe-only Web Bot Auth classification (RFC 9421 HTTP
// Message Signatures).  Runs at classify time when web_bot_auth is enabled.
// The common path — a request with NEITHER Signature-Input NOR Signature —
// short-circuits BEFORE any allocation or crypto and gets no label at all
// (only requests carrying signature material earn an x-verified-bot header,
// so the label never inflates every human response).  Signature material
// with no web-bot-auth-tagged signature (e.g. a CDN signing scheme) is
// classified as if unsigned: no label, only the other-signature counter.
// Partial signature material (either header alone) goes through the verifier
// and fail-closes to "unknown", matching the core's posture.  Signed
// requests are verified against the worker-warmed key store (never a fetch —
// the store read is pure and synchronous) and counted in the shared
// serve-stats mmap.  OBSERVE-ONLY: the verdict is stored on the ctx for the
// header filter and NEVER changes how the request is handled.
static void ngx_http_pagespeed_webbotauth_classify(
    ngx_http_request_t* r, ngx_http_pagespeed_ctx_t* ctx) {
  namespace wba = pagespeed::webbotauth;

  static ngx_str_t sig_input_name = ngx_string("Signature-Input");
  static ngx_str_t sig_name = ngx_string("Signature");
  static ngx_str_t sig_agent_name = ngx_string("Signature-Agent");

  ngx_str_t sig_input = ngx_http_pagespeed_get_header(r, sig_input_name);
  ngx_str_t sig = ngx_http_pagespeed_get_header(r, sig_name);
  if (sig_input.len == 0 && sig.len == 0) {
    // No signature material at all: a plain (human / non-signing) client.
    // A spoofed bot User-Agent alone earns no trust — it never reaches the
    // verifier — and the response carries no verdict header.  This presence
    // probe reads only the FIRST matching line of each header — allocation-
    // free — the full ", "-join below runs only once material is present.
    return;
  }

  // RFC 9110/9421: repeated Signature-Input / Signature field lines are the
  // comma-joined value (an intermediary legitimately adds its signature as a
  // SEPARATE line, RFC 9421 section 4.1).  Join ALL lines before parsing so
  // a bot's tagged signature on a second line — or signature bytes split
  // from their label across lines — is still seen.  The parse bounds
  // (member/component caps) apply to the joined value.
  std::string sig_input_joined =
      ngx_http_pagespeed_join_header_values(r, sig_input_name);
  std::string sig_joined = ngx_http_pagespeed_join_header_values(r, sig_name);

  ngx_str_t empty_str = ngx_null_string;
  ngx_str_t user_agent =
      r->headers_in.user_agent ? r->headers_in.user_agent->value : empty_str;

  // RFC 9421 @authority is the normalized (lower-cased) authority.
  std::string authority(ngx_http_pagespeed_hostname(r));
  for (char& c : authority) {
    c = static_cast<char>(ngx_tolower(static_cast<u_char>(c)));
  }
  // RFC 9421 @path is the WIRE path: use the path portion of unparsed_uri
  // (r->uri is percent-decoded + slash-merged, so any %XX in the signed
  // path would break the signature base).
  std::string_view wire_path(
      r->unparsed_uri.data ? reinterpret_cast<const char*>(r->unparsed_uri.data)
                           : "",
      r->unparsed_uri.data ? r->unparsed_uri.len : 0);
  wire_path = wire_path.substr(0, wire_path.find('?'));

  wba::RequestView req;
  req.method = std::string_view(
      reinterpret_cast<const char*>(r->method_name.data), r->method_name.len);
  req.authority = authority;
  req.path = wire_path;
  req.signature_input = sig_input_joined;
  req.signature = sig_joined;
  req.user_agent = std::string_view(
      user_agent.data ? reinterpret_cast<const char*>(user_agent.data) : "",
      user_agent.data ? user_agent.len : 0);
  // Wire the HTTP fields the verifier may need as RFC 9421 covered field
  // components (raw wire bytes, EVERY field line; canonicalization happens
  // in the core).  The Web Bot Auth draft covers
  // ("@authority" "signature-agent"); the Signature-Agent value is
  // advisory-only here — it participates in the signature base but NEVER
  // selects a key directory (directory hosts stay operator-configured).
  // Allocation happens only on this signed-request path, never for plain
  // traffic.
  ngx_http_pagespeed_collect_header_fields(r, sig_agent_name, "signature-agent",
                                           &req.fields);
  static ngx_str_t user_agent_name = ngx_string("User-Agent");
  ngx_http_pagespeed_collect_header_fields(r, user_agent_name, "user-agent",
                                           &req.fields);
  // req.directory_host stays empty: the provider resolves the keyid across
  // every operator-configured directory host (never a request-derived one).

  wba::MultiHostWarmedKeyDirectoryProvider provider(
      &g_webbotauth_store, /*realm=*/"wba", g_webbotauth_dir_hosts);
  // Measure the verify latency around the (synchronous, store-backed) verify
  // call for the opt-in counter's coarse latency histogram.
  // Overhead is a pair of steady_clock reads on the signed-request path only.
  const auto verify_t0 = std::chrono::steady_clock::now();
  wba::VerifyResult result = wba::VerifyAndClassify(
      req, &provider, g_webbotauth_registry, static_cast<int64_t>(ngx_time()));
  const auto verify_t1 = std::chrono::steady_clock::now();
  const uint64_t verify_latency_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(verify_t1 -
                                                            verify_t0)
          .count());

  // Signature material with no web-bot-auth-tagged signature (the ONLY way
  // kHuman can come back on this path — the no-material case short-circuited
  // above): some other RFC 9421 signing scheme.  Classified exactly like an
  // unsigned request — no label, not "invalid" — but kept visible via the
  // other-signature counter.
  if (result.verdict == wba::Verdict::kHuman) {
    pagespeed::RecordWebBotAuthOtherSignature(g_serve_stats);
    return;
  }

  // Telemetry: only requests that CARRIED web-bot-auth signature material
  // are counted as verified/invalid.
  const bool verified = result.verdict == wba::Verdict::kSignedAgent ||
                        result.verdict == wba::Verdict::kVerifiedBot;
  pagespeed::RecordWebBotAuthSigned(g_serve_stats, verified);
  // Opt-in counter (experimental): on a VERIFIED verdict, also
  // record the per-signer count (keyed by the crypto-bound result.keyid) and
  // bucket the verify latency.  ALL verified keyids are recorded here,
  // including our own probe key — probe-kid / first-party exclusion is an
  // AGGREGATOR-side concern; the engine stays neutral.  These counters are
  // published only when the operator has enabled a non-off counter mode.
  if (verified) {
    pagespeed::RecordWebBotAuthVerifiedSigner(g_serve_stats, result.keyid,
                                              verify_latency_us);
  }

  std::string verdict_value;
  if (result.verdict == wba::Verdict::kVerifiedBot) {
    // Same rendering as 1.15: "<bot-name>, ed25519-verified".
    verdict_value = result.bot_name + ", ed25519-verified";
  } else {
    verdict_value = wba::VerdictToken(result.verdict);
  }

  u_char* data =
      static_cast<u_char*>(ngx_pnalloc(r->pool, verdict_value.size()));
  if (data == nullptr) {
    return;  // Allocation failure: skip the label; the request proceeds.
  }
  ngx_memcpy(data, verdict_value.data(), verdict_value.size());
  ctx->webbotauth_verdict.data = data;
  ctx->webbotauth_verdict.len = verdict_value.size();
}

// Defined with the other response-header helpers below; needed here because
// the PREACCESS enforcement handler precedes them in the file.
static ngx_table_elt_t* add_response_header_cstr(ngx_http_request_t* r,
                                                 const char* key,
                                                 const char* val);

// Experimental: RSL-CAP capability-token enforcement handler.
// Runs in the PREACCESS phase (mirroring 1.15's ps_rsl_cap_preaccess_handler
// intent within 2.0's module structure) so the token gate is applied before
// any content is served, independent of the pagespeed optimization
// PRECONTENT handler.  Default OFF: a single bool check and immediate
// NGX_DECLINED when the operator has not enabled enforcement — zero token
// work, zero allocation, structurally free.  When ON: validate the
// Authorization: License token against the operator-required license/scope
// and return 401/402/pass.  It emits STATUS CODES ONLY — plus the
// WWW-Authenticate challenge RFC 7235 requires on 401 — and never settles,
// meters, escrows, or custodies money, nor modifies the response beyond that.
static ngx_int_t ngx_http_pagespeed_rslcap_handler(ngx_http_request_t* r) {
  // Default OFF fast path: structurally zero-cost when disabled.  (The
  // enforcement-on flip itself is picked up by the PRECONTENT-phase poll — no
  // refresh call here, so disabled stays a single bool check.)
  if (!g_shared_config.rsl_cap_enforcement) {
    return NGX_DECLINED;
  }
  // Enforce only on the main request's first (external) pass; internal
  // redirects (index/try_files/error_page) and subrequests re-enter this phase
  // for the SAME client transaction and must not be re-adjudicated.
  if (r != r->main || r->internal) {
    return NGX_DECLINED;
  }

  // Poll the worker-published state BEFORE adjudicating.  This must live in
  // THIS handler, not only in the PRECONTENT phase: a 401/402 returned here
  // finalizes the request before PRECONTENT ever runs, so if this handler
  // relied on the PRECONTENT-phase poll, an enforcement-enabled nginx with an
  // empty key store (worker still inside its initial warm delay, or a
  // rotation transiently emptying the directory) would deny every request and
  // thereby starve the very poll that re-warms the store — a self-perpetuating
  // lockout.  Polling here also means the gate adjudicates against
  // this-interval-fresh config/keys instead of state one poll cycle behind.
  // Throttled + idempotent with the PRECONTENT-phase call (same CAS slot);
  // the per-request cost when enabled is exactly what PRECONTENT already pays.
  {
    auto* conf = static_cast<ngx_http_pagespeed_loc_conf_t*>(
        ngx_http_get_module_loc_conf(r, ngx_http_pagespeed_module));
    if (conf != nullptr && conf->cache_path.len > 0) {
      std::string_view cache_path_sv(
          reinterpret_cast<const char*>(conf->cache_path.data),
          conf->cache_path.len);
      MaybePollSharedState(cache_path_sv, r->connection->log);
      // The poll may have flipped enforcement off this very interval.
      if (!g_shared_config.rsl_cap_enforcement) {
        return NGX_DECLINED;
      }
    }
  }

  namespace wba = pagespeed::webbotauth;

  // Cap the Authorization header length BEFORE parsing so an abusive multi-KB
  // header cannot drive parser work (a real token is well under this).
  // Oversized -> treated as no valid token (401).
  static constexpr size_t kMaxRslCapAuthHeaderBytes = size_t{8} * 1024;
  static ngx_str_t authorization_name = ngx_string("Authorization");
  ngx_str_t auth_hdr = ngx_http_pagespeed_get_header(r, authorization_name);
  int http_status;
  if (auth_hdr.len > kMaxRslCapAuthHeaderBytes) {
    http_status = 401;
  } else {
    std::string_view auth(
        auth_hdr.data ? reinterpret_cast<const char*>(auth_hdr.data) : "",
        auth_hdr.data ? auth_hdr.len : 0);

    // Resolve issuer keys from the worker-warmed store under realm "rsl"
    // (distinct trust domain from the observe-only "wba" keys).  The provider
    // tries every operator-configured directory host; the request-supplied host
    // is ignored (plane-split).  A pure, synchronous, side-effect-free read —
    // never a fetch on the request path.
    wba::MultiHostWarmedKeyDirectoryProvider provider(
        &g_rslcap_store, /*realm=*/"rsl", g_rslcap_dir_hosts);
    wba::RslCapValidator validator(&provider);
    wba::RslCapToken token;
    wba::RslCapStatus status = validator.Validate(
        auth, g_shared_config.rsl_cap_requested_license,
        g_shared_config.rsl_cap_requested_scope, /*directory_host=*/"",
        static_cast<int64_t>(ngx_time()), &token);

    // Issuer pin (hardening): when an issuer is configured, an otherwise-
    // authorized token whose iss does not match is rejected as an unknown
    // issuer (401).  The core parses iss but leaves it unbound; this binds it
    // at the policy layer.
    const std::string& want_iss = g_shared_config.rsl_cap_issuer;
    if (status == wba::RslCapStatus::kAuthorized && !want_iss.empty() &&
        token.iss != want_iss) {
      status = wba::RslCapStatus::kUnknownIssuer;
    }

    // The verdict->status mapping is a kernel free function (unit-tested
    // without nginx).  0 = allow (NGX_DECLINED); 402/401 are returned verbatim.
    http_status = wba::RslCapStatusToHttpStatus(status);
  }

  // Telemetry: record every evaluated verdict (0/401/402) in the shared
  // serve-stats mmap (the worker exposes it via /v1/metrics).
  pagespeed::RecordRslCapVerdict(g_serve_stats, http_status);

  // RFC 7235 §3.1: a 401 MUST carry a WWW-Authenticate challenge naming the
  // expected scheme.  Deliberate forward deviation from the 1.15 port source,
  // which omits it (same convention as the #864 extensions): headers pushed
  // into headers_out survive the special-response path, the auth-module
  // idiom.  The slot assignment lets downstream code find the challenge
  // without a list scan.
  if (http_status == 401) {
    ngx_table_elt_t* h =
        add_response_header_cstr(r, "WWW-Authenticate", "License");
    if (h != nullptr) {
      r->headers_out.www_authenticate = h;
    }
  }

  return http_status == 0 ? NGX_DECLINED : http_status;
}

// Build the full request URL (path + query string) for cache key
// construction.  Nginx splits the URI into r->uri (path) and r->args
// (query string); both must be included so that distinct query
// parameters produce distinct cache entries.
// URL normalization (param stripping, extension-based stripping, sorting)
// is applied so that semantically equivalent URLs share cache entries.
static std::string ngx_http_pagespeed_cache_url(ngx_http_request_t* r) {
  std::string url(reinterpret_cast<const char*>(r->uri.data), r->uri.len);
  if (r->args.len > 0) {
    url.push_back('?');
    url.append(reinterpret_cast<const char*>(r->args.data), r->args.len);
  }
  return pagespeed::NormalizeCacheUrl(url, g_url_norm_config);
}

// Normalize hostname for cache key operations.
static std::string ngx_http_pagespeed_cache_hostname(ngx_http_request_t* r) {
  return pagespeed::NormalizeCacheHostname(ngx_http_pagespeed_hostname(r),
                                           g_url_norm_config);
}

// Whether the server block that received `r` stands behind `hostname` (the
// normalized cache hostname, which is also where the worker fetches from for
// the /llms.txt build and the agent render): one of the block's server names
// (exact, wildcard or regex) names it, or the operator's host alias table
// does.  A catch-all block ("_", "") vouches for nothing.
static bool ngx_http_pagespeed_host_vouched(ngx_http_request_t* r,
                                            std::string_view hostname) {
  if (hostname.empty()) {
    return false;
  }
  if (pagespeed::HostAliasesName(g_url_norm_config.host_aliases, hostname)) {
    return true;
  }
  // A port in the hostname is part of where the worker fetches from, so a
  // listed name is vouched for only on the port the request arrived on.
  // (The normalized hostname has no port for 80 and 443.)
  std::string_view port;
  if (hostname.front() == '[') {
    const size_t close = hostname.find(']');
    if (close != std::string_view::npos) {
      port = hostname.substr(close + 1);
    }
  } else {
    const size_t colon = hostname.find(':');
    if (colon != std::string_view::npos) {
      port = hostname.substr(colon);
    }
  }
  if (!port.empty()) {
    if (port.front() != ':') {
      return false;
    }
    port.remove_prefix(1);
    const in_port_t local_port =
        ngx_inet_get_port(r->connection->local_sockaddr);
    if (local_port == 0 || port != std::to_string(local_port)) {
      return false;
    }
  }
  std::string host = pagespeed::AuthorityHost(hostname);
  if (host.empty()) {
    return false;
  }
  const auto* sconf = static_cast<const ngx_http_pagespeed_srv_conf_t*>(
      ngx_http_get_module_srv_conf(r, ngx_http_pagespeed_module));
  if (sconf == nullptr) {
    return false;
  }
  const ngx_http_server_name_t* names = sconf->server_names;
  for (ngx_uint_t i = 0; i < sconf->server_names_count; ++i) {
#if (NGX_PCRE)
    if (names[i].regex != nullptr) {
      // Match only: no captures, so the request's $1.. stay untouched.
      ngx_str_t subject;
      subject.data = reinterpret_cast<u_char*>(host.data());
      subject.len = host.size();
      if (ngx_regex_exec(names[i].regex->regex, &subject, nullptr, 0) >= 0) {
        return true;
      }
      continue;
    }
#endif
    std::string_view name(reinterpret_cast<const char*>(names[i].name.data),
                          names[i].name.len);
    if (pagespeed::ServerNameMatchesHost(name, host)) {
      return true;
    }
  }
  return false;
}

// While agent optimization is on, an HTML notification can make the worker
// render the page and fetch its subresources from the notification's
// hostname.  Withhold it for a hostname this block does not vouch for
// (`host_vouched`, from ngx_http_pagespeed_host_vouched); the response itself
// is served as usual, just not optimized.
static bool ngx_http_pagespeed_withhold_html_notify(ContentType ct,
                                                    bool host_vouched) {
  return ct == ContentType::kHtml && g_shared_config.agent_optimize_entitled &&
         !host_vouched;
}

// Extract the request scheme ("http" or "https").
// When pagespeed_trust_x_forwarded_proto is on, the X-Forwarded-Proto
// header is checked first (for reverse-proxy / TLS-termination setups).
// Otherwise the scheme is taken from the connection's TLS state (see below).
static std::string_view ngx_http_pagespeed_scheme(
    ngx_http_request_t* r, ngx_http_pagespeed_loc_conf_t* lcf) {
  // Check X-Forwarded-Proto if trusted.
  if (lcf->trust_x_forwarded_proto) {
    ngx_table_elt_t* h = NULL;
    ngx_list_part_t* part = &r->headers_in.headers.part;
    ngx_table_elt_t* header = (ngx_table_elt_t*)part->elts;
    for (ngx_uint_t i = 0;; i++) {
      if (i >= part->nelts) {
        if (part->next == NULL) break;
        part = part->next;
        header = (ngx_table_elt_t*)part->elts;
        i = 0;
      }
      if (header[i].key.len == 17 &&
          ngx_strncasecmp(header[i].key.data, (u_char*)"X-Forwarded-Proto",
                          17) == 0) {
        h = &header[i];
        break;
      }
    }
    if (h && h->value.len > 0) {
      std::string_view proto(reinterpret_cast<const char*>(h->value.data),
                             h->value.len);
      if (proto == "https") return "https";
      if (proto == "http") return "http";
      // Invalid value — fall through to connection-level check.
    }
  }
  // Without a trusted X-Forwarded-Proto, the scheme comes from the connection
  // for every protocol: TLS present means "https", otherwise "http".
  //  - HTTP/1.x: r->schema is set only for an absolute-form request line and
  //    is whatever the client wrote, so it is not used.
  //  - HTTP/2 and HTTP/3: r->schema comes from the client's :scheme and is not
  //    used either; the per-stream connection carries the TLS state of the
  //    connection it belongs to.
  // This matches nginx's own $scheme, and one page has one cache key whatever
  // protocol fetched it.
  // The guard is the condition under which nginx declares the field (a module
  // built with --with-compat has no NGX_HTTP_SSL).
#if (NGX_SSL || NGX_COMPAT)
  if (r->connection->ssl != NULL) {
    return "https";
  }
#endif
  return "http";
}

// Local aliases for recording-path CC checks (no-store, private,
// no-transform).  Map to AlternateMetadata flag constants.
static constexpr uint16_t kCCNoStore = AlternateMetadata::kCCOriginNoStore;
static constexpr uint16_t kCCPrivate = AlternateMetadata::kCCOriginPrivate;
static constexpr uint16_t kCCNoTransform =
    AlternateMetadata::kCCOriginNoTransform;

// RFC 9111 §3.5: does this request carry an Authorization header?  §3.5
// triggers on header PRESENCE, so the core-populated slot being bound is
// sufficient (nginx binds the header to r->headers_in.authorization during
// request-header processing) — no value inspection, no header-list scan.
// Raw signal only: gate call sites go through
// ngx_http_pagespeed_authz_gate_has_auth() below so the RSL-CAP exemption
// is applied uniformly.
static bool ngx_http_pagespeed_request_has_authorization(
    ngx_http_request_t* r) {
  return r->headers_in.authorization != nullptr;
}

// Effective §3.5 gate input for this request: raw Authorization presence
// minus the RSL-CAP exemption, DERIVED at gate time from live enforcement
// state plus the header's auth scheme — no per-request state.  With
// rsl_cap_enforcement ON, the PREACCESS gate 401s every main external
// request whose Authorization is not a VALID License token, so a
// License-scheme credential reaching the serve/record gates on that same
// main external pass was necessarily validated and terminated by the
// module — not end-user authentication toward the origin.  Exempt only
// with !r->internal: error_page/index/try_files/rewrite-last redirect
// targets keep r == r->main but set r->internal, subrequests set both,
// and PREACCESS declines all of them — so an internal request was never
// License-validated and is gated as ordinary Authorization (fail-closed).
// Canonical rationale incl. the error_page-401 hazard and the accepted
// enforcement-flip edge: src/nginx/authz_cache_gate.h.  The scheme match
// is the validator's own case-sensitive "License " prefix
// (RslCapValidator::kSchemePrefix) — tighter than RFC 7235's
// case-insensitive scheme rule, and exactly what PREACCESS admits: a
// differently-cased scheme is 401'd there and never reaches a gate.
// Enforcement OFF and every other scheme stay fail-closed.  Feeds
// AuthzCacheGateAllows() on both the HIT (serve) and record (store)
// paths, and AuthzCacheGateAllowsStale() on the coalesced-stale and
// stale-if-error serves.
static bool ngx_http_pagespeed_authz_gate_has_auth(ngx_http_request_t* r) {
  if (!ngx_http_pagespeed_request_has_authorization(r)) {
    return false;
  }
  if (g_shared_config.rsl_cap_enforcement && !r->internal) {
    const ngx_str_t& value = r->headers_in.authorization->value;
    const char* prefix = pagespeed::webbotauth::RslCapValidator::kSchemePrefix;
    const size_t prefix_len = ngx_strlen(prefix);
    if (value.len >= prefix_len &&
        ngx_strncmp(value.data, prefix, prefix_len) == 0) {
      return false;
    }
  }
  return true;
}

// Does this request carry a Cookie (or the obsolete Cookie2) header?  Input
// to the cookie gate (src/nginx/cookie_cache_gate.h) on the record, HIT and
// 304-restamp paths.  Presence is what counts, whatever the value, as for
// Authorization.  Scans the raw header list rather than a core slot, whose
// name and shape differ across nginx versions.
static bool ngx_http_pagespeed_request_has_cookie(ngx_http_request_t* r) {
  ngx_list_part_t* part = &r->headers_in.headers.part;
  auto* h = static_cast<ngx_table_elt_t*>(part->elts);
  for (ngx_uint_t i = 0;; i++) {
    if (i >= part->nelts) {
      if (part->next == nullptr) {
        break;
      }
      part = part->next;
      h = static_cast<ngx_table_elt_t*>(part->elts);
      i = 0;
    }
    if ((h[i].key.len == 6 &&
         ngx_strncasecmp(h[i].key.data, (u_char*)"Cookie", 6) == 0) ||
        (h[i].key.len == 7 &&
         ngx_strncasecmp(h[i].key.data, (u_char*)"Cookie2", 7) == 0)) {
      return true;
    }
  }
  return false;
}

// Derive the origin's Cache-Control state from the response's header lines.
//
// The derivation itself lives in lib/cache
// (pagespeed::AccumulateOriginCacheControl) and is shared with every other
// front end and with the C ABI, so there is one parse of a directive list
// rather than one per consumer.  What stays here is the part that is
// genuinely nginx's: walking `headers_out.cache_control`.
//
// Accumulating per line is what RFC 9111 §5.2 asks for — several
// Cache-Control lines are one directive list — and matches what this loop
// did before.  The shared function sets kCCOriginHeaderPresent on every
// call, so the bit ends up set exactly when the response carried at least
// one line, which is what the old post-loop assignment computed.
static pagespeed::OriginCacheControl ngx_http_pagespeed_parse_cache_control(
    ngx_http_request_t* r) {
  pagespeed::OriginCacheControl result;
  for (ngx_table_elt_t* cc = r->headers_out.cache_control; cc != nullptr;
       cc = cc->next) {
    pagespeed::AccumulateOriginCacheControl(
        std::string_view(reinterpret_cast<const char*>(cc->value.data),
                         cc->value.len),
        &result);
  }
  return result;
}

// Look up a response header by name from headers_out.headers.
static ngx_str_t ngx_http_pagespeed_get_response_header(ngx_http_request_t* r,
                                                        std::string_view name) {
  ngx_str_t value = ngx_null_string;
  ngx_list_part_t* part = &r->headers_out.headers.part;
  ngx_table_elt_t* h = static_cast<ngx_table_elt_t*>(part->elts);

  for (ngx_uint_t i = 0;; i++) {
    if (i >= part->nelts) {
      if (part->next == nullptr) break;
      part = part->next;
      h = static_cast<ngx_table_elt_t*>(part->elts);
      i = 0;
    }
    // hash == 0 is nginx's convention for a DEACTIVATED header — the entry
    // is still in the list but must be treated as absent (this same 304
    // branch uses it to retire upstream's Cache-Control before emitting
    // ours).  Skipping it keeps "header present" from reading true for a
    // header nginx has already removed.
    if (h[i].hash != 0 && h[i].key.len == name.size() &&
        ngx_strncasecmp(
            h[i].key.data,
            const_cast<u_char*>(reinterpret_cast<const u_char*>(name.data())),
            name.size()) == 0) {
      value = h[i].value;
      break;
    }
  }
  return value;
}

// The freshness lifetime the response's Expires header states, measured from
// its Date (or now, without one), clamped at 0.  False when there is no
// parseable Expires.  The record path stores this as the entry's max-age when
// the origin sent no Cache-Control line; the cookie gate reads it the same
// way.
static bool ngx_http_pagespeed_expires_lifetime(ngx_http_request_t* r,
                                                int64_t* lifetime) {
  ngx_str_t expires_hdr = ngx_http_pagespeed_get_response_header(r, "Expires");
  if (expires_hdr.len == 0) {
    return false;
  }
  time_t expires_time = ngx_parse_http_time(expires_hdr.data, expires_hdr.len);
  if (expires_time <= 0) {
    return false;
  }
  time_t date_time = 0;
  if (r->headers_out.date) {
    date_time = ngx_parse_http_time(r->headers_out.date->value.data,
                                    r->headers_out.date->value.len);
  }
  if (date_time <= 0) {
    date_time = time(nullptr);
  }
  int64_t from_expires = expires_time - date_time;
  *lifetime = from_expires < 0 ? 0 : from_expires;
  return true;
}

// Collect the response's Vary header lines into one comma-separated value.
//
// HTTP allows multiple Vary headers which combine as a single comma-list
// (RFC 9110 §5.3), and the store-side predicates must see the JOINED list —
// a per-line check accepts combinations the whole list refuses.  This is the
// part that is genuinely nginx's; every decision made from the result lives
// in lib/cache.
//
// `saw_any` (optional) reports whether the response carried a Vary header AT
// ALL, which an empty result cannot express: `Vary: ` and no Vary at all both
// combine to "".  The 304 path needs the distinction — RFC 9111 §4.3.4 says a
// header absent from a 304 is UNCHANGED, not cleared, so only a Vary that is
// actually present may re-decide a stored entry's marker.
static std::string ngx_http_pagespeed_combined_vary(ngx_http_request_t* r,
                                                    bool* saw_any = nullptr) {
  if (saw_any != nullptr) *saw_any = false;
  std::string combined;
  ngx_list_part_t* part = &r->headers_out.headers.part;
  ngx_table_elt_t* h = static_cast<ngx_table_elt_t*>(part->elts);
  for (ngx_uint_t i = 0;; i++) {
    if (i >= part->nelts) {
      if (part->next == nullptr) break;
      part = part->next;
      h = static_cast<ngx_table_elt_t*>(part->elts);
      i = 0;
    }
    if (h[i].key.len == 4 &&
        ngx_strncasecmp(h[i].key.data,
                        reinterpret_cast<u_char*>(const_cast<char*>("Vary")),
                        4) == 0) {
      if (saw_any != nullptr) *saw_any = true;
      if (!combined.empty()) combined += ',';
      combined.append(reinterpret_cast<const char*>(h[i].value.data),
                      h[i].value.len);
    }
  }
  return combined;
}

// =====================================================================
// Response-header sidecar, store side (issue #208).  The
// decisions live in src/nginx/headers_sidecar_record.h; this is the nginx
// glue: the origin-header snapshot, the MISS-block gate and its log line.
// =====================================================================

// Rate-limited "not caching: header cannot be reproduced" log: one NOTICE
// per (server block, first reason) per nginx worker lifetime, capped like
// the missing-Cache-Control warning.  Keyed on the server block that handled
// the request (its address in the configuration, which lives as long as the
// process) rather than on the request's Host header: a client choosing Host
// values could otherwise use up the cap and silence later notices.  The
// header names come from the origin, so the key space is bounded by the
// configuration and the origins behind it.
static std::unordered_set<std::string> g_sidecar_fall_through_warned;

// Snapshot the ORIGIN's header lines.  Called at the top of the header
// filter, before the module pushes anything, so position alone separates
// origin lines from the module's own.  Copies the ngx_str_t pairs (the bytes
// stay in the pool-owned list entries, which live as long as the request).
// Deactivated entries (hash == 0) are absent by nginx's convention.
//
// Fails closed: if the snapshot cannot be allocated the response is marked
// uncacheable, because an empty snapshot would classify as "nothing to
// carry" and an entry recorded from it would drop every header the origin
// sent.
static void ngx_http_pagespeed_snapshot_origin_headers(
    ngx_http_request_t* r, ngx_http_pagespeed_ctx_t* ctx) {
  ctx->origin_headers = nullptr;
  ngx_uint_t live = 0;
  for (ngx_list_part_t* part = &r->headers_out.headers.part; part != nullptr;
       part = part->next) {
    const auto* h = static_cast<const ngx_table_elt_t*>(part->elts);
    for (ngx_uint_t i = 0; i < part->nelts; i++) {
      if (h[i].hash != 0) live++;
    }
  }
  if (live == 0) {
    return;
  }
  ngx_array_t* pairs =
      ngx_array_create(r->pool, live, sizeof(ngx_http_pagespeed_header_pair_t));
  if (pairs == nullptr) {
    ctx->uncacheable = 1;
    return;
  }
  for (ngx_list_part_t* part = &r->headers_out.headers.part; part != nullptr;
       part = part->next) {
    const auto* h = static_cast<const ngx_table_elt_t*>(part->elts);
    for (ngx_uint_t i = 0; i < part->nelts; i++) {
      if (h[i].hash == 0) continue;
      auto* pair =
          static_cast<ngx_http_pagespeed_header_pair_t*>(ngx_array_push(pairs));
      if (pair == nullptr) {
        ctx->uncacheable = 1;
        return;
      }
      pair->key = h[i].key;
      pair->value = h[i].value;
    }
  }
  ctx->origin_headers = pairs;
}

// Views over the snapshot, in received order, one per line (repeated names
// stay separate entries, as the gate requires).
static std::vector<pagespeed::HeaderField>
ngx_http_pagespeed_origin_header_views(const ngx_http_pagespeed_ctx_t* ctx) {
  std::vector<pagespeed::HeaderField> views;
  if (ctx->origin_headers == nullptr) {
    return views;
  }
  const auto* pairs = static_cast<const ngx_http_pagespeed_header_pair_t*>(
      ctx->origin_headers->elts);
  views.reserve(ctx->origin_headers->nelts);
  for (ngx_uint_t i = 0; i < ctx->origin_headers->nelts; i++) {
    views.push_back(pagespeed::HeaderField{
        std::string_view(reinterpret_cast<const char*>(pairs[i].key.data),
                         pairs[i].key.len),
        std::string_view(reinterpret_cast<const char*>(pairs[i].value.data),
                         pairs[i].value.len)});
  }
  return views;
}

// The MISS-block gate and its refusal eviction.  Runs on a 200 the body
// filter would otherwise record, after every other store gate, so a
// response already refused for no-store / Set-Cookie / Vary is not counted
// a second time here.
//
//   * swap-eligible  → record through the sidecar path (record_response);
//   * fall-through   → not cached (served from the origin each time), counted,
//                      one NOTICE per (server block, header) naming the
//                      remedy;
//   * never-optimized (nonce-bearing CSP) → not cached, counted.
//
// On a refusal an entry recorded earlier for the URL is removed: the fresh
// origin response is the evidence that the stored entry can no longer be
// served with the origin's headers.  This has to happen here because an
// uncacheable response never reaches record_response.  It is driven by the
// origin's response, never by a request header.
static void ngx_http_pagespeed_classify_origin_headers(
    ngx_http_request_t* r, ngx_http_pagespeed_ctx_t* ctx) {
  ngx_http_pagespeed_loc_conf_t* conf =
      static_cast<ngx_http_pagespeed_loc_conf_t*>(
          ngx_http_get_module_loc_conf(r, ngx_http_pagespeed_module));
  if (conf->cache_path.len == 0) {
    return;
  }

  const std::vector<pagespeed::HeaderField> views =
      ngx_http_pagespeed_origin_header_views(ctx);
  const pagespeed::SidecarClassification classification =
      pagespeed::ClassifyForHeadersSidecar(views);
  if (classification.swap_eligible()) {
    ctx->sidecar_classified = 1;
    return;
  }

  ctx->uncacheable = 1;
  if (classification.verdict == pagespeed::SidecarVerdict::kNeverOptimized) {
    pagespeed::RecordHeadersSidecarNeverOptimized(g_serve_stats);
  } else {
    pagespeed::RecordHeadersSidecarFallThrough(g_serve_stats);
    std::string_view header =
        pagespeed::FirstUnreproducibleHeader(classification);
    std::string_view reason =
        !header.empty()
            ? header
            : (classification.reasons.empty()
                   ? std::string_view("unknown")
                   : std::string_view(classification.reasons.front()));
    const auto* cscf = static_cast<const ngx_http_core_srv_conf_t*>(
        ngx_http_get_module_srv_conf(r, ngx_http_core_module));
    // The log names the server block's configured name, not the request's
    // Host, for the same reason the key does.
    std::string_view host_sv =
        cscf->server_name.len > 0
            ? std::string_view(
                  reinterpret_cast<const char*>(cscf->server_name.data),
                  cscf->server_name.len)
            : std::string_view("the default server");
    std::string warn_key = std::to_string(reinterpret_cast<uintptr_t>(cscf));
    warn_key.append("|").append(reason);
    if (g_sidecar_fall_through_warned.size() < kMaxCCWarnings &&
        g_sidecar_fall_through_warned.insert(warn_key).second) {
      if (!header.empty()) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                      "pagespeed: not caching responses for %*s: origin "
                      "header %*s cannot be reproduced on cache hits; hide it "
                      "with proxy_hide_header or set it with add_header on "
                      "this server to keep caching",
                      host_sv.size(), host_sv.data(), header.size(),
                      header.data());
      } else {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                      "pagespeed: not caching responses for %*s: origin "
                      "headers cannot be reproduced on cache hits (%*s)",
                      host_sv.size(), host_sv.data(), reason.size(),
                      reason.data());
      }
    }
  }

  std::string_view cache_path_sv(
      reinterpret_cast<const char*>(conf->cache_path.data),
      conf->cache_path.len);
  std::shared_ptr<PageSpeedCache> cache =
      CheckGenerationAndGetCache(cache_path_sv, r->connection->log);
  if (!cache) {
    cache = GetCache(cache_path_sv, r->connection->log);
  }
  if (!cache) {
    return;
  }
  std::string url_str = ngx_http_pagespeed_cache_url(r);
  std::string hostname = ngx_http_pagespeed_cache_hostname(r);
  CapabilityMask default_mask;
  const AlternateId identity_id =
      MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
  if (pagespeed::EvictOnSidecarRefusal(*cache, url_str, hostname,
                                       ngx_http_pagespeed_scheme(r, conf),
                                       identity_id)) {
    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                  "pagespeed: origin response for %V carries headers a cache "
                  "hit cannot reproduce; removed the stored entries",
                  &r->uri);
  }
}

// The URL's stored entries were just removed because its origin's answer may
// no longer be shared (a `Vary` the cache does not key on).  Tell the worker,
// with the same origin-refreshed sentinel a freshness re-fetch sends: it
// purges whatever it still holds for the URL and clears its record of having
// processed it, so once the origin stops varying the re-recorded original is
// optimized again rather than skipped as already done.  Fire-and-forget.
static void ngx_http_pagespeed_notify_url_dropped(ngx_http_request_t* r,
                                                  std::string_view url,
                                                  std::string_view hostname,
                                                  std::string_view scheme,
                                                  ContentType ct) {
  if (g_shared_config.socket_path.empty()) {
    return;
  }
  CacheNotification refresh;
  refresh.url = std::string(url);
  refresh.hostname = std::string(hostname);
  refresh.scheme = std::string(scheme);
  refresh.content_type = ct;
  refresh.capability_mask = pagespeed::kOriginRefreshedSentinel;
  refresh.host_vouched = ngx_http_pagespeed_host_vouched(r, hostname);
  auto send_result = SendNotificationPersistent(
      std::string_view(g_shared_config.socket_path), refresh);
  if (!send_result.success) {
    ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                  "pagespeed: dropped-entry notify failed: %s",
                  send_result.error_message.c_str());
  }
}

// A 2xx origin response whose `Vary` the cache may not store
// (lib/cache/vary_storability.h).  Not recording it is not enough when the
// URL already has entries — stored before its origin started varying, or by
// an earlier version — because they would keep answering other clients.
// Drop them, as the headers-sidecar refusal does.
static void ngx_http_pagespeed_drop_on_vary_refusal(ngx_http_request_t* r) {
  ngx_http_pagespeed_loc_conf_t* conf =
      static_cast<ngx_http_pagespeed_loc_conf_t*>(
          ngx_http_get_module_loc_conf(r, ngx_http_pagespeed_module));
  if (conf->cache_path.len == 0) {
    return;
  }
  std::string_view cache_path_sv(
      reinterpret_cast<const char*>(conf->cache_path.data),
      conf->cache_path.len);
  std::shared_ptr<PageSpeedCache> cache =
      CheckGenerationAndGetCache(cache_path_sv, r->connection->log);
  if (!cache) {
    cache = GetCache(cache_path_sv, r->connection->log);
  }
  if (!cache) {
    return;
  }
  std::string url_str = ngx_http_pagespeed_cache_url(r);
  std::string hostname = ngx_http_pagespeed_cache_hostname(r);
  std::string_view scheme = ngx_http_pagespeed_scheme(r, conf);
  CapabilityMask default_mask;
  const AlternateId identity_id =
      MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
  if (pagespeed::EvictOnSidecarRefusal(*cache, url_str, hostname, scheme,
                                       identity_id)) {
    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                  "pagespeed: origin response for %V varies on a request "
                  "header the cache does not key on; removed the stored "
                  "entries",
                  &r->uri);
    ngx_http_pagespeed_notify_url_dropped(
        r, url_str, hostname, scheme,
        ngx_http_pagespeed_detect_content_type(r));
  }
}

// =====================================================================
// Response-header sidecar, serve side (issue #208).  The
// decisions live in src/nginx/headers_sidecar_replay.h; this is the nginx
// glue: the one block read per flagged hit, its pool copy, and the replay.
// =====================================================================

// Read the URL's stored header block for an entry that requires one and
// copy its lines into the request pool (ctx->sidecar_fields).  Returns false
// when the entry requires a block and none can be served with — the caller
// then treats the request as a MISS (the record arm rewrites the block).
// Entries that require no block return true without reading anything.
static bool ngx_http_pagespeed_load_sidecar(
    ngx_http_request_t* r, ngx_http_pagespeed_ctx_t* ctx, PageSpeedCache& cache,
    const cyclone::CacheKey& key, uint8_t entry_flags, bool agent_markdown) {
  ctx->sidecar_fields = nullptr;
  ctx->sidecar_carried_vary = 0;
  ctx->sidecar_carried_nosniff = 0;
  if (!pagespeed::SidecarReadRequired(entry_flags, agent_markdown)) {
    return true;
  }
  // The block's ReadResult is released when this function returns, so every
  // byte the response needs is copied into the pool first.
  auto block = cache.ReadHeadersSidecarByKey(key);
  std::optional<std::string_view> payload;
  if (block.has_value() && block->is_valid()) {
    payload = PageSpeedCache::HeadersSidecarPayload(*block);
  }
  const pagespeed::SidecarServePlan plan =
      pagespeed::PlanSidecarServe(entry_flags, agent_markdown, payload);
  if (plan.foreign_field) {
    ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                  "pagespeed: stored headers for %V name a field the cache "
                  "builds itself (damaged cache file?) — treating as miss; "
                  "the response is recorded again",
                  &r->uri);
  }
  if (plan.outcome != pagespeed::SidecarServeOutcome::kReplay) {
    return plan.outcome == pagespeed::SidecarServeOutcome::kNotRequired;
  }
  ngx_array_t* pairs = ngx_array_create(
      r->pool, plan.fields.size(), sizeof(ngx_http_pagespeed_header_pair_t));
  if (pairs == nullptr) {
    return false;  // fail closed: never a hit without its headers
  }
  for (const auto& field : plan.fields) {
    auto* pair =
        static_cast<ngx_http_pagespeed_header_pair_t*>(ngx_array_push(pairs));
    auto* key_buf =
        static_cast<u_char*>(ngx_pnalloc(r->pool, field.name.size()));
    auto* value_buf = static_cast<u_char*>(
        ngx_pnalloc(r->pool, field.value.empty() ? 1 : field.value.size()));
    if (pair == nullptr || key_buf == nullptr || value_buf == nullptr) {
      return false;
    }
    ngx_memcpy(key_buf, field.name.data(), field.name.size());
    if (!field.value.empty()) {
      ngx_memcpy(value_buf, field.value.data(), field.value.size());
    }
    pair->key.data = key_buf;
    pair->key.len = field.name.size();
    pair->value.data = value_buf;
    pair->value.len = field.value.size();
  }
  ctx->sidecar_fields = pairs;
  ctx->sidecar_carried_vary = plan.carried_vary ? 1 : 0;
  ctx->sidecar_carried_nosniff = plan.carried_nosniff ? 1 : 0;
  return true;
}

// Push the stored header lines onto the response, verbatim and in stored
// order.  Called by every path that serves the entry, BEFORE that path emits
// its own Vary / Cache-Control / Age / ETag (the stored lines never carry
// those names; see headers_sidecar_replay.h).  Counts one replayed response
// when at least one line went out.  Lines that cannot be pushed (allocation
// failure) end the replay; the pool copy is already complete, so this can
// only fail together with the rest of the response.
static void ngx_http_pagespeed_replay_sidecar(ngx_http_request_t* r,
                                              ngx_http_pagespeed_ctx_t* ctx) {
  if (ctx->sidecar_fields == nullptr || ctx->sidecar_fields->nelts == 0) {
    return;
  }
  const auto* pairs = static_cast<const ngx_http_pagespeed_header_pair_t*>(
      ctx->sidecar_fields->elts);
  ngx_uint_t pushed = 0;
  for (ngx_uint_t i = 0; i < ctx->sidecar_fields->nelts; i++) {
    ngx_table_elt_t* h =
        static_cast<ngx_table_elt_t*>(ngx_list_push(&r->headers_out.headers));
    if (h == nullptr) {
      break;
    }
    h->hash = 1;
    h->next = nullptr;
    h->key = pairs[i].key;
    h->value = pairs[i].value;
    pushed++;
  }
  if (pushed > 0) {
    pagespeed::RecordHeadersSidecarReplayed(g_serve_stats);
  }
}

// Record completed response body to cache and notify worker
static void ngx_http_pagespeed_record_response(ngx_http_request_t* r,
                                               ngx_http_pagespeed_ctx_t* ctx) {
  ngx_http_pagespeed_loc_conf_t* conf =
      static_cast<ngx_http_pagespeed_loc_conf_t*>(
          ngx_http_get_module_loc_conf(r, ngx_http_pagespeed_module));

  if (conf->cache_path.len == 0) {
    return;
  }

  // Assemble buffered body into a contiguous string
  std::string body;
  for (ngx_chain_t* cl = ctx->buffered_body; cl; cl = cl->next) {
    ngx_buf_t* b = cl->buf;
    if (b->in_file) {
      continue;  // Skip file-backed buffers for MVP
    }
    if (b->pos < b->last) {
      body.append(reinterpret_cast<const char*>(b->pos),
                  static_cast<size_t>(b->last - b->pos));
    }
  }

  std::string_view cache_path_sv(
      reinterpret_cast<const char*>(conf->cache_path.data),
      conf->cache_path.len);

  // Unconditional generation check (not time-gated like the content handler)
  // because writes to a deleted volume inode after a purge-all would be
  // silently lost.  The content handler's read path can tolerate a brief
  // stale window; the write path cannot.
  std::shared_ptr<PageSpeedCache> cache =
      CheckGenerationAndGetCache(cache_path_sv, r->connection->log);
  if (!cache) {
    cache = GetCache(cache_path_sv, r->connection->log);
  }
  if (!cache) {
    // A cache-directory generation mismatch detected while this response
    // was in flight is not an open failure: it was logged once already.
    if (g_cache_dir_gen.cache_enabled()) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                    "pagespeed: failed to open cache at %V", &conf->cache_path);
    }
    return;
  }

  // Write original content with default mask (0x08 = Desktop/Identity/Original).
  std::string url_str = ngx_http_pagespeed_cache_url(r);
  std::string_view url(url_str);
  std::string hostname = ngx_http_pagespeed_cache_hostname(r);
  CapabilityMask default_mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));

  ContentType ct = ngx_http_pagespeed_detect_content_type(r);
  AlternateMetadata meta;
  meta.full_mask = default_mask.Encode();
  meta.content_type = ct;
  // Preserve the origin's full Content-Type (e.g. "text/html; charset=utf-8")
  // so the HIT path can set the correct header without extension sniffing.
  if (r->headers_out.content_type.len > 0 &&
      r->headers_out.content_type.len <= 8192) {
    meta.origin_content_type = std::string(
        reinterpret_cast<const char*>(r->headers_out.content_type.data),
        r->headers_out.content_type.len);
  }

  // Parse origin Cache-Control and populate metadata fields.
  auto parsed_cc = ngx_http_pagespeed_parse_cache_control(r);
  meta.origin_max_age = parsed_cc.max_age;
  meta.origin_s_maxage = parsed_cc.s_maxage;
  meta.origin_cc_flags = parsed_cc.cc_flags;

  // Persist the origin's own Accept negotiation with the entry (the flag bit
  // the v8 format reserved for it).  It has to be durable for the same reason
  // kCCOriginNoTransform is: on every later serve the origin's headers are
  // gone, and both the "do not optimize this" decision and the "declare Accept
  // in Vary" decision are made from the entry alone.  Classified in the header
  // filter from the SAME verdict that gates notification below, so the stored
  // marker and the notify decision cannot disagree.
  if (ctx->origin_varies_accept) {
    meta.flags |= AlternateMetadata::kFlagOriginVariesAccept;
  }

  // Warn when origin sends no Cache-Control header (rate-limited).
  if (!(parsed_cc.cc_flags & AlternateMetadata::kCCOriginHeaderPresent)) {
    std::string_view host_sv = ngx_http_pagespeed_hostname(r);
    const char* type_name;
    switch (ct) {
      case ContentType::kHtml:
        type_name = "text/html";
        break;
      case ContentType::kCss:
        type_name = "text/css";
        break;
      case ContentType::kJs:
        type_name = "application/javascript";
        break;
      case ContentType::kImage:
        type_name = "image/*";
        break;
      default:
        type_name = "other";
        break;
    }
    std::string warn_key = std::string(host_sv) + ":" + type_name;
    if (g_missing_cc_warned.size() < kMaxCCWarnings &&
        g_missing_cc_warned.find(warn_key) == g_missing_cc_warned.end()) {
      g_missing_cc_warned.insert(warn_key);
      ngx_int_t default_ma = 0;
      if (ct == ContentType::kCss || ct == ContentType::kJs) {
        default_ma = conf->css_max_age;
      } else if (ct == ContentType::kImage) {
        default_ma = conf->image_max_age;
      } else if (ct == ContentType::kHtml) {
        default_ma = conf->html_max_age;
      }
      ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                    "pagespeed: origin for %*s sent no Cache-Control header "
                    "for %s response. Using default max-age=%d. Set "
                    "Cache-Control headers on your origin for optimal "
                    "caching behavior.",
                    host_sv.size(), host_sv.data(), type_name,
                    static_cast<int>(default_ma));
    }
  }

  // Extract ETag (stored verbatim, including quotes and W/ prefix).
  if (r->headers_out.etag) {
    std::string_view etag_sv(
        reinterpret_cast<const char*>(r->headers_out.etag->value.data),
        r->headers_out.etag->value.len);
    if (etag_sv.size() <= 65535) {
      meta.origin_etag = std::string(etag_sv);
    } else {
      g_etag_too_long.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // Extract Last-Modified (as Unix timestamp).
  if (r->headers_out.last_modified_time > 0) {
    meta.origin_last_modified =
        static_cast<uint32_t>(r->headers_out.last_modified_time);
  }

  // Expires fallback: when no Cache-Control header is present.
  if (!(parsed_cc.cc_flags & AlternateMetadata::kCCOriginHeaderPresent)) {
    int64_t max_age_from_expires = 0;
    if (ngx_http_pagespeed_expires_lifetime(r, &max_age_from_expires)) {
      if (max_age_from_expires > static_cast<int64_t>(UINT32_MAX)) {
        meta.origin_max_age = UINT32_MAX;
      } else {
        meta.origin_max_age = static_cast<uint32_t>(max_age_from_expires);
      }
      meta.origin_cc_flags |= AlternateMetadata::kCCOriginHeaderPresent;
    }
  }

  // Set insertion timestamp, adjusted by inbound Age header (D3).
  // Age is delta-seconds (RFC 9111 §1.2.2), the same grammar and the same
  // saturating parse as a Cache-Control lifetime, so it goes through the
  // shared one rather than a second copy of it.
  meta.cache_inserted_at = static_cast<uint32_t>(time(nullptr));
  ngx_str_t inbound_age = ngx_http_pagespeed_get_response_header(r, "Age");
  if (inbound_age.len > 0) {
    std::string_view age_sv(reinterpret_cast<const char*>(inbound_age.data),
                            inbound_age.len);
    uint32_t age_val = pagespeed::ParseCacheControlSeconds(age_sv);
    if (age_val > 0 && age_val <= meta.cache_inserted_at) {
      meta.cache_inserted_at -= age_val;
    }
  }

  std::string_view scheme = ngx_http_pagespeed_scheme(r, conf);

  // Check if the default alternate already exists (prevents duplicate chain
  // entries when multiple concurrent MISS requests write the same URL).
  // Issue #652 review: on a freshness-driven re-fetch the STALE identity is
  // still present (only the worker's later sentinel purge removes the stale
  // variants), so this short-circuit would silently DROP the fresh body —
  // forcing a second origin fetch plus an unoptimized MISS after the purge.
  // origin_refreshed therefore bypasses it and overwrites the identity
  // alternate in place (WriteAlternate supports same-id overwrite; the 304
  // restamp path relies on the same property).
  //
  // A marker DISAGREEMENT also bypasses it.  The short-circuit keeps whatever
  // metadata is already stored, so an entry written while its origin did not
  // negotiate on Accept could never learn that it now does, and — the case
  // that bites — an entry written while it DID could never learn that it has
  // stopped: the serve path then keeps declaring `Accept` and keeps the URL
  // out of optimization on the strength of a header the origin no longer
  // sends, with no path back. This response carries the origin's current
  // headers, so it is exactly the evidence needed to settle it; the identity
  // is overwritten in place, as on the refresh path.
  //
  // The stored entry is READ rather than merely probed for existence: the
  // read answers both questions (is it there, does its marker still match)
  // in one lookup, so this costs no extra round trip over the existence
  // check it replaces.
  bool stored_exists = false;
  bool stored_marker_disagrees = false;
  uint8_t stored_flags = 0;
  {
    auto stored = cache->ReadAlternate(url, hostname, scheme, id);
    if (stored.has_value() && stored->is_valid()) {
      stored_exists = true;
      stored_flags = stored->metadata.flags;
      const bool stored_marked =
          (stored->metadata.flags &
           AlternateMetadata::kFlagOriginVariesAccept) != 0;
      stored_marker_disagrees =
          stored_marked != (ctx->origin_varies_accept != 0);
    }
  }
  if (ctx->sidecar_classified) {
    // Response-header sidecar (issue #208).  The block goes in FIRST and the
    // entry only after it, flagged so a later serve can tell a missing block
    // from "nothing to carry"; the "stored exists, skip the write" shortcut
    // above is additionally bypassed when the stored classification is out
    // of date (pre-gate entry, missing block, changed block, carry-state
    // switch).  Decisions and their rationale:
    // src/nginx/headers_sidecar_record.h.
    const std::vector<pagespeed::HeaderField> origin_views =
        ngx_http_pagespeed_origin_header_views(ctx);
    pagespeed::StoredIdentityState stored_state;
    stored_state.exists = stored_exists;
    stored_state.flags = stored_flags;
    const bool force_rewrite = ctx->origin_refreshed || stored_marker_disagrees;
    const std::string slot_key =
        std::string(url) + "|" + hostname + "|" + std::string(scheme);
    const auto recorded = pagespeed::RecordEntryWithHeadersSidecar(
        *cache, url, hostname, scheme, id, meta, std::as_bytes(std::span(body)),
        origin_views, stored_state, force_rewrite, [&slot_key] {
          // The same per-URL slot the age-expiry refresh coalesces on, with
          // the full window rather than that path's min(window, max-age):
          // while a rewrite holds it, a concurrent genuine expiry of the
          // same URL takes the bounded coalesced stale serve for up to its
          // own grace.  Bounded and harmless; the next request after the
          // window refreshes normally.
          return ngx_http_pagespeed_acquire_refresh_slot(
              slot_key, ngx_time(), kRefreshGraceWindowSecs);
        });
    if (recorded.block_written) {
      pagespeed::RecordHeadersSidecarStored(g_serve_stats);
    }
    switch (recorded.outcome) {
      case pagespeed::SidecarRecordOutcome::kSkipped:
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "pagespeed: alternate exists, skip write for %V",
                       &r->uri);
        break;
      case pagespeed::SidecarRecordOutcome::kWritten:
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "pagespeed: recorded %uz bytes to cache", body.size());
        break;
      case pagespeed::SidecarRecordOutcome::kRemovedAndWritten:
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                      "pagespeed: origin headers for %V changed what a cache "
                      "hit has to carry; removed the stored entries and "
                      "recorded the fresh response",
                      &r->uri);
        break;
      case pagespeed::SidecarRecordOutcome::kNotSlotHolder:
        // Another request in this process is already rewriting this URL's
        // stored headers; recording here as well would only widen the
        // window in which the block is unlinked.
        return;
      case pagespeed::SidecarRecordOutcome::kSidecarWriteFailed:
        pagespeed::RecordHeadersSidecarWriteFailed(g_serve_stats);
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "pagespeed: failed to store the origin headers for %V; "
                      "not caching this response",
                      &r->uri);
        return;
      case pagespeed::SidecarRecordOutcome::kEntryWriteFailed:
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "pagespeed: failed to write original to cache for %V",
                      &r->uri);
        return;
      case pagespeed::SidecarRecordOutcome::kNotEligible:
        // Cannot happen: the header filter classified the same snapshot as
        // eligible.  Fail closed regardless.
        return;
    }
  } else if (!ctx->origin_refreshed && !stored_marker_disagrees &&
             stored_exists) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "pagespeed: alternate exists, skip write for %V", &r->uri);
    // Fall through to notification — worker may still need to process variants.
  } else {
    auto wh =
        cache->WriteAlternate(url, hostname, scheme, id, body.size(), meta);
    if (!wh.has_value()) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                    "pagespeed: failed to write original to cache for %V",
                    &r->uri);
      return;
    }
    auto bytes = std::as_bytes(std::span(body));
    if (!wh->write_sync(bytes).has_value() || !wh->close_sync().has_value()) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                    "pagespeed: cache write_sync/close_sync failed for %V",
                    &r->uri);
      return;
    }
    // Cyclone's RAM tier is write-around (reads populate it, writes do NOT
    // evict): on an origin_refreshed re-record this process read the stale
    // identity before re-fetching (and the SWR-coalesced serves during the
    // re-fetch admit it into the RAM tier via CLFUS), so the overwrite
    // above leaves THIS process serving the pre-refetch body indefinitely
    // (issue #1126, same class as #1125; the worker covers its own RAM tier
    // in HandleOriginRefreshed but cannot reach this process's).  Evict the
    // RAM copy of the just-committed slot so same-process readers observe
    // the fresh bytes.  Post-commit, so a concurrent read cannot repopulate
    // with old bytes before the new document is published.  The eviction is
    // unconditional because it is load-bearing beyond the re-record case:
    // a cross-process purge drops the disk document but cannot reach this
    // process's RAM tier, so a miss-record after such a purge can still
    // hold a pre-purge RAM entry that would otherwise shadow the fresh
    // write.  Only when neither happened is this a no-op.
    cache->EvictAlternateFromRamCache(url, hostname, scheme, id);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "pagespeed: recorded %uz bytes to cache", body.size());
  }

  // Issue #652: this response is a freshness-driven re-fetch of expired
  // origin content (stale verdict full re-fetch, or a conditional
  // revalidation answered 200).  The URL's other alternates — the worker's
  // optimized/compressed variants — are still stale in cache and the
  // worker's dedup set blocks reprocessing.  The fresh body was just
  // written over the identity alternate above; send the origin-refreshed
  // sentinel INSTEAD of a normal notification: the worker purges the stale
  // NON-identity variants (preserving the fresh identity), clears dedup,
  // and rebuilds the variant set inline from the fresh body.  Gated on
  // 2xx so an error re-fetch never purges still-servable variants.
  //
  // Sent for an Accept-negotiating origin too, and that is load-bearing
  // rather than an oversight.  This sentinel is the ONLY thing that purges a
  // URL's non-identity alternates, so withholding it from a URL that was
  // optimized BEFORE its origin started sending `Vary: Accept` would strand
  // that stale variant set forever — and the variants outscore the fresh
  // identity for any client whose encoding they match, so it would be served
  // in preference to it.  The worker splits the two halves: it purges, then
  // declines to rebuild for a marked identity (worker.cc HandleNotification),
  // which is what turns this into the purge-only event that case needs.
  // Every notification below carries this verdict, so the worker runs the
  // work that fetches from the hostname (the agent render, the llms.txt
  // build) only for a hostname this block vouches for.
  const bool host_vouched = ngx_http_pagespeed_host_vouched(r, hostname);
  if (ctx->origin_refreshed && !g_shared_config.socket_path.empty() &&
      r->headers_out.status >= 200 && r->headers_out.status < 300) {
    if (ngx_http_pagespeed_withhold_html_notify(ct, host_vouched)) {
      // No notification for this hostname (see the helper).  Without it
      // nothing purges the URL's older variants, so drop the URL here; the
      // next request records the fresh body again.
      (void)cache->Remove(url, hostname, scheme);
      return;
    }
    CacheNotification refresh;
    refresh.url = std::string(url);
    refresh.hostname = std::string(hostname);
    refresh.scheme = std::string(scheme);
    refresh.content_type = ct;
    refresh.capability_mask = pagespeed::kOriginRefreshedSentinel;
    refresh.host_vouched = host_vouched;
    std::string_view socket_path(g_shared_config.socket_path);
    auto send_result = SendNotificationPersistent(socket_path, refresh);
    if (!send_result.success) {
      ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                    "pagespeed: origin-refreshed notify failed: %s",
                    send_result.error_message.c_str());
    }
    return;
  }

  // Send notification to worker (fire-and-forget).
  // Skip for no-transform: original is cached but must not be optimized.
  // `origin_varies_accept` is never set now (a `Vary: Accept` response is not
  // stored at all); it stays in the condition for the marker's readers.
  // Skip HTML notifications when disable_html is set in shared config.
  // Skip for non-2xx responses: error pages (4xx/5xx) should not trigger
  // worker optimization.  Without this, bots hitting non-existent URLs
  // cause a flood of notifications for cached error pages (see Issue 1
  // in docs/staging-fixes-plan.md).
  bool skip_notification =
      ctx->no_transform || ctx->origin_varies_accept ||
      (ct == ContentType::kHtml && g_shared_config.disable_html) ||
      r->headers_out.status < 200 || r->headers_out.status >= 300 ||
      ngx_http_pagespeed_withhold_html_notify(ct, host_vouched);
  if (!g_shared_config.socket_path.empty() && !skip_notification) {
    CacheNotification notification;
    notification.url = std::string(url);
    notification.hostname = std::string(hostname);
    notification.scheme = std::string(scheme);
    notification.content_type = ct;
    notification.capability_mask = ctx->mask_valid ? ctx->mask.Encode() : 0;
    // Demand-gate the worker's per-URL forced agent render — only
    // an agent request with the operator flag on flags intent (else the worker
    // would render markdown for every browser-touched warm-template URL).
    notification.agent_request =
        ctx->agent_wants_markdown && g_shared_config.agent_optimize_entitled;
    notification.host_vouched = host_vouched;

    std::string_view socket_path(g_shared_config.socket_path);

    auto send_result = SendNotificationPersistent(socket_path, notification);
    if (!send_result.success) {
      ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                    "pagespeed: failed to notify worker: %s",
                    send_result.error_message.c_str());
    }
  }
}

// Restore client's original conditional headers after revalidation.
static void restore_client_conditional_headers(ngx_http_request_t* r,
                                               ngx_http_pagespeed_ctx_t* ctx) {
  r->headers_in.if_none_match = ctx->saved_client_if_none_match;
  r->headers_in.if_modified_since = ctx->saved_client_if_modified_since;
}

// Push a request header added at runtime, filled in as nginx stores parsed
// headers: the key, a lowercase copy of it and the hash of that copy.  Both
// allocations happen before the push, so nothing is pushed on failure.
// Returns the entry, or nullptr.
static ngx_table_elt_t* push_request_header(ngx_http_request_t* r,
                                            const char* name, size_t len,
                                            const u_char* value,
                                            size_t value_len) {
  u_char* lc = static_cast<u_char*>(ngx_pnalloc(r->pool, len));
  u_char* val = static_cast<u_char*>(ngx_pnalloc(r->pool, value_len));
  if (!lc || !val) return nullptr;
  ngx_table_elt_t* h =
      static_cast<ngx_table_elt_t*>(ngx_list_push(&r->headers_in.headers));
  if (!h) return nullptr;
  ngx_memcpy(val, value, value_len);
  h->next = nullptr;
  h->key.data = reinterpret_cast<u_char*>(const_cast<char*>(name));
  h->key.len = len;
  h->lowcase_key = lc;
  h->value.data = val;
  h->value.len = value_len;
  h->hash = pagespeed::LowercaseHeaderKey(std::string_view(name, len), lc);
  return h;
}

// Inject If-None-Match / If-Modified-Since for conditional revalidation.
// Saves client's original conditional headers for later restoration.
static void inject_conditional_headers(ngx_http_request_t* r,
                                       ngx_http_pagespeed_ctx_t* ctx,
                                       const AlternateMetadata& meta) {
  // Save client's original conditional headers. The client's
  // If-None-Match contains PageSpeed's weak ETag (W/"ps-..."),
  // which is in a completely different namespace from the origin's ETag.
  ctx->saved_client_if_none_match = r->headers_in.if_none_match;
  ctx->saved_client_if_modified_since = r->headers_in.if_modified_since;

  // Inject If-None-Match with stored origin ETag.
  if (!meta.origin_etag.empty()) {
    ngx_table_elt_t* h = push_request_header(
        r, "If-None-Match", sizeof("If-None-Match") - 1,
        reinterpret_cast<const u_char*>(meta.origin_etag.data()),
        meta.origin_etag.size());
    if (h) r->headers_in.if_none_match = h;
  }

  // Inject If-Modified-Since (sent alongside If-None-Match as a
  // compatibility measure for origins that only implement IMS).
  if (meta.origin_last_modified > 0) {
    // Format as HTTP-date using nginx's time formatting.
    u_char date[sizeof("Mon, 28 Sep 1970 06:00:00 GMT")];
    u_char* end =
        ngx_http_time(date, static_cast<time_t>(meta.origin_last_modified));
    ngx_table_elt_t* h = push_request_header(
        r, "If-Modified-Since", sizeof("If-Modified-Since") - 1, date,
        static_cast<size_t>(end - date));
    if (h) r->headers_in.if_modified_since = h;
  }
}

// Add a response header with a C string key and value data/length.
// Returns the header element, or nullptr on allocation failure.
static ngx_table_elt_t* add_response_header(ngx_http_request_t* r,
                                            const char* key,
                                            const u_char* val_data,
                                            size_t val_len) {
  ngx_table_elt_t* h =
      static_cast<ngx_table_elt_t*>(ngx_list_push(&r->headers_out.headers));
  if (!h) return nullptr;
  h->hash = 1;
  h->next = nullptr;
  h->key.data = const_cast<u_char*>(reinterpret_cast<const u_char*>(key));
  h->key.len = strlen(key);
  h->value.data = const_cast<u_char*>(val_data);
  h->value.len = val_len;
  return h;
}

// Convenience: add a response header with a C string value.
static ngx_table_elt_t* add_response_header_cstr(ngx_http_request_t* r,
                                                 const char* key,
                                                 const char* val) {
  return add_response_header(r, key, reinterpret_cast<const u_char*>(val),
                             strlen(val));
}

// Emit Vary header so downstream caches and CDNs serve the correct variant
// per client capabilities.
//
// The value itself lives in lib/cache (pagespeed::VaryForServedResponse), for
// the same reason the storability predicate does: it is a property of what
// the optimizer negotiates on, not of nginx, and it now takes a second input
// no per-content-type table can supply.
//
// `entry_varies_accept` is the SERVED ENTRY's persisted
// kFlagOriginVariesAccept — the origin declared it negotiates on Accept
// itself.  On a HIT the origin's headers are gone, so without the stored
// marker that axis is silently dropped from the response we build.  Pass
// false on the MISS path: there the origin's own Vary passes through
// untouched and re-adding Accept would duplicate a list member.
//
// On MISS the origin's response may already include Vary; HTTP allows
// multiple Vary headers which combine as a comma-list (RFC 9110), so adding
// ours is safe even if duplicating tokens like Accept-Encoding.
static void ngx_http_pagespeed_emit_vary(ngx_http_request_t* r, ContentType ct,
                                         bool entry_varies_accept) {
  std::string_view vary_val =
      pagespeed::VaryForServedResponse(ct, entry_varies_accept);
  if (vary_val.empty()) return;
  add_response_header(r, "Vary",
                      reinterpret_cast<const u_char*>(vary_val.data()),
                      vary_val.size());
}

// Set Content-Encoding from a stored variant's capability mask, so a body
// served from a pre-compressed variant is labelled with the encoding it is
// stored in.  Assigning content_encoding also keeps nginx's gzip and brotli
// filters from compressing the body a second time.  Any Content-Encoding line
// already in the response (an upstream error page, a 304) is deactivated
// first.  The agent markdown variant is skipped: its mask (0x7C) is a sentinel
// marker, not a real transfer-encoding, and would decode to a bogus gzip.
static void ngx_http_pagespeed_set_variant_content_encoding(
    ngx_http_request_t* r, uint32_t full_mask) {
  for (ngx_table_elt_t* old = r->headers_out.content_encoding; old;
       old = old->next) {
    old->hash = 0;
  }
  r->headers_out.content_encoding = nullptr;
  if ((full_mask & 0xFF) == static_cast<uint32_t>(SentinelId::kAgentMarkdown)) {
    return;
  }
  auto te = CapabilityMask::Decode(full_mask).transfer_encoding();
  const char* ce_val = nullptr;
  if (te == CapabilityMask::TransferEncoding::kGzip) {
    ce_val = "gzip";
  } else if (te == CapabilityMask::TransferEncoding::kBrotli) {
    ce_val = "br";
  }
  if (ce_val == nullptr) return;
  ngx_table_elt_t* ce =
      static_cast<ngx_table_elt_t*>(ngx_list_push(&r->headers_out.headers));
  if (ce) {
    ce->hash = 1;
    ce->next = nullptr;
    ngx_str_set(&ce->key, "Content-Encoding");
    ce->value.data =
        const_cast<u_char*>(reinterpret_cast<const u_char*>(ce_val));
    ce->value.len = strlen(ce_val);
    r->headers_out.content_encoding = ce;
  }
}

// Issue #652 review (RFC 9111 §4.2.4): may the stashed stale entry be
// served in place of an upstream error?  Requires: the feature enabled,
// no revalidation-required directive on the entry (must-revalidate /
// proxy-revalidate / s-maxage / no-cache forbid serving stale without
// successful validation), and the entry's age within
// effective_max_age + pagespeed_stale_if_error_max_age.
static bool ngx_http_pagespeed_stale_if_error_allowed(
    ngx_http_pagespeed_loc_conf_t* conf, const AlternateMetadata& meta) {
  if (conf->stale_if_error_max_age <= 0) {
    return false;
  }
  if (pagespeed::ComputeSharedRevalidationRequired(meta.origin_cc_flags,
                                                   /*is_shared_cache=*/true)) {
    return false;
  }
  FreshnessConfig fc;
  fc.max_age_cap = static_cast<uint32_t>(conf->max_age_cap);
  fc.immutable_max_age_cap = static_cast<uint32_t>(conf->immutable_max_age_cap);
  fc.html_max_age = static_cast<uint32_t>(conf->html_max_age);
  fc.css_max_age = static_cast<uint32_t>(conf->css_max_age);
  fc.image_max_age = static_cast<uint32_t>(conf->image_max_age);
  FreshnessInput fi;
  fi.now_seconds = static_cast<uint32_t>(ngx_time());
  fi.cache_inserted_at = meta.cache_inserted_at;
  fi.origin_max_age = meta.origin_max_age;
  fi.origin_s_maxage = meta.origin_s_maxage;
  fi.origin_cc_flags = meta.origin_cc_flags;
  fi.content_type = meta.content_type;
  fi.is_shared_cache = true;
  auto fr = EvaluateFreshness(fi, fc);
  uint64_t limit = static_cast<uint64_t>(fr.effective_max_age) +
                   static_cast<uint64_t>(conf->stale_if_error_max_age);
  return static_cast<uint64_t>(fr.age_seconds) <= limit;
}

// Full stale-if-error serve permission for THIS request: the feature's
// age/revalidation window (above) AND the §3.5 stale tightening — once
// stale, an Authorization-bearing request may reuse the entry only when
// it carries `public` (AuthzCacheGateAllowsStale; the same rule the
// SWR coalesced stale serve applies).  Anonymous requests reduce to the
// window check alone.  The gate input is re-derived here (header-filter
// time), not carried from the content handler: the exemption is
// stateless by design and must reflect live enforcement state.
//
// Response-header sidecar (issue #208): a stash that requires stored header
// lines is served only with them.  The handler never stashes such an entry
// without its block, so this is a guard; without the block the upstream
// error passes through (fail closed).
static bool ngx_http_pagespeed_stale_on_error_permitted(
    ngx_http_request_t* r, ngx_http_pagespeed_loc_conf_t* conf,
    const AlternateMetadata& meta) {
  const auto* ctx = static_cast<const ngx_http_pagespeed_ctx_t*>(
      ngx_http_get_module_ctx(r, ngx_http_pagespeed_module));
  const bool have_block = ctx != nullptr && ctx->sidecar_fields != nullptr;
  return ngx_http_pagespeed_stale_if_error_allowed(conf, meta) &&
         pagespeed::StaleServePermitted(
             ngx_http_pagespeed_authz_gate_has_auth(r), meta.origin_cc_flags) &&
         pagespeed::StaleErrorServeHasHeaders(meta.flags, have_block);
}

// Serve the stashed stale entry in place of an upstream error response
// (issue #652 review; RFC 9111 §4.2.4 explicitly permits serving stale
// when the origin cannot be reached).  Mirrors the 304-revalidation serve:
// rewrites the response to 200 with the cached body; the body filter
// delivers the content and swallows the upstream error body.  Takes
// ownership of ctx->stale_read_result.
static ngx_int_t ngx_http_pagespeed_serve_stale_on_error(
    ngx_http_request_t* r, ngx_http_pagespeed_ctx_t* ctx) {
  ReadResult* stale = ctx->stale_read_result;
  ctx->stale_read_result = nullptr;
  auto content = stale->content();
  const auto& smeta = stale->metadata;

  r->headers_out.status = NGX_HTTP_OK;
  // The upstream error response set its own status line; nginx sends a set
  // status_line verbatim, so clear it or the client sees "500" with a 200.
  ngx_str_null(&r->headers_out.status_line);
  r->headers_out.content_length_n = static_cast<off_t>(content.size());

  if (!smeta.origin_content_type.empty()) {
    u_char* ct_buf = static_cast<u_char*>(
        ngx_pnalloc(r->pool, smeta.origin_content_type.size()));
    if (ct_buf) {
      ngx_memcpy(ct_buf, smeta.origin_content_type.data(),
                 smeta.origin_content_type.size());
      r->headers_out.content_type.data = ct_buf;
      r->headers_out.content_type.len = smeta.origin_content_type.size();
      r->headers_out.content_type_len = smeta.origin_content_type.size();
    }
  }

  // Deactivate the upstream error response's Cache-Control headers and
  // emit a defensive no-cache: downstream caches must not store a
  // stale-if-error response as if it were fresh.
  for (ngx_table_elt_t* cc = r->headers_out.cache_control; cc; cc = cc->next) {
    cc->hash = 0;
  }
  r->headers_out.cache_control = nullptr;
  {
    ngx_table_elt_t* cc_h =
        static_cast<ngx_table_elt_t*>(ngx_list_push(&r->headers_out.headers));
    if (cc_h) {
      cc_h->hash = 1;
      cc_h->next = nullptr;
      ngx_str_set(&cc_h->key, "Cache-Control");
      ngx_str_set(&cc_h->value, "no-cache");
      r->headers_out.cache_control = cc_h;
    }
  }

  // The stashed entry may be a pre-compressed variant — emit its
  // Content-Encoding so the client can decode it (and clear any encoding
  // the upstream error page carried).
  ngx_http_pagespeed_set_variant_content_encoding(r, smeta.full_mask);

  // Response-header sidecar (issue #208): for every classified stash the
  // error page's own lines of the names the stored block carries (CSP, ACAO,
  // Vary, ...) are dropped — also when there is nothing to carry, since the
  // origin sent none of them — and any stored lines of the cached
  // representation are replayed in their place, so the error page's policy is
  // never mixed into the stale serve's.  Other error-page lines are left as
  // they were.
  if (pagespeed::StaleErrorDropsCarriedNames(smeta.flags)) {
    for (ngx_list_part_t* part = &r->headers_out.headers.part; part != nullptr;
         part = part->next) {
      auto* h = static_cast<ngx_table_elt_t*>(part->elts);
      for (ngx_uint_t i = 0; i < part->nelts; i++) {
        if (h[i].hash != 0 &&
            pagespeed::IsSidecarCarriedName(std::string_view(
                reinterpret_cast<const char*>(h[i].key.data), h[i].key.len))) {
          h[i].hash = 0;
        }
      }
    }
    ngx_http_pagespeed_replay_sidecar(r, ctx);
  }

  // Built from the stashed entry, so its stored Accept-negotiation marker is
  // the only thing that can put Accept back into Vary here (unless the
  // replayed origin Vary line already lists it).
  ngx_http_pagespeed_emit_vary(
      r, smeta.content_type,
      pagespeed::ComputedVaryListsAccept(
          (smeta.flags & AlternateMetadata::kFlagOriginVariesAccept) != 0,
          ctx->sidecar_carried_vary != 0));
  add_response_header_cstr(r, "X-PageSpeed", "STALE");
  add_response_header_cstr(r, "X-PageSpeed-Stale", "if-error");

  // Transfer ownership: the body filter serves the cached content and
  // discards the upstream error body.
  ctx->read_result = stale;
  ctx->served_from_cache = 1;
  ctx->serve_stale_body = 1;
  ctx->discard_upstream_body = 1;
  ctx->done = 1;

  pagespeed::RecordStaleIfErrorServe(g_serve_stats);
  ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                "pagespeed: upstream error — serving stale entry for %V "
                "(stale-if-error)",
                &r->uri);
  return ngx_http_next_header_filter(r);
}

// Header filter: adds X-PageSpeed: MISS on proxied (non-cached) responses
static ngx_int_t ngx_http_pagespeed_header_filter(ngx_http_request_t* r) {
  // Guard: skip if headers were already sent (avoids "header already sent"
  // crash under high concurrency).
  if (r->header_sent) {
    return ngx_http_next_header_filter(r);
  }

  // No license-status header of any kind is emitted here: the
  // 2.0-era "x-pagespeed-warn: unlicensed" is gone, because the state it
  // asserted no longer exists.

  auto* ctx = static_cast<ngx_http_pagespeed_ctx_t*>(
      ngx_http_get_module_ctx(r, ngx_http_pagespeed_module));

  // Response-header sidecar (issue #208): snapshot the ORIGIN's header lines
  // before this filter adds any of its own below.  Covers the MISS and the
  // 304-revalidation branches; a served-from-cache response has no origin
  // lines to classify.
  if (ctx != nullptr && !ctx->served_from_cache) {
    ngx_http_pagespeed_snapshot_origin_headers(r, ctx);
  }

  // Observe-only: surface the Web Bot Auth verdict computed at
  // classify time.  A label in the transport header map only — never part of
  // the cached artifact, never added to Vary, never a serving decision.
  if (ctx != nullptr && ctx->webbotauth_verdict.len > 0) {
    add_response_header(r, "x-verified-bot", ctx->webbotauth_verdict.data,
                        ctx->webbotauth_verdict.len);
  }

  // Advertise the DPR client hint on pagespeed-handled HTML responses so
  // subsequent subresource requests carry Sec-CH-DPR (explicit density
  // beats UA sniffing in CapabilityMask::FromHeaders).
  if (ctx != nullptr &&
      ngx_http_pagespeed_detect_content_type(r) == ContentType::kHtml) {
    add_response_header_cstr(r, "Accept-CH", "Sec-CH-DPR");
  }

  // --- Conditional revalidation: handle 304 Not Modified ---
  if (ctx && ctx->stale_revalidation &&
      r->headers_out.status == NGX_HTTP_NOT_MODIFIED) {
    ngx_http_pagespeed_loc_conf_t* conf =
        static_cast<ngx_http_pagespeed_loc_conf_t*>(
            ngx_http_get_module_loc_conf(r, ngx_http_pagespeed_module));

    auto* stale = ctx->stale_read_result;
    AlternateMetadata updated_meta = stale->metadata;

    // Reset freshness. Adjust by inbound Age header from the 304
    // response (same logic as initial recording per RFC 9111 D3).
    updated_meta.cache_inserted_at = static_cast<uint32_t>(ngx_time());
    ngx_str_t inbound_age_304 =
        ngx_http_pagespeed_get_response_header(r, "Age");
    if (inbound_age_304.len > 0) {
      std::string_view age_sv(
          reinterpret_cast<const char*>(inbound_age_304.data),
          inbound_age_304.len);
      uint32_t age_val = pagespeed::ParseCacheControlSeconds(age_sv);
      if (age_val > 0 && age_val <= updated_meta.cache_inserted_at) {
        updated_meta.cache_inserted_at -= age_val;
      }
    }

    // Update Cache-Control if 304 carried new directives.
    auto new_cc = ngx_http_pagespeed_parse_cache_control(r);
    if (new_cc.cc_flags & AlternateMetadata::kCCOriginHeaderPresent) {
      updated_meta.origin_max_age = new_cc.max_age;
      updated_meta.origin_s_maxage = new_cc.s_maxage;
      updated_meta.origin_cc_flags = new_cc.cc_flags;
    }

    // Update ETag if 304 carried a new one (RFC 9111 Section 4.3.4).
    if (r->headers_out.etag) {
      std::string_view new_etag(
          reinterpret_cast<const char*>(r->headers_out.etag->value.data),
          r->headers_out.etag->value.len);
      if (new_etag.size() <= 65535) {
        updated_meta.origin_etag = std::string(new_etag);
      }
    }

    // Update Last-Modified if 304 carried a new one.
    if (r->headers_out.last_modified_time > 0) {
      updated_meta.origin_last_modified =
          static_cast<uint32_t>(r->headers_out.last_modified_time);
    }

    // Check Vary consistency (RFC 9111 Section 4.3), and re-decide the
    // Accept-negotiation marker from the same parse.
    //
    // Same shape as the Cache-Control update above: a header the 304 does not
    // carry is UNCHANGED, not cleared (RFC 9111 §4.3.4), so the marker is
    // re-evaluated ONLY when the 304 actually carries a Vary line.  Without
    // this the marker was copied verbatim forever and neither direction could
    // heal on the revalidation path — an origin that starts sending
    // `Vary: Accept` kept getting optimized, and one that stops stayed frozen
    // out of optimization.  That path is on by default, so for most entries
    // it is the one that runs.
    bool saw_vary_304 = false;
    const std::string vary_304 =
        ngx_http_pagespeed_combined_vary(r, &saw_vary_304);
    const pagespeed::VaryStoreVerdict vary_verdict_304 =
        pagespeed::ClassifyVaryForStore(vary_304);
    const bool was_marked =
        (updated_meta.flags & AlternateMetadata::kFlagOriginVariesAccept) != 0;
    const bool now_marked = saw_vary_304 && vary_verdict_304.varies_accept;
    if (saw_vary_304) {
      if (vary_verdict_304.varies_accept) {
        updated_meta.flags |= AlternateMetadata::kFlagOriginVariesAccept;
      } else {
        updated_meta.flags &=
            static_cast<uint8_t>(~AlternateMetadata::kFlagOriginVariesAccept);
      }
    }
    // The origin has just STARTED negotiating on Accept, and what we hold was
    // stored while it did not — so it may be an optimized copy, and for text
    // the optimizer writes that copy over the original itself.  Restamping
    // would keep serving a transformed representation of a resource whose
    // origin now picks the representation, which is the whole harm this
    // change exists to remove; and no amount of metadata fixing brings the
    // origin's bytes back, because they are gone.  Only a fetch restores
    // them, so drop the URL's entries and let the next request take the miss
    // path, which re-records the original and marks it correctly.
    //
    // Costs one origin fetch, once, at the moment an origin changes how it
    // negotiates.  Scoped to the transition (was_marked is false only the
    // first time), so a marked entry revalidates normally forever after.
    if (now_marked && !was_marked) {
      std::string_view cache_path_ev(
          reinterpret_cast<const char*>(conf->cache_path.data),
          conf->cache_path.len);
      std::shared_ptr<PageSpeedCache> ev_cache =
          CheckGenerationAndGetCache(cache_path_ev, r->connection->log);
      if (!ev_cache) {
        ev_cache = GetCache(cache_path_ev, r->connection->log);
      }
      if (ev_cache) {
        std::string ev_url_str = ngx_http_pagespeed_cache_url(r);
        (void)ev_cache->Remove(ev_url_str, ngx_http_pagespeed_cache_hostname(r),
                               ngx_http_pagespeed_scheme(r, conf));
      }
      ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                    "pagespeed: origin for %V now negotiates on Accept — "
                    "dropped the stored (possibly optimized) entries so the "
                    "next request re-records the origin's own representation",
                    &r->uri);
      // Deliberately NOT returning here.  This request keeps going through
      // the normal restamp-and-serve path below, which rewrites the 304 as a
      // 200 carrying the bytes we still hold a borrow on.  Two reasons:
      //
      //   * The conditional request was OURS.  The module injects
      //     If-None-Match / If-Modified-Since on its own initiative when it
      //     revalidates a stale entry, so a client that sent no conditional
      //     at all would receive a bare 304 it cannot interpret — and this
      //     branch fires once per URL for every entry that predates the
      //     marker, i.e. across the whole upgrade population.
      //   * The write-back is already fenced.  Removing the entry above
      //     makes the restamp's existence check report the entry gone, so it
      //     skips the write instead of re-creating what we just dropped.
      //
      // Net: this requester is served the bytes it would have been served
      // anyway, and the next request takes the miss path and re-records the
      // origin's own representation.
      //
      // Those bytes are one response of exactly the harm this change exists
      // to remove — a possibly-transformed representation of a resource whose
      // origin now does its own negotiation — and that is accepted knowingly:
      // it is one response per URL, at the single moment the origin changes,
      // against the alternative of answering with a 304 the client cannot
      // use.
    }
    // A 304 whose Vary makes the entry unstorable (Vary: *, or a header we
    // do not key on) must not extend or rewrite it.  The conditional request
    // was the module's own, though, so a client that sent none must not get
    // the bare 304: fall through and serve the held bytes once as a 200,
    // skip every cache write, and drop the entry once the bytes are copied.
    const bool unstorable_after_304 = !vary_verdict_304.storable;

    // Unconditional generation check (not time-gated like the content
    // handler) — writes to a stale cache instance would be silently lost.
    std::string_view cache_path_sv(
        reinterpret_cast<const char*>(conf->cache_path.data),
        conf->cache_path.len);
    std::shared_ptr<PageSpeedCache> cache =
        CheckGenerationAndGetCache(cache_path_sv, r->connection->log);
    if (!cache) {
      cache = GetCache(cache_path_sv, r->connection->log);
    }
    // Copy content from mmap'd stale entry before any cache write — the
    // mmap source is in the same Cyclone volume, and writes may trigger
    // internal remapping.  Capture size and data from the same content()
    // call to avoid TOCTOU if the mmap is invalidated between calls.
    auto stale_content = stale->content();
    std::string content_copy(
        reinterpret_cast<const char*>(stale_content.data()),
        stale_content.size());
    const off_t stale_content_length = static_cast<off_t>(content_copy.size());

    // Issue #1016: re-evaluate storability over the MERGED (restamped)
    // headers before the write-back.  The store-side checks lived only in
    // the header-filter MISS path, so a 304 carrying no-store/private —
    // or one withdrawing the §3.5 permit an Authorization-bearing exchange
    // needs — re-freshened and persisted an entry this shared cache is no
    // longer allowed to hold.  This change gates RETENTION only; the
    // current requester is still served the validated bytes, which is the
    // pre-existing behavior of this branch and is left unchanged here.
    // (Whether §3.5 should additionally suppress that serve once the
    // permit is withdrawn is an open product question, not settled by this
    // commit.)  Rationale for the evict/skip split:
    // src/nginx/revalidation_storability.h.
    pagespeed::RestampStorabilityInputs storability_in;
    storability_in.refreshed_cc_flags = updated_meta.origin_cc_flags;
    storability_in.request_has_authorization =
        ngx_http_pagespeed_authz_gate_has_auth(r);
    storability_in.response_has_set_cookie =
        ngx_http_pagespeed_get_response_header(r, "Set-Cookie").len > 0;
    // The stashed entry passed the HIT-side cookie gate; this keeps a 304
    // that withdraws what the cookie rule needs (cookie_cache_gate.h) from
    // extending it for a cookie-bearing exchange.
    storability_in.refreshed_s_maxage = updated_meta.origin_s_maxage;
    storability_in.refreshed_max_age = updated_meta.origin_max_age;
    storability_in.request_has_cookie =
        ngx_http_pagespeed_request_has_cookie(r);
    storability_in.entry_class =
        (stale->metadata.full_mask & 0xFF) ==
                static_cast<uint32_t>(SentinelId::kAgentMarkdown)
            ? pagespeed::CookieShareClass::kPage
            : pagespeed::StoredEntryCookieClass(
                  stale->metadata.origin_content_type,
                  stale->metadata.content_type);
    pagespeed::RestampVerdict storability_verdict =
        pagespeed::EvaluateRestampStorability(storability_in);
    if (unstorable_after_304) {
      storability_verdict = pagespeed::RestampVerdict::kSkipRestamp;
    }

    // Response-header sidecar: for an entry recorded with
    // the gate, merge the stored block with the 304's lines (RFC 9111
    // §4.3.4) and re-classify.  Same block → restamp only; same carry state,
    // different block → rewrite the block FIRST, then restamp; a refusal or
    // a carry-state switch → drop the URL (the 304 restamps only the stashed
    // variant, and its siblings would keep the old bits) and serve this
    // response; the next request re-records.  Entries recorded before the
    // gate restamp exactly as before.  Plan: headers_sidecar_record.h.
    // `stored_block` owns the bytes the plan's stored views point into; the
    // 304 views point into the pool-owned header list.
    std::string stored_block;
    std::vector<pagespeed::HeaderField> views_304;
    pagespeed::RestampSidecarPlan sidecar_plan;

    if (cache) {
      std::string url_str = ngx_http_pagespeed_cache_url(r);
      std::string_view url(url_str);
      std::string hostname = ngx_http_pagespeed_cache_hostname(r);
      std::string_view scheme = ngx_http_pagespeed_scheme(r, conf);
      AlternateId id = MaskToAlternateId(
          static_cast<uint8_t>(updated_meta.full_mask & 0xFF));

      bool sidecar_removed = false;
      const uint8_t stash_flags = stale->metadata.flags;
      if ((stash_flags & AlternateMetadata::kFlagHeadersClassified) != 0) {
        std::optional<std::string_view> stored_payload;
        if ((stash_flags & AlternateMetadata::kFlagHeadersSidecarRequired) !=
            0) {
          auto block = cache->ReadHeadersSidecar(url, hostname, scheme);
          if (block.has_value() && block->is_valid()) {
            stored_block =
                std::string(PageSpeedCache::HeadersSidecarPayload(*block));
            stored_payload = stored_block;
          }
        }
        views_304 = ngx_http_pagespeed_origin_header_views(ctx);
        sidecar_plan = pagespeed::PlanRestampSidecar(stash_flags,
                                                     stored_payload, views_304);
        switch (sidecar_plan.action) {
          case pagespeed::RestampSidecarAction::kRemove:
            // Whole key, like the Accept-transition eviction above.  The
            // restamp below is skipped: there is nothing left to restamp.
            (void)cache->Remove(url, hostname, scheme);
            sidecar_removed = true;
            ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                          "pagespeed: 304 for %V changed what a cache hit has "
                          "to carry (or its stored headers are gone) — "
                          "dropped the stored entries; the next request "
                          "re-records",
                          &r->uri);
            break;
          case pagespeed::RestampSidecarAction::kRewriteThenRestamp:
            if (storability_verdict == pagespeed::RestampVerdict::kRestamp) {
              auto written = cache->WriteHeadersSidecar(url, hostname, scheme,
                                                        sidecar_plan.merged);
              if (written.has_value() && written->stored) {
                pagespeed::RecordHeadersSidecarStored(g_serve_stats);
              } else {
                // Without the new block the restamp would extend an entry
                // whose stored headers are stale: skip it, the entry simply
                // revalidates again next time.
                pagespeed::RecordHeadersSidecarWriteFailed(g_serve_stats);
                ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                              "pagespeed: failed to store the refreshed "
                              "origin headers for %V; not restamping",
                              &r->uri);
                storability_verdict = pagespeed::RestampVerdict::kSkipRestamp;
              }
            }
            break;
          case pagespeed::RestampSidecarAction::kLegacy:
          case pagespeed::RestampSidecarAction::kRestampOnly:
            break;
        }
      }

      // Carry out the verdict.  The dispatch itself lives in
      // revalidation_storability.h so it can be exercised against a real
      // cache in tests — the eviction actually firing, and the skip
      // actually not writing, have no other seam and previously went
      // unasserted.  Logging stays here, where the request context is.
      //
      // Purge fence (issue #652), inside the dispatch: this write-back
      // copies the STALE alternate's bytes and restamps cache_inserted_at
      // — an nginx-process write that would silently undo a purge racing
      // the in-flight conditional revalidation (and the restamp would
      // defeat the worker's generation fence for all subsequent derived
      // writes).  The purge's Remove() makes the alternate vanish from the
      // shared directory, so "entry gone => skip the write-back" is a
      // one-lookup cross-process fence.  The next request takes the MISS
      // path and re-fetches fresh content.  A purge landing between that
      // check and close_sync is still possible (no cross-process write
      // fence exists in Cyclone today); the window is one syscall wide and
      // a true 304 means origin attests the bytes are current, so the
      // residual risk is confined to upstream caches answering 304 against
      // stale validators.
      //
      // The eviction is safe against the borrow this request is still
      // serving from.  The current requester is served DIRECTLY from the
      // mmap (ctx->read_result -> content() -> b->memory = 1), not from
      // content_copy, for the whole client-paced drain — so this must not
      // invalidate a live borrow, and it does not: Volume::remove_sync
      // performs no physical reclamation.  It clears one directory entry
      // under the bucket writer lock; it never toggles phase, never moves
      // write_pos, never records a wrap, never overwrites bytes.
      // Reclamation in Cyclone is positional (circular-buffer wrap), and
      // THAT path is lease-gated.  The unmap inside remove_sync is
      // a no-op under the persistent whole-file mapping this module runs
      // with.
      //
      // A URL the sidecar plan just dropped has nothing left to restamp.
      if (unstorable_after_304) {
        // Drop the entry (no new write); the next request takes the miss
        // path.  The borrow this request serves from stays valid: removal
        // clears a directory entry and reclaims nothing.
        if (!sidecar_removed) {
          (void)cache->Remove(url, hostname, scheme);
        }
        ngx_http_pagespeed_notify_url_dropped(r, url, hostname, scheme,
                                              updated_meta.content_type);
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                      "pagespeed: 304 for %V is no longer storable (Vary) - "
                      "served the held bytes once and dropped the entry",
                      &r->uri);
      } else if (!sidecar_removed) {
        const auto action = pagespeed::ApplyRestampVerdict(
            *cache, storability_verdict, url, hostname, scheme, id,
            updated_meta, std::as_bytes(std::span(content_copy)));

        switch (action) {
          case pagespeed::RestampAction::kEvicted:
            ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                          "pagespeed: 304 for %V is no longer storable in a "
                          "shared cache (no-store/private) — evicted instead "
                          "of restamped",
                          &r->uri);
            break;
          case pagespeed::RestampAction::kEvictFailed:
            // Never discard this silently: with stripe affinity enabled a
            // NotOwned stripe would turn the eviction into a no-op, and a
            // security fix that quietly stops firing is worse than one that
            // fails loudly.
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "pagespeed: FAILED to evict non-storable entry for "
                          "%V after 304 — a response marked no-store/private "
                          "may remain cached",
                          &r->uri);
            break;
          case pagespeed::RestampAction::kSkipped:
            // Still storable, but this exchange may not extend its life
            // (RFC 9111 §3.5 permit withdrawn, or the 304 set a cookie).
            // Self-correcting: the entry revalidates again next request, and
            // it cannot be used to flush another client's cached entry.
            ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                          "pagespeed: skipping 304 restamp for %V — refreshed "
                          "headers do not permit this request to extend the "
                          "shared entry",
                          &r->uri);
            break;
          case pagespeed::RestampAction::kEntryGone:
            ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                          "pagespeed: skipping 304 restamp for %V — entry "
                          "purged during revalidation",
                          &r->uri);
            break;
          case pagespeed::RestampAction::kWrittenBack:
          case pagespeed::RestampAction::kWriteFailed:
            break;
        }
      }
    }

    // Restore the client's conditional headers BEFORE passing to
    // ngx_http_next_header_filter: the not_modified filter then compares
    // them against the weak tag set above, which is the same tag a cache
    // hit of this entry carries, so a client holding the hit copy gets its
    // 304.
    restore_client_conditional_headers(r, ctx);

    // Rewrite response as 200 with cached content.
    r->headers_out.status = NGX_HTTP_OK;
    // Same as the stale-if-error rewrite: the 304 set "304 Not Modified" as
    // the status line, which nginx would send verbatim over the 200 (and
    // then treat as bodyless).  Clear it so the line follows the code and
    // the not-modified filter answers a matching conditional client.
    ngx_str_null(&r->headers_out.status_line);
    // The upstream module turns the not-modified filter off for responses it
    // does not cache itself.  This one is served from our own entry, so a
    // client whose If-None-Match matches the ETag below must get its 304.
    r->disable_not_modified = 0;
    r->headers_out.content_length_n = stale_content_length;

    // The stored body may be a pre-compressed variant; label it the way the
    // HIT path does, or the client receives encoded bytes as identity.
    ngx_http_pagespeed_set_variant_content_encoding(r, updated_meta.full_mask);

    // The ETag of this 200 is the one a cache HIT of the same entry carries
    // (FormatHitETag over the restamped metadata and the served length), not
    // the origin's validator from the 304: otherwise the same bytes go out
    // under two tags and a client holding the HIT copy never matches here.
    // When there is no tag (no content identity), the origin's validator is
    // dropped and none is sent, as on the HIT path.
    {
      u_char* etag_buf = static_cast<u_char*>(
          ngx_pnalloc(r->pool, pagespeed::kHitETagBufSize));
      size_t etag_len = 0;
      if (etag_buf != nullptr) {
        etag_len = pagespeed::FormatHitETag(
            updated_meta, static_cast<size_t>(stale_content_length),
            reinterpret_cast<char*>(etag_buf), pagespeed::kHitETagBufSize);
      }
      if (etag_len > 0) {
        ngx_table_elt_t* etag_h = r->headers_out.etag;
        if (etag_h == nullptr) {
          etag_h = static_cast<ngx_table_elt_t*>(
              ngx_list_push(&r->headers_out.headers));
          if (etag_h != nullptr) {
            etag_h->hash = 1;
            etag_h->next = nullptr;
            ngx_str_set(&etag_h->key, "ETag");
          }
        }
        if (etag_h != nullptr) {
          etag_h->hash = 1;
          etag_h->value.data = etag_buf;
          etag_h->value.len = etag_len;
          r->headers_out.etag = etag_h;
        }
      } else if (r->headers_out.etag != nullptr) {
        r->headers_out.etag->hash = 0;
        r->headers_out.etag = nullptr;
      }
    }

    // Reuse cached content-type from metadata.
    const auto& smeta = stale->metadata;
    if (!smeta.origin_content_type.empty()) {
      u_char* ct_buf = static_cast<u_char*>(
          ngx_pnalloc(r->pool, smeta.origin_content_type.size()));
      if (ct_buf) {
        ngx_memcpy(ct_buf, smeta.origin_content_type.data(),
                   smeta.origin_content_type.size());
        r->headers_out.content_type.data = ct_buf;
        r->headers_out.content_type.len = smeta.origin_content_type.size();
        r->headers_out.content_type_len = smeta.origin_content_type.size();
      }
    }

    // Deactivate upstream 304 Cache-Control headers before emitting ours.
    // Without this, both upstream's and our CC headers appear in the response.
    for (ngx_table_elt_t* cc = r->headers_out.cache_control; cc;
         cc = cc->next) {
      cc->hash = 0;
    }
    r->headers_out.cache_control = nullptr;

    // Emit Cache-Control for the client from the updated metadata, through
    // the SAME canonical builder the HIT path uses (issue #1016).  This
    // branch previously hand-rolled a "simplified CC" (max-age plus an
    // optional must-revalidate), which silently dropped no-cache,
    // no-transform and proxy-revalidate, and — once storability is
    // re-checked — would have replaced a no-store/private with a plain
    // freshness lifetime, inviting the next cache in the chain to store
    // exactly what this one just evicted.  Routing through
    // BuildCacheControlHeader forwards the full directive set and removes
    // a third divergent emitter from the module.
    if (updated_meta.origin_cc_flags &
        AlternateMetadata::kCCOriginHeaderPresent) {
      CacheControlInput cci;
      if (conf->cache_mode == NGX_CONF_UNSET_UINT) {
        cci.mode = (g_shared_config.cache_mode == "aggressive")
                       ? CacheMode::kAggressive
                       : CacheMode::kSafe;
      } else {
        cci.mode = static_cast<CacheMode>(conf->cache_mode);
      }
      cci.origin_cc_flags = updated_meta.origin_cc_flags;
      // A revalidated entry is freshly stamped, so the origin's own
      // lifetime IS the effective one — no age to subtract.
      cci.effective_max_age = (updated_meta.origin_cc_flags &
                               AlternateMetadata::kCCOriginSMaxagePresent)
                                  ? updated_meta.origin_s_maxage
                                  : updated_meta.origin_max_age;
      cci.origin_max_age = updated_meta.origin_max_age;
      cci.synthesize_swr = conf->synthesize_swr;
      cci.is_shared_cache = true;  // nginx is always a shared cache.
      cci.content_type = updated_meta.content_type;
      // ALWAYS relay the origin's no-cache, on every 304 outcome.  This
      // branch previously had a dedicated no-cache case; folding it into
      // the eviction predicate would strip `no-cache` from every
      // `no-cache, max-age=N` response — the origin demands revalidation
      // before each reuse and the client would instead reuse for max-age.
      cci.relay_origin_no_cache = true;
      // Forward the STORAGE restrictions only on the responses whose entry
      // we just evicted, so the directive that drove the eviction also
      // reaches every cache downstream of us.
      cci.forward_origin_restrictions =
          pagespeed::RefreshedHeadersForbidSharedStorage(
              updated_meta.origin_cc_flags);

      static constexpr size_t kCCBufSize = 256;
      u_char* cc_buf = static_cast<u_char*>(ngx_pnalloc(r->pool, kCCBufSize));
      if (cc_buf) {
        auto cc_result = BuildCacheControlHeader(
            cci, reinterpret_cast<char*>(cc_buf), kCCBufSize);
        // Push only once there is something to emit: ngx_list_push does NOT
        // zero the slot, so appending first and bailing on an empty build
        // would leave an uninitialized entry (garbage hash/key/value) in
        // the list, and the downstream filter emits anything with a
        // non-zero hash.
        if (cc_result.len > 0) {
          ngx_table_elt_t* cc_h = static_cast<ngx_table_elt_t*>(
              ngx_list_push(&r->headers_out.headers));
          if (cc_h) {
            cc_h->hash = 1;
            cc_h->next = nullptr;
            ngx_str_set(&cc_h->key, "Cache-Control");
            cc_h->value.data = cc_buf;
            cc_h->value.len = cc_result.len;
            r->headers_out.cache_control = cc_h;
          }
        }
      }
    }

    // Response-header sidecar: the 304's own lines are already in
    // headers_out and stay; the stored lines whose names the 304 did not
    // carry are added, so this 200 carries the same request-independent
    // headers as the response the entry was recorded from.  Entries recorded
    // before the gate have no stored lines and add nothing.
    bool replayed_vary = false;
    for (const auto& field : sidecar_plan.stored_not_overridden) {
      auto* key = static_cast<u_char*>(ngx_pnalloc(r->pool, field.name.size()));
      auto* value = static_cast<u_char*>(ngx_pnalloc(
          r->pool, field.value.size() > 0 ? field.value.size() : 1));
      if (key == nullptr || value == nullptr) {
        break;
      }
      ngx_memcpy(key, field.name.data(), field.name.size());
      ngx_memcpy(value, field.value.data(), field.value.size());
      ngx_table_elt_t* h =
          static_cast<ngx_table_elt_t*>(ngx_list_push(&r->headers_out.headers));
      if (h == nullptr) {
        break;
      }
      h->hash = 1;
      h->next = nullptr;
      h->key.data = key;
      h->key.len = field.name.size();
      h->value.data = value;
      h->value.len = field.value.size();
      if (pagespeed::HeaderNameEquals(field.name, "vary")) {
        replayed_vary = true;
      }
    }

    // Emit Vary so downstream CDNs key on the right dimensions.  This 304 is
    // rewritten as a 200 carrying the cached bytes, so it is a served-from-
    // entry response: updated_meta is a copy of the stored metadata (flags
    // included), which is what carries the origin's Accept negotiation.
    // When the origin's own Vary line was just added from the stored block,
    // it already lists Accept, so the computed line does not repeat it (the
    // MISS shape).
    ngx_http_pagespeed_emit_vary(
        r, updated_meta.content_type,
        !replayed_vary && (updated_meta.flags &
                           AlternateMetadata::kFlagOriginVariesAccept) != 0);

    // Add X-PageSpeed: REVALIDATED header.
    {
      add_response_header_cstr(r, "X-PageSpeed", "REVALIDATED");
    }
    {
      ngx_table_elt_t* h =
          static_cast<ngx_table_elt_t*>(ngx_list_push(&r->headers_out.headers));
      if (h) {
        h->hash = 1;
        h->next = nullptr;
        ngx_str_set(&h->key, "X-PageSpeed-Revalidation");
        ngx_str_set(&h->value, "304");
      }
    }

    // Transfer ownership: body filter will serve the cached content.
    ctx->read_result = stale;
    ctx->stale_read_result = nullptr;
    ctx->stale_revalidation = 0;
    ctx->served_from_cache = 1;
    ctx->serve_stale_body = 1;
    ctx->done = 1;

    g_conditional_304s.fetch_add(1, std::memory_order_relaxed);

    return ngx_http_next_header_filter(r);
  }

  // --- Conditional revalidation: handle 200 (content changed) ---
  if (ctx && ctx->stale_revalidation && r->headers_out.status >= 200 &&
      r->headers_out.status < 300) {
    // Free stale entry — content has changed.
    delete ctx->stale_read_result;
    ctx->stale_read_result = nullptr;
    ctx->stale_revalidation = 0;
    // Origin attests the content changed while the URL's other stale
    // alternates remain in cache — flag for the origin-refreshed sentinel
    // so the worker purges + rebuilds the variant set (issue #652).
    ctx->origin_refreshed = 1;
    // Restore client conditional headers.
    restore_client_conditional_headers(r, ctx);
    // Add revalidation debugging header.
    {
      ngx_table_elt_t* h =
          static_cast<ngx_table_elt_t*>(ngx_list_push(&r->headers_out.headers));
      if (h) {
        h->hash = 1;
        h->next = nullptr;
        ngx_str_set(&h->key, "X-PageSpeed-Revalidation");
        ngx_str_set(&h->value, "200");
      }
    }
    g_conditional_200s.fetch_add(1, std::memory_order_relaxed);
    // Fall through to normal MISS handling below.
  }

  // --- Conditional revalidation: error from upstream (5xx, etc.) ---
  if (ctx && ctx->stale_revalidation) {
    restore_client_conditional_headers(r, ctx);
    ctx->stale_revalidation = 0;
    // Stale-if-error (issue #652 review, RFC 9111 §4.2.4): a 5xx answer
    // to the conditional re-fetch means the origin is erroring, not that
    // the content changed — serve the stashed stale entry (bounded by the
    // stale-if-error window) instead of propagating the outage.  Refused
    // (window exceeded, revalidation required, or §3.5 stale tightening
    // for an Authorization-bearing request) → the stash is dropped and
    // the upstream error propagates unchanged.
    if (r->headers_out.status >= NGX_HTTP_INTERNAL_SERVER_ERROR &&
        ctx->stale_read_result != nullptr) {
      ngx_http_pagespeed_loc_conf_t* sie_conf =
          static_cast<ngx_http_pagespeed_loc_conf_t*>(
              ngx_http_get_module_loc_conf(r, ngx_http_pagespeed_module));
      if (ngx_http_pagespeed_stale_on_error_permitted(
              r, sie_conf, ctx->stale_read_result->metadata)) {
        return ngx_http_pagespeed_serve_stale_on_error(r, ctx);
      }
    }
    delete ctx->stale_read_result;
    ctx->stale_read_result = nullptr;
  }

  // --- Stale-if-error for the full re-fetch path (issue #652 review) ---
  // The handler stashed the age-expired entry before re-fetching.  On a
  // 5xx upstream answer serve it (RFC 9111 §4.2.4) — pre-#652, stale
  // content without a revalidation directive was served without ever
  // contacting origin, so an origin outage was invisible for cached
  // pages; retiring kStaleServe must not turn that into a site-wide 5xx
  // within one TTL.  On success (or a non-5xx answer) drop the stash and
  // continue with normal MISS handling/recording.
  if (ctx && ctx->stale_full_refetch) {
    ctx->stale_full_refetch = 0;
    if (r->headers_out.status >= NGX_HTTP_INTERNAL_SERVER_ERROR &&
        ctx->stale_read_result != nullptr) {
      ngx_http_pagespeed_loc_conf_t* sie_conf =
          static_cast<ngx_http_pagespeed_loc_conf_t*>(
              ngx_http_get_module_loc_conf(r, ngx_http_pagespeed_module));
      if (ngx_http_pagespeed_stale_on_error_permitted(
              r, sie_conf, ctx->stale_read_result->metadata)) {
        return ngx_http_pagespeed_serve_stale_on_error(r, ctx);
      }
    }
    delete ctx->stale_read_result;
    ctx->stale_read_result = nullptr;
  }

  if (ctx && !ctx->served_from_cache && r->headers_out.status >= 200 &&
      r->headers_out.status < 300) {
    add_response_header_cstr(r, "X-PageSpeed", "MISS");

    // Parse Cache-Control and Vary to decide whether to record/optimize.
    // IMPORTANT: Check origin's Vary BEFORE emitting our own Vary header,
    // otherwise our User-Agent token poisons the uncacheable check and
    // causes nginx to incorrectly mark every cacheable response as
    // uncacheable (all responses MISS, cache never populates).
    auto parsed_cc = ngx_http_pagespeed_parse_cache_control(r);
    if (parsed_cc.cc_flags & (kCCNoStore | kCCPrivate)) {
      ctx->uncacheable = 1;
    }
    // RFC 9111 §3.5: the response to a request that carried Authorization
    // (module-validated RSL-CAP License credentials exempt) must not enter
    // the shared cache unless the response's Cache-Control explicitly
    // permits it (public / must-revalidate / s-maxage).  uncacheable also
    // keeps the worker un-notified: the body filter never starts
    // recording, so record_response (write + notify) is unreachable.
    if (!pagespeed::AuthzCacheGateAllows(
            ngx_http_pagespeed_authz_gate_has_auth(r), parsed_cc.cc_flags)) {
      ctx->uncacheable = 1;
    }
    // The cookie rule (src/nginx/cookie_cache_gate.h): for a request that
    // carried a cookie, a page enters the shared cache only when the origin
    // marked it public or gave it an s-maxage above zero, and a static asset
    // only with a lifetime the origin stated.  The Markdown copy is rendered
    // from the stored page, so it is covered by the same refusal.  Same
    // mechanism as the §3.5 gate above: uncacheable keeps the worker
    // un-notified.  The lifetime is read as the record path stores it:
    // Expires counts only when no Cache-Control line was sent.
    if (ngx_http_pagespeed_request_has_cookie(r)) {
      uint16_t cookie_cc_flags = parsed_cc.cc_flags;
      uint32_t cookie_max_age = parsed_cc.max_age;
      if ((cookie_cc_flags & AlternateMetadata::kCCOriginHeaderPresent) == 0) {
        int64_t from_expires = 0;
        if (ngx_http_pagespeed_expires_lifetime(r, &from_expires)) {
          cookie_cc_flags |= AlternateMetadata::kCCOriginHeaderPresent;
          cookie_max_age = static_cast<uint32_t>(
              std::min<int64_t>(from_expires, UINT32_MAX));
        }
      }
      const ngx_str_t& ct = r->headers_out.content_type;
      if (!pagespeed::CookieCacheGateAllowsStore(
              true,
              std::string_view(reinterpret_cast<const char*>(ct.data), ct.len),
              cookie_cc_flags, cookie_max_age, parsed_cc.s_maxage)) {
        ctx->uncacheable = 1;
      }
    }
    if (parsed_cc.cc_flags & kCCNoTransform) {
      ctx->no_transform = 1;
    }
    // Responses with Set-Cookie are implicitly private — caching them could
    // leak session cookies or serve personalized content to other users.
    if (ngx_http_pagespeed_get_response_header(r, "Set-Cookie").len > 0) {
      ctx->uncacheable = 1;
    }
    // The origin's Vary (lib/cache/vary_storability.h): a response that
    // varies on any request header other than Accept-Encoding is not stored,
    // and whatever the URL already has stored is dropped, so each client gets
    // the origin's answer to its own request.  `varies_accept` is now always
    // false; the branch stays so the marker keeps one writer.
    const pagespeed::VaryStoreVerdict vary_verdict =
        pagespeed::ClassifyVaryForStore(ngx_http_pagespeed_combined_vary(r));
    if (!vary_verdict.storable) {
      ctx->uncacheable = 1;
      ngx_http_pagespeed_drop_on_vary_refusal(r);
    }
    if (vary_verdict.varies_accept) {
      ctx->origin_varies_accept = 1;
    }

    // Emit Vary on MISS so CDNs key on the right dimensions.
    // Must be after the Vary uncacheable check above.
    //
    // false: on MISS the origin's response — including its own Vary — passes
    // through to the client untouched, so the marker would only duplicate a
    // list member the client is already receiving.
    ngx_http_pagespeed_emit_vary(r, ngx_http_pagespeed_detect_content_type(r),
                                 /*entry_varies_accept=*/false);
    if (r->method == NGX_HTTP_HEAD) {
      ctx->head_request = 1;
    }

    // Guard: reject pre-compressed upstream responses.  PageSpeed
    // caches raw (uncompressed) bytes and applies encoding at serve
    // time.  If the origin sends Content-Encoding (e.g. gzip), the
    // cached bytes would be compressed, corrupting all downstream
    // processing (HTML parse, CSS minify, image transcode).
    // Fix: ensure nginx.conf has `proxy_set_header Accept-Encoding "";`
    // in every proxy_pass location.
    if (r->headers_out.content_encoding) {
      ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                    "pagespeed: upstream sent Content-Encoding: %V — skipping "
                    "cache recording. Add 'proxy_set_header Accept-Encoding "
                    "\"\";' to your proxy_pass location to fix this.",
                    &r->headers_out.content_encoding->value);
      ctx->uncacheable = 1;
    }

    // Response-header sidecar gate (issue #208), last of the store gates:
    // a response another gate already refused is neither re-classified nor
    // counted again.  Only a 200 that will actually be recorded (the body
    // filter records nothing else) is classified.
    if (!ctx->uncacheable && !ctx->head_request &&
        r->headers_out.status == NGX_HTTP_OK) {
      ngx_http_pagespeed_classify_origin_headers(r, ctx);
    }
  }
  return ngx_http_next_header_filter(r);
}

// Body filter: captures upstream response body for cache recording
static ngx_int_t ngx_http_pagespeed_body_filter(ngx_http_request_t* r,
                                                ngx_chain_t* in) {
  ngx_http_pagespeed_loc_conf_t* conf;
  ngx_http_pagespeed_ctx_t* ctx;

  // Get location config
  conf = static_cast<ngx_http_pagespeed_loc_conf_t*>(
      ngx_http_get_module_loc_conf(r, ngx_http_pagespeed_module));

  if (!conf->enable) {
    return ngx_http_next_body_filter(r, in);
  }

  // Only process main requests (not subrequests)
  if (r != r->main) {
    return ngx_http_next_body_filter(r, in);
  }

  // Get request context
  ctx = static_cast<ngx_http_pagespeed_ctx_t*>(
      ngx_http_get_module_ctx(r, ngx_http_pagespeed_module));

  if (ctx == nullptr) {
    return ngx_http_next_body_filter(r, in);
  }

  // Discard upstream body chunks after a stale-if-error rewrite (issue
  // #652 review): the header filter replaced an upstream 5xx with the
  // cached stale body, so the upstream error-page bytes must never reach
  // the client.  Mark every incoming buffer consumed.  (The 304 path
  // never needed this — a 304 has no body.)
  if (ctx->discard_upstream_body) {
    for (ngx_chain_t* cl = in; cl != nullptr; cl = cl->next) {
      cl->buf->pos = cl->buf->last;
      cl->buf->file_pos = cl->buf->file_last;
      cl->buf->sync = 1;
    }
    in = nullptr;
    if (!ctx->serve_stale_body) {
      // Cached body already delivered (last_buf sent) — swallow trailing
      // upstream chunks entirely.
      return NGX_OK;
    }
  }

  // Serve cached content after 304 conditional revalidation or a
  // stale-if-error rewrite.  The header filter set serve_stale_body; we
  // deliver the cached body here instead of the upstream body.
  if (ctx->serve_stale_body) {
    ctx->serve_stale_body = 0;
    auto cached_data = ctx->read_result->content();

    // Pool-allocate chain (never stack-allocate — if
    // ngx_http_next_body_filter returns NGX_AGAIN, nginx's write
    // event handler will reference the chain later).
    ngx_buf_t* b = static_cast<ngx_buf_t*>(ngx_calloc_buf(r->pool));
    ngx_chain_t* out =
        static_cast<ngx_chain_t*>(ngx_palloc(r->pool, sizeof(ngx_chain_t)));
    if (b == nullptr || out == nullptr) {
      return NGX_ERROR;
    }
    b->last_buf = 1;
    b->last_in_chain = 1;
    out->buf = b;
    out->next = nullptr;

    // Emit-time barrier: strict-renew the borrow in the same stack as
    // the send it feeds, then keep aliasing / copy out / abort.  The must_copy
    // gate (h2/h3 shadow bufs, Range, in-memory filters, subrequests — the
    // subrequest term is vacuous here, this filter already passes subrequests
    // through above) forces the copy path whenever downstream could retain its
    // own reference into the mmap: those references neither advance our buf's
    // cursor nor die with our copy-out release.
    const bool stale_must_copy = ps_zerocopy_must_copy(r);
    switch (stale_must_copy ? pagespeed::ps_barrier::BarrierAction::kCopyOut
                            : ps_zerocopy_emit_barrier(ctx->read_result)) {
      case pagespeed::ps_barrier::BarrierAction::kAbort:
        pagespeed::RecordZerocopyTornAbort(g_serve_stats);
        ngx_log_error(
            NGX_LOG_WARN, r->connection->log, 0,
            "pagespeed: stale serve torn at emit (cache entry recycled), "
            "aborting %V",
            &r->uri);
        return NGX_ERROR;
      case pagespeed::ps_barrier::BarrierAction::kCopyOut:
        if (!ps_zerocopy_copy_out(r, ctx->read_result, b)) {
          return NGX_ERROR;  // torn during copy-then-verify: fail closed.
        }
        if (!stale_must_copy) {
          // Count only wrap-pressure copy-outs; a gate-forced copy is normal
          // operation on h2/h3/filtered serves, not a lease event.
          pagespeed::RecordZerocopyProactiveCopyout(g_serve_stats);
        }
        break;
      case pagespeed::ps_barrier::BarrierAction::kAlias:
        b->pos = const_cast<u_char*>(
            reinterpret_cast<const u_char*>(cached_data.data()));
        b->last = b->pos + cached_data.size();
        b->memory = 1;
        ctx->aliased_buf = b;  // timer re-checks this borrow while it drains.
        ngx_http_pagespeed_ensure_lease_renew_timer(r, ctx);
        break;
    }

    return ngx_http_next_body_filter(r, out);
  }

  if (ctx->done || ctx->served_from_cache) {
    return ngx_http_next_body_filter(r, in);
  }

  // Skip if the connection has errored out (client disconnect, etc.).
  if (r->connection->error) {
    return ngx_http_next_body_filter(r, in);
  }

  // Skip recording for uncacheable responses and HEAD requests.
  if (ctx->uncacheable || ctx->head_request) {
    return ngx_http_next_body_filter(r, in);
  }

  // Only record 200 OK responses.  Other 2xx codes (204 No Content,
  // 206 Partial) have different semantics that we can't preserve on
  // the HIT path (which always serves as 200).
  if (r->headers_out.status != NGX_HTTP_OK) {
    return ngx_http_next_body_filter(r, in);
  }

  // Start recording if not already
  if (!ctx->recording) {
    ContentType ct = ngx_http_pagespeed_detect_content_type(r);
    // Only record content types we can optimize
    if (ct == ContentType::kOther) {
      return ngx_http_next_body_filter(r, in);
    }
    ctx->recording = 1;
    ctx->buffered_body = nullptr;
    ctx->buffered_last = &ctx->buffered_body;
  }

  // Buffer the body chain (copy into pool-allocated buffers).
  // Buffers can be memory-backed (b->pos/last) or file-backed
  // (b->file, b->file_pos/file_last) when the response exceeds
  // nginx's proxy_buffers limit and overflows to a temp file.
  // We must handle both to avoid silently dropping the tail of
  // large responses (and missing the last_buf sentinel).
  for (ngx_chain_t* cl = in; cl; cl = cl->next) {
    ngx_buf_t* b = cl->buf;

    // Determine data source and size.
    const unsigned char* src = nullptr;
    size_t size = 0;
    unsigned char* file_data = nullptr;

    if (b->in_file && b->file && b->file_last > b->file_pos) {
      // File-backed buffer: read from the temp file into a
      // pool-allocated block so we can cache the full response.
      size = static_cast<size_t>(b->file_last - b->file_pos);
      file_data = static_cast<unsigned char*>(ngx_pnalloc(r->pool, size));
      if (file_data == nullptr) {
        ctx->recording = 0;
        ctx->done = 1;
        return ngx_http_next_body_filter(r, in);
      }
      ssize_t n = ngx_read_file(b->file, file_data, static_cast<size_t>(size),
                                b->file_pos);
      if (n != static_cast<ssize_t>(size)) {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "pagespeed: failed to read %uz bytes from "
                      "temp file for %V (got %z)",
                      size, &r->uri, n);
        ctx->recording = 0;
        ctx->done = 1;
        return ngx_http_next_body_filter(r, in);
      }
      src = file_data;
    } else if (!b->in_file) {
      size = static_cast<size_t>(b->last - b->pos);
      src = b->pos;
    }
    // else: file buffer with no valid range — skip data but still
    // check last_buf below.

    if (size > 0) {
      // Check recording size limit to prevent unbounded allocation.
      if (ctx->recorded_bytes + size > kMaxRecordingSize) {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "pagespeed: response body exceeds %uz byte "
                      "recording limit, stopping cache recording "
                      "for %V",
                      kMaxRecordingSize, &r->uri);
        ctx->recording = 0;
        ctx->done = 1;
        return ngx_http_next_body_filter(r, in);
      }
      ctx->recorded_bytes += size;
      // Allocate a new chain link and buffer
      ngx_chain_t* new_cl =
          static_cast<ngx_chain_t*>(ngx_palloc(r->pool, sizeof(ngx_chain_t)));
      ngx_buf_t* new_buf = static_cast<ngx_buf_t*>(ngx_calloc_buf(r->pool));

      if (new_cl == nullptr || new_buf == nullptr) {
        ctx->recording = 0;
        ctx->done = 1;
        return ngx_http_next_body_filter(r, in);
      }

      unsigned char* data;
      if (file_data != nullptr) {
        // Already allocated and read above.
        data = file_data;
      } else {
        data = static_cast<unsigned char*>(ngx_pnalloc(r->pool, size));
        if (data == nullptr) {
          ctx->recording = 0;
          ctx->done = 1;
          return ngx_http_next_body_filter(r, in);
        }
        ngx_memcpy(data, src, size);
      }

      new_buf->pos = data;
      new_buf->last = data + size;
      new_buf->memory = 1;

      new_cl->buf = new_buf;
      new_cl->next = nullptr;

      *ctx->buffered_last = new_cl;
      ctx->buffered_last = &new_cl->next;
    }

    // Check if this is the final buffer of the entire response.
    // Only last_buf marks the true end — last_in_chain fires on
    // intermediate chain segments when nginx delivers large
    // responses in multiple body filter calls.
    if (b->last_buf) {
      ctx->done = 1;
      ngx_http_pagespeed_record_response(r, ctx);
      break;
    }
  }

  // Pass the original chain through unchanged
  return ngx_http_next_body_filter(r, in);
}

// =================================================================
// Web Bot Auth opt-in counter endpoint (experimental)
// =================================================================
// Builds the machine-readable counter doc served at
// /.well-known/webbotauth-counter.  Rendered under a lock into a short-lived
// per-kind cache; the handler copies the bytes into the request pool so the
// served body outlives the handler.  ZERO request data is reflected into the
// doc — it is derived solely from the shared serve-stats mmap and the operator
// verified-bot registry.  No version/build identifiers appear anywhere.

namespace {

// JSON-escape operator-controlled free text (signer names, keyids): escape the
// two structural characters (" and \), ALSO escape < and > as </> so
// a `</script>` in an operator name can never break out of a <script> block if
// this doc is ever embedded in HTML (stored-XSS defense — names are operator
// free text), DROP control characters entirely, and cap the input length on a
// UTF-8 codepoint boundary (a mid-multibyte cut would emit invalid JSON that a
// malicious operator could weaponize as denial-of-parsing downstream).
// Non-ASCII bytes otherwise pass through unchanged (valid UTF-8 in a JSON
// string needs no escaping).
std::string CounterJsonEscape(std::string_view in, size_t max_len) {
  size_t n = in.size() > max_len ? max_len : in.size();
  // If we truncated in the middle of a UTF-8 multibyte sequence, back off to
  // the sequence's lead byte so we never emit a partial codepoint.
  if (n < in.size()) {
    while (n > 0 && (static_cast<unsigned char>(in[n]) & 0xC0) == 0x80) {
      --n;
    }
  }
  std::string out;
  out.reserve(n + 8);
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = static_cast<unsigned char>(in[i]);
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '<') {
      out += "\\u003c";
    } else if (c == '>') {
      out += "\\u003e";
    } else if (c < 0x20 || c == 0x7f) {
      // Drop control characters (including CR/LF/TAB) outright.
      continue;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

// Render a 16-byte boot identity as a canonical RFC-4122 UUID string.
std::string CounterFormatUuid(const uint8_t id[16]) {
  static const char* kHex = "0123456789abcdef";
  std::string s;
  s.reserve(36);
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
    s += kHex[(id[i] >> 4) & 0x0F];
    s += kHex[id[i] & 0x0F];
  }
  return s;
}

// 64-bit hash → 16 lowercase hex chars (reproducible identifier for a verified
// signer whose keyid is not in the operator registry — the engine stores only
// the unsalted FNV-1a-64 of the keyid, never the raw keyid).  The hash is
// deliberately reproducible so a downstream consumer can recognise a known
// keyid at a third-party origin where that keyid is not registered.
std::string CounterFormatHashHex(uint64_t h) {
  static const char* kHex = "0123456789abcdef";
  std::string s;
  s.reserve(16);
  for (int shift = 60; shift >= 0; shift -= 4) {
    s += kHex[(h >> shift) & 0x0F];
  }
  return s;
}

// Days since the Unix epoch → "YYYY-MM-DD" (UTC, Hinnant civil-from-days).
std::string CounterFormatSince(uint64_t unix_day) {
  int64_t z = static_cast<int64_t>(unix_day) + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  uint64_t doe = static_cast<uint64_t>(z - era * 146097);
  uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = static_cast<int64_t>(yoe) + era * 400;
  uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint64_t mp = (5 * doy + 2) / 153;
  uint64_t d = doy - (153 * mp + 2) / 5 + 1;
  uint64_t m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04lld-%02llu-%02llu",
                static_cast<long long>(y), static_cast<unsigned long long>(m),
                static_cast<unsigned long long>(d));
  return std::string(buf);
}

// Coarse count tier (~2 significant figures) for the public/unauthenticated
// doc.  Fixed, monotone buckets — the exact cumulative value is never exposed
// to an unauthenticated caller.
const char* CounterCoarseTier(uint64_t n) {
  if (n == 0) return "0";
  if (n < 100) return "1-99";
  if (n < 1000) return "100-999";
  if (n < 10000) return "1k-10k";
  if (n < 100000) return "10k-100k";
  if (n < 1000000) return "100k-1M";
  return "1M+";
}

inline uint64_t CounterLoad(uint64_t& f) {
  return std::atomic_ref<uint64_t>(f).load(std::memory_order_relaxed);
}

// Build the counter doc.  `exact` selects the token-gated EXACT doc vs the
// public COARSE doc.  The two docs differ deliberately, to minimise
// fingerprinting on the unauthenticated surface:
//   EXACT : {since, boot_id, raw totals, by_signer:[{kid,name?,count}] for
//            EVERY verified signer (registered + hash-only), other_verified_bots}
//   COARSE: {tiered totals, by_signer:[{name,count-tier}] for REGISTERED
//            signers ONLY, other_verified_bots tier}  — no `since` (instance-age
//            fingerprint), no boot_id, no hash-only entries, no exact distinct-
//            signer count, no raw cumulative.
// Verify latency is NEVER on either doc (first-party /v1/metrics only).
std::string BuildCounterDoc(bool exact) {
  pagespeed::ServeStats* s = g_serve_stats;

  uint64_t verified = 0, invalid = 0, other = 0, other_bots = 0, since_day = 0;
  uint8_t boot[16] = {0};
  struct Sig {
    uint64_t hash;
    uint64_t count;
  };
  std::vector<Sig> sigs;
  if (s != nullptr) {
    verified = CounterLoad(s->webbotauth_signed_verified);
    invalid = CounterLoad(s->webbotauth_signed_invalid);
    other = CounterLoad(s->webbotauth_other_signature);
    other_bots = CounterLoad(s->webbotauth_other_verified_bots);
    since_day = s->counting_since_unix_day;  // instance identity, set at create
    std::memcpy(boot, s->boot_id, sizeof(boot));
    for (auto& slot : s->webbotauth_signers) {
      uint64_t h = CounterLoad(slot.kid_hash);
      if (h != 0) sigs.push_back({h, CounterLoad(slot.count)});
    }
  }

  std::string doc;
  doc.reserve(256);
  doc += "{";
  bool first_field = true;
  auto sep = [&]() {
    if (!first_field) doc += ",";
    first_field = false;
  };
  if (exact) {
    // `since` and `boot_id` are exact-doc only (both are instance fingerprints).
    sep();
    doc += "\"since\":\"";
    doc += CounterFormatSince(since_day);
    doc += "\"";
    sep();
    doc += "\"boot_id\":\"";
    doc += CounterFormatUuid(boot);
    doc += "\"";
  }
  auto count_field = [&](const char* key, uint64_t n) {
    sep();
    doc += "\"";
    doc += key;
    doc += "\":";
    if (exact) {
      doc += std::to_string(n);
    } else {
      doc += "\"";
      doc += CounterCoarseTier(n);
      doc += "\"";
    }
  };
  count_field("verified_total", verified);
  count_field("invalid_total", invalid);
  count_field("other_total", other);
  sep();
  doc += "\"by_signer\":[";
  bool first_sig = true;
  for (const auto& sig : sigs) {
    std::string kid, name;
    g_webbotauth_registry.ForEach([&](std::string_view k, std::string_view nm) {
      if (pagespeed::HashWebBotAuthKeyid(k) == sig.hash) {
        kid.assign(k);
        name.assign(nm);
      }
    });
    const bool registered = !kid.empty();
    if (!exact && !registered) {
      // COARSE doc lists REGISTERED signers only (drop hash-only entries; do
      // not reveal the exact distinct-signer count).
      continue;
    }
    if (!registered) {
      // EXACT doc: verified signer not in the operator registry — identify by
      // the reproducible (unsalted FNV-1a-64) keyid hash.
      kid = "hash:" + CounterFormatHashHex(sig.hash);
    }
    if (!first_sig) doc += ",";
    first_sig = false;
    doc += "{";
    if (exact) {
      doc += "\"kid\":\"";
      doc += CounterJsonEscape(kid, 128);
      doc += "\"";
      if (!name.empty()) {
        doc += ",\"name\":\"";
        doc += CounterJsonEscape(name, 64);
        doc += "\"";
      }
      doc += ",\"count\":";
      doc += std::to_string(sig.count);
    } else {
      // COARSE: name → tier only (no kid/hash, no raw count).
      doc += "\"name\":\"";
      doc += CounterJsonEscape(name, 64);
      doc += "\",\"count\":\"";
      doc += CounterCoarseTier(sig.count);
      doc += "\"";
    }
    doc += "}";
  }
  doc += "]";
  count_field("other_verified_bots", other_bots);
  doc += "}";
  return doc;
}

std::mutex g_counter_render_mutex;
struct CounterRenderCache {
  std::string body;
  time_t rendered_at = 0;
  bool valid = false;
};
CounterRenderCache g_counter_exact_cache;
CounterRenderCache g_counter_coarse_cache;

// Render the requested doc into a short-lived per-kind cache under a lock, then
// copy it out.  Bounds render cost under load; the returned copy is then placed
// in the request pool by the handler so the served bytes outlive the request.
void RenderCounterDoc(bool exact, std::string* out) {
  std::lock_guard<std::mutex> lk(g_counter_render_mutex);
  CounterRenderCache& c =
      exact ? g_counter_exact_cache : g_counter_coarse_cache;
  time_t now = ngx_time();
  if (!c.valid || now - c.rendered_at >= 1) {
    c.body = BuildCounterDoc(exact);
    c.rendered_at = now;
    c.valid = true;
  }
  *out = c.body;
}

}  // namespace

// Main request handler (PRECONTENT phase, after the access checks)
// Is the page recorded under `key` (the default-mask entry the record path
// writes) marked shareable for a cookie-bearing request?  For what the cache
// derives from that page and stores without Cache-Control of its own: the
// Markdown copy and the Early Hints set.  No recorded page: false (fail
// closed).
static bool ngx_http_pagespeed_recorded_page_shared(
    PageSpeedCache& cache, const cyclone::CacheKey& key) {
  const AlternateId recorded_id =
      MaskToAlternateId(static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));
  auto recorded = cache.ReadAlternateByKey(key, recorded_id);
  if (!recorded.has_value() || !recorded->is_valid()) {
    return false;
  }
  return pagespeed::CookieCacheGateAllowsServe(
      true, pagespeed::CookieShareClass::kPage,
      recorded->metadata.origin_cc_flags, recorded->metadata.origin_s_maxage);
}

static ngx_int_t ngx_http_pagespeed_handler(ngx_http_request_t* r) {
  ngx_http_pagespeed_loc_conf_t* conf;
  ngx_http_pagespeed_ctx_t* ctx;
  ngx_int_t rc;

  // Get location config
  conf = static_cast<ngx_http_pagespeed_loc_conf_t*>(
      ngx_http_get_module_loc_conf(r, ngx_http_pagespeed_module));

  // Check if enabled
  if (!conf->enable) {
    return NGX_DECLINED;
  }

  // Only handle main requests (not subrequests like auth_request)
  if (r != r->main) {
    return NGX_DECLINED;
  }

  // Only handle GET and HEAD
  if (!(r->method & (NGX_HTTP_GET | NGX_HTTP_HEAD))) {
    return NGX_DECLINED;
  }

  // CSP-safe async-CSS loader: serve the shared loader JS at its reserved
  // same-origin path. Fully static (a compile-time constant) — it needs no
  // cache, no cache key, no license, and must NOT be subject to url-disallow,
  // a missing cache path, or a transient cache-open failure (the deferred HTML
  // it backs may already be cached upstream). So it is served HERE, before all
  // of those gates — matching the in-process .NET middleware, which serves it
  // before its own exclude/method gates ("always reachable when enabled").
  // Match the raw decoded request path (nginx percent-decodes/collapses
  // r->uri; the path carries no query string), mirroring the .NET Request.Path
  // match. Same single source of truth (pagespeed::kAsyncCssLoaderJs) the .NET
  // middleware serves, so both front-ends emit byte-identical content. ctx is
  // not yet created here, but both the header and body filters guard ctx==NULL,
  // so finalizing now is safe.
  {
    std::string_view req_path(reinterpret_cast<const char*>(r->uri.data),
                              r->uri.len);
    if (req_path == pagespeed::kAsyncCssLoaderPath) {
      std::string_view body = pagespeed::kAsyncCssLoaderJs;
      r->headers_out.status = NGX_HTTP_OK;
      r->headers_out.content_length_n = static_cast<off_t>(body.size());
      ngx_str_t js_ct;
      ngx_str_set(&js_ct, "text/javascript; charset=utf-8");
      r->headers_out.content_type = js_ct;
      r->headers_out.content_type_len = js_ct.len;
      add_response_header_cstr(r, "X-Content-Type-Options", "nosniff");
      // Provenance marker so a deploy smoke can assert the loader was served by
      // this module (not a 200 from elsewhere), matching the .NET X-PageSpeed.
      add_response_header_cstr(r, "X-PageSpeed", "async-css-loader");
      ngx_table_elt_t* cc =
          static_cast<ngx_table_elt_t*>(ngx_list_push(&r->headers_out.headers));
      if (cc != nullptr) {
        cc->hash = 1;
        cc->next = nullptr;
        ngx_str_set(&cc->key, "Cache-Control");
        // The path is CONTENT-ADDRESSED (kAsyncCssLoaderPath embeds an FNV-1a
        // hash of the loader body), so a future loader change yields a NEW path
        // and can never be masked by a stale cache. That makes immutable+1yr
        // correct-by-construction here, and makes prod's generic
        // `location ~* \.(js)$ { expires 1y; }` correct for this path too.
        ngx_str_set(&cc->value, "public, max-age=31536000, immutable");
        r->headers_out.cache_control = cc;
      }
      ngx_int_t lrc = ngx_http_send_header(r);
      if (lrc == NGX_ERROR || lrc > NGX_OK || r->header_only) {
        ngx_http_finalize_request(r, lrc);
        return NGX_DONE;
      }
      ngx_buf_t* b = static_cast<ngx_buf_t*>(ngx_calloc_buf(r->pool));
      ngx_chain_t* out =
          static_cast<ngx_chain_t*>(ngx_palloc(r->pool, sizeof(ngx_chain_t)));
      if (b == nullptr || out == nullptr) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return NGX_DONE;
      }
      b->pos =
          const_cast<u_char*>(reinterpret_cast<const u_char*>(body.data()));
      b->last = b->pos + body.size();
      b->memory = 1;
      b->last_buf = 1;
      b->last_in_chain = 1;
      out->buf = b;
      out->next = nullptr;
      lrc = ngx_http_output_filter(r, out);
      ngx_http_finalize_request(r, lrc);
      return NGX_DONE;
    }
  }

  // Web Bot Auth opt-in counter endpoint (experimental).  Served
  // HERE — after the main-request + GET/HEAD guards above — before the cache /
  // license / url-disallow gates, like the async-CSS loader.  GET/HEAD only is
  // already enforced (HEAD auto-honored via r->header_only).  ZERO request data
  // is reflected; the doc is derived only from the shared serve-stats and the
  // operator registry.  No version/build identifiers in the doc or headers.
  {
    std::string_view req_path(reinterpret_cast<const char*>(r->uri.data),
                              r->uri.len);
    if (req_path == "/.well-known/webbotauth-counter") {
      const std::string& mode = g_shared_config.web_bot_auth_public_counter;
      if (mode == "off") {
        // Endpoint invisible: fall through to normal handling (404).
        return NGX_DECLINED;
      }
      // Inline bearer-token check against the env-configured secret.  The
      // "Bearer " prefix and the length are non-secret (early-exit is fine),
      // but the token bytes are compared in constant time to avoid a timing
      // oracle on the secret.
      static ngx_str_t authorization_name = ngx_string("Authorization");
      ngx_str_t auth = ngx_http_pagespeed_get_header(r, authorization_name);
      bool token_ok = false;
      if (!g_web_bot_auth_counter_token.empty() && auth.len > 0) {
        std::string_view av(reinterpret_cast<const char*>(auth.data), auth.len);
        static constexpr std::string_view kBearer = "Bearer ";
        const size_t tok_len = g_web_bot_auth_counter_token.size();
        if (av.size() == kBearer.size() + tok_len &&
            av.substr(0, kBearer.size()) == kBearer) {
          // Constant-time compare over the equal-length token bytes.
          const char* a = av.data() + kBearer.size();
          const char* b = g_web_bot_auth_counter_token.data();
          unsigned char diff = 0;
          for (size_t i = 0; i < tok_len; ++i) {
            diff |= static_cast<unsigned char>(a[i] ^ b[i]);
          }
          token_ok = (diff == 0);
        }
      }
      bool exact;
      if (token_ok) {
        exact = true;  // valid token (any non-off mode) → exact doc
      } else if (mode == "public") {
        exact = false;  // public + no/invalid token → coarse doc
      } else {
        // private + no/invalid token → 404 (only surface B — the counter's
        // existence stays hidden from unauthenticated callers).
        return NGX_DECLINED;
      }

      std::string doc;
      RenderCounterDoc(exact, &doc);

      r->headers_out.status = NGX_HTTP_OK;
      r->headers_out.content_length_n = static_cast<off_t>(doc.size());
      ngx_str_t json_ct;
      ngx_str_set(&json_ct, "application/json");
      r->headers_out.content_type = json_ct;
      r->headers_out.content_type_len = json_ct.len;
      // Never let a browser sniff this JSON as HTML (stored-XSS defense in
      // depth, matching the sibling async-CSS handler).
      add_response_header_cstr(r, "X-Content-Type-Options", "nosniff");
      // Cache-Control via the dedicated slot: no-store for the exact doc,
      // short public TTL for the coarse doc.  Literals only (nginx stores the
      // pointer without copying).
      ngx_table_elt_t* cc =
          static_cast<ngx_table_elt_t*>(ngx_list_push(&r->headers_out.headers));
      if (cc != nullptr) {
        cc->hash = 1;
        cc->next = nullptr;
        ngx_str_set(&cc->key, "Cache-Control");
        if (exact) {
          ngx_str_set(&cc->value, "no-store");
        } else {
          ngx_str_set(&cc->value, "public, max-age=300");
        }
        r->headers_out.cache_control = cc;
      }
      // The coarse doc is public+cacheable but its content depends on whether a
      // valid token was presented (a token flips it to the exact doc), so a
      // shared cache MUST key on Authorization to avoid serving a stale coarse
      // entry to a token-bearing request.
      if (!exact) {
        add_response_header_cstr(r, "Vary", "Authorization");
      }

      ngx_int_t lrc = ngx_http_send_header(r);
      if (lrc == NGX_ERROR || lrc > NGX_OK || r->header_only) {
        // HEAD (r->header_only) sends headers only — no body.
        ngx_http_finalize_request(r, lrc);
        return NGX_DONE;
      }
      // Copy the rendered bytes into the request pool: nginx may defer output
      // (NGX_AGAIN) and re-read the buffer after this handler returns, so the
      // served bytes MUST outlive the handler (the static render cache is
      // under a lock we do not hold past this point).
      u_char* buf = static_cast<u_char*>(ngx_pnalloc(r->pool, doc.size()));
      ngx_buf_t* b = static_cast<ngx_buf_t*>(ngx_calloc_buf(r->pool));
      ngx_chain_t* out =
          static_cast<ngx_chain_t*>(ngx_palloc(r->pool, sizeof(ngx_chain_t)));
      if (buf == nullptr || b == nullptr || out == nullptr) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return NGX_DONE;
      }
      if (!doc.empty()) {
        ngx_memcpy(buf, doc.data(), doc.size());
      }
      b->pos = buf;
      b->last = buf + doc.size();
      b->memory = 1;
      b->last_buf = 1;
      b->last_in_chain = 1;
      out->buf = b;
      out->next = nullptr;
      lrc = ngx_http_output_filter(r, out);
      ngx_http_finalize_request(r, lrc);
      return NGX_DONE;
    }
  }

  // Check URL disallow patterns
  if (ngx_http_pagespeed_url_disallowed(conf, &r->uri)) {
    return NGX_DECLINED;
  }

  // Get or create request context.  Done up front so that the header filter
  // can add X-PageSpeed: MISS on every evaluated request — the MISS header
  // lets clients and tests observe that the module is loaded and the request
  // was evaluated.
  ctx = static_cast<ngx_http_pagespeed_ctx_t*>(
      ngx_http_get_module_ctx(r, ngx_http_pagespeed_module));
  if (ctx == nullptr) {
    ctx = static_cast<ngx_http_pagespeed_ctx_t*>(
        ngx_pcalloc(r->pool, sizeof(ngx_http_pagespeed_ctx_t)));
    if (ctx == nullptr) {
      return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    // ngx_pcalloc zeroes the struct, but 0 is a VALID fd (stdin).  Set the
    // sendfile-FD sentinel explicitly so cleanup never closes fd 0.
    ctx->sendfile_fd = NGX_INVALID_FILE;
    ngx_http_set_ctx(r, ctx, ngx_http_pagespeed_module);

    // Register cleanup handler
    ngx_http_cleanup_t* cln = ngx_http_cleanup_add(r, 0);
    if (cln == nullptr) {
      return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    cln->handler = ngx_http_pagespeed_cleanup;
    cln->data = ctx;
  }

  // Thread safety note for the g_shared_config reads below: they are done
  // without a mutex, which is safe because nginx worker processes are
  // single-threaded.  The write path (RefreshSharedConfigIfChanged) also runs
  // on this thread.

  std::string_view cache_path_sv(
      reinterpret_cast<const char*>(conf->cache_path.data),
      conf->cache_path.len);

  // Periodic generation check: detect worker cache purge/reset.
  // Time-based: checks at most once per kGenerationCheckIntervalSec second(s)
  // using nginx's cached time (zero-cost read).  This ensures even low-traffic
  // sites detect a purge within ~1s, unlike the old request-count approach
  // which could leave the stale mmap in use for 100+ requests.
  // Also checks the shared config file mtime so that runtime config changes
  // (e.g., PATCH /v1/config) are picked up within ~1s.
  // Runs BEFORE classify so a worker-published web_bot_auth toggle or
  // key-store update takes effect for THIS request's classification, not the
  // next one (on a cold box the worker publishes keys a few
  // seconds after nginx starts; with the old classify-then-poll order every
  // first signed request in that window verified against an empty store).
  // The poll itself is shared with the PREACCESS RSL-CAP handler (see
  // MaybePollSharedState): when enforcement already polled this interval,
  // this call is a no-op and `cache` stays null (GetCache below covers it).
  std::shared_ptr<PageSpeedCache> cache =
      MaybePollSharedState(cache_path_sv, r->connection->log);

  // Classify request and build cache key
  rc = ngx_http_pagespeed_classify(r, ctx);
  if (rc != NGX_OK) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "pagespeed: failed to classify request");
    return NGX_DECLINED;
  }

  ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                 "pagespeed: classified request, mask=%uD, uri=%V",
                 ctx->mask.Encode(), &r->uri);

  // Detect client force-refresh (Ctrl+F5).
  ctx->force_refresh = ngx_http_pagespeed_is_force_refresh(r) ? 1 : 0;

  // --- Cache lookup via Cyclone native alternates ---

  // Need a cache path to do lookups
  if (conf->cache_path.len == 0) {
    return NGX_DECLINED;
  }

  // Cache-directory generation mismatch with the optimizer (logged once when
  // detected, not per request): pass through uncached.  uncacheable keeps the
  // header/body filters from recording the response or notifying the
  // optimizer, exactly like a no-store response.  The poll above re-reads
  // the shared config, so this lifts itself once the generations match.
  if (!g_cache_dir_gen.cache_enabled()) {
    ctx->uncacheable = 1;
    return NGX_DECLINED;
  }

  if (!cache) {
    cache = GetCache(cache_path_sv, r->connection->log);
  }
  if (!cache) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "pagespeed: failed to open cache at %V", &conf->cache_path);
    return NGX_DECLINED;
  }

  std::string url_str = ngx_http_pagespeed_cache_url(r);
  std::string_view url(url_str);
  std::string hostname = ngx_http_pagespeed_cache_hostname(r);
  std::string_view scheme = ngx_http_pagespeed_scheme(r, conf);

  // Pre-compose the cache key once.  Hostname is already normalized by
  // ngx_http_pagespeed_cache_hostname(), so use the pre-normalized path
  // to avoid redundant NormalizeHostname() inside ComposeKey().
  auto cache_key =
      PageSpeedCache::ComposeKeyPreNormalized(url, hostname, scheme);

  // Synthesized /llms.txt site-index. Self-contained — serves the
  // worker-produced kLlmsTxt sentinel by exact id and returns, never entering
  // the generic variant-negotiation/serve path below. ZERO egress on this path
  // (read mmap + write out); the (re)build happens worker-side off this thread.
  if (url == "/llms.txt") {
    if (!g_shared_config.agent_optimize_llms_txt_enabled) {
      return NGX_DECLINED;  // off: origin serves its own (or 404)
    }
    // Built from, and served for, only a hostname this block vouches for: the
    // build fetches from the hostname, so any other name is left to the
    // origin, including an index stored under it by an earlier version.
    const bool llms_host_vouched = ngx_http_pagespeed_host_vouched(r, hostname);
    if (!llms_host_vouched) {
      return NGX_DECLINED;
    }
    // The serve below reuses stored-response data (the kLlmsTxt sentinel),
    // so it takes the same §3.5 gate as any other serve.  Sentinels store
    // no Cache-Control metadata, so there is no permit signal — the gate
    // is consulted with flags 0 and fails closed for every
    // Authorization-bearing request.  Validated License agents (the
    // feature's actual consumers) are exempt via
    // ngx_http_pagespeed_authz_gate_has_auth() and keep full behavior;
    // internal requests carrying a License header are correctly refused
    // (the d40f5f18 exemption semantics, reused not reimplemented).
    if (!pagespeed::AuthzCacheGateAllows(
            ngx_http_pagespeed_authz_gate_has_auth(r),
            /*origin_cc_flags=*/0)) {
      return NGX_DECLINED;  // §3.5: pass through; origin serves /llms.txt
    }
    // The index is built from the stored pages' Markdown copies, and a
    // sentinel carries no sharing mark, so under the cookie rule
    // (cookie_cache_gate.h) a cookie-bearing request is never served it:
    // pass through, the origin serves its own /llms.txt (or 404).
    if (!pagespeed::CookieCacheGateAllowsServe(
            ngx_http_pagespeed_request_has_cookie(r),
            pagespeed::CookieShareClass::kPage, /*origin_cc_flags=*/0,
            /*origin_s_maxage=*/0)) {
      return NGX_DECLINED;
    }
    auto llms = cache->ReadAlternateByKey(
        cache_key, static_cast<AlternateId>(SentinelId::kLlmsTxt));
    const bool hit = llms.has_value() && llms->is_valid();

    // Decide whether to trigger a lazy (re)build purely from the kLlmsTxtMeta
    // next-refresh epoch — for BOTH a hit and a miss. The worker stamps this
    // epoch on every build attempt (success or failure), so a cold/stale site
    // with an unreachable sitemap re-notifies at most once per backoff window,
    // not once per request (abuse control). Absent/short meta => (re)build.
    bool notify_build = true;
    {
      auto fresh = cache->ReadAlternateByKey(
          cache_key, static_cast<AlternateId>(SentinelId::kLlmsTxtMeta));
      if (fresh.has_value() && fresh->is_valid() &&
          fresh->content().size() >= AlternateMetadata::kHashSize + 8) {
        auto mc = fresh->content();
        const auto* p = reinterpret_cast<const unsigned char*>(mc.data());
        int64_t next_refresh = 0;
        for (int i = 0; i < 8; ++i) {
          next_refresh =
              (next_refresh << 8) | p[AlternateMetadata::kHashSize + i];
        }
        notify_build = (ngx_time() >= next_refresh);
      }
    }
    if (notify_build && !g_shared_config.socket_path.empty()) {
      CacheNotification n;
      n.url = std::string(url);
      n.hostname = std::string(hostname);
      n.scheme = std::string(scheme);
      n.capability_mask = pagespeed::kLlmsTxtSentinel;
      n.content_type =
          ContentType::kHtml;  // unused by the builder; must be valid
      n.host_vouched = llms_host_vouched;
      std::string_view socket_path(g_shared_config.socket_path);
      (void)SendNotificationPersistent(socket_path, n);
    }
    if (!hit) {
      return NGX_DECLINED;  // miss: origin handles now; the build populates next
    }

    // Hit — serve the markdown verbatim with noindex/private headers.
    auto content = llms->content();
    std::string_view body(reinterpret_cast<const char*>(content.data()),
                          content.size());
    ctx->read_result = new ReadResult(std::move(*llms));
    ctx->served_from_cache = 1;
    ctx->cache_anchor = new std::shared_ptr<PageSpeedCache>(cache);
    // The lease-renewal timer is armed at emit, ONLY on the kAlias
    // outcome — a copy-out releases the borrow and an abort finalizes, so
    // neither has anything to renew.

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = static_cast<off_t>(body.size());
    ngx_str_t md_ct;
    ngx_str_set(&md_ct, "text/markdown; charset=utf-8");
    r->headers_out.content_type = md_ct;
    r->headers_out.content_type_len = md_ct.len;
    add_response_header_cstr(r, "X-Robots-Tag", "noindex");
    ngx_table_elt_t* cc =
        static_cast<ngx_table_elt_t*>(ngx_list_push(&r->headers_out.headers));
    if (cc != nullptr) {
      cc->hash = 1;
      cc->next = nullptr;
      ngx_str_set(&cc->key, "Cache-Control");
      ngx_str_set(&cc->value, "private");
      r->headers_out.cache_control = cc;
    }

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
      ngx_http_finalize_request(r, rc);
      return NGX_DONE;
    }
    ngx_buf_t* b = static_cast<ngx_buf_t*>(ngx_calloc_buf(r->pool));
    ngx_chain_t* out =
        static_cast<ngx_chain_t*>(ngx_palloc(r->pool, sizeof(ngx_chain_t)));
    if (b == nullptr || out == nullptr) {
      ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
      return NGX_DONE;
    }
    b->last_buf = 1;
    b->last_in_chain = 1;
    out->buf = b;
    out->next = nullptr;

    // Emit-time barrier (same stack as the send it feeds).  The
    // must_copy gate forces the copy path when downstream (h2/h3 framing,
    // Range, in-memory filters, a subrequest) could retain its own reference
    // into the mmap that our tracked buf's cursor cannot account for.
    const bool llms_must_copy = ps_zerocopy_must_copy(r);
    switch (llms_must_copy ? pagespeed::ps_barrier::BarrierAction::kCopyOut
                           : ps_zerocopy_emit_barrier(ctx->read_result)) {
      case pagespeed::ps_barrier::BarrierAction::kAbort:
        pagespeed::RecordZerocopyTornAbort(g_serve_stats);
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "pagespeed: llms.txt serve torn at emit (cache entry "
                      "recycled), aborting %V",
                      &r->uri);
        ngx_http_finalize_request(r, NGX_ERROR);
        return NGX_DONE;
      case pagespeed::ps_barrier::BarrierAction::kCopyOut:
        if (!ps_zerocopy_copy_out(r, ctx->read_result, b)) {
          ngx_http_finalize_request(r, NGX_ERROR);
          return NGX_DONE;
        }
        if (!llms_must_copy) {
          // Count only wrap-pressure copy-outs (see the stale-serve site).
          pagespeed::RecordZerocopyProactiveCopyout(g_serve_stats);
        }
        break;
      case pagespeed::ps_barrier::BarrierAction::kAlias:
        b->pos =
            const_cast<u_char*>(reinterpret_cast<const u_char*>(body.data()));
        b->last = b->pos + body.size();
        b->memory = 1;
        ctx->aliased_buf = b;  // timer re-checks this borrow while it drains.
        ngx_http_pagespeed_ensure_lease_renew_timer(r, ctx);
        break;
    }
    rc = ngx_http_output_filter(r, out);
    ngx_http_finalize_request(r, rc);
    return NGX_DONE;
  }

  // ReadBestAlternate uses PageSpeedSelector to score all stored
  // alternates and return the best fit for the client's mask.
  // ONLY an `Accept: text/markdown` request with the operator's
  // agent_optimize flag on may select the markdown variant. The AND is computed
  // HERE (not in the selector) and gates ONLY the kAgentMarkdown sentinel — any
  // other request transparently gets the normal best variant (never a hard
  // decline).
  // The rendered markdown was fetched from the hostname, so it is served only
  // under a hostname this block vouches for.
  bool agent_request_entitled = ctx->agent_wants_markdown &&
                                g_shared_config.agent_optimize_entitled &&
                                ngx_http_pagespeed_host_vouched(r, hostname);
  // The serve gate (the flag + the content-hash equality
  // check) lives in the shared cache primitive — a stale/unbound markdown is
  // refused there (returns NotFound), so this falls through to the miss path
  // exactly as the old inline gate's `goto cache_miss` did.
  auto read_result = cache->ReadBestAlternateAgentByKey(cache_key, ctx->mask,
                                                        agent_request_entitled);
  // RFC 9111 §3.5: a shared cache MUST NOT use a stored response to satisfy
  // a request carrying Authorization unless the STORED response's
  // Cache-Control explicitly permits it (public / must-revalidate /
  // s-maxage).  Otherwise treat as a MISS and fall through to normal origin
  // proxying.  Gating HERE also covers the conditional-revalidation,
  // stale-if-error, and SWR coalesced-stale serves at STORE-permit
  // strength: all of them only ever serve an entry stashed by this HIT
  // path, so an impermissible entry is never stashed in the first place.
  // Once STALE, reuse additionally requires `public`
  // (AuthzCacheGateAllowsStale) — checked at each stale serve site: the
  // kRevalidate coalesced-stale branch below and the stale-if-error serve
  // in the header filter (ngx_http_pagespeed_stale_on_error_permitted).
  // Stored metadata with no CC signal at all carries no permission and is
  // bypassed (fails closed).  The skipped read result's borrow is
  // released when the local goes out of scope at handler return — before
  // the content phase contacts the upstream.  Computed once; also
  // consulted by the coalesced-stale check and the cache_miss Early Hints
  // suppression.
  const bool request_has_authorization =
      ngx_http_pagespeed_authz_gate_has_auth(r);
  bool authz_cache_bypass =
      read_result.has_value() && read_result->is_valid() &&
      !pagespeed::AuthzCacheGateAllows(request_has_authorization,
                                       read_result->metadata.origin_cc_flags);
  if (authz_cache_bypass) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "pagespeed: request carries Authorization and stored "
                   "entry lacks an explicit cache permit — bypassing cache "
                   "for %V",
                   &r->uri);
  }
  // Cookie rule (src/nginx/cookie_cache_gate.h): a stored page is served to
  // a request carrying a Cookie header only when the stored entry is marked
  // public or has an s-maxage above zero; static assets are served as
  // before.  Gated here, with the same reach as the §3.5 bypass above: the
  // conditional revalidation, stale-if-error and coalesced-stale serves only
  // ever use an entry this HIT path stashed.  The Markdown copy stores no
  // Cache-Control of its own; it is rendered from the recorded page, so the
  // recorded page decides for it (none recorded: fail closed).  The extra
  // read happens only for a cookie-bearing request that selected the copy.
  const bool request_has_cookie = ngx_http_pagespeed_request_has_cookie(r);
  bool cookie_cache_bypass = false;
  if (read_result.has_value() && read_result->is_valid() &&
      request_has_cookie) {
    const bool selected_markdown =
        (read_result->metadata.full_mask & 0xFF) ==
        static_cast<uint32_t>(SentinelId::kAgentMarkdown);
    if (selected_markdown) {
      cookie_cache_bypass =
          !ngx_http_pagespeed_recorded_page_shared(*cache, cache_key);
    } else {
      cookie_cache_bypass = !pagespeed::CookieCacheGateAllowsServe(
          true,
          pagespeed::StoredEntryCookieClass(
              read_result->metadata.origin_content_type,
              read_result->metadata.content_type),
          read_result->metadata.origin_cc_flags,
          read_result->metadata.origin_s_maxage);
    }
    if (cookie_cache_bypass) {
      ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                     "pagespeed: request carries a cookie and the stored "
                     "page is not marked public or s-maxage — bypassing "
                     "cache for %V",
                     &r->uri);
    }
  }
  const bool request_cache_bypass = authz_cache_bypass || cookie_cache_bypass;
  // Defense-in-depth (issue #1335): re-validate the selected variant's
  // image format against the request's negotiated mask before serving.
  // PageSpeedSelector already hard-disqualifies a format-incompatible
  // variant (#1334), so this second, independent layer must never change
  // what a correctly-functioning selector hands down — it exists so a
  // selector regression (or a variant stored by a peer that scored
  // differently) cannot put bytes in a format this request never
  // advertised on the wire.  The compatibility rule mirrors ScoreAlternate
  // exactly: stored Original (0) is the universal fallback, stored SVG (3)
  // is universal, an exact format match serves, and any other raster
  // mismatch refuses the variant.  Sentinel AlternateIds (the
  // kAgentMarkdown 0x7C et al.) all carry format bits 0, so they pass here
  // as Original; non-image content types are a zero-cost pass-through.
  // On mismatch the request falls through to the miss path below with zero
  // undo logic: the skipped read_result's borrow releases when the local
  // goes out of scope at handler return, exactly as in the authz-bypass
  // above.
  bool variant_format_mismatch = false;
  if (read_result.has_value() && read_result->is_valid() &&
      read_result->metadata.content_type == ContentType::kImage) {
    const uint8_t stored_format =
        static_cast<uint8_t>(read_result->metadata.full_mask) & 0x03;
    const uint8_t request_format =
        static_cast<uint8_t>(ctx->mask.image_format());
    variant_format_mismatch = stored_format != 0 && stored_format != 3 &&
                              stored_format != request_format;
    if (variant_format_mismatch) {
      ngx_log_error(
          NGX_LOG_WARN, r->connection->log, 0,
          "pagespeed: refusing cached variant for %V: stored image "
          "format %ui not advertised by the request "
          "(stored mask 0x%xi, request mask 0x%xi) — "
          "treating as miss",
          &r->uri, static_cast<ngx_uint_t>(stored_format),
          static_cast<ngx_uint_t>(read_result->metadata.full_mask & 0xFF),
          static_cast<ngx_uint_t>(ctx->mask.Encode() & 0xFF));
    }
  }
  // Response-header sidecar (issue #208): an entry recorded with stored
  // header lines must be served WITH them.  Read the URL's block once, here,
  // after the gates above and before the freshness decision, so every serving
  // arm below (fresh hit, no-cache hit, coalesced stale serve) and the
  // stale-if-error serve in the header filter replay the same copy.  A block
  // that cannot be read makes this request a MISS — not stashed for
  // conditional revalidation or stale-if-error, not origin-refreshed — and
  // the record arm rewrites the block from the origin's response.  Entries
  // that carry no block, entries recorded before the gate, and the agent
  // markdown variant pay nothing.  Decisions: headers_sidecar_replay.h.
  bool sidecar_missing = false;
  if (!request_cache_bypass && !variant_format_mismatch &&
      read_result.has_value() && read_result->is_valid()) {
    const bool selected_agent_markdown =
        (read_result->metadata.full_mask & 0xFF) ==
        static_cast<uint32_t>(SentinelId::kAgentMarkdown);
    if (!ngx_http_pagespeed_load_sidecar(r, ctx, *cache, cache_key,
                                         read_result->metadata.flags,
                                         selected_agent_markdown)) {
      sidecar_missing = true;
      pagespeed::RecordHeadersSidecarMissing(g_serve_stats);
      ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                     "pagespeed: cached entry for %V requires its stored "
                     "headers and they cannot be read — treating as miss",
                     &r->uri);
    }
  }
  if (!request_cache_bypass && !variant_format_mismatch && !sidecar_missing &&
      read_result.has_value() && read_result->is_valid()) {
    auto content = read_result->content();
    {  // Zero-byte cache entries are valid (is_valid() checked above).
      // Cache hit — build a string_view over the mmap'd content.
      std::string_view cached_data(
          reinterpret_cast<const char*>(content.data()), content.size());

      ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                     "pagespeed: cache hit, %uz bytes for %V",
                     cached_data.size(), &r->uri);

      // Transfer ownership of the read result to the request context
      // so the mmap stays valid until the request completes.
      ctx->read_result = new ReadResult(std::move(*read_result));
      ctx->served_from_cache = 1;

      // Did the selector hand us the agent markdown variant? (Only an
      // entitled agent request can reach here, and ReadBestAlternateAgentByKey
      // already enforced the §2.4 content-hash binding gate — a stale/unbound
      // markdown was refused upstream as a miss, so a 0x7C variant here is fresh
      // and bound.)  Gates the markdown-only headers below.
      ctx->served_agent_markdown =
          ((ctx->read_result->metadata.full_mask & 0xFF) ==
           static_cast<uint32_t>(SentinelId::kAgentMarkdown))
              ? 1
              : 0;

      // Anchor the cache GENERATION this ReadResult points into to the
      // request's lifetime.  ngx_http_output_filter may DEFER the socket
      // write for a slow client (NGX_AGAIN); nginx then re-reads the mmap
      // span below across later event cycles, AFTER this handler returns.
      // Holding a shared_ptr keeps this exact PageSpeedCache instance — and
      // therefore its mmap — alive until ngx_http_pagespeed_cleanup runs,
      // regardless of how many generation changes (worker purge/reset)
      // occur in the meantime.  The ctx is nginx-pool-allocated (no C++
      // dtor), so we heap-allocate the shared_ptr and delete it in cleanup,
      // mirroring the read_result raw-pointer pattern.  Cost on the hot
      // path: one shared_ptr copy (atomic refcount bump) + one small alloc.
      ctx->cache_anchor = new std::shared_ptr<PageSpeedCache>(cache);

      // The borrow parked above outlives this handler.  The lease-
      // renewal timer is armed exactly where a long hold begins: at the emit
      // site on the kAlias outcome (a slow client-paced drain), and at the
      // stale-revalidation stash sites below (the borrow is held across an
      // upstream fetch).  A copy-out releases the borrow and an abort
      // finalizes, so neither arms the timer.

      // Check if the match is exact (stored mask == client mask).
      bool exact_match =
          (ctx->read_result->metadata.full_mask == ctx->mask.Encode());

      // Determine Content-Type for the response.
      // Priority: (1) transcoded image format from mask, (2) origin
      // Content-Type stored in metadata, (3) URL extension, (4) sniffing.
      const auto& meta = ctx->read_result->metadata;
      const char* mime = nullptr;
      std::string_view origin_ct = meta.origin_content_type;

      // For transcoded images, the stored content is a different format
      // than the origin. Use the variant's actual format.
      if (meta.content_type == ContentType::kImage) {
        uint8_t format_bits = static_cast<uint8_t>(meta.full_mask) & 0x03;
        if (format_bits == 1) {
          mime = "image/webp";
        } else if (format_bits == 2) {
          mime = "image/avif";
        } else if (format_bits == 3) {
          mime = "image/svg+xml";
        }
      }

      // If not a transcoded image, use the origin Content-Type from
      // metadata (preserves charset, e.g. "text/html; charset=utf-8").
      // Pool-allocate since origin_ct is backed by the mmap'd ReadResult.
      u_char* ct_buf = nullptr;
      if (mime == nullptr && !origin_ct.empty()) {
        ct_buf = static_cast<u_char*>(ngx_pnalloc(r->pool, origin_ct.size()));
        if (ct_buf) {
          ngx_memcpy(ct_buf, origin_ct.data(), origin_ct.size());
        }
      }

      // Fallback: URL extension sniffing + content sniffing.
      if (mime == nullptr && ct_buf == nullptr) {
        mime = pagespeed::MimeFromUrl(url);
        if (mime == nullptr) {
          mime = "application/octet-stream";
        }
        // Content-sniff HTML for extensionless URLs (e.g., "/page").
        if (strcmp(mime, "application/octet-stream") == 0) {
          if (cached_data.size() >= 15 &&
              (cached_data.starts_with("<!DOCTYPE") ||
               cached_data.starts_with("<!doctype") ||
               cached_data.starts_with("<html") ||
               cached_data.starts_with("<HTML"))) {
            mime = "text/html";
          }
        }
      }

      // Set response headers.
      r->headers_out.status = NGX_HTTP_OK;
      r->headers_out.content_length_n = static_cast<off_t>(cached_data.size());

      ngx_str_t content_type_str;
      if (ct_buf != nullptr) {
        content_type_str.data = ct_buf;
        content_type_str.len = origin_ct.size();
      } else {
        content_type_str.data =
            const_cast<u_char*>(reinterpret_cast<const u_char*>(mime));
        content_type_str.len = strlen(mime);
      }
      r->headers_out.content_type = content_type_str;
      r->headers_out.content_type_len = content_type_str.len;

      // The agent_optimize markdown variant is a private, non-indexed,
      // content-negotiated response. Emit its 3 headers here; the freshness
      // Cache-Control and the Content-Encoding blocks below are skipped for it
      // (they would emit a conflicting CC and — because full_mask 0x7C decodes to
      // gzip bits — a spurious Content-Encoding).
      if (ctx->served_agent_markdown) {
        add_response_header_cstr(r, "Vary", "Accept");
        add_response_header_cstr(r, "X-Robots-Tag", "noindex");
        ngx_table_elt_t* md_cc = static_cast<ngx_table_elt_t*>(
            ngx_list_push(&r->headers_out.headers));
        if (md_cc) {
          md_cc->hash = 1;
          md_cc->next = nullptr;
          ngx_str_set(&md_cc->key, "Cache-Control");
          ngx_str_set(&md_cc->value, "private");
          r->headers_out.cache_control = md_cc;
        }
      }

      // (Our own Vary is emitted at serve_hit, once this request is known to
      // be served from cache — see the note there.)

      // RFC 9111 compliant Cache-Control serving algorithm.
      // Uses EvaluateFreshness() as the single source of truth for
      // freshness decisions (lib/cache/freshness.h). Skipped for the agent
      // markdown variant, which set Cache-Control: private above.
      if (!ctx->served_agent_markdown) {
        uint16_t cc_flags = meta.origin_cc_flags;

        // Populate freshness config from nginx location directives.
        FreshnessConfig freshness_config;
        freshness_config.max_age_cap = static_cast<uint32_t>(conf->max_age_cap);
        freshness_config.immutable_max_age_cap =
            static_cast<uint32_t>(conf->immutable_max_age_cap);
        freshness_config.html_max_age =
            static_cast<uint32_t>(conf->html_max_age);
        freshness_config.css_max_age = static_cast<uint32_t>(conf->css_max_age);
        freshness_config.image_max_age =
            static_cast<uint32_t>(conf->image_max_age);

        // Populate per-request freshness input from metadata.
        FreshnessInput fi;
        fi.now_seconds = static_cast<uint32_t>(ngx_time());
        fi.cache_inserted_at = meta.cache_inserted_at;
        fi.origin_max_age = meta.origin_max_age;
        fi.origin_s_maxage = meta.origin_s_maxage;
        fi.origin_cc_flags = cc_flags;
        fi.content_type = meta.content_type;
        fi.is_shared_cache = true;  // nginx is always a shared cache

        // Apply force-refresh per content type:
        //   HTML: pagespeed_force_refresh_html (default on)
        //   Other: pagespeed_force_refresh (default off)
        if (ctx->force_refresh) {
          if (meta.content_type == ContentType::kHtml) {
            fi.force_revalidate = conf->force_refresh_html;
          } else {
            fi.force_revalidate = conf->force_refresh;
          }
        }

        auto freshness = EvaluateFreshness(fi, freshness_config);
        uint32_t age = freshness.age_seconds;
        uint32_t effective_max_age = freshness.effective_max_age;

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "pagespeed: freshness verdict=%d age=%uD",
                       static_cast<int>(freshness.verdict), age);

        // The Vary a response SERVED FROM CACHE carries: the per-content-type
        // axes we negotiate on, plus this entry's own stored
        // Accept-negotiation marker (on a HIT the origin's headers are gone,
        // so the marker is the only surviving record of it).
        //
        // A helper called from every serving arm rather than one statement
        // before the switch, and that is the whole point.  Placed before the
        // switch it would also run on the arms that hand the request OFF to
        // the origin — and the header would still be attached when the
        // request came back, where the store-side classifier reads it as if
        // the origin had sent it (that is the leak the emit was moved out of
        // the header prologue to fix: images read their own `Sec-CH-DPR` back
        // and stopped being cacheable, marked entries re-marked themselves
        // forever).  Placed in only ONE serving arm it silently goes missing
        // from the others, which is exactly what happened to kServeNoCache.
        //
        // So: every arm that serves calls this, every arm that hands off does
        // not, and a future fifth verdict has to make that choice explicitly
        // instead of inheriting whichever answer happened to be nearby.
        //
        // (The markdown variant reaches neither call site and is excluded
        // here anyway — it sets its own Vary: Accept above, and emitting it
        // twice would be a duplicate list member.)
        //
        // The stored origin header lines (issue #208) go out first, from the
        // same call, for the same reason: only a response actually served
        // from the entry may carry them.  When they include the origin's own
        // Vary line, the computed line follows it without repeating Accept —
        // the shape a MISS sends.
        auto emit_served_vary = [&] {
          ngx_http_pagespeed_replay_sidecar(r, ctx);
          ngx_http_pagespeed_emit_vary(
              r, meta.content_type,
              !ctx->served_agent_markdown &&
                  pagespeed::ComputedVaryListsAccept(
                      (meta.flags &
                       AlternateMetadata::kFlagOriginVariesAccept) != 0,
                      ctx->sidecar_carried_vary != 0));
        };

        switch (freshness.verdict) {
          case FreshnessVerdict::kServeNoCache: {
            // no-store/private or HTML-no-CC: serve with no-cache header.
            //
            // This IS a cache-HIT serve (the Age header below treats it as
            // one), so it declares its Vary like any other.  It is also the
            // DEFAULT arm for HTML from an origin that sends no
            // Cache-Control, so losing the header here loses `User-Agent` —
            // the viewport-variant key — on ordinary pages.
            emit_served_vary();
            if (cc_flags & (AlternateMetadata::kCCOriginNoStore |
                            AlternateMetadata::kCCOriginPrivate)) {
              ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                            "pagespeed: cached entry has no-store/private "
                            "flag for %V",
                            &r->uri);
            }
            ngx_table_elt_t* cc_h = static_cast<ngx_table_elt_t*>(
                ngx_list_push(&r->headers_out.headers));
            if (cc_h) {
              cc_h->hash = 1;
              cc_h->next = nullptr;
              ngx_str_set(&cc_h->key, "Cache-Control");
              ngx_str_set(&cc_h->value, "no-cache");
              r->headers_out.cache_control = cc_h;
            }
            break;
          }

          case FreshnessVerdict::kRevalidate: {
            // Single-flight refresh coalescing (issue #652 review): for a
            // GENUINELY age-expired entry, only one request per URL per
            // grace window takes the origin re-fetch; concurrent requests
            // serve the cached stale entry below — bounded SWR instead of
            // an origin stampede at every TTL expiry.  Never applied to
            // client force-refresh or origin no-cache (expired_by_age is
            // false there): those must always revalidate per RFC 9111.
            if (freshness.expired_by_age && effective_max_age > 0) {
              uint32_t grace =
                  std::min(kRefreshGraceWindowSecs, effective_max_age);
              std::string slot_key;
              slot_key.reserve(url_str.size() + hostname.size() +
                               scheme.size() + 2);
              slot_key.append(url_str)
                  .append("|")
                  .append(hostname)
                  .append("|")
                  .append(scheme);
              if (!ngx_http_pagespeed_acquire_refresh_slot(slot_key, ngx_time(),
                                                           grace)) {
                // RFC 9111 §5.2.2.2 / §5.2.2.8: a stale entry whose origin
                // Cache-Control carried must-revalidate, proxy-revalidate or
                // s-maxage (§5.2.2.10) is never served without revalidation, for any request.
                // RFC 9111 §4.2.4 / §5.2.2.10: once stale, must-revalidate
                // and s-maxage require revalidation before reuse — the very
                // premise under which §3.5 permits them — so an
                // Authorization-bearing request may take the bounded
                // coalesced stale serve only when the entry carries
                // `public`.  Refused → treat as a MISS to origin.
                if (!pagespeed::StaleServePermitted(request_has_authorization,
                                                    cc_flags)) {
                  ngx_log_debug1(
                      NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                      "pagespeed: refresh in flight but stale entry requires "
                      "revalidation "
                      "(must-revalidate/proxy-revalidate/s-maxage, or "
                      "Authorization without public) — "
                      "bypassing coalesced stale serve for %V",
                      &r->uri);
                  delete ctx->read_result;
                  ctx->read_result = nullptr;
                  ctx->served_from_cache = 0;
                  goto cache_miss;
                }
                pagespeed::RecordSwrCoalescedServe(g_serve_stats);
                ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                               "pagespeed: refresh in flight, serving stale "
                               "within grace for %V",
                               &r->uri);
                // Fall through to the serve block (bounded stale serve).
                goto serve_hit;
              }
            }
            // Stale + revalidation-required: conditional or full re-fetch.
            // An entry recorded before the headers-sidecar gate (issue #208)
            // takes the full re-fetch once, at its genuine expiry, even with
            // validators: a 304 carries only part of the header set, so a
            // restamp would keep it unclassified indefinitely.  The re-record
            // classifies it (headers_sidecar_replay.h,
            // ConditionalRevalidationAllowed).
            if (conf->conditional_revalidation &&
                pagespeed::ConditionalRevalidationAllowed(
                    meta.flags, freshness.expired_by_age) &&
                (!meta.origin_etag.empty() || meta.origin_last_modified > 0)) {
              ctx->stale_revalidation = 1;
              ctx->stale_read_result = ctx->read_result;
              ctx->read_result = nullptr;
              ctx->served_from_cache = 0;
              // The stashed borrow is held across the upstream
              // fetch — keep its lease re-stamped until it is served/freed.
              ngx_http_pagespeed_ensure_lease_renew_timer(r, ctx);
              inject_conditional_headers(r, ctx, meta);
              g_conditional_revalidations.fetch_add(1,
                                                    std::memory_order_relaxed);
              goto cache_miss;
            }
            // No revalidation tokens or disabled: full re-fetch.
            if (freshness.expired_by_age) {
              // Flag the refresh (issue #652): the re-fetched body will be
              // served and recorded, but the URL's OTHER alternates
              // (gzip/brotli/mobile variants) remain stale in cache and
              // keep winning selection for matching clients — without the
              // flag the post-store notification would be dedup-skipped in
              // the worker and the URL would re-fetch from origin on every
              // request, forever.  The sentinel notification makes the
              // worker purge the stale variants (preserving the fresh
              // identity) and rebuild.  GATED on expired_by_age (issue
              // #652 review, blocker): kRevalidate is also reached via
              // client force-refresh (a plain browser reload sends
              // Cache-Control: max-age=0) and origin no-cache content —
              // neither means the variants are outdated, and ungated this
              // let any anonymous client purge a URL's entire optimized
              // variant set.
              ctx->origin_refreshed = 1;
              // Stash the stale entry for stale-if-error (issue #652
              // review, RFC 9111 §4.2.4): if the re-fetch fails upstream
              // (>= 500), the header filter serves this instead of
              // propagating the error site-wide within one TTL of an
              // origin outage.
              ctx->stale_full_refetch = 1;
              ctx->stale_read_result = ctx->read_result;
              ctx->read_result = nullptr;
              // Stashed borrow held across the upstream re-fetch.
              ngx_http_pagespeed_ensure_lease_renew_timer(r, ctx);
            } else {
              delete ctx->read_result;
              ctx->read_result = nullptr;
            }
            ctx->served_from_cache = 0;
            goto cache_miss;
          }

          case FreshnessVerdict::kFresh:
          // kStaleServe is RETIRED (issue #652): EvaluateFreshness no longer
          // returns it — all stale content takes kRevalidate above.  The
          // label is kept so the switch stays exhaustive; only kFresh (and
          // the coalesced bounded-SWR goto from kRevalidate) reaches this
          // block now.
          case FreshnessVerdict::kStaleServe: {
          serve_hit:
            // Reached only by a response actually served from cache (kFresh,
            // and the coalesced bounded-stale goto); all three
            // `goto cache_miss` paths skip it.  See emit_served_vary above
            // for why the emission lives in the serving arms rather than
            // before the switch.
            emit_served_vary();

            CacheControlInput cci;
            // Use shared config cache_mode as fallback when no directive is set.
            if (conf->cache_mode == NGX_CONF_UNSET_UINT) {
              cci.mode = (g_shared_config.cache_mode == "aggressive")
                             ? CacheMode::kAggressive
                             : CacheMode::kSafe;
            } else {
              cci.mode = static_cast<CacheMode>(conf->cache_mode);
            }
            cci.origin_cc_flags = cc_flags;
            cci.effective_max_age = effective_max_age;
            cci.origin_max_age = meta.origin_max_age;
            cci.synthesize_swr = conf->synthesize_swr;
            // nginx is always a shared (proxy) cache — see issue #260.
            // BuildCacheControlHeader derives revalidation_required via
            // ComputeSharedRevalidationRequired, matching freshness.cc.
            cci.is_shared_cache = true;
            cci.content_type = meta.content_type;
            // Guard only: the coalesced bounded-SWR serve requires
            // expired_by_age, which is never set for a no-cache entry, so
            // this does not fire today; it keeps no-cache if that changes.
            cci.relay_origin_no_cache =
                (freshness.verdict == FreshnessVerdict::kRevalidate);

            static constexpr size_t kCCBufSize = 256;
            u_char* cc_buf =
                static_cast<u_char*>(ngx_pnalloc(r->pool, kCCBufSize));
            if (cc_buf) {
              auto result = BuildCacheControlHeader(
                  cci, reinterpret_cast<char*>(cc_buf), kCCBufSize);

              ngx_table_elt_t* cc_h = static_cast<ngx_table_elt_t*>(
                  ngx_list_push(&r->headers_out.headers));
              if (cc_h) {
                cc_h->hash = 1;
                cc_h->next = nullptr;
                ngx_str_set(&cc_h->key, "Cache-Control");
                cc_h->value.data = cc_buf;
                cc_h->value.len = result.len;
                r->headers_out.cache_control = cc_h;
              }
            }
            break;
          }
        }

        // Emit Age header on all HIT paths.
        {
          static constexpr size_t kAgeBufSize = 16;
          u_char* age_buf =
              static_cast<u_char*>(ngx_pnalloc(r->pool, kAgeBufSize));
          if (age_buf) {
            u_char* p =
                ngx_slprintf(age_buf, age_buf + kAgeBufSize, "%uD", age);
            ngx_table_elt_t* age_h = static_cast<ngx_table_elt_t*>(
                ngx_list_push(&r->headers_out.headers));
            if (age_h) {
              age_h->hash = 1;
              age_h->next = nullptr;
              ngx_str_set(&age_h->key, "Age");
              age_h->value.data = age_buf;
              age_h->value.len = static_cast<size_t>(p - age_buf);
            }
          }
        }
      }

      // Set Content-Encoding when serving a pre-compressed variant.
      // Assigning content_encoding prevents nginx's gzip and brotli
      // filters from double-compressing the already-compressed content.
      // Skipped for the agent markdown variant (its full_mask 0x7C is a sentinel
      // marker, NOT a real transfer-encoding — it would decode to a bogus gzip).
      ngx_http_pagespeed_set_variant_content_encoding(r, meta.full_mask);

      // SVG security headers: restrict execution context and prevent
      // MIME-type sniffing to mitigate XSS via inline SVG scripts.
      if (mime != nullptr && strcmp(mime, "image/svg+xml") == 0) {
        ngx_table_elt_t* csp = static_cast<ngx_table_elt_t*>(
            ngx_list_push(&r->headers_out.headers));
        if (csp) {
          csp->hash = 1;
          csp->next = nullptr;
          ngx_str_set(&csp->key, "Content-Security-Policy");
          ngx_str_set(&csp->value,
                      "default-src 'none'; style-src 'unsafe-inline'");
        }
        // Not a second X-Content-Type-Options when the origin's own line was
        // replayed from the stored headers (the CSP above is still added:
        // several policies intersect, so it can only tighten).
        ngx_table_elt_t* xcto =
            ctx->sidecar_carried_nosniff
                ? nullptr
                : static_cast<ngx_table_elt_t*>(
                      ngx_list_push(&r->headers_out.headers));
        if (xcto) {
          xcto->hash = 1;
          xcto->next = nullptr;
          ngx_str_set(&xcto->key, "X-Content-Type-Options");
          ngx_str_set(&xcto->value, "nosniff");
        }
      }

      // For HTML cache hits, add Link preload headers from Early Hints
      // sentinel if the worker has previously stored stylesheet hints.
      if (meta.content_type == ContentType::kHtml) {
        auto hints_read = cache->ReadAlternateByKey(
            cache_key, static_cast<AlternateId>(SentinelId::kEarlyHints));
        if (hints_read.has_value()) {
          auto hints_content = hints_read->content();
          if (!hints_content.empty()) {
            // The carried header lines, viewed once for the Link dedupe.
            std::vector<pagespeed::HeaderField> carried_fields;
            if (ctx->sidecar_fields != nullptr) {
              const auto* carried =
                  static_cast<const ngx_http_pagespeed_header_pair_t*>(
                      ctx->sidecar_fields->elts);
              carried_fields.reserve(ctx->sidecar_fields->nelts);
              for (ngx_uint_t ci = 0; ci < ctx->sidecar_fields->nelts; ci++) {
                carried_fields.push_back(
                    {std::string_view(
                         reinterpret_cast<const char*>(carried[ci].key.data),
                         carried[ci].key.len),
                     std::string_view(
                         reinterpret_cast<const char*>(carried[ci].value.data),
                         carried[ci].value.len)});
              }
            }
            std::string_view hints_sv(
                reinterpret_cast<const char*>(hints_content.data()),
                hints_content.size());
            while (!hints_sv.empty()) {
              size_t nl = hints_sv.find('\n');
              std::string_view line;
              if (nl == std::string_view::npos) {
                line = hints_sv;
                hints_sv = {};
              } else {
                line = hints_sv.substr(0, nl);
                hints_sv.remove_prefix(nl + 1);
              }
              if (!line.empty()) {
                auto res = pagespeed::ParseHintLine(line);
                std::string safe_url = pagespeed::SanitizeLinkUrl(res.url);
                // The origin's own Link (carried in the stored block) may
                // already send this hint: emit it once.
                const bool origin_has_link =
                    !safe_url.empty() &&
                    pagespeed::SidecarLinkCarriesHint(
                        carried_fields, safe_url, res.rel,
                        res.extra.find("crossorigin") != std::string::npos);
                if (!safe_url.empty() && !origin_has_link) {
                  // Build Link header value: <url>; rel=X[; as=Y][; extra]
                  std::string link_val = "<";
                  link_val += safe_url;
                  link_val += ">; rel=";
                  link_val += res.rel;
                  if (!res.as_type.empty()) {
                    link_val += "; as=";
                    link_val += res.as_type;
                  }
                  if (!res.extra.empty()) {
                    link_val += "; ";
                    link_val += res.extra;
                  }
                  ngx_table_elt_t* h = static_cast<ngx_table_elt_t*>(
                      ngx_list_push(&r->headers_out.headers));
                  if (h) {
                    h->hash = 1;
                    h->next = nullptr;
                    ngx_str_set(&h->key, "Link");
                    u_char* val = static_cast<u_char*>(
                        ngx_pnalloc(r->pool, link_val.size()));
                    if (val) {
                      ngx_memcpy(val, link_val.data(), link_val.size());
                      h->value.data = val;
                      h->value.len = link_val.size();
                    } else {
                      // Suppress header on alloc failure to avoid
                      // emitting garbage value bytes to the client.
                      h->hash = 0;
                    }
                  }
                }
              }
            }
          }
        }
      }

      // Add X-PageSpeed: HIT header for cache-served responses
      {
        add_response_header_cstr(r, "X-PageSpeed", "HIT");
      }

      // Track serve-time bandwidth savings for worker-processed variants.
      // The gate stays here; the per-type atomic increments live in the
      // shared RecordServeHit() helper (single source of truth, also used by
      // the in-process .NET front-end via the C API).
      if (g_serve_stats != nullptr &&
          (meta.flags & AlternateMetadata::kFlagWorkerProcessed) &&
          meta.origin_content_length > 0) {
        if (const auto encoded = meta.EncodedOriginLength();
            encoded.has_value()) {
          pagespeed::RecordServeHitSplit(
              g_serve_stats, meta.content_type, meta.origin_content_length,
              *encoded, cached_data.size(), meta.full_mask, std::string_view());
        } else {
          pagespeed::RecordServeHit(g_serve_stats, meta.content_type,
                                    meta.origin_content_length,
                                    cached_data.size(), meta.full_mask);
        }
      }

      // Generate weak ETag for cache HIT responses (see etag_util.h).
      // Format: W/"ps-<mask_hex><flags_hex>-<identity_hex>-<length_hex>".
      // When the metadata carries no content-identity signal (no stored
      // content hash, no origin ETag/Last-Modified), FormatHitETag returns
      // 0 and NO ETag header is emitted: a length-only tag would validate
      // any revision of the same optimized length, answering 304 for
      // changed bytes.  With no tag there is no 304, so a stale copy can
      // never be confirmed.
      // The mask differentiates variants (format, encoding, viewport).
      // The flags byte differentiates revalidation states.
      // The identity differentiates content revisions even when the byte
      // length is unchanged (stored content hash, else origin validators);
      // the length remains as a cheap secondary discriminator.
      // Weak because PageSpeed transforms origin content (RFC 9110
      // Section 8.8.1).  If-Range with a weak ETag correctly falls
      // back to a full 200 response (nginx handles per RFC 9110).
      {
        u_char* etag_buf = static_cast<u_char*>(
            ngx_pnalloc(r->pool, pagespeed::kHitETagBufSize));
        if (etag_buf != nullptr) {
          size_t n = pagespeed::FormatHitETag(meta, cached_data.size(),
                                              reinterpret_cast<char*>(etag_buf),
                                              pagespeed::kHitETagBufSize);
          if (n > 0) {
            ngx_table_elt_t* etag_h = static_cast<ngx_table_elt_t*>(
                ngx_list_push(&r->headers_out.headers));
            if (etag_h) {
              etag_h->hash = 1;
              etag_h->next = nullptr;
              ngx_str_set(&etag_h->key, "ETag");
              etag_h->value.data = etag_buf;
              etag_h->value.len = n;
              r->headers_out.etag = etag_h;
            }
          }
        }
      }

      // Enable range request support on cache HITs.
      // RFC 9110 §13.1.5: If-Range requires strong comparison.  Nginx core
      // doesn't handle weak ETags in If-Range correctly (falls through to
      // range processing instead of ignoring Range).  Suppress ranges when
      // If-Range carries a weak ETag so the client gets a full 200.
      bool suppress_range = false;
      if (r->headers_in.if_range && r->headers_in.if_range->value.len >= 2) {
        u_char* data = r->headers_in.if_range->value.data;
        if (data[0] == 'W' && data[1] == '/') {
          suppress_range = true;
        }
      }
      if (!suppress_range) {
        r->allow_ranges = 1;
      }

      // Send headers.  This handler runs in the PRECONTENT phase, not the
      // CONTENT phase. We must finalize the request and return NGX_DONE
      // to prevent the phase engine from continuing to the CONTENT
      // phase (proxy_pass), which would attempt to send headers again.
      rc = ngx_http_send_header(r);
      if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        ngx_http_finalize_request(r, rc);
        return NGX_DONE;
      }

      // Create a zero-copy buffer pointing to the mmap'd data
      ngx_buf_t* b = static_cast<ngx_buf_t*>(ngx_calloc_buf(r->pool));
      if (b == nullptr) {
        // Headers already sent — cannot return an error status code.
        ngx_http_finalize_request(r, NGX_ERROR);
        return NGX_DONE;
      }

      if (!cached_data.empty()) {
        // Emit-time barrier: strict-renew the borrow in the same call
        // stack as the send it feeds (sendfile and the mmap alias both borrow
        // the cache volume, so both are gated).  Decide BEFORE choosing the
        // serve form so a copy-out never takes a sendfile dup.  The must_copy
        // gate forces the copy path when downstream could retain its OWN
        // reference into the mmap (h2/h3 per-frame shadow bufs that drain
        // with flow control without advancing this buf's cursor, Range
        // multipart bufs, in-memory filters like gzip that hold raw pointers
        // across drains, subrequests): the timer's drained check and tail
        // de-alias track only THIS buf, and a copy-out releases the pin those
        // shadow references still need — aliasing is safe only when this buf
        // is provably the sole reference.
        const bool hit_must_copy = ps_zerocopy_must_copy(r);
        switch (hit_must_copy ? pagespeed::ps_barrier::BarrierAction::kCopyOut
                              : ps_zerocopy_emit_barrier(ctx->read_result)) {
          case pagespeed::ps_barrier::BarrierAction::kAbort:
            pagespeed::RecordZerocopyTornAbort(g_serve_stats);
            ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                          "pagespeed: cache HIT serve torn at emit (cache "
                          "entry recycled), aborting %V",
                          &r->uri);
            // Headers already sent — reset the connection rather than emit
            // possibly-foreign bytes.
            ngx_http_finalize_request(r, NGX_ERROR);
            return NGX_DONE;
          case pagespeed::ps_barrier::BarrierAction::kCopyOut:
            if (!ps_zerocopy_copy_out(r, ctx->read_result, b)) {
              ngx_http_finalize_request(r, NGX_ERROR);
              return NGX_DONE;
            }
            if (!hit_must_copy) {
              // Count only wrap-pressure copy-outs; a gate-forced copy is
              // normal operation on h2/h3/Range/filtered serves.
              pagespeed::RecordZerocopyProactiveCopyout(g_serve_stats);
            }
            break;
          case pagespeed::ps_barrier::BarrierAction::kAlias: {
            // Prefer sendfile when the content has a known file offset
            // within the cache volume (persistent mmap, not RAM cache).
            uint64_t file_off = ctx->read_result->content_file_offset();
            // dup() the worker-global sendfile FD into a request-owned FD so the
            // deferred (slow-client) write reads from a descriptor this request
            // controls.  A later generation change closes g_cache_sendfile_fd
            // immediately; without the dup, nginx could sendfile from a closed
            // or recycled descriptor — wrong content or EBADF.  The dup is
            // closed in ngx_http_pagespeed_cleanup.  -1 dup means fall back to
            // the mmap memory buffer (which is anchored via cache_anchor).
            ngx_fd_t serve_fd = NGX_INVALID_FILE;
            if (file_off != ReadResult::kNoFileOffset &&
                g_cache_sendfile_fd != NGX_INVALID_FILE) {
              serve_fd = dup(g_cache_sendfile_fd);
            }
            if (serve_fd != NGX_INVALID_FILE) {
              // Pool-allocate ngx_file_t for sendfile.
              ngx_file_t* file = static_cast<ngx_file_t*>(
                  ngx_pcalloc(r->pool, sizeof(ngx_file_t)));
              if (file != nullptr) {
                ctx->sendfile_fd = serve_fd;
                file->fd = serve_fd;
                file->log = r->connection->log;
                file->name = conf->cache_path;
                b->in_file = 1;
                b->file = file;
                b->file_pos = static_cast<off_t>(file_off);
                b->file_last =
                    static_cast<off_t>(file_off + cached_data.size());
              } else {
                // Fallback to memory buffer on alloc failure.  Close the dup we
                // just took; the mmap span stays valid via cache_anchor.
                ngx_close_file(serve_fd);
                b->pos = const_cast<u_char*>(
                    reinterpret_cast<const u_char*>(cached_data.data()));
                b->last = b->pos + cached_data.size();
                b->memory = 1;
              }
            } else {
              b->pos = const_cast<u_char*>(
                  reinterpret_cast<const u_char*>(cached_data.data()));
              b->last = b->pos + cached_data.size();
              b->memory = 1;
            }
            // Park the aliased buffer so the lease-renewal timer re-checks
            // this borrow (mmap span or sendfile offset) while it drains.
            ctx->aliased_buf = b;
            ngx_http_pagespeed_ensure_lease_renew_timer(r, ctx);
            break;
          }
        }
      }
      b->last_buf = 1;
      b->last_in_chain = 1;

      // Pool-allocate chain (never stack-allocate — if
      // ngx_http_output_filter returns NGX_AGAIN, nginx's write
      // event handler will reference the chain later).
      ngx_chain_t* out =
          static_cast<ngx_chain_t*>(ngx_palloc(r->pool, sizeof(ngx_chain_t)));
      if (out == nullptr) {
        // Headers already sent — cannot return an error status code.
        ngx_http_finalize_request(r, NGX_ERROR);
        return NGX_DONE;
      }
      out->buf = b;
      out->next = nullptr;

      // Re-notify the worker when:
      // (a) Fallback hit (stored mask != client mask) — generate the
      //     specific variant for this capability mask.
      // (b) Variant marked for revalidation — HTML whose external CSS
      //     was not yet cached, or CSS with unresolved @imports.  Re-
      //     notify so the worker can re-process once dependencies are
      //     available.
      bool needs_revalidation =
          ((meta.content_type == ContentType::kHtml ||
            meta.content_type == ContentType::kCss) &&
           (meta.flags & pagespeed::AlternateMetadata::kFlagNeedsRevalidation));
      bool html_disabled = (meta.content_type == ContentType::kHtml &&
                            g_shared_config.disable_html);
      // The kAgentMarkdown variant is worker-written, terminal, and
      // intentionally never matches the client mask (0x7C carries sentinel
      // viewport bits), so it would otherwise spuriously fire a fallback re-
      // notify + WARN log + hot-URL warmup on every HIT.  Suppress all of that
      // for a markdown serve, mirroring the freshness/Content-Encoding skips.
      // Suppress the permanent format-bit mismatch for worker-vectorized SVG
      // variants (image format bits == kSvg == 3).  kSvg is worker-only — it is
      // never set from a client Accept header (capability_mask.h) — so a client
      // can never request format 3 and the SVG variant is always a fallback
      // selection, never an exact-format match.  exact_match therefore stays
      // false on every HIT that serves the SVG, firing a re-notify the worker's
      // dedup already discards (commits 864ec4a/bb136b6/8f2fb63): pure wasted
      // Unix-socket IPC.  Gate on kFlagWorkerProcessed (always set on the
      // vectorized SVG write) and AND only into the !exact_match term so HTML/
      // CSS revalidation re-notifies still fire.  The hot-URL warmup counter is
      // nested in `if (should_notify)` and so is auto-suppressed.
      //
      // NOTE (correctness): the format==0 (Original) arm proposed in the issue
      // is deliberately NOT included.  The worker sets kFlagWorkerProcessed on
      // the optimized-original variant of every optimizable raster (worker.cc
      // WriteImageVariants), and each format sibling is built independently
      // (FindMissingFormats), so a fmt==0 + kFlagWorkerProcessed variant can
      // legitimately exist for a JPEG/PNG whose WebP/AVIF siblings are not built
      // yet.  Suppressing its re-notify would permanently block that build.
      // Native SVG and unsupported types (ICO/BMP/TIFF) are also stored at
      // fmt==0, so they cannot be safely distinguished here either.  SVG-only
      // (fmt==3) is the unambiguously safe subset.
      bool permanent_format_mismatch =
          (meta.content_type == ContentType::kImage &&
           (meta.full_mask & 0x03) == 3 &&
           (meta.flags & pagespeed::AlternateMetadata::kFlagWorkerProcessed));
      // An entry whose origin negotiates on Accept is never handed to the
      // optimizer -- and this site, not the store path, is what would
      // otherwise do it.  The stored original sits at the default mask, so a
      // request on any other mask is a "variant miss" and re-notifies here on
      // essentially every hit; the store-side skip alone therefore only
      // delays the second negotiation rather than preventing it.  Gating on
      // the entry's own persisted marker is what makes "stored, never
      // optimized" hold for the life of the entry.  Suppressing the nested
      // hot-URL warmup with it is deliberate for the same reason: a warmup is
      // a request to optimize.
      const bool origin_negotiates_accept =
          (meta.flags &
           pagespeed::AlternateMetadata::kFlagOriginVariesAccept) != 0;
      bool should_notify = ((!exact_match && !permanent_format_mismatch) ||
                            needs_revalidation) &&
                           !html_disabled && !ctx->served_agent_markdown &&
                           !origin_negotiates_accept;
      // Carried by the notification and the warmup below (see the record
      // path).
      const bool host_vouched =
          should_notify && ngx_http_pagespeed_host_vouched(r, hostname);
      should_notify = should_notify && !ngx_http_pagespeed_withhold_html_notify(
                                           meta.content_type, host_vouched);
      if (should_notify && !g_shared_config.socket_path.empty()) {
        CacheNotification notification;
        notification.url = std::string(url);
        notification.hostname = std::string(hostname);
        notification.scheme = std::string(scheme);
        notification.capability_mask = ctx->mask.Encode();
        notification.content_type = meta.content_type;
        // Demand-gate the per-URL forced agent render (see the
        // MISS-notify site) — an agent request that HIT HTML (markdown absent)
        // with the operator flag on is the demand signal that builds this
        // URL's markdown.
        notification.agent_request = ctx->agent_wants_markdown &&
                                     g_shared_config.agent_optimize_entitled;
        notification.host_vouched = host_vouched;
        std::string_view socket_path(g_shared_config.socket_path);
        auto send_result =
            SendNotificationPersistent(socket_path, notification);
        if (!send_result.success) {
          ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                        "pagespeed: variant miss, worker notify failed: %s",
                        send_result.error_message.c_str());
        }

        // Hot URL tracking: count fallback hits (not revalidation re-
        // notifications) and send a warmup when the threshold is reached.
        if (!exact_match && conf->hot_threshold > 0) {
          uint64_t url_hash = FNV1a(url);
          size_t slot = url_hash & (kHotUrlMapSize - 1);
          auto& entry = g_hot_url_map[slot];
          uint64_t stored = entry.url_hash.load(std::memory_order_relaxed);
          if (stored == url_hash) {
            int c = entry.count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (c == conf->hot_threshold) {
              CacheNotification warmup;
              warmup.url = std::string(url);
              warmup.hostname = std::string(hostname);
              warmup.scheme = std::string(scheme);
              warmup.capability_mask = pagespeed::kWarmupSentinel;
              warmup.content_type = notification.content_type;
              warmup.host_vouched = host_vouched;
              // Warmup hint is best-effort; failure is non-fatal.
              (void)SendNotificationPersistent(socket_path, warmup);
              entry.count.store(0, std::memory_order_relaxed);
              ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                             "pagespeed: sent warmup for hot URL %V", &r->uri);
            }
          } else {
            entry.url_hash.store(url_hash, std::memory_order_relaxed);
            entry.count.store(1, std::memory_order_relaxed);
          }
        }
      }

      // Evict this alternate from the RAM cache when it's flagged for
      // revalidation.  The worker will re-process and write an updated
      // (non-flagged) variant to disk.  Evicting ensures the next request
      // reads from disk instead of serving the stale revalidation-flagged
      // copy from RAM indefinitely.

      rc = ngx_http_output_filter(r, out);
      ngx_http_finalize_request(r, rc);
      return NGX_DONE;
    }
  }

cache_miss:
  // Cache miss - decline to let upstream handle.
  // The body filter will record the response on the way back.
  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                 "pagespeed: cache miss for %V, passing to upstream", &r->uri);

  // Try to send Early Hints (103) for HTML MISS requests.
  //
  // The hint set derives from a previously STORED response (the
  // kEarlyHints sentinel), so emitting it is a partial reuse of
  // stored-response data and takes the same §3.5 gate as a serve.  No
  // stored CC signal is available here (WriteSentinel stores no metadata
  // prefix; the main entry's metadata is absent, moved-from, or just
  // gate-refused), so the gate is consulted with flags 0 — fail closed
  // for every Authorization-bearing miss, incl. the authz-bypass arrival
  // that must not leak the stored page's preload/preconnect set.  The
  // HIT-path Link headers already passed the gate.
  //
  // The cookie rule takes the same view (cookie_cache_gate.h): the hint set is
  // derived from the recorded page, so a cookie-bearing miss gets it only
  // when that page is marked public or s-maxage above zero.
  if (pagespeed::AuthzCacheGateAllows(request_has_authorization,
                                      /*no stored CC signal*/ 0) &&
      (!request_has_cookie ||
       ngx_http_pagespeed_recorded_page_shared(*cache, cache_key))) {
    // Key on the content type, not only the extension: a clean URL such as
    // "/about" has no extension, but its stored hint set (written for HTML
    // only) is still wanted when the client asks for HTML.
    const char* mime = pagespeed::MimeFromUrl(url);
    static ngx_str_t accept_hdr_name = ngx_string("Accept");
    ngx_str_t accept_hdr = ngx_http_pagespeed_get_header(r, accept_hdr_name);
    if (pagespeed::ShouldSendHintsOnMiss(
            mime != nullptr ? std::string_view(mime) : std::string_view(),
            std::string_view(
                accept_hdr.data ? reinterpret_cast<const char*>(accept_hdr.data)
                                : "",
                accept_hdr.data ? accept_hdr.len : 0))) {
      auto hints_read = cache->ReadAlternate(
          url, hostname, scheme,
          static_cast<AlternateId>(SentinelId::kEarlyHints));
      if (hints_read.has_value()) {
        auto hints_content = hints_read->content();
        if (!hints_content.empty()) {
          std::vector<pagespeed::PreloadResource> resources;
          std::string_view hints_sv(
              reinterpret_cast<const char*>(hints_content.data()),
              hints_content.size());
          while (!hints_sv.empty()) {
            size_t nl = hints_sv.find('\n');
            std::string_view line;
            if (nl == std::string_view::npos) {
              line = hints_sv;
              hints_sv = {};
            } else {
              line = hints_sv.substr(0, nl);
              hints_sv.remove_prefix(nl + 1);
            }
            if (!line.empty()) {
              resources.push_back(pagespeed::ParseHintLine(line));
            }
          }
          if (!resources.empty()) {
            pagespeed::SendEarlyHints(r, resources);
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "pagespeed: sent 103 Early Hints with %uz "
                           "preloads for %V",
                           resources.size(), &r->uri);
          }
        }
      }
    }
  }

  return NGX_DECLINED;
}

// Runs once per configuration cycle (start and every reload) after the
// configuration is read and the error log is open, before worker processes
// are forked: writes the cache-directory generation line that
// merge_loc_conf evaluated but could not yet log.  The workers inherit the
// "already logged" state, so the line appears once per cycle, not once per
// worker.
static ngx_int_t ngx_http_pagespeed_init_module(ngx_cycle_t* cycle) {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  LogCacheDirGenerationIfPending(cycle->log);
  return NGX_OK;
}

// $pagespeed_cache_generation           match | mismatch | unknown
// $pagespeed_cache_generation_module    this module's generation
// $pagespeed_cache_generation_optimizer the optimizer's published generation
//                                       (not found = the optimizer stated none)
// The status of the cache-directory generation check as this nginx worker
// process last evaluated it (at start/reload, then on every shared-config
// change).  Operators expose it where they want it: a log_format field, or a
// restricted status location (`return 200 "$pagespeed_cache_generation\n";`).
enum : uintptr_t {
  kCacheGenVarState = 0,
  kCacheGenVarModule = 1,
  kCacheGenVarOptimizer = 2,
};

static ngx_int_t ngx_http_pagespeed_cache_generation_variable(
    ngx_http_request_t* r, ngx_http_variable_value_t* v, uintptr_t data) {
  const pagespeed::CacheGenerationVerdict& verdict = g_cache_dir_gen.verdict();
  std::string value;
  switch (data) {
    case kCacheGenVarState:
      value = pagespeed::CacheGenerationStateName(verdict.state);
      break;
    case kCacheGenVarModule:
      value = std::to_string(verdict.module_generation);
      break;
    case kCacheGenVarOptimizer:
      if (verdict.optimizer_generation <= 0) {
        v->not_found = 1;
        return NGX_OK;
      }
      value = std::to_string(verdict.optimizer_generation);
      break;
    default:
      v->not_found = 1;
      return NGX_OK;
  }
  u_char* p = static_cast<u_char*>(ngx_pnalloc(r->pool, value.size()));
  if (p == nullptr) {
    return NGX_ERROR;
  }
  ngx_memcpy(p, value.data(), value.size());
  v->data = p;
  v->len = value.size();
  v->valid = 1;
  v->no_cacheable = 1;
  v->not_found = 0;
  return NGX_OK;
}

static ngx_int_t ngx_http_pagespeed_add_variables(ngx_conf_t* cf) {
  struct VarDef {
    const char* name;
    uintptr_t data;
  };
  static const VarDef kVars[] = {
      {"pagespeed_cache_generation", kCacheGenVarState},
      {"pagespeed_cache_generation_module", kCacheGenVarModule},
      {"pagespeed_cache_generation_optimizer", kCacheGenVarOptimizer},
  };
  for (const VarDef& def : kVars) {
    ngx_str_t name;
    name.data = reinterpret_cast<u_char*>(const_cast<char*>(def.name));
    name.len = std::strlen(def.name);
    ngx_http_variable_t* var =
        ngx_http_add_variable(cf, &name, NGX_HTTP_VAR_NOCACHEABLE);
    if (var == nullptr) {
      return NGX_ERROR;
    }
    var->get_handler = ngx_http_pagespeed_cache_generation_variable;
    var->data = def.data;
  }
  return NGX_OK;
}

// Process lifecycle hooks — on nginx reload (SIGHUP) the master forks
// a new worker process.  The forked child inherits g_cache, but
// Cyclone file descriptors may be stale.  Reinitialize to get a fresh
// cache handle.
static ngx_int_t ngx_http_pagespeed_init_process(ngx_cycle_t* cycle) {
  // Web Bot Auth opt-in counter: read the secret bearer token
  // from the environment once per worker.  Log PRESENCE only — never the value
  // (mirrors the PAGESPEED_PURGE_TOKEN model on the worker side).
  {
    const char* tok = getenv("PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN");
    if (tok != nullptr && tok[0] != '\0') {
      g_web_bot_auth_counter_token = tok;
      ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                    "pagespeed: web-bot-auth counter token configured "
                    "(exact doc gated)");
    }
  }
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  // Do NOT destroy g_cache. After fork() the inherited handle is valid for
  // reads and allows the new worker to serve cache HITs immediately — no
  // cold-start gap during nginx reload.
  // Reset generation to 0 so the first CheckGenerationAndGetCache() call
  // detects a "change" (actual gen > 0) and does a clean reopen, replacing
  // the inherited handle. Keep g_cache_gen_path so that generation detection
  // continues to work.
  g_cache_generation = 0;
  // NOTE: Do NOT clear g_cache_gen_path — needed for generation detection.
  g_last_generation_check.store(0, std::memory_order_relaxed);
  g_shared_config_mtime = 0;    // Force shared config re-read after fork.
  ResetPersistentConnection();  // Close inherited fd from parent process.
  // Invalidate the inherited sendfile FD (do NOT close — parent still uses
  // it).  Open a fresh FD for this worker process immediately so sendfile
  // works even before CheckGenerationAndGetCache() triggers a full reopen
  // (which only happens on generation change, not always on fork).
  g_cache_sendfile_fd = NGX_INVALID_FILE;
  if (g_cache) {
    // Cyclone's structural-fingerprint naming means the on-disk file is not
    // the configured cache path — take the real path from the inherited
    // cache instance (kept alive above precisely so the new worker can
    // serve immediately).
    const std::string cache_file = g_cache->VolumeFilePath();
    g_cache_sendfile_fd =
        ngx_open_file(cache_file.c_str(), NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (g_cache_sendfile_fd == NGX_INVALID_FILE) {
      ngx_log_error(NGX_LOG_WARN, cycle->log, ngx_errno,
                    "pagespeed: sendfile FD open in init_process failed");
    }
  }
  for (size_t i = 0; i < kHotUrlMapSize; ++i) {
    g_hot_url_map[i].url_hash.store(0, std::memory_order_relaxed);
    g_hot_url_map[i].count.store(0, std::memory_order_relaxed);
  }
  ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                "pagespeed: process init — generation reset, "
                "cache will reopen on first request");
  return NGX_OK;
}

static void ngx_http_pagespeed_exit_process(ngx_cycle_t* cycle) {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  // Drop the worker-global reference.  By the time exit_process runs, nginx
  // has closed all connections, so per-request ctx cache anchors (which kept
  // retired generations alive) have already been released via their cleanup
  // handlers; this drops the last reference and destroys the instance.
  g_cache.reset();
  g_cache_generation = 0;
  g_cache_gen_path.clear();
  g_last_generation_check.store(0, std::memory_order_relaxed);
  g_shared_config_mtime = 0;
  g_shared_config = SharedConfig{};
  g_url_norm_config = pagespeed::UrlNormalizationConfig{};
  if (g_cache_sendfile_fd != NGX_INVALID_FILE) {
    ngx_close_file(g_cache_sendfile_fd);
    g_cache_sendfile_fd = NGX_INVALID_FILE;
  }
  if (g_serve_stats != nullptr) {
    pagespeed::CloseServeStats(g_serve_stats);
    g_serve_stats = nullptr;
  }
  ClosePersistentConnection();
  ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                "pagespeed: process exit — cache released");
}

// The worker's agent fetches (agent_optimize render, llms.txt synthesis)
// reach this server from loopback or from the address they connect to when
// the worker shares the host, so an access rule that admits local clients
// (`allow 127.0.0.1`) would admit them whoever asked for the page being
// rendered, and the restricted content could reach a remote client through
// the agent markdown variant or /llms.txt.  POST_READ runs before every access
// rule, and this handler runs before realip's (handlers of one phase run in
// reverse order of registration, and this module's postconfiguration runs
// after the built-in modules'), so a recognized agent fetch is presented as
// the reserved address 224.0.0.0 to allow/deny, $remote_addr, the access log
// and realip for the whole request, internal redirects included; the request
// pool's cleanup puts the real address back for the connection's next
// request.  Recognition takes both the worker's User-Agent marker and a peer
// that is this server (loopback, or the address the connection arrived on),
// so a remote client that connects directly keeps its own address whatever
// User-Agent it sends.  Behind a proxy on the same host every request
// arrives from this host, so the peer test cannot tell the worker's fetch
// from a proxied client; recognition there rests on the User-Agent alone.
// See src/nginx/agent_self_fetch.h.
struct ngx_http_pagespeed_agent_relabel_t {
  struct sockaddr_in addr;
  u_char text[sizeof("224.0.0.0")];
  ngx_connection_t* connection;
  struct sockaddr* saved_sockaddr;
  socklen_t saved_socklen;
  ngx_str_t saved_addr_text;
};

// The address bytes of `sa` (4 for IPv4, 16 for IPv6), empty otherwise.
static std::span<const unsigned char> ngx_http_pagespeed_addr_bytes(
    const struct sockaddr* sa) {
  if (sa == nullptr) {
    return {};
  }
  if (sa->sa_family == AF_INET) {
    const auto* in4 = reinterpret_cast<const struct sockaddr_in*>(sa);
    return {reinterpret_cast<const unsigned char*>(&in4->sin_addr.s_addr), 4};
  }
#if (NGX_HAVE_INET6)
  if (sa->sa_family == AF_INET6) {
    const auto* in6 = reinterpret_cast<const struct sockaddr_in6*>(sa);
    return {in6->sin6_addr.s6_addr, 16};
  }
#endif
  return {};
}

static void ngx_http_pagespeed_agent_relabel_restore(void* data) {
  auto* rl = static_cast<ngx_http_pagespeed_agent_relabel_t*>(data);
  if (rl->connection == nullptr) {
    return;
  }
  rl->connection->sockaddr = rl->saved_sockaddr;
  rl->connection->socklen = rl->saved_socklen;
  rl->connection->addr_text = rl->saved_addr_text;
}

static ngx_int_t ngx_http_pagespeed_agent_fetch_post_read_handler(
    ngx_http_request_t* r) {
  if (r != r->main || r->headers_in.user_agent == nullptr) {
    return NGX_DECLINED;
  }
  const ngx_str_t& ua = r->headers_in.user_agent->value;
  if (!pagespeed::IsAgentFetchUserAgent(
          std::string_view(reinterpret_cast<const char*>(ua.data), ua.len))) {
    return NGX_DECLINED;
  }
  ngx_connection_t* c = r->connection;
  // Resolves the address a wildcard listener accepted the connection on.
  const struct sockaddr* local =
      ngx_connection_local_sockaddr(c, nullptr, 0) == NGX_OK ? c->local_sockaddr
                                                             : nullptr;
  if (!pagespeed::AgentFetchPeerIsThisServer(
          ngx_http_pagespeed_addr_bytes(c->sockaddr),
          ngx_http_pagespeed_addr_bytes(local))) {
    return NGX_DECLINED;
  }
  ngx_pool_cleanup_t* cln =
      ngx_pool_cleanup_add(r->pool, sizeof(ngx_http_pagespeed_agent_relabel_t));
  if (cln == nullptr) {
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }
  auto* rl = static_cast<ngx_http_pagespeed_agent_relabel_t*>(cln->data);
  ngx_memzero(rl, sizeof(*rl));
  rl->connection = c;
  rl->saved_sockaddr = c->sockaddr;
  rl->saved_socklen = c->socklen;
  rl->saved_addr_text = c->addr_text;
  rl->addr.sin_family = AF_INET;
  ngx_memcpy(&rl->addr.sin_addr.s_addr, pagespeed::kAgentSelfFetchLabelBytes,
             sizeof(pagespeed::kAgentSelfFetchLabelBytes));
  static_assert(sizeof(rl->text) == pagespeed::kAgentSelfFetchLabel.size() + 1,
                "label text buffer matches the label");
  ngx_memcpy(rl->text, pagespeed::kAgentSelfFetchLabel.data(),
             pagespeed::kAgentSelfFetchLabel.size());
  c->sockaddr = reinterpret_cast<struct sockaddr*>(&rl->addr);
  c->socklen = sizeof(rl->addr);
  c->addr_text.data = rl->text;
  c->addr_text.len = pagespeed::kAgentSelfFetchLabel.size();
  cln->handler = ngx_http_pagespeed_agent_relabel_restore;
  return NGX_DECLINED;
}

// Module initialization
static ngx_int_t ngx_http_pagespeed_init(ngx_conf_t* cf) {
  ngx_http_handler_pt* h;
  ngx_http_core_main_conf_t* cmcf;

  cmcf = static_cast<ngx_http_core_main_conf_t*>(
      ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module));

  // Install the handler in the PRECONTENT phase, which runs after the ACCESS
  // phase: the server's access decisions (allow/deny, auth_basic,
  // auth_request and any other access module) are made before anything is
  // served from the cache.  nginx added this phase in 1.13.4; every supported
  // nginx has it.
  h = static_cast<ngx_http_handler_pt*>(
      ngx_array_push(&cmcf->phases[NGX_HTTP_PRECONTENT_PHASE].handlers));
  if (h == nullptr) {
    return NGX_ERROR;
  }
  *h = ngx_http_pagespeed_handler;

  // Experimental: install the RSL-CAP enforcement handler in the
  // PREACCESS phase so the token gate runs before the PRECONTENT
  // optimization handler and before content is served.  Default OFF — the
  // handler is a single bool check and NGX_DECLINED until the operator enables
  // enforcement.
  h = static_cast<ngx_http_handler_pt*>(
      ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers));
  if (h == nullptr) {
    return NGX_ERROR;
  }
  *h = ngx_http_pagespeed_rslcap_handler;

  // The worker's agent fetches arriving from this server present as a
  // non-local client to every access rule; see
  // ngx_http_pagespeed_agent_fetch_post_read_handler.
  h = static_cast<ngx_http_handler_pt*>(
      ngx_array_push(&cmcf->phases[NGX_HTTP_POST_READ_PHASE].handlers));
  if (h == nullptr) {
    return NGX_ERROR;
  }
  *h = ngx_http_pagespeed_agent_fetch_post_read_handler;

  // Install header filter (for X-PageSpeed: MISS on proxied responses)
  ngx_http_next_header_filter = ngx_http_top_header_filter;
  ngx_http_top_header_filter = ngx_http_pagespeed_header_filter;

  // Install body filter
  ngx_http_next_body_filter = ngx_http_top_body_filter;
  ngx_http_top_body_filter = ngx_http_pagespeed_body_filter;

  // Read the shared config early (once at startup) so the socket path, the
  // serve-side toggles and the URL-normalization config are live before the
  // first request; the 1s mtime poll takes over from here.
  {
    // Find a server conf with cache_path to derive the shared config path.
    ngx_http_conf_ctx_t* ctx_conf = static_cast<ngx_http_conf_ctx_t*>(cf->ctx);
    if (ctx_conf && ctx_conf->loc_conf) {
      auto* lc = static_cast<ngx_http_pagespeed_loc_conf_t*>(
          ctx_conf->loc_conf[ngx_http_pagespeed_module.ctx_index]);
      if (lc && lc->cache_path.len > 0) {
        std::string_view cache_path_sv(
            reinterpret_cast<const char*>(lc->cache_path.data),
            lc->cache_path.len);
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_shared_config = ReadSharedConfig(cache_path_sv, cf->log);
        RebuildUrlNormConfig(cache_path_sv, g_shared_config);
        ApplyCacheDirGeneration(cache_path_sv, nullptr);
      }
    }
  }

  // Log cache mode and TTL defaults at startup.  Reads from the http-level
  // loc_conf, so per-location overrides are not shown here.
  {
    ngx_http_conf_ctx_t* log_ctx = static_cast<ngx_http_conf_ctx_t*>(cf->ctx);
    if (log_ctx && log_ctx->loc_conf) {
      auto* log_lc = static_cast<ngx_http_pagespeed_loc_conf_t*>(
          log_ctx->loc_conf[ngx_http_pagespeed_module.ctx_index]);
      if (log_lc) {
        const char* mode_str;
        if (log_lc->cache_mode == 1) {
          mode_str = "aggressive";
        } else if (log_lc->cache_mode == NGX_CONF_UNSET_UINT) {
          mode_str = (g_shared_config.cache_mode == "aggressive")
                         ? "aggressive (from shared config)"
                         : "safe (default)";
        } else {
          mode_str = "safe";
        }
        ngx_log_error(
            NGX_LOG_NOTICE, cf->log, 0,
            "pagespeed: cache_mode=%s css_max_age=%i image_max_age=%i",
            mode_str, log_lc->css_max_age, log_lc->image_max_age);
      }
    }
  }

  ngx_log_error(NGX_LOG_NOTICE, cf->log, 0, "mod_pagespeed %s loaded",
                pagespeed::kPageSpeedVersion);

  return NGX_OK;
}
