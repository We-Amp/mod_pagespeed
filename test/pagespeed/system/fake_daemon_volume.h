// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// An in-memory stand-in for the optimizer's shared volume, as a DaemonAbi:
// sited entries by exact id, the header block (sidecar) with its pairing
// hash, the per-site removal, and the per-site purge epoch with the
// library's max-merge and its refusal of a future value.  It decides no
// policy; the HTML serve set's verdicts come from the injected-verdict fake
// layered over it (injected_verdict_daemon_abi.h).
//
// Every read result is a heap copy of the entry, so a result stays valid
// after the entry is removed, as a peer's read result does.

#ifndef TEST_PAGESPEED_SYSTEM_FAKE_DAEMON_VOLUME_H_
#define TEST_PAGESPEED_SYSTEM_FAKE_DAEMON_VOLUME_H_

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_abi.h"

namespace net_instaweb {

struct FakeVolumeEntry {
  GoogleString body;
  uint8_t alternate_id = 0;
  uint32_t inserted_at = 0;
  uint32_t max_age = 0;
  uint16_t cc_flags = 0;
  uint8_t flags = 0;
  int content_type = kPsContentHtml;
  GoogleString origin_ct = "text/html; charset=utf-8";
  bool has_hash = false;
  uint8_t hash[kPsHtmlHashBytes] = {};
  // A withheld marker's reason (kPsHtmlWithheld*).
  int withheld_reason = kPsHtmlWithheldBelowFloor;
};

struct FakeVolumeSidecar {
  std::vector<std::pair<GoogleString, GoogleString>> lines;
  bool has_hash = false;
  uint8_t hash[kPsHtmlHashBytes] = {};
  // The header block format: 3 carries the policy stamp (C API 1.15).
  int format = 2;
  uint64_t policy_stamp = 0;
};

class FakeDaemonVolume : public DaemonAbi {
 public:
  // Unix seconds the epoch writer compares a future value against.
  void set_now_seconds(uint64_t now) { now_seconds_ = now; }

  static GoogleString Key(const char* site, const char* url) {
    return StrCat(site == nullptr ? "" : site, "\n", url == nullptr ? "" : url);
  }
  // The real volume keys an entry by its URL path, host name, scheme and
  // site.  Tests name an entry by "scheme://host/path": a path is keyed
  // under that form, so both name the same entry.
  static GoogleString EntryUrl(const char* url, const char* hostname,
                               const char* scheme) {
    if (url == nullptr || url[0] != '/') return url == nullptr ? "" : url;
    return StrCat(scheme == nullptr ? "" : scheme, "://",
                  hostname == nullptr ? "" : hostname, url);
  }
  // The volume takes a URL path ("/..."), never a full URL: a call with
  // anything else is refused, as a misuse.
  static bool IsPath(const char* url) { return url != nullptr && url[0] == '/'; }
  static GoogleString Key(const char* site, const char* url,
                          const char* hostname, const char* scheme) {
    return Key(site, EntryUrl(url, hostname, scheme).c_str());
  }

  // The volume, for a test to fill and inspect.
  std::map<std::pair<GoogleString, uint8_t>, FakeVolumeEntry>& entries() {
    return entries_;
  }
  std::map<GoogleString, FakeVolumeSidecar>& sidecars() { return sidecars_; }
  std::map<GoogleString, uint64_t>& epochs() { return epochs_; }

  const FakeVolumeEntry* Find(StringPiece site, StringPiece url,
                              uint8_t id) const {
    auto it = entries_.find({StrCat(site, "\n", url), id});
    return it == entries_.end() ? nullptr : &it->second;
  }

