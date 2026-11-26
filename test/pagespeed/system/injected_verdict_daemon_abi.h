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

// A DaemonAbi whose HTML serve set (C API 1.13 + 1.14) answers what the test
// injected, and records every call into that set with its arguments.
// Everything else forwards to a real binding (the stub library), counted, as
// RecordingDaemonAbi does.
//
// It exists so that a test of the HTML cache engine can assert what the
// engine ASKED the library -- the request and facts structs it filled, the
// header lines it stored, the site it removed from -- and that it OBEYED the
// answer, without asserting any policy outcome itself.  Policy belongs to the
// library and is proven by the library's own tests; here every verdict is
// whatever the case says it is.

#ifndef TEST_PAGESPEED_SYSTEM_INJECTED_VERDICT_DAEMON_ABI_H_
#define TEST_PAGESPEED_SYSTEM_INJECTED_VERDICT_DAEMON_ABI_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_abi.h"
#include "test/pagespeed/system/recording_daemon_abi.h"

namespace net_instaweb {

// One call into the HTML serve set, with its arguments rendered as text.
// Pointer arguments are copied (strings verbatim, hashes as hex), so a
// record stays valid after the caller's buffers are gone.
struct InjectedHtmlCall {
  GoogleString fn;
  std::vector<GoogleString> args;
};

// The request struct as the engine passed it, strings copied; `*_null`
// tells an absent field from an empty one.
struct RecordedServeRequest {
  size_t struct_size = 0;
  int mode = 0;
  GoogleString method, accept, cache_control, pragma, if_none_match;
  bool accept_null = true, cache_control_null = true, pragma_null = true,
       if_none_match_null = true;
  uint32_t request_flags = 0;
  uint32_t now_seconds = 0;
};

// A header block the fake hands out.  Owned by the fake until
// HeaderBlockFree, so live_blocks() can prove every block was released.
struct InjectedHeaderBlock {
  std::vector<std::pair<GoogleString, GoogleString>> lines;
  bool has_pairing_hash = false;
  uint8_t pairing_hash[kPsHtmlHashBytes] = {};
};

// What each entry point answers.  Defaults are the answers of a healthy
// library with nothing stored: the set is bound, the worker derives Markdown
// with agent-optimize on, every write succeeds, and every read misses.
struct HtmlServeInjection {
  bool has_set = true;
  GoogleString unavailable_reason;

  // ps_html_serve_classify: the return code and the verdict.  The verdict is
  // copied into the caller's struct only as far as its struct_size reaches,
  // and its own struct_size is left as the caller set it.
  int serve_rc = 0;
  PsHtmlServeVerdict verdict = {};

  // ps_html_serve_reason_name: index = reason; nullptr past the end.
  std::vector<const char*> reason_names = {"fresh",
                                           "mode",
                                           "auth_request",
                                           "cookie_request",
                                           "client_no_cache",
                                           "needs_validation",
                                           "stale",
                                           "headers_not_reproducible",
                                           "sidecar_missing",
                                           "purged",
                                           "absent",
                                           "binding_stale",
                                           "parent_stale",
                                           "method",
                                           "range",
                                           "cors",
                                           "no_transform",
                                           "withheld",
                                           "skew"};

  // ps_html_store_write_params: the return code, and on success the insert
  // time it stamps (0 = capture_start_ms floored to seconds).
  int write_params_rc = 0;
  uint32_t write_params_inserted_at = 0;

  // ps_html_stored_facts_from_reads: the return code and the facts.
  int facts_rc = 0;
  PsHtmlStoredFacts facts = {};

  // ps_html_markdown_etag and ps_if_none_match_matches_weak.
  int etag_rc = 0;
  GoogleString etag = "W/\"psm-0123456789abcdef\"";
  int inm_match = 0;

  // ps_cache_write_headers_sidecar_site.
  int sidecar_write_rc = kPsOk;
  int sidecar_verdict = 0;
  int sidecar_stored = 1;

  // ps_headers_sidecar_read: the return code, and with kPsOk the block.
  int sidecar_read_rc = kPsErrNotFound;
  InjectedHeaderBlock block;
  // ps_header_block_pairing: the answer for any non-null block.
  int pairing = kPsSidecarPaired;

  // ps_cache_remove_site and the purge epoch.
  int remove_rc = kPsOk;
  int epoch_write_rc = kPsOk;
  int epoch_read_rc = kPsOk;
  uint64_t epoch = 0;

