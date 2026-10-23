// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SYSTEM_RULE_PACK_STORE_H_
#define PAGESPEED_SYSTEM_RULE_PACK_STORE_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "net/instaweb/rewriter/public/rule_pack_provider.h"
#include "pagespeed/system/rule_pack_signature.h"

namespace net_instaweb {

class RulePackDecisionLog;
class Timer;

// The rule packs of one server process, and their decision logs.
//
// A pack is read lazily, on the first document that needs it in a process,
// so it is parsed by the serving process (after the server dropped its
// privileges), never by a parent process at configuration time.
//
// Packs are keyed by canonical path, so two virtual hosts that name the same
// file, by any path, share one parsed pack. Each entry remembers the file's
// modification time, size and SHA-256. A path is looked at again at most
// every kPackRecheckIntervalSec seconds: when the modification time or the
// size changed and the content hash differs, the file is parsed again. A
// file that fails to load keeps the previous good pack of that path (a
// process without one runs without the filter) and is reported once per
// version of the file. The documented way to apply a changed pack is a
// graceful restart or reload; the recheck makes a process converge within
// seconds when that is forgotten.
//
// The file work (resolving the path, stat, read, hash, parse) happens
// outside the store's lock. One thread refreshes an entry at a time; while
// it does, every other document keeps getting the pack the entry already
// has (none, before the first load of a path completes), so a slow file
// system or a large pack never holds up the documents of other hosts.
//
// Decision logs are kept per (log directory, pack id), under
// <LogDir>/packs/<pack-id>/. An empty LogDir disables the log, with one
// warning per process.
class RulePackStore : public RulePackProvider {
 public:
  static constexpr int64_t kPackRecheckIntervalSec = 10;
  // How many announced load lines are remembered before the record starts
  // over (which can repeat a line).
  static constexpr size_t kMaxAnnounced = 4096;

  // `timer` paces the recheck; it must outlive the store. `engine_version`
  // is what a pack's engine_min is compared against.
  RulePackStore(Timer* timer, std::string_view engine_version);
  ~RulePackStore() override;

  LoadedRulePack Get(const std::string& path,
                     const pagespeed::packs::PackTrust& trust,
                     MessageHandler* handler, LoadEvent* event,
                     SignatureEvent* signature_event) override;
  RulePackStatus Status(const std::string& path) const override;
  bool PackEditsLinkHeader(const std::string& path) const override;
  LogResult LogDecision(const RewriteOptions& options,
                        const LoadedRulePack& pack,
                        const pagespeed::packs::PageUrl& url,
                        const pagespeed::packs::PackDecision& decision,
                        int64_t now_ms, MessageHandler* handler) override;

  // The decision-log line of `decision`: the JSON object of
  // DecisionToJsonLine() with a leading timestamp ("ts", ms since the epoch),
  // the pack ("pack", id@version) and the first 16 hex digits of its
  // SHA-256 ("sha256").
  static std::string DecisionLine(
      const LoadedRulePack& pack, const pagespeed::packs::PageUrl& url,
      const pagespeed::packs::PackDecision& decision, int64_t now_ms);
  // The per-process dedup key of a decision line: host, path, rule, pack
  // hash, mode, action and reason.
  static std::string DedupKey(const LoadedRulePack& pack,
                              const pagespeed::packs::PageUrl& url,
                              const pagespeed::packs::PackDecision& decision);

  // Called by the refreshing thread, outside the lock, before the file is
  // looked at. For tests that hold a refresh open.
  void set_refresh_hook_for_testing(std::function<void()> hook) {
    refresh_hook_ = std::move(hook);
  }

  // Lowers the bound on the record of announced loads (at least 3), so a
  // test can cross its wraparound without loading kMaxAnnounced / 2 packs.
  void set_max_announced_for_testing(size_t max_announced) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_announced_ = max_announced;
  }

  size_t announced_size_for_testing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return announced_.size();
  }

 private:
  struct Entry {
    int64_t checked_ms = -1;  // when the file was last looked at
    bool refreshing = false;  // a thread is looking at the file
    bool has_stat = false;
    bool unreadable_reported = false;
    int64_t mtime = 0;
    int64_t size = -1;
    std::string sha256;          // of the content last read (good or bad)
    int64_t last_event_ms = -1;  // when a load last succeeded or failed
    std::string last_error;      // of the last attempt; empty if it worked
    LoadedRulePack loaded;
    // The bytes `loaded.pack` was parsed from, kept for verification.
    std::shared_ptr<const std::string> bytes;
    // The signature sidecar, <path>.sig, and its stat.
    bool sig_stat_known = false;
    bool sig_present = false;
    int64_t sig_mtime = 0;
    int64_t sig_size = -1;
    SidecarState sidecar;
    std::string sidecar_sha;  // "-" absent, "!" unreadable, else SHA-256
  };
  struct Verdict {
    pagespeed::packs::SignatureStatus status;
    std::string key_id;
  };
  struct Alias {
    int64_t resolved_ms = -1;
    std::string canonical;
  };

  // Brings `entry`, a copy of the entry of the file at canonical path
  // `path`, up to date. Runs without the lock and touches nothing else.
  LoadEvent Refresh(const std::string& path, Entry* entry,
                    MessageHandler* handler) const;

  // Looks at the signature sidecar of the file at canonical path `path`.
  void RefreshSidecar(const std::string& path, bool force, Entry* entry) const;

  // Which pack `trust` may use, given the latest `loaded` one; logs and
  // reports the signature verdict once.
  LoadedRulePack CheckSignature(
      const std::string& canonical, const pagespeed::packs::PackTrust& trust,
      LoadedRulePack loaded, const std::shared_ptr<const std::string>& bytes,
      const SidecarState& sidecar, const std::string& sidecar_sha,
      MessageHandler* handler, SignatureEvent* signature_event);

  Timer* timer_;
  const std::string engine_version_;

  std::function<void()> refresh_hook_;

  // Guards the maps and the set below; never held across file work.
  mutable std::mutex mutex_;
  std::map<std::string, Alias> aliases_;  // configured path -> canonical
  std::map<std::string, Entry> entries_;  // canonical path -> pack
  std::set<std::string> announced_;       // what was logged, once each
  size_t max_announced_ = kMaxAnnounced;  // guarded by mutex_
  // Signature verdicts per (pack hash, sidecar hash, trust configuration).
  std::map<std::string, Verdict> verdicts_;
  std::set<std::string> reported_;  // verdict keys already logged/counted
  // The pack accepted last per (canonical path, trust fingerprint).
  std::map<std::pair<std::string, std::string>, LoadedRulePack> accepted_;

  std::mutex log_mutex_;
  bool warned_no_log_dir_ = false;
  std::map<std::string, std::unique_ptr<RulePackDecisionLog>> logs_;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_RULE_PACK_STORE_H_
