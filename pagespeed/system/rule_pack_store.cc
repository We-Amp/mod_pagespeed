// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/rule_pack_store.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/pack_loader.h"
#include "pagespeed/kernel/packs/signature_format.h"
#include "pagespeed/kernel/packs/template.h"
#include "pagespeed/system/rule_pack_decision_log.h"
#include "pagespeed/system/system_rewrite_options.h"

// SHA-256 from BoringSSL, as in option_context.cc: the pack hash keys the
// decision-log dedup and names the pack in every line, so it must be a real
// hash on every platform.
#include <openssl/sha.h>

namespace net_instaweb {

namespace {

namespace fs = std::filesystem;

constexpr int64_t kRecheckMs = RulePackStore::kPackRecheckIntervalSec * 1000;

fs::path Utf8Path(const std::string& s) {
  return fs::path(std::u8string(s.begin(), s.end()));
}

std::string PathString(const fs::path& p) {
  const std::u8string u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
}

std::string Sha256Hex(std::string_view data) {
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const uint8_t*>(data.data()), data.size(), digest);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(size_t{2} * SHA256_DIGEST_LENGTH);
  for (uint8_t b : digest) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0xF]);
  }
  return out;
}

// A pack id is a single path component of the log directory.
bool IsSafePathComponent(std::string_view id) {
  if (id.empty() || id == "." || id == "..") return false;
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.';
    if (!ok) return false;
  }
  return true;
}

// Reads at most `max_bytes` + 1 bytes of `path`.
bool ReadBounded(const std::string& path, size_t max_bytes,
                 std::string* contents) {
  std::ifstream in(Utf8Path(path), std::ios::binary);
  if (!in) return false;
  contents->assign(max_bytes + 1, '\0');
  in.read(contents->data(), static_cast<std::streamsize>(contents->size()));
  if (in.bad()) return false;
  contents->resize(static_cast<size_t>(in.gcount()));
  return true;
}

std::string PagePath(const pagespeed::packs::PageUrl& url) {
  return url.query.empty() ? url.path : absl::StrCat(url.path, "?", url.query);
}

}  // namespace

RulePackStore::RulePackStore(Timer* timer, std::string_view engine_version)
    : timer_(timer), engine_version_(engine_version) {}

RulePackStore::~RulePackStore() = default;

RulePackProvider::LoadEvent RulePackStore::Refresh(
    const std::string& path, Entry* entry, MessageHandler* handler) const {
  const fs::path file = Utf8Path(path);
  std::error_code size_ec, time_ec;
  const uintmax_t size = fs::file_size(file, size_ec);
  const fs::file_time_type mtime = fs::last_write_time(file, time_ec);
  if (size_ec || time_ec) {
    const std::error_code& ec = size_ec ? size_ec : time_ec;
    // Report a missing or unreadable file once, until it can be read again.
    if (!entry->unreadable_reported) {
      entry->unreadable_reported = true;
      entry->has_stat = false;
      entry->sha256.clear();
      entry->last_error = absl::StrCat("cannot be read: ", ec.message());
      handler->Message(kWarning, "rule pack %s cannot be read: %s; %s",
                       path.c_str(), ec.message().c_str(),
                       entry->loaded.pack != nullptr
                           ? "keeping the pack loaded earlier"
                           : "the rule_packs filter is off for it");
      return LoadEvent::kFailed;
    }
    return LoadEvent::kNone;
  }
  entry->unreadable_reported = false;
  const int64_t mtime_ticks =
      static_cast<int64_t>(mtime.time_since_epoch().count());
  const bool pack_unchanged = entry->has_stat && entry->mtime == mtime_ticks &&
                              entry->size == static_cast<int64_t>(size);
  // The sidecar belongs to the pack that is loaded: it is looked at again
  // only while the file still holds that pack's content, and read together
  // with a successful load below. A new version that fails to load leaves
  // the loaded pack with its own sidecar.
  const bool file_is_loaded_pack = entry->sha256 == entry->loaded.sha256;
  if (pack_unchanged) {
    if (file_is_loaded_pack) RefreshSidecar(path, false, entry);
    return LoadEvent::kNone;
  }
  entry->has_stat = true;
  entry->mtime = mtime_ticks;
  entry->size = static_cast<int64_t>(size);

  std::string contents;
  std::string error;
  if (!ReadBounded(path, pagespeed::packs::kMaxPackFileBytes, &contents)) {
    error = "cannot be read";
  }
  const std::string sha = Sha256Hex(contents);
  if (error.empty() && sha == entry->sha256) {
    // Touched, not changed.
    if (file_is_loaded_pack) RefreshSidecar(path, false, entry);
    return LoadEvent::kNone;
  }
  entry->sha256 = sha;
  if (error.empty() && contents.size() > pagespeed::packs::kMaxPackFileBytes) {
    error = absl::StrCat("is larger than ", pagespeed::packs::kMaxPackFileBytes,
                         " bytes");
  }
  absl::StatusOr<pagespeed::packs::Pack> pack;
  if (error.empty()) {
    pack = pagespeed::packs::LoadPack(contents, engine_version_, path);
    if (!pack.ok()) error = std::string(pack.status().message());
  }
  if (!error.empty()) {
    entry->last_error = error;
    handler->Message(
        kWarning, "rule pack %s (sha256=%s) failed to load: %s; %s",
        path.c_str(), sha.c_str(), error.c_str(),
        entry->loaded.pack != nullptr ? "keeping the pack loaded earlier"
                                      : "the rule_packs filter is off for it");
    return LoadEvent::kFailed;
  }
  entry->loaded.pack =
      std::make_shared<const pagespeed::packs::Pack>(*std::move(pack));
  entry->loaded.sha256 = sha;
  entry->last_error.clear();
  entry->bytes = std::make_shared<const std::string>(std::move(contents));
  RefreshSidecar(path, true, entry);
  return LoadEvent::kLoaded;
}

