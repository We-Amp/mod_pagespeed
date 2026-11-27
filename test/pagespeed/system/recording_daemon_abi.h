/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

// A DaemonAbi that forwards every call to a real binding (the stub library,
// loaded through LoadDaemonAbi) and records what the module asked: the number
// of calls of any kind, the site every sited cache call named, and a copy of
// every notification.  It is what lets a case say "no call at all reached the
// peer" and "this site reached the _site entry point" without trusting the
// code under test to report either.

#ifndef TEST_PAGESPEED_SYSTEM_RECORDING_DAEMON_ABI_H_
#define TEST_PAGESPEED_SYSTEM_RECORDING_DAEMON_ABI_H_

#include <cstdint>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_abi.h"

namespace net_instaweb {

struct RecordedNotify {
  GoogleString url;
  GoogleString site;
  bool site_null = true;
  int content_type = -1;
  uint32_t mask = 0;
  int agent_request = 0;
  uint32_t notify_flags = 0;
  size_t struct_size = 0;
};

class RecordingDaemonAbi : public DaemonAbi {
 public:
  explicit RecordingDaemonAbi(const DaemonAbi* inner) : inner_(inner) {}

  int calls() const { return calls_; }
  const std::vector<GoogleString>& sites() const { return sites_; }
  const std::vector<RecordedNotify>& notifies() const { return notifies_; }
  void Reset() {
    calls_ = 0;
    sites_.clear();
    notifies_.clear();
  }