  int VersionMajor() const override { return 1; }
  int VersionMinor() const override { return 14; }
  void CacheConfigInit(PsCacheConfig* c) const override {
    *c = PsCacheConfig();
    c->struct_size = sizeof(*c);
  }
  bool HasSizedCacheConfigInit() const override { return true; }
  uint64_t SharedConfigVolumeSize(const char*) const override { return 0; }
  bool PublishesVolumeSize() const override { return false; }
  uint32_t SharedConfigGeneration(const char*) const override { return 0; }
  bool PublishesGeneration() const override { return false; }
  int CacheOpen(const PsCacheConfig*, void** out) const override {
    *out = const_cast<FakeDaemonVolume*>(this);
    return kPsOk;
  }
  void CacheClose(void*) const override {}
  const char* StrError(int) const override { return "fake error"; }
  const char* LastErrorMessage() const override { return ""; }
  void WriteParamsInit(PsWriteParams* p) const override {
    *p = PsWriteParams();
    p->struct_size = sizeof(*p);
  }
  void NotifyParamsInit(PsNotifyParams* p) const override {
    *p = PsNotifyParams();
    p->struct_size = sizeof(*p);
  }

  int CacheWriteOriginalSite(void*, const char* url, const char* hostname,
                             const char* scheme, const char* site,
                             const PsWriteParams* params,
                             void** out) const override {
    if (!IsPath(url)) return kPsErrInvalidArg;
    auto* w = new PendingWrite;
    w->key = Key(site, url, hostname, scheme);
    w->entry.alternate_id = params->alternate_id;
    w->entry.inserted_at = params->cache_inserted_at;
    w->entry.max_age = params->origin_max_age;
    w->entry.cc_flags = params->origin_cc_flags;
    w->entry.flags = params->flags;
    w->entry.content_type = params->content_type;
    if (params->origin_ct != nullptr) w->entry.origin_ct = params->origin_ct;
    if (params->struct_size >= sizeof(PsWriteParams) &&
        params->origin_html_hash != nullptr) {
      w->entry.has_hash = true;
      memcpy(w->entry.hash, params->origin_html_hash, kPsHtmlHashBytes);
    }
    *out = w;
    return kPsOk;
  }
  int WriteData(void* h, const void* d, size_t n) const override {
    static_cast<PendingWrite*>(h)->entry.body.append(
        static_cast<const char*>(d), n);
    return kPsOk;
  }
  int WriteClose(void* h) const override {
    std::unique_ptr<PendingWrite> w(static_cast<PendingWrite*>(h));
    // A durable-original write clears the key's withheld marker.
    if (w->entry.alternate_id == kPsSentinelOriginal) {
      entries_.erase({w->key, kPsSentinelMarkdownWithheld});
    }
    entries_[{w->key, w->entry.alternate_id}] = w->entry;
    ++writes_closed_;
    return kPsOk;
  }
  void WriteAbort(void* h) const override {
    delete static_cast<PendingWrite*>(h);
  }
  int writes_closed() const { return writes_closed_; }