// Looks at <path>.sig, next to the resolved pack file. Re-reads it when its
// existence, modification time or size changed, or when `force` (the pack
// itself is read again).
void RulePackStore::RefreshSidecar(const std::string& path, bool force,
                                   Entry* entry) const {
  const fs::path file = Utf8Path(path + ".sig");
  // Anything at that name counts as a sidecar (a dangling symlink, a
  // directory too); one that is not a readable regular file is "signature
  // unreadable", never "unsigned".
  std::error_code type_ec, size_ec, time_ec, regular_ec;
  const fs::file_status status = fs::symlink_status(file, type_ec);
  const bool present = status.type() != fs::file_type::not_found;
  const bool regular = present && fs::is_regular_file(file, regular_ec);
  const uintmax_t size = regular ? fs::file_size(file, size_ec) : 0;
  const fs::file_time_type mtime =
      regular ? fs::last_write_time(file, time_ec) : fs::file_time_type();
  const int64_t mtime_ticks =
      regular && !time_ec
          ? static_cast<int64_t>(mtime.time_since_epoch().count())
          : 0;
  const int64_t size_bytes =
      regular && !size_ec ? static_cast<int64_t>(size) : -1;
  if (!force && entry->sig_stat_known && entry->sig_present == present &&
      entry->sig_mtime == mtime_ticks && entry->sig_size == size_bytes) {
    return;
  }
  entry->sig_stat_known = true;
  entry->sig_present = present;
  entry->sig_mtime = mtime_ticks;
  entry->sig_size = size_bytes;
  entry->sidecar = SidecarState();
  entry->sidecar.present = present;
  entry->sidecar_sha = "-";
  if (!present || !regular) {
    entry->sidecar_sha = present ? "!" : "-";
    return;
  }
  std::string text;
  if (ReadBounded(path + ".sig", pagespeed::packs::kMaxSidecarBytes, &text) &&
      text.size() <= pagespeed::packs::kMaxSidecarBytes) {
    entry->sidecar.read_ok = true;
    entry->sidecar_sha = Sha256Hex(text);
    entry->sidecar.text = std::move(text);
  } else {
    entry->sidecar_sha = "!";
  }
}