  int VersionMajor() const override { return Count(), inner_->VersionMajor(); }
  int VersionMinor() const override { return Count(), inner_->VersionMinor(); }
  void CacheConfigInit(PsCacheConfig* c) const override {
    Count();
    inner_->CacheConfigInit(c);
  }
  bool HasSizedCacheConfigInit() const override {
    return Count(), inner_->HasSizedCacheConfigInit();
  }
  uint64_t SharedConfigVolumeSize(const char* p) const override {
    return Count(), inner_->SharedConfigVolumeSize(p);
  }
  bool PublishesVolumeSize() const override {
    return Count(), inner_->PublishesVolumeSize();
  }
  uint32_t SharedConfigGeneration(const char* p) const override {
    return Count(), inner_->SharedConfigGeneration(p);
  }
  bool PublishesGeneration() const override {
    return Count(), inner_->PublishesGeneration();
  }
  int CacheOpen(const PsCacheConfig* c, void** out) const override {
    return Count(), inner_->CacheOpen(c, out);
  }
  void CacheClose(void* cache) const override {
    Count();
    inner_->CacheClose(cache);
  }
  const char* StrError(int e) const override {
    return Count(), inner_->StrError(e);
  }
  const char* LastErrorMessage() const override {
    return Count(), inner_->LastErrorMessage();
  }
  void WriteParamsInit(PsWriteParams* p) const override {
    Count();
    inner_->WriteParamsInit(p);
  }
  void NotifyParamsInit(PsNotifyParams* p) const override {
    Count();
    inner_->NotifyParamsInit(p);
  }
  int CacheWriteOriginalSite(void* cache, const char* url, const char* host,
                             const char* scheme, const char* site,
                             const PsWriteParams* params,
                             void** out) const override {
    Count();
    Site(site);
    return inner_->CacheWriteOriginalSite(cache, url, host, scheme, site,
                                          params, out);
  }
  int WriteData(void* h, const void* d, size_t n) const override {
    return Count(), inner_->WriteData(h, d, n);
  }
  int WriteClose(void* h) const override {
    return Count(), inner_->WriteClose(h);
  }
  void WriteAbort(void* h) const override {
    Count();
    inner_->WriteAbort(h);
  }
  int VaryUncacheable(const char* v) const override {
    return Count(), inner_->VaryUncacheable(v);
  }
  int VaryVariesAccept(const char* v) const override {
    return Count(), inner_->VaryVariesAccept(v);
  }
  int ParseCacheControl(const char* v, PsCacheControl* out) const override {
    return Count(), inner_->ParseCacheControl(v, out);
  }
  uint32_t AgeAdjustedInsertTime(uint32_t now, uint32_t age) const override {
    return Count(), inner_->AgeAdjustedInsertTime(now, age);
  }
  uint32_t Classify(const char* a, const char* u, const char* s,
                    const char* e) const override {
    return Count(), inner_->Classify(a, u, s, e);
  }
  int ClassifyContentType(const char* ct) const override {
    return Count(), inner_->ClassifyContentType(ct);
  }
  int OptionContextSignature(const char* p, size_t n, char* out,
                             size_t size) const override {
    return Count(), inner_->OptionContextSignature(p, n, out, size);
  }
  int NotifyWorker(const char* socket,
                   const PsNotifyParams* params) const override {
    Count();
    RecordedNotify n;
    n.url = params->url == nullptr ? "" : params->url;
    n.site_null = params->site == nullptr;
    n.site = params->site == nullptr ? "" : params->site;
    n.content_type = params->content_type;
    n.mask = params->mask;
    n.agent_request = params->agent_request;
    n.notify_flags = params->notify_flags;
    n.struct_size = params->struct_size;
    notifies_.push_back(n);
    return inner_->NotifyWorker(socket, params);
  }
  int CacheReadBestSite(void* cache, const char* url, const char* host,
                        const char* scheme, const char* site, uint32_t mask,
                        void** out) const override {
    Count();
    Site(site);
    return inner_->CacheReadBestSite(cache, url, host, scheme, site, mask,
                                     out);
  }
  int CacheReadAlternateSite(void* cache, const char* url, const char* host,
                             const char* scheme, const char* site,
                             uint8_t id, void** out) const override {
    Count();
    Site(site);
    return inner_->CacheReadAlternateSite(cache, url, host, scheme, site, id,
                                          out);
  }
  int ReadContent(const void* r, const uint8_t** d,
                  size_t* n) const override {
    return Count(), inner_->ReadContent(r, d, n);
  }
  uint32_t ReadMask(const void* r) const override {
    return Count(), inner_->ReadMask(r);
  }
  uint8_t ReadFlags(const void* r) const override {
    return Count(), inner_->ReadFlags(r);
  }
  int ReadContentType(const void* r) const override {
    return Count(), inner_->ReadContentType(r);
  }
  const char* ReadOriginContentType(const void* r) const override {
    return Count(), inner_->ReadOriginContentType(r);
  }
  uint32_t ReadCacheInsertedAt(const void* r) const override {
    return Count(), inner_->ReadCacheInsertedAt(r);
  }
  uint32_t ReadOriginMaxAge(const void* r) const override {
    return Count(), inner_->ReadOriginMaxAge(r);
  }
  uint32_t ReadOriginSMaxAge(const void* r) const override {
    return Count(), inner_->ReadOriginSMaxAge(r);
  }
  uint16_t ReadOriginCcFlags(const void* r) const override {
    return Count(), inner_->ReadOriginCcFlags(r);
  }
  uint32_t ReadOriginLastModified(const void* r) const override {
    return Count(), inner_->ReadOriginLastModified(r);
  }
  const char* ReadOriginEtag(const void* r) const override {
    return Count(), inner_->ReadOriginEtag(r);
  }
  const uint8_t* ReadOriginHtmlHash(const void* r) const override {
    return Count(), inner_->ReadOriginHtmlHash(r);
  }
  int ReadIsWorkerProcessed(const void* r) const override {
    return Count(), inner_->ReadIsWorkerProcessed(r);
  }
  uint32_t ReadOriginContentLength(const void* r) const override {
    return Count(), inner_->ReadOriginContentLength(r);
  }
  bool ReadEncodedOriginLength(const void* r, uint64_t* out) const override {
    return Count(), inner_->ReadEncodedOriginLength(r, out);
  }
  void ReadFree(void* r) const override {
    Count();
    inner_->ReadFree(r);
  }
  int EvaluateFreshness(const PsFreshnessInput* in,
                        const PsFreshnessConfig* config,
                        PsFreshnessResult* out) const override {
    return Count(), inner_->EvaluateFreshness(in, config, out);
  }
  int BuildCacheControl(const PsCacheControlInput* in, char* buf,
                        size_t capacity, size_t* out_len,
                        uint32_t* out_final) const override {
    return Count(),
           inner_->BuildCacheControl(in, buf, capacity, out_len, out_final);
  }
  int ServeStatsOpen(const char* path, void** out) const override {
    return Count(), inner_->ServeStatsOpen(path, out);
  }
  void ServeStatsRecordServeClass(void* h, int c,
                                  uint32_t flags) const override {
    Count();
    inner_->ServeStatsRecordServeClass(h, c, flags);
  }
  void ServeStatsRecordHit(void* h, int ct, uint64_t o, uint64_t opt,
                           uint32_t mask) const override {
    Count();
    inner_->ServeStatsRecordHit(h, ct, o, opt, mask);
  }
  void ServeStatsClose(void* h) const override {
    Count();
    inner_->ServeStatsClose(h);
  }