  int VaryUncacheable(const char*) const override { return 0; }
  int VaryVariesAccept(const char*) const override { return 0; }
  int ParseCacheControl(const char*, PsCacheControl* out) const override {
    *out = PsCacheControl();
    return kPsOk;
  }
  uint32_t AgeAdjustedInsertTime(uint32_t now, uint32_t age) const override {
    return age > now ? 0 : now - age;
  }
  uint32_t Classify(const char*, const char*, const char*,
                    const char*) const override {
    return 0;
  }
  int ClassifyContentType(const char*) const override { return kPsContentHtml; }
  int OptionContextSignature(const char*, size_t, char*,
                             size_t) const override {
    return kPsErrInvalidArg;
  }
  int NotifyWorker(const char*, const PsNotifyParams*) const override {
    return notify_rc;
  }
  // What NotifyWorker answers.
  int notify_rc = kPsOk;
  int CacheReadBestSite(void*, const char*, const char*, const char*,
                        const char*, uint32_t, void** out) const override {
    *out = nullptr;
    return kPsErrNotFound;
  }
  int CacheReadAlternateSite(void*, const char* url, const char* hostname,
                             const char* scheme, const char* site, uint8_t id,
                             void** out) const override {
    if (!IsPath(url)) {
      *out = nullptr;
      return kPsErrInvalidArg;
    }
    auto it = entries_.find({Key(site, url, hostname, scheme), id});
    if (it == entries_.end()) {
      *out = nullptr;
      return kPsErrNotFound;
    }
    *out = new FakeVolumeEntry(it->second);
    return kPsOk;
  }
  int ReadContent(const void* r, const uint8_t** d, size_t* n) const override {
    const auto* e = static_cast<const FakeVolumeEntry*>(r);
    *d = reinterpret_cast<const uint8_t*>(e->body.data());
    *n = e->body.size();
    return kPsOk;
  }
  uint32_t ReadMask(const void* r) const override {
    return static_cast<const FakeVolumeEntry*>(r)->alternate_id;
  }
  uint8_t ReadFlags(const void* r) const override {
    return static_cast<const FakeVolumeEntry*>(r)->flags;
  }
  int ReadContentType(const void* r) const override {
    return static_cast<const FakeVolumeEntry*>(r)->content_type;
  }
  const char* ReadOriginContentType(const void* r) const override {
    return static_cast<const FakeVolumeEntry*>(r)->origin_ct.c_str();
  }
  uint32_t ReadCacheInsertedAt(const void* r) const override {
    return static_cast<const FakeVolumeEntry*>(r)->inserted_at;
  }
  uint32_t ReadOriginMaxAge(const void* r) const override {
    return static_cast<const FakeVolumeEntry*>(r)->max_age;
  }
  uint32_t ReadOriginSMaxAge(const void*) const override { return 0; }
  uint16_t ReadOriginCcFlags(const void* r) const override {
    return static_cast<const FakeVolumeEntry*>(r)->cc_flags;
  }
  uint32_t ReadOriginLastModified(const void*) const override { return 0; }
  const char* ReadOriginEtag(const void*) const override { return nullptr; }
  const uint8_t* ReadOriginHtmlHash(const void* r) const override {
    const auto* e = static_cast<const FakeVolumeEntry*>(r);
    return e->has_hash ? e->hash : nullptr;
  }
  int ReadIsWorkerProcessed(const void*) const override { return 0; }
  uint32_t ReadOriginContentLength(const void* r) const override {
    return static_cast<uint32_t>(
        static_cast<const FakeVolumeEntry*>(r)->body.size());
  }
  void ReadFree(void* r) const override {
    delete static_cast<FakeVolumeEntry*>(r);
  }
  int EvaluateFreshness(const PsFreshnessInput*, const PsFreshnessConfig*,
                        PsFreshnessResult*) const override {
    return kPsErrInvalidArg;
  }
  int BuildCacheControl(const PsCacheControlInput*, char*, size_t, size_t*,
                        uint32_t*) const override {
    return kPsErrInvalidArg;
  }
  int ServeStatsOpen(const char*, void** out) const override {
    *out = nullptr;
    return kPsErrNotFound;
  }
  void ServeStatsRecordServeClass(void*, int, uint32_t) const override {}
  void ServeStatsRecordHit(void*, int, uint64_t, uint64_t,
                           uint32_t) const override {}
  void ServeStatsClose(void*) const override {}