  // The worker's published state, and the Accept preference.
  int html_mode = kPsHtmlModeMarkdownStatic;
  int agent_entitled = 1;
  int wants_markdown = 0;
};

class InjectedVerdictDaemonAbi : public RecordingDaemonAbi {
 public:
  explicit InjectedVerdictDaemonAbi(const DaemonAbi* inner)
      : RecordingDaemonAbi(inner) {
    injection_.verdict.struct_size = sizeof(PsHtmlServeVerdict);
    injection_.facts.struct_size = sizeof(PsHtmlStoredFacts);
  }

  HtmlServeInjection* mutable_injection() { return &injection_; }
  const std::vector<InjectedHtmlCall>& html_calls() const {
    return html_calls_;
  }
  const std::vector<RecordedServeRequest>& serve_requests() const {
    return serve_requests_;
  }
  const std::vector<PsHtmlStoredFacts>& serve_facts() const {
    return serve_facts_;
  }
  // The header lines of every sidecar write, in call order.
  const std::vector<std::vector<std::pair<GoogleString, GoogleString>>>&
  sidecar_writes() const {
    return sidecar_writes_;
  }
  int live_blocks() const { return live_blocks_; }
  void ResetHtmlCalls() {
    html_calls_.clear();
    serve_requests_.clear();
    serve_facts_.clear();
    sidecar_writes_.clear();
  }

  // The names of the calls made, in order.
  std::vector<GoogleString> CallNames() const {
    std::vector<GoogleString> names;
    for (const InjectedHtmlCall& c : html_calls_) names.push_back(c.fn);
    return names;
  }

  bool HasHtmlServeSet() const override { return injection_.has_set; }
  // It answers the store policy itself, whatever the inner library has.
  bool PublishesHtmlStorePolicy() const override { return true; }
  GoogleString HtmlServeSetUnavailableReason() const override {
    return injection_.has_set ? GoogleString() : injection_.unavailable_reason;
  }

  int HtmlServeClassify(const PsHtmlServeRequest* request,
                        const PsHtmlStoredFacts* facts,
                        PsHtmlServeVerdict* out) const override {
    RecordedServeRequest r;
    if (request != nullptr) {
      r.struct_size = request->struct_size;
      r.mode = request->mode;
      r.method = Str(request->method);
      r.accept = Str(request->accept);
      r.accept_null = request->accept == nullptr;
      r.cache_control = Str(request->cache_control);
      r.cache_control_null = request->cache_control == nullptr;
      r.pragma = Str(request->pragma);
      r.pragma_null = request->pragma == nullptr;
      r.if_none_match = Str(request->if_none_match);
      r.if_none_match_null = request->if_none_match == nullptr;
      r.request_flags = request->request_flags;
      r.now_seconds = request->now_seconds;
    }
    serve_requests_.push_back(r);
    serve_facts_.push_back(facts == nullptr ? PsHtmlStoredFacts() : *facts);
    Record("ps_html_serve_classify", {request == nullptr ? "null" : "request",
                                      facts == nullptr ? "null" : "facts"});
    if (injection_.serve_rc != 0) return injection_.serve_rc;
    if (out == nullptr || out->struct_size < sizeof(size_t)) return -1;
    const size_t size = out->struct_size;
    PsHtmlServeVerdict v = injection_.verdict;
    v.struct_size = size;
    memcpy(out, &v, size < sizeof(v) ? size : sizeof(v));
    return 0;
  }

  const char* HtmlServeReasonName(int reason) const override {
    Record("ps_html_serve_reason_name", {Num(reason)});
    if (reason < 0 ||
        static_cast<size_t>(reason) >= injection_.reason_names.size()) {
      return nullptr;
    }
    return injection_.reason_names[reason];
  }

  int HtmlStoreWriteParams(int action, uint32_t lifetime_s,
                           uint32_t store_flags, uint16_t cc_flags,
                           int sidecar_stored, int64_t capture_start_ms,
                           const char* const* age_lines, size_t age_count,
                           uint64_t content_length,
                           const char* origin_content_type,
                           PsWriteParams* params) const override {
    std::vector<GoogleString> args = {
        Num(action),         Num(lifetime_s),         Num(store_flags),
        Num(cc_flags),       Num(sidecar_stored),     Num(capture_start_ms),
        Num(content_length), Str(origin_content_type)};
    for (size_t i = 0; i < age_count; ++i) {
      args.push_back(StrCat("age=", Str(age_lines[i])));
    }
    Record("ps_html_store_write_params", args);
    if (injection_.write_params_rc != 0) return injection_.write_params_rc;
    if (params == nullptr) return -1;
    params->alternate_id = kPsSentinelOriginal;
    params->content_type = kPsContentHtml;
    params->content_length = content_length;
    params->origin_max_age = lifetime_s;
    params->origin_cc_flags = cc_flags;
    params->flags = static_cast<uint8_t>(
        kPsFlagHeadersClassified |
        (sidecar_stored != 0 ? kPsFlagHeadersSidecarRequired : 0));
    params->cache_inserted_at =
        injection_.write_params_inserted_at != 0
            ? injection_.write_params_inserted_at
            : static_cast<uint32_t>(capture_start_ms / 1000);
    return 0;
  }