  // The HTML serve set, forwarded like everything else; the sited calls
  // also record their site.
  bool HasHtmlServeSet() const override {
    return Count(), inner_->HasHtmlServeSet();
  }
  GoogleString HtmlServeSetUnavailableReason() const override {
    return Count(), inner_->HtmlServeSetUnavailableReason();
  }
  int HtmlServeClassify(const PsHtmlServeRequest* request,
                        const PsHtmlStoredFacts* facts,
                        PsHtmlServeVerdict* out) const override {
    return Count(), inner_->HtmlServeClassify(request, facts, out);
  }
  const char* HtmlServeReasonName(int reason) const override {
    return Count(), inner_->HtmlServeReasonName(reason);
  }
  int HtmlStoreWriteParams(int action, uint32_t lifetime_s,
                           uint32_t store_flags, uint16_t cc_flags,
                           int sidecar_stored, int64_t capture_start_ms,
                           const char* const* age_lines, size_t age_count,
                           uint64_t content_length, const char* origin_ct,
                           PsWriteParams* params) const override {
    return Count(), inner_->HtmlStoreWriteParams(
                        action, lifetime_s, store_flags, cc_flags,
                        sidecar_stored, capture_start_ms, age_lines, age_count,
                        content_length, origin_ct, params);
  }
  int HtmlStoredFactsFromReads(const void* original, const void* markdown,
                               const void* withheld, int sidecar_present,
                               PsHtmlStoredFacts* out) const override {
    return Count(), inner_->HtmlStoredFactsFromReads(
                        original, markdown, withheld, sidecar_present, out);
  }
  int HtmlMarkdownEtag(const uint8_t* binding, const uint8_t* markdown,
                       char* buf, size_t capacity,
                       size_t* out_len) const override {
    return Count(),
           inner_->HtmlMarkdownEtag(binding, markdown, buf, capacity, out_len);
  }
  int IfNoneMatchMatchesWeak(const char* inm, const char* etag) const override {
    return Count(), inner_->IfNoneMatchMatchesWeak(inm, etag);
  }
  int CacheWriteHeadersSidecarSite(void* cache,
                                   const PsSidecarWriteParams* params,
                                   const char* const* names,
                                   const char* const* values, size_t count,
                                   int* out_verdict,
                                   int* out_stored) const override {
    Count();
    Site(params == nullptr ? nullptr : params->site);
    return inner_->CacheWriteHeadersSidecarSite(cache, params, names, values,
                                                count, out_verdict, out_stored);
  }
  int HeadersSidecarRead(void* cache, const char* url, const char* host,
                         const char* scheme, const char* site,
                         void** out) const override {
    Count();
    Site(site);
    return inner_->HeadersSidecarRead(cache, url, host, scheme, site, out);
  }
  size_t HeaderBlockCount(const void* block) const override {
    return Count(), inner_->HeaderBlockCount(block);
  }
  int HeaderBlockGet(const void* block, size_t index, const char** name,
                     size_t* name_len, const char** value,
                     size_t* value_len) const override {
    return Count(), inner_->HeaderBlockGet(block, index, name, name_len, value,
                                           value_len);
  }
  const uint8_t* HeaderBlockPairingHash(const void* block) const override {
    return Count(), inner_->HeaderBlockPairingHash(block);
  }
  int HeaderBlockPairing(const void* block,
                         const uint8_t* entry_hash) const override {
    return Count(), inner_->HeaderBlockPairing(block, entry_hash);
  }
  void HeaderBlockFree(void* block) const override {
    Count();
    inner_->HeaderBlockFree(block);
  }
  int CacheRemoveSite(void* cache, const char* url, const char* host,
                      const char* scheme, const char* site) const override {
    Count();
    Site(site);
    return inner_->CacheRemoveSite(cache, url, host, scheme, site);
  }
  int CacheWriteSitePurgeEpoch(void* cache, const char* site,
                               uint64_t epoch_s) const override {
    Count();
    Site(site);
    return inner_->CacheWriteSitePurgeEpoch(cache, site, epoch_s);
  }
  int CacheReadSitePurgeEpoch(void* cache, const char* site,
                              uint64_t* out) const override {
    Count();
    Site(site);
    return inner_->CacheReadSitePurgeEpoch(cache, site, out);
  }
  int ReadSharedConfigHtmlMode(const char* path) const override {
    return Count(), inner_->ReadSharedConfigHtmlMode(path);
  }
  int ReadSharedConfigAgentEntitled(const char* path) const override {
    return Count(), inner_->ReadSharedConfigAgentEntitled(path);
  }
  int WantsAgentMarkdown(const char* accept) const override {
    return Count(), inner_->WantsAgentMarkdown(accept);
  }

