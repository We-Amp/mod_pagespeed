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