  int HtmlStoredFactsFromReads(const void* original, const void* markdown,
                               const void* withheld_marker, int sidecar_present,
                               PsHtmlStoredFacts* out) const override {
    Record("ps_html_stored_facts_from_reads",
           {original == nullptr ? "null" : "original",
            markdown == nullptr ? "null" : "markdown",
            withheld_marker == nullptr ? "null" : "withheld",
            Num(sidecar_present)});
    if (injection_.facts_rc != 0) return injection_.facts_rc;
    if (out == nullptr || out->struct_size < sizeof(PsHtmlStoredFacts)) {
      return -1;
    }
    *out = injection_.facts;
    out->struct_size = sizeof(PsHtmlStoredFacts);
    return 0;
  }

  int HtmlMarkdownEtag(const uint8_t* binding_hash,
                       const uint8_t* markdown_hash, char* buf, size_t capacity,
                       size_t* out_len) const override {
    Record("ps_html_markdown_etag", {Hex(binding_hash), Hex(markdown_hash)});
    if (injection_.etag_rc != 0) return injection_.etag_rc;
    if (out_len == nullptr) return -1;
    *out_len = injection_.etag.size();
    if (buf == nullptr || capacity <= injection_.etag.size()) {
      if (buf != nullptr && capacity > 0) buf[0] = '\0';
      return -3;
    }
    memcpy(buf, injection_.etag.c_str(), injection_.etag.size() + 1);
    return 0;
  }

  int IfNoneMatchMatchesWeak(const char* if_none_match,
                             const char* etag) const override {
    Record("ps_if_none_match_matches_weak",
           {if_none_match == nullptr ? "(null)" : if_none_match, Str(etag)});
    return injection_.inm_match;
  }

  int CacheWriteHeadersSidecarSite(void* cache,
                                   const PsSidecarWriteParams* params,
                                   const char* const* names,
                                   const char* const* values, size_t count,
                                   int* out_verdict,
                                   int* out_stored) const override {
    std::vector<std::pair<GoogleString, GoogleString>> lines;
    for (size_t i = 0; i < count; ++i) {
      lines.emplace_back(Str(names[i]), Str(values[i]));
    }
    sidecar_writes_.push_back(lines);
    if (params == nullptr) {
      Record("ps_cache_write_headers_sidecar_site", {"null"});
    } else {
      Record("ps_cache_write_headers_sidecar_site",
             {Num(params->struct_size), Str(params->url), Str(params->hostname),
              Str(params->scheme), Str(params->site),
              Hex(params->origin_html_hash), Num(count)});
    }
    if (injection_.sidecar_write_rc != kPsOk) {
      return injection_.sidecar_write_rc;
    }
    if (out_verdict != nullptr) *out_verdict = injection_.sidecar_verdict;
    if (out_stored != nullptr) *out_stored = injection_.sidecar_stored;
    return kPsOk;
  }

  int HeadersSidecarRead(void* cache, const char* url, const char* hostname,
                         const char* scheme, const char* site,
                         void** out_block) const override {
    Record("ps_headers_sidecar_read",
           {Str(url), Str(hostname), Str(scheme), Str(site)});
    if (out_block == nullptr) return kPsErrInvalidArg;
    *out_block = nullptr;
    if (injection_.sidecar_read_rc != kPsOk) {
      return injection_.sidecar_read_rc;
    }
    blocks_.push_back(std::make_unique<InjectedHeaderBlock>(injection_.block));
    ++live_blocks_;
    *out_block = blocks_.back().get();
    return kPsOk;
  }

  size_t HeaderBlockCount(const void* block) const override {
    return block == nullptr
               ? 0
               : static_cast<const InjectedHeaderBlock*>(block)->lines.size();
  }

