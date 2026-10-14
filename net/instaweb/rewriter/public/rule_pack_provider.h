// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_RULE_PACK_PROVIDER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_RULE_PACK_PROVIDER_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "pagespeed/kernel/packs/signature_format.h"

namespace pagespeed::packs {
struct Pack;
struct PackDecision;
struct PageUrl;
}  // namespace pagespeed::packs

namespace net_instaweb {

class MessageHandler;
class RewriteOptions;

// A parsed rule pack and the SHA-256 (lowercase hex) of the file bytes it was
// parsed from. `pack` is null when no usable pack is available.
struct LoadedRulePack {
  std::shared_ptr<const pagespeed::packs::Pack> pack;
  std::string sha256;
  std::string key_id;  // set when the signature verified
};

// What a provider knows about the pack file at one configured path, for the
// read-only admin view. Looking never loads or re-reads anything.
struct RulePackStatus {
  enum class State {
    kNone,                  // nothing loaded or tried in this process yet
    kLoaded,                // the file's current content is loaded
    kFailedKeepingEarlier,  // the last attempt failed; an earlier pack is kept
    kRefused,               // the last attempt failed; there is no pack
  };
  State state = State::kNone;
  LoadedRulePack loaded;              // the pack in use, if any
  int64_t last_event_ms = -1;         // last load or failed load; -1 if none
  std::string last_error;             // empty unless the last attempt failed
  std::vector<std::string> warnings;  // loader warnings of `loaded`
};

// Where the rule_packs filter gets its pack and sends its decisions: one per
// process, shared by every virtual host. Implementations are thread-safe.
class RulePackProvider {
 public:
  // What a Get() call did, so the caller can count it on its own (possibly
  // per virtual host) statistics.
  enum class LoadEvent {
    kNone,    // served what was already loaded (or already failed)
    kLoaded,  // parsed a new version of the file
    kFailed,  // tried to parse a new version of the file and failed
  };
  // What the signature check of a Get() found, reported once per verdict
  // (process x pack hash x sidecar x trust configuration), so the caller can
  // count it. kNone: nothing new to report.
  enum class SignatureEvent {
    kNone,
    kVerified,
    kRefusedUnsigned,
    kRefusedSignatureUnreadable,
    kRefusedPublisherMismatch,
    kRefusedUnknownKey,
    kRefusedBadSignature,
  };
  // What became of one decision line.
  enum class LogResult {
    kWritten,     // appended to the decision log
    kSuppressed,  // already written by this process (same key)
    kDropped,     // not written: the daily cap, or the file is not writable
    kDisabled,    // no decision log is configured
  };

  virtual ~RulePackProvider() = default;

  // The pack in the file at `path` (as configured), checked against `trust`.
  // Never blocks on the network; may read and parse the file. On a load error
  // the previous good pack of that file, if any, is kept. A pack that
  // `trust` requires to be signed and is not is refused: the pack accepted
  // earlier under the same trust configuration is returned, else a null pack.
  virtual LoadedRulePack Get(const std::string& path,
                             const pagespeed::packs::PackTrust& trust,
                             MessageHandler* handler, LoadEvent* event,
                             SignatureEvent* signature_event) = 0;

  // The state of the pack file at `path` (as configured), without loading it.
  virtual RulePackStatus Status(const std::string& path) const {
    (void)path;
    return RulePackStatus();
  }

  // True when the pack of the file at `path` (as configured) has a canonical
  // rule with `link_header` set, so the response headers may still be edited.
  // False when the file is known to have no such pack: it has no such rule,
  // or its load failed and no earlier pack is kept. True while the file has
  // not been looked at yet in this process (a pack is read on the first
  // document that needs it). Looking never loads anything.
  virtual bool PackEditsLinkHeader(const std::string& path) const {
    (void)path;
    return false;
  }

  // Records one decision of `pack` on the page `url`, under the decision log
  // that `options` configures. `now_ms` is the server clock.
  virtual LogResult LogDecision(const RewriteOptions& options,
                                const LoadedRulePack& pack,
                                const pagespeed::packs::PageUrl& url,
                                const pagespeed::packs::PackDecision& decision,
                                int64_t now_ms, MessageHandler* handler) = 0;

  // True the first time this process asks with `key`, false after: for the
  // messages and counters that move once per process (a refused duplicate
  // pack id, an overlap warning). Thread-safe.
  bool FirstTime(const std::string& key);

 private:
  std::mutex once_mutex_;
  std::set<std::string> once_keys_;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_RULE_PACK_PROVIDER_H_