LoadedRulePack RulePackStore::Get(const std::string& path,
                                  const pagespeed::packs::PackTrust& trust,
                                  MessageHandler* handler, LoadEvent* event,
                                  SignatureEvent* signature_event) {
  LoadEvent ev = LoadEvent::kNone;
  if (signature_event != nullptr) *signature_event = SignatureEvent::kNone;
  const int64_t now = timer_->NowMs();

  // Which file: resolved again at most every recheck interval. Two threads
  // may resolve the same path at once; both get the same answer.
  std::string canonical;
  bool resolve;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const Alias& alias = aliases_[path];
    resolve = alias.resolved_ms < 0 || now - alias.resolved_ms >= kRecheckMs;
    canonical = alias.canonical;
  }
  if (resolve) {
    std::error_code ec;
    const fs::path resolved = fs::canonical(Utf8Path(path), ec);
    canonical = ec ? path : PathString(resolved);
    std::lock_guard<std::mutex> lock(mutex_);
    Alias& alias = aliases_[path];
    alias.resolved_ms = now;
    alias.canonical = canonical;
  }

  // Whether this thread looks at the file. Every other thread is served the
  // pack the entry has meanwhile.
  LoadedRulePack result;
  Entry work;
  bool refresh = false;
  std::shared_ptr<const std::string> bytes;
  SidecarState sidecar;
  std::string sidecar_sha;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& entry = entries_[canonical];
    if (!entry.refreshing &&
        (entry.checked_ms < 0 || now - entry.checked_ms >= kRecheckMs)) {
      entry.refreshing = true;
      entry.checked_ms = now;
      work = entry;
      refresh = true;
    } else {
      result = entry.loaded;
      bytes = entry.bytes;
      sidecar = entry.sidecar;
      sidecar_sha = entry.sidecar_sha;
    }
  }
  if (refresh) {
    if (refresh_hook_) refresh_hook_();
    ev = Refresh(canonical, &work, handler);
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& entry = entries_[canonical];
    work.checked_ms = entry.checked_ms;
    work.refreshing = false;
    if (ev != LoadEvent::kNone) work.last_event_ms = now;
    entry = work;
    result = entry.loaded;
    bytes = entry.bytes;
    sidecar = entry.sidecar;
    sidecar_sha = entry.sidecar_sha;
  }
  if (event != nullptr) *event = ev;
  if (result.pack == nullptr) return result;
  return CheckSignature(canonical, trust, std::move(result), bytes, sidecar,
                        sidecar_sha, handler, signature_event);
}

