// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// cache_path must be an absolute path for correct file placement.

#ifndef SRC_WORKER_SHARED_CONFIG_H_
#define SRC_WORKER_SHARED_CONFIG_H_

#ifdef _WIN32
typedef int pid_t;
#else
#include <sys/types.h>
#endif

#include <cstdint>
#include <string>
#include <string_view>

#include "src/worker/html_mode.h"

namespace pagespeed {

class MessageHandler;

// Schema version this build writes and is able to read.  A file declaring a
// HIGHER version is not parsed at all (see ParseSharedConfig).
//
// BUMP HAZARD: src/nginx/ngx_pagespeed_module.cc carries an independent
// inline parser whose version gate is hardcoded, not derived from this
// constant.  Bumping this makes the daemon write the new version while that
// mirror falls back to its compiled-in defaults (loudly, but still) — update
// the mirror in the same change.
//
// AND NOTE WHAT THE HAZARD IMPLIES ABOUT WHEN TO BUMP.  The gate is a
// reader-side "I cannot read this at all": a reader that sees a higher version
// discards the ENTIRE file — socket path, every serve toggle — and runs on
// compiled-in defaults.  That is the right answer for a schema an
// old reader would MISREAD, and the wrong answer for one it can simply skip.
// Adding an optional key is the second case: ParseSharedConfig ignores unknown
// keys by contract, so every already-deployed reader keeps working unchanged
// and a bump would instead degrade all of them to defaults in exchange for
// announcing a key they do not need.  So: bump for a change in the meaning or
// the required set of existing keys; do NOT bump to add an optional key.
//
// Removing an optional key is the same case in the other direction (2.1):
// the daemon stopped writing license_key / license_valid /
// license_checked_once.  A 2.0 reader that still knows those keys simply
// sees them missing and keeps its defaults (no key, not valid, not yet
// checked — the last of which suppresses its warning header), so the schema
// version stays at 1 and the two sides remain mutually readable.
inline constexpr int kSharedConfigVersion = 1;

// Cache-directory generation: the daemon and every peer that
// shares its cache resolve their default cache directory as
// /var/cache/pagespeed-optimizer/v<N> with N == kCacheDirGeneration.  The
// daemon publishes N in pagespeed-shared.conf (cache_dir_generation=) so a
// peer compiled against a different N fails the handshake LOUDLY instead of
// silently opening a cold, empty sibling directory.
//
// Bump N (and the default directory) when:
//   1. the ownership/identity model changes in a way that makes prior content
//      unusable by a new install (N started at 1 for the H1 privilege drop —
//      no installer may chown or migrate root-owned cache content, so the
//      drop cold-starts into v1/);
//   2. cyclone's VolumeHeader::kFormatVersionMajor bumps;
//   3. any future event ruled in by a design decision.
// N counts SHIPPED generations, so triggers that fire before the current N has
// been released collapse into it rather than each costing a bump.  2.1 is that
// case: the H1 privilege drop introduced v1 and the cyclone format major went
// 6 -> 7 in the same unreleased train, so both triggers are satisfied by the
// single 0 -> 1 move.  No released binary has ever resolved a v1 directory, so
// there is no peer to skew against and nothing in v1 to be made unusable.  A
// format major that bumps AFTER 2.1 ships takes N to 2.
//
// N = 2: cyclone's format major went 7 -> 8 (CRC-32C document checksums)
// after 2.1.0 shipped with v1, so trigger 2 fires against a released
// generation.  The new binary opens a new, empty volume in v2/ and never
// touches v1/ -- including v1/pagespeed-shared.conf, which 2.1.0 left saying
// generation 1.  What the handshake does and does not catch:
//   - a NEW peer (compiled for 2) whose configuration still names v1 reads
//     that stale file, sees 1, and refuses loudly;
//   - an OLD peer (compiled for 1) next to this daemon also reads v1/, sees
//     the matching 1, and attaches to a volume nobody writes any more: a
//     SILENT split.  The mod_pagespeed 1.1 packages declare an exact-version
//     Depends on pagespeed-optimizer, which is what prevents that pairing;
//     the 2.x nginx module from 2.1.0 checks nothing,
//     and the generation check added to it later covers only a new module
//     next to an old worker.
// N = 3: cyclone's format major went 8 -> 9 (the document checksum covers the
// fixed document header) after 2.4.0 shipped with v2, so trigger 2 fires
// against a released generation again.  The new binary opens a new, empty
// volume in v3/ and never touches v2/, including v2/pagespeed-shared.conf,
// which 2.4.0 left saying generation 2.  The handshake limits listed under
// N = 2 apply unchanged.
// Do NOT bump for entry-schema changes (those ride per-entry versioning
// inside the volume) or serve-stats layout changes (own namespace, own file).
inline constexpr int kCacheDirGeneration = 3;

// Keep in sync with inline SharedConfig in src/nginx/ngx_pagespeed_module.cc.
struct SharedConfig {
  // Reader-side fallback when no shared config was read; matches the
  // daemon's compiled default (WorkerConfig::socket_path).
  std::string socket_path = "/run/pagespeed-optimizer/notify.sock";
  // The worker's HTML mode.  On the wire as two keys: html_mode (optional,
  // added without a schema bump, see kSharedConfigVersion) and the legacy
  // disable_html, which the writer derives as "mode is off" so a reader that
  // predates html_mode keeps working.  The parser applies the precedence rule
  // of ResolveHtmlMode: a valid html_mode wins, whatever the line order;
  // otherwise disable_html decides; with neither, full.
  HtmlMode html_mode = kDefaultHtmlMode;
  // agent_optimize: the SERVE-side toggle nginx reads to gate
  // the same-URL markdown variant.  Since 2.1 this is simply the
  // operator's agent_optimize flag — there is no token entitlement behind it.
  // The wire key keeps its historical name (agent_optimize_entitled) so a
  // 2.0-era reader of this file keeps working unchanged.  Worker→nginx only;
  // OFF by default.
  bool agent_optimize_entitled = false;
  // agent_optimize /llms.txt: the SERVE-side toggle nginx reads to
  // decide whether to intercept /llms.txt.  AND of both operator flags
  // (agent_optimize && agent_optimize_llms_txt).  Worker→nginx only; OFF by
  // default.
  bool agent_optimize_llms_txt_enabled = false;
  // Web Bot Auth: the observe-only RFC 9421 verifier toggle
  // nginx reads to decide whether to classify requests.  OFF by default —
  // when false nginx does no signature work at all.  Worker→nginx only;
  // classification NEVER changes request handling (label + counter only).
  bool web_bot_auth = false;
  // Operator verified-bot registry, "keyid=name,keyid2=name2" (same format
  // as the 1.15 WebBotAuthVerifiedBots directive).  Parsed by nginx on each
  // shared-config reload; empty = no keyid is promoted past signed-agent.
  std::string web_bot_auth_verified_bots;
  // Comma-separated directory HOSTS (derived by the worker from the
  // configured key-directory URLs).  nginx resolves a signature's keyid
  // against the warmed key store under each of these hosts.  Operator
  // config only — never derived from request data.
  std::string web_bot_auth_directory_hosts;
  // Web Bot Auth opt-in counter (experimental): the mode nginx
  // reads to decide whether — and how — to expose the well-known counter
  // endpoint /.well-known/webbotauth-counter.  One of "off" (default;
  // endpoint invisible, 404), "private" (endpoint exists but only a valid
  // bearer token gets the exact doc; unauthenticated requests 404), or
  // "public" (unauthenticated requests get a COARSE bucketed doc; a valid
  // token gets the exact doc).  NON-secret operator config; the gating bearer
  // token is NOT here (it rides an env var — pagespeed-shared.conf is
  // group-readable by daemon+peers).  Worker→nginx only.
  std::string web_bot_auth_public_counter = "off";
  // RSL-CAP enforcement (experimental): the operator-gated toggle
  // nginx reads to decide whether to validate an `Authorization: License`
  // capability token and return 401/402/pass.  OFF by default — when false
  // nginx does NO token work at all (zero-cost fast path).  Worker→nginx only.
  bool rsl_cap_enforcement = false;
  // Comma-separated RSL-CAP issuer directory HOSTS (derived by the worker from
  // the configured RSL key-directory URLs), used as the realm-"rsl" store-key
  // host namespace for enforcement key lookups.  Operator config only.
  std::string rsl_cap_directory_hosts;
  // The license id and scope a token MUST grant to be authorized (empty fails
  // closed -> 402), and an optional issuer pin (empty = unbound).  Operator
  // config only — never derived from request data.
  std::string rsl_cap_requested_license;
  std::string rsl_cap_requested_scope;
  std::string rsl_cap_issuer;
  // The volume size the worker actually opened its cache with, in bytes.
  // 0 means NOT STATED — either an older worker wrote this file, or the
  // worker could not determine it.  Consumers must treat 0 as "unknown" and
  // must not substitute a default: the whole point of publishing this is that
  // guessing it is the failure mode.
  //
  // Why it is here at all: Cyclone derives the volume's on-disk FILENAME from
  // its geometry, and the size is the geometry input an operator sets.  A peer
  // that opens the same directory with a different size does not collide with
  // the worker — it silently creates and uses a DIFFERENT file, shares nothing,
  // and runs a permanently cold cache with no error on either side.  Publishing
  // the real number is what lets a peer inherit it instead of assuming one.
  // The C API exposes it through ps_read_shared_config_volume_size.
  uint64_t volume_size = 0;
  // The cache-directory generation the writer was compiled with
  // (kCacheDirGeneration).  0 means NOT STATED — a worker predating this
  // field wrote the file.  A peer whose compiled-in generation differs from
  // a non-zero value here must treat it as a handshake failure (loud log,
  // substrate down), never as a hint to go looking for another directory:
  // split-brain across generations is the failure this field exists to
  // prevent.
  int cache_dir_generation = 0;
  std::string cache_mode;              // "safe" or "aggressive" (empty = safe)
  std::string strip_query_extensions;  // Comma-separated, e.g., ".jpg,.png"
  std::string strip_query_params;  // Comma-separated, e.g., "utm_source,fbclid"