  // The 1.15 set, forwarded like everything else.
  bool HasHtmlServe15Set() const override {
    return Count(), inner_->HasHtmlServe15Set();
  }
  GoogleString HtmlServe15SetUnavailableReason() const override {
    return Count(), inner_->HtmlServe15SetUnavailableReason();
  }
  int HtmlRequestGuard(const char* raw_path, const char* const* names,
                       const char* const* values, size_t count,
                       const char* keyed_scheme,
                       PsHtmlRequestGuard* out) const override {
    return Count(), inner_->HtmlRequestGuard(raw_path, names, values, count,
                                             keyed_scheme, out);
  }
  int HtmlClassifyCookies(const char* const* lines, size_t line_count,
                          int ignore_builtin, const char* const* additions,
                          size_t addition_count, PsHtmlCookieSpan* values,
                          size_t capacity,
                          PsHtmlCookieClass* out) const override {
    return Count(), inner_->HtmlClassifyCookies(
                        lines, line_count, ignore_builtin, additions,
                        addition_count, values, capacity, out);
  }
  int HtmlIgnoreCookieGuard(const char* addition,
                            int* out_warning) const override {
    return Count(), inner_->HtmlIgnoreCookieGuard(addition, out_warning);
  }
  int HtmlPolicyStamp(int credentialed, int ignore_builtin,
                      uint32_t list_version, const char* const* additions,
                      size_t addition_count, uint64_t* out) const override {
    return Count(),
           inner_->HtmlPolicyStamp(credentialed, ignore_builtin, list_version,
                                   additions, addition_count, out);
  }
  int HtmlUnsafeMethodInvalidates(const char* method,
                                  int status) const override {
    return Count(), inner_->HtmlUnsafeMethodInvalidates(method, status);
  }
  int HeaderBlockPolicyStamp(const void* block, uint64_t* out) const override {
    return Count(), inner_->HeaderBlockPolicyStamp(block, out);
  }
  int HtmlStoredFactsFromReadsEx(const void* original, const void* markdown,
                                 const void* withheld, int sidecar_present,
                                 const void* block,
                                 PsHtmlStoredFacts* out) const override {
    return Count(),
           inner_->HtmlStoredFactsFromReadsEx(original, markdown, withheld,
                                              sidecar_present, block, out);
  }

 private:
  void Count() const { ++calls_; }
  void Site(const char* site) const {
    sites_.push_back(site == nullptr ? GoogleString("(null)") : site);
  }

  const DaemonAbi* inner_;
  mutable int calls_ = 0;
  mutable std::vector<GoogleString> sites_;
  mutable std::vector<RecordedNotify> notifies_;
};

}  // namespace net_instaweb

#endif  // TEST_PAGESPEED_SYSTEM_RECORDING_DAEMON_ABI_H_