  // The volume side of the HTML serve set.
  bool HasHtmlServeSet() const override { return true; }
  int CacheWriteHeadersSidecarSite(void*, const PsSidecarWriteParams* params,
                                   const char* const* names,
                                   const char* const* values, size_t count,
                                   int* out_verdict,
                                   int* out_stored) const override {
    FakeVolumeSidecar s;
    for (size_t i = 0; i < count; ++i)
      s.lines.emplace_back(names[i], values[i]);
    if (params->origin_html_hash != nullptr) {
      s.has_hash = true;
      memcpy(s.hash, params->origin_html_hash, kPsHtmlHashBytes);
    }
    if (params->struct_size >= sizeof(PsSidecarWriteParams) &&
        (params->write_flags & kPsSidecarWritePolicyBlock) != 0) {
      s.format = 3;
      s.policy_stamp = params->policy_stamp;
    }
    if (!IsPath(params->url)) return kPsErrInvalidArg;
    sidecars_[Key(params->site, params->url, params->hostname,
                  params->scheme)] = s;
    if (out_verdict != nullptr) *out_verdict = 0;
    if (out_stored != nullptr) *out_stored = 1;
    return kPsOk;
  }
  int HeadersSidecarRead(void*, const char* url, const char* hostname,
                         const char* scheme, const char* site,
                         void** out) const override {
    if (!IsPath(url)) {
      *out = nullptr;
      return kPsErrInvalidArg;
    }
    auto it = sidecars_.find(Key(site, url, hostname, scheme));
    if (it == sidecars_.end()) {
      *out = nullptr;
      return kPsErrNotFound;
    }
    *out = new FakeVolumeSidecar(it->second);
    return kPsOk;
  }
  size_t HeaderBlockCount(const void* b) const override {
    return b == nullptr
               ? 0
               : static_cast<const FakeVolumeSidecar*>(b)->lines.size();
  }
  int HeaderBlockGet(const void* b, size_t i, const char** name,
                     size_t* name_len, const char** value,
                     size_t* value_len) const override {
    const auto* s = static_cast<const FakeVolumeSidecar*>(b);
    if (s == nullptr || i >= s->lines.size()) return kPsErrInvalidArg;
    *name = s->lines[i].first.data();
    *name_len = s->lines[i].first.size();
    *value = s->lines[i].second.data();
    *value_len = s->lines[i].second.size();
    return kPsOk;
  }
  const uint8_t* HeaderBlockPairingHash(const void* b) const override {
    const auto* s = static_cast<const FakeVolumeSidecar*>(b);
    return s != nullptr && s->has_hash ? s->hash : nullptr;
  }
  int HeaderBlockPairing(const void* b,
                         const uint8_t* entry_hash) const override {
    const auto* s = static_cast<const FakeVolumeSidecar*>(b);
    if (s == nullptr) return -1;
    return s->has_hash && entry_hash != nullptr &&
                   memcmp(s->hash, entry_hash, kPsHtmlHashBytes) == 0
               ? kPsSidecarPaired
               : kPsSidecarUnpaired;
  }
  void HeaderBlockFree(void* b) const override {
    delete static_cast<FakeVolumeSidecar*>(b);
  }
  int CacheRemoveSite(void*, const char* url, const char* hostname,
                      const char* scheme, const char* site) const override {
    if (!IsPath(url)) return kPsErrInvalidArg;
    const GoogleString key = Key(site, url, hostname, scheme);
    bool any = sidecars_.erase(key) > 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->first.first == key) {
        it = entries_.erase(it);
        any = true;
      } else {
        ++it;
      }
    }
    return any ? kPsOk : kPsErrNotFound;
  }
  int CacheWriteSitePurgeEpoch(void*, const char* site,
                               uint64_t epoch_s) const override {
    if (now_seconds_ != 0 && epoch_s > now_seconds_ + 5) {
      return kPsErrInvalidArg;
    }
    uint64_t& stored = epochs_[site];
    if (epoch_s > stored) stored = epoch_s;
    return kPsOk;
  }
  int CacheReadSitePurgeEpoch(void*, const char* site,
                              uint64_t* out) const override {
    auto it = epochs_.find(site);
    *out = it == epochs_.end() ? 0 : it->second;
    return kPsOk;
  }

 private:
  struct PendingWrite {
    GoogleString key;
    FakeVolumeEntry entry;
  };

  uint64_t now_seconds_ = 0;
  mutable std::map<std::pair<GoogleString, uint8_t>, FakeVolumeEntry> entries_;
  mutable std::map<GoogleString, FakeVolumeSidecar> sidecars_;
  mutable std::map<GoogleString, uint64_t> epochs_;
  mutable int writes_closed_ = 0;
};

}  // namespace net_instaweb

#endif  // TEST_PAGESPEED_SYSTEM_FAKE_DAEMON_VOLUME_H_