  bool operator==(const SharedConfig&) const = default;
};

// Returns path to pagespeed-shared.conf given the cache_path.
// Same parent directory logic as ConfigFilePath().
std::string SharedConfigFilePath(const std::string& cache_path);

// A shared-config file whose declared schema version this build cannot read.
//
// This is the ONE version-skew condition in the system that is loud, and it is
// loud on purpose.  Every other skew surface between components sharing a
// cache is silent by construction — a peer built against a different on-disk
// format opens a differently-named volume file and simply starts cold, which
// is safe (no corruption) but invisible.  The shared-config handshake is where
// two components that mean to talk to each other state a version to each
// other, so it is the only place a mismatch can be DETECTED at all.  Reporting
// it is therefore not optional politeness: it is the sole signal an operator
// gets that their components are out of step, and without it a mismatched
// install presents as a site that mysteriously never optimizes.
struct SharedConfigVersionSkew {
  bool mismatch = false;                         // Ever observed?
  int observed_version = 0;                      // What the file declared.
  int supported_version = kSharedConfigVersion;  // What this build reads.
  uint64_t observations = 0;                     // How many parses hit it.
};

// Process-wide, sticky snapshot of the above.  Sticky rather than
// last-parse-wins because the condition it reports is a deployment state, not
// an event: a mismatch that has been observed once stays true for the life of
// the process and remains visible on the diagnostic surfaces even if a later
// parse (of a different file, or after the file is fixed) succeeds.
// Thread-safe.
SharedConfigVersionSkew SharedConfigVersionSkewState();

// Clear the sticky state.  Tests only — there is no production reason to make
// an observed mismatch un-observed.
void ResetSharedConfigVersionSkewForTesting();

// Redirect the version-mismatch signal to a specific handler.  Passing
// nullptr restores the default, which writes to stderr.
//
// The default is what makes EVERY consumer loud without every consumer having
// to opt in: ParseSharedConfig is a pure function called from the worker, from
// the HTTP config path and from the embedding C API, and a signal that only
// fires for callers who remembered to pass a logger is
// exactly the silent path this replaces.  An in-tree host with its own log
// stream can route the message into it.  The embedding C API does not export
// this hook (the shared object's version script exposes ps_* only), so an
// embedder always gets the stderr default — which is the point: a host that
// does nothing still gets the signal.
// Not thread-safe against concurrent parses; call during setup or from a test.
void SetSharedConfigMessageHandler(MessageHandler* handler);

// Parse key=value content into SharedConfig.
// Unknown keys silently ignored. Missing keys keep defaults.
//
// A file declaring a version this build cannot read (> kSharedConfigVersion)
// is NOT parsed best-effort: the returned config is the compiled-in defaults,
// whatever the rest of the file said.  That fail direction is deliberate —
// half-reading a schema written by a newer peer would apply some of its
// settings and silently drop others, and a partly-applied serve-time
// configuration is worse than a known-default one.  The mismatch is recorded
// in SharedConfigVersionSkewState() and reported once per distinct version.
SharedConfig ParseSharedConfig(std::string_view content);

// Read and parse pagespeed-shared.conf from disk.
// Returns defaults if file missing or unreadable.
SharedConfig ReadSharedConfigFile(const std::string& path);

// Write pagespeed-shared.conf atomically (tmp + fsync + rename).
// Mode 0640 (owner+group: the daemon and its cache-sharing peers). Returns
// true on success. pid=0 uses getpid().
bool WriteSharedConfigFile(const std::string& path, const SharedConfig& config,
                           pid_t pid = 0);

}  // namespace pagespeed

#endif  // SRC_WORKER_SHARED_CONFIG_H_