// The signature stage of Get(): which pack this trust configuration may use.
// Verification runs outside the lock, once per verdict (pack hash x sidecar
// x trust configuration); the thread whose verdict insert succeeds logs and
// reports it, so a race costs one extra verification and never a second
// line.
LoadedRulePack RulePackStore::CheckSignature(
    const std::string& canonical, const pagespeed::packs::PackTrust& trust,
    LoadedRulePack loaded, const std::shared_ptr<const std::string>& bytes,
    const SidecarState& sidecar, const std::string& sidecar_sha,
    MessageHandler* handler, SignatureEvent* signature_event) {
  using pagespeed::packs::SignatureStatus;
  const std::string fingerprint = trust.Fingerprint();
  const std::string verdict_key =
      absl::StrCat(loaded.sha256, "|", sidecar_sha, "|", fingerprint);
  Verdict verdict;
  bool found;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = verdicts_.find(verdict_key);
    found = it != verdicts_.end();
    if (found) verdict = it->second;
  }
  if (!found) {
    verdict.status = bytes == nullptr ? SignatureStatus::kUnsigned
                                      : CheckPackSignature(
                                            *bytes, loaded.pack->info.publisher,
                                            sidecar, trust, &verdict.key_id);
    std::lock_guard<std::mutex> lock(mutex_);
    if (verdicts_.size() > 256) verdicts_.clear();
    verdicts_.emplace(verdict_key, verdict);
  }
  // The thread whose insert succeeds reports the verdict. The set of reported
  // verdicts is bounded apart from the cache above, so an eviction of a
  // cached verdict does not report it again; past kMaxReported distinct
  // verdicts (many trust configurations, or many pack versions) it starts
  // over, which can repeat a line.
  bool first;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    constexpr size_t kMaxReported = 4096;
    if (reported_.size() >= kMaxReported) reported_.clear();
    first = reported_.insert(verdict_key).second;
  }

  const bool verified = verdict.status == SignatureStatus::kVerified;
  const pagespeed::packs::Pack& p = *loaded.pack;
  const std::string phrase(
      pagespeed::packs::SignatureStatusPhrase(verdict.status));
  bool log_signed = false, log_unsigned = false, log_meta = false;
  bool log_not_verified = false, log_refused = false;
  LoadedRulePack accepted;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = std::make_pair(canonical, fingerprint);
    if (verified || !trust.require_signed) {
      if (verified) loaded.key_id = verdict.key_id;
      LoadedRulePack& slot = accepted_[key];
      if (slot.pack != loaded.pack || slot.key_id != loaded.key_id) {
        slot = loaded;
      }
      accepted = loaded;
      // Bounded like reported_: one load adds at most three lines here.
      if (announced_.size() > kMaxAnnounced - 3) announced_.clear();
      log_signed =
          verified &&
          announced_.insert(absl::StrCat(loaded.sha256, "|signed")).second;
      log_unsigned =
          !verified &&
          announced_.insert(absl::StrCat(loaded.sha256, "|unsigned")).second;
      log_meta = (log_signed || log_unsigned) &&
                 announced_.insert(absl::StrCat(loaded.sha256, "|meta")).second;
      log_not_verified =
          !verified && verdict.status != SignatureStatus::kUnsigned &&
          announced_.insert(absl::StrCat(loaded.sha256, "|nv|", phrase)).second;
    } else {
      auto it = accepted_.find(key);
      if (it != accepted_.end()) accepted = it->second;
      log_refused = first;
    }
  }
  if (handler == nullptr) {
    log_signed = log_unsigned = log_meta = log_not_verified = log_refused =
        false;
  }
  if (log_signed) {
    handler->Message(
        kInfo, "rule pack loaded signed: %s@%s key=%s sha256=%s path=%s",
        p.info.id.c_str(), p.info.version.c_str(), loaded.key_id.c_str(),
        loaded.sha256.c_str(), canonical.c_str());
  }
  if (log_unsigned) {
    handler->Message(kWarning,
                     "rule pack loaded unsigned: %s@%s sha256=%s path=%s",
                     p.info.id.c_str(), p.info.version.c_str(),
                     loaded.sha256.c_str(), canonical.c_str());
  }
  if (log_not_verified) {
    handler->Message(
        kWarning,
        "rule pack signature not verified (%s): %s@%s sha256=%s path=%s",
        phrase.c_str(), p.info.id.c_str(), p.info.version.c_str(),
        loaded.sha256.c_str(), canonical.c_str());
  }
  if (log_meta) {
    for (const std::string& warning : p.warnings) {
      handler->Message(kWarning, "rule pack %s: %s", canonical.c_str(),
                       warning.c_str());
    }
    if (p.info.license.has_value()) {
      const auto& l = *p.info.license;
      auto or_none = [](const std::string& v) {
        return v.empty() ? "none" : v.c_str();
      };
      handler->Message(
          kInfo,
          "rule pack license: %s@%s entitlement=%s expires=%s terms=%s "
          "sha256=%s path=%s",
          p.info.id.c_str(), p.info.version.c_str(), or_none(l.entitlement),
          or_none(l.expires), or_none(l.terms), loaded.sha256.c_str(),
          canonical.c_str());
    }
  }
  if (log_refused) {
    handler->Message(
        kWarning, "rule pack refused (%s): %s@%s sha256=%s path=%s; %s",
        phrase.c_str(), p.info.id.c_str(), p.info.version.c_str(),
        loaded.sha256.c_str(), canonical.c_str(),
        accepted.pack != nullptr ? "keeping the pack loaded earlier"
                                 : "the rule_packs filter is off for it");
  }
  if (signature_event != nullptr && first) {
    switch (verdict.status) {
      case SignatureStatus::kVerified:
        *signature_event = SignatureEvent::kVerified;
        break;
      case SignatureStatus::kUnsigned:
        if (trust.require_signed) {
          *signature_event = SignatureEvent::kRefusedUnsigned;
        }
        break;
      case SignatureStatus::kUnreadable:
        if (trust.require_signed) {
          *signature_event = SignatureEvent::kRefusedSignatureUnreadable;
        }
        break;
      case SignatureStatus::kPublisherMismatch:
        if (trust.require_signed) {
          *signature_event = SignatureEvent::kRefusedPublisherMismatch;
        }
        break;
      case SignatureStatus::kUnknownKey:
        if (trust.require_signed) {
          *signature_event = SignatureEvent::kRefusedUnknownKey;
        }
        break;
      case SignatureStatus::kBadSignature:
        if (trust.require_signed) {
          *signature_event = SignatureEvent::kRefusedBadSignature;
        }
        break;
    }
  }
  return accepted;
}

RulePackStatus RulePackStore::Status(const std::string& path) const {
  RulePackStatus status;
  std::lock_guard<std::mutex> lock(mutex_);
  std::string canonical = path;
  const auto alias = aliases_.find(path);
  if (alias != aliases_.end()) canonical = alias->second.canonical;
  const auto it = entries_.find(canonical);
  if (it == entries_.end()) return status;
  const Entry& entry = it->second;
  status.loaded = entry.loaded;
  status.last_event_ms = entry.last_event_ms;
  status.last_error = entry.last_error;
  if (!entry.last_error.empty()) {
    status.state = entry.loaded.pack != nullptr
                       ? RulePackStatus::State::kFailedKeepingEarlier
                       : RulePackStatus::State::kRefused;
  } else if (entry.loaded.pack != nullptr) {
    status.state = RulePackStatus::State::kLoaded;
  }
  if (entry.loaded.pack != nullptr)
    status.warnings = entry.loaded.pack->warnings;
  return status;
}