  int HeaderBlockGet(const void* block, size_t index, const char** out_name,
                     size_t* out_name_len, const char** out_value,
                     size_t* out_value_len) const override {
    if (block == nullptr) return kPsErrInvalidArg;
    const auto* b = static_cast<const InjectedHeaderBlock*>(block);
    if (index >= b->lines.size()) return kPsErrInvalidArg;
    const auto& line = b->lines[index];
    if (out_name != nullptr) *out_name = line.first.c_str();
    if (out_name_len != nullptr) *out_name_len = line.first.size();
    if (out_value != nullptr) *out_value = line.second.c_str();
    if (out_value_len != nullptr) *out_value_len = line.second.size();
    return kPsOk;
  }

  const uint8_t* HeaderBlockPairingHash(const void* block) const override {
    if (block == nullptr) return nullptr;
    const auto* b = static_cast<const InjectedHeaderBlock*>(block);
    return b->has_pairing_hash ? b->pairing_hash : nullptr;
  }

  int HeaderBlockPairing(const void* block,
                         const uint8_t* entry_hash) const override {
    Record("ps_header_block_pairing", {Hex(entry_hash)});
    return block == nullptr ? -1 : injection_.pairing;
  }

  void HeaderBlockFree(void* block) const override {
    if (block == nullptr) return;
    for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
      if (it->get() == block) {
        blocks_.erase(it);
        --live_blocks_;
        return;
      }
    }
  }

  int CacheRemoveSite(void* cache, const char* url, const char* hostname,
                      const char* scheme, const char* site) const override {
    Record("ps_cache_remove_site",
           {Str(url), Str(hostname), Str(scheme), Str(site)});
    return injection_.remove_rc;
  }

  int CacheWriteSitePurgeEpoch(void* cache, const char* site,
                               uint64_t epoch_s) const override {
    Record("ps_cache_write_site_purge_epoch", {Str(site), Num(epoch_s)});
    return injection_.epoch_write_rc;
  }

  int CacheReadSitePurgeEpoch(void* cache, const char* site,
                              uint64_t* out_epoch_s) const override {
    Record("ps_cache_read_site_purge_epoch", {Str(site)});
    if (injection_.epoch_read_rc != kPsOk) return injection_.epoch_read_rc;
    if (out_epoch_s == nullptr) return kPsErrInvalidArg;
    *out_epoch_s = injection_.epoch;
    return kPsOk;
  }

  int ReadSharedConfigHtmlMode(const char* cache_path) const override {
    Record("ps_read_shared_config_html_mode", {Str(cache_path)});
    return injection_.html_mode;
  }

  int ReadSharedConfigAgentEntitled(const char* cache_path) const override {
    Record("ps_read_shared_config_agent_entitled", {Str(cache_path)});
    return injection_.agent_entitled;
  }

  int WantsAgentMarkdown(const char* accept) const override {
    Record("ps_wants_agent_markdown", {accept == nullptr ? "(null)" : accept});
    return injection_.wants_markdown;
  }

 private:
  static GoogleString Num(int64_t v) {
    return Integer64ToString(static_cast<int64>(v));
  }

  static GoogleString Str(const char* s) {
    return s == nullptr ? GoogleString() : GoogleString(s);
  }

  static GoogleString Hex(const uint8_t* bytes) {
    if (bytes == nullptr) return "(null)";
    static const char kDigits[] = "0123456789abcdef";
    GoogleString out;
    for (size_t i = 0; i < kPsHtmlHashBytes; ++i) {
      out.push_back(kDigits[bytes[i] >> 4]);
      out.push_back(kDigits[bytes[i] & 0x0F]);
    }
    return out;
  }

  void Record(const char* fn, std::vector<GoogleString> args) const {
    html_calls_.push_back(InjectedHtmlCall{fn, std::move(args)});
  }

  HtmlServeInjection injection_;
  mutable std::vector<InjectedHtmlCall> html_calls_;
  mutable std::vector<RecordedServeRequest> serve_requests_;
  mutable std::vector<PsHtmlStoredFacts> serve_facts_;
  mutable std::vector<std::vector<std::pair<GoogleString, GoogleString>>>
      sidecar_writes_;
  mutable std::vector<std::unique_ptr<InjectedHeaderBlock>> blocks_;
  mutable int live_blocks_ = 0;
};

}  // namespace net_instaweb

#endif  // TEST_PAGESPEED_SYSTEM_INJECTED_VERDICT_DAEMON_ABI_H_