bool RulePackStore::PackEditsLinkHeader(const std::string& path) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string canonical = path;
  const auto alias = aliases_.find(path);
  if (alias != aliases_.end()) canonical = alias->second.canonical;
  const auto it = entries_.find(canonical);
  if (it == entries_.end()) return true;  // not looked at yet
  const Entry& entry = it->second;
  // The first load is under way: nothing loaded or failed yet.
  if (entry.loaded.pack == nullptr && entry.last_event_ms < 0) return true;
  if (entry.loaded.pack != nullptr && entry.loaded.pack->has_link_header_rule) {
    return true;
  }
  // A trust configuration that refused the newest file keeps serving the pack
  // it accepted earlier, which may still edit the header.
  for (const auto& slot : accepted_) {
    if (slot.first.first == canonical && slot.second.pack != nullptr &&
        slot.second.pack->has_link_header_rule) {
      return true;
    }
  }
  return false;
}

std::string RulePackStore::DecisionLine(
    const LoadedRulePack& pack, const pagespeed::packs::PageUrl& url,
    const pagespeed::packs::PackDecision& decision, int64_t now_ms) {
  const std::string body =
      pagespeed::packs::DecisionToJsonLine(url.host, PagePath(url), decision);
  const pagespeed::packs::PackInfo& info = pack.pack->info;
  return absl::StrCat("{\"ts\":", now_ms, ",\"pack\":\"",
                      pagespeed::packs::EscapeJsonString(
                          absl::StrCat(info.id, "@", info.version)),
                      "\",\"sha256\":\"", pack.sha256.substr(0, 16), "\",",
                      std::string_view(body).substr(1));
}

std::string RulePackStore::DedupKey(
    const LoadedRulePack& pack, const pagespeed::packs::PageUrl& url,
    const pagespeed::packs::PackDecision& decision) {
  // NUL-separated: no field can contain a NUL that would make two different
  // tuples collide.
  const char sep = '\0';
  return absl::StrCat(
      url.host, std::string_view(&sep, 1), PagePath(url),
      std::string_view(&sep, 1), decision.rule_id, std::string_view(&sep, 1),
      pack.sha256, std::string_view(&sep, 1),
      pagespeed::packs::ModeName(decision.mode), std::string_view(&sep, 1),
      pagespeed::packs::ActionName(decision.action), std::string_view(&sep, 1),
      pagespeed::packs::ReasonName(decision.reason), std::string_view(&sep, 1),
      decision.header ? "h" : "");
}

RulePackProvider::LogResult RulePackStore::LogDecision(
    const RewriteOptions& options, const LoadedRulePack& pack,
    const pagespeed::packs::PageUrl& url,
    const pagespeed::packs::PackDecision& decision, int64_t now_ms,
    MessageHandler* handler) {
  if (pack.pack == nullptr) return LogResult::kDisabled;
  const SystemRewriteOptions* system_options =
      SystemRewriteOptions::DynamicCast(&options);
  const std::string log_dir =
      system_options == nullptr ? std::string() : system_options->log_dir();
  const std::string& id = pack.pack->info.id;
  RulePackDecisionLog* log = nullptr;
  {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (log_dir.empty()) {
      if (!warned_no_log_dir_) {
        warned_no_log_dir_ = true;
        handler->Message(kWarning,
                         "rule pack decision log is off: LogDir is not set "
                         "(statistics are still kept)");
      }
      return LogResult::kDisabled;
    }
    if (!IsSafePathComponent(id)) return LogResult::kDisabled;
    const std::string dir = PathString(Utf8Path(log_dir) / "packs" / id);
    std::unique_ptr<RulePackDecisionLog>& slot = logs_[dir];
    if (slot == nullptr) slot = std::make_unique<RulePackDecisionLog>(dir);
    log = slot.get();
  }
  switch (log->Write(DedupKey(pack, url, decision),
                     DecisionLine(pack, url, decision, now_ms), now_ms,
                     handler)) {
    case RulePackDecisionLog::Result::kWritten:
      return LogResult::kWritten;
    case RulePackDecisionLog::Result::kDuplicate:
      return LogResult::kSuppressed;
    case RulePackDecisionLog::Result::kDropped:
      return LogResult::kDropped;
  }
  return LogResult::kDropped;
}

}  // namespace net_instaweb
