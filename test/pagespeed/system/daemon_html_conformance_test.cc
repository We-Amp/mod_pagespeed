// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The engine runner: every `engine` row of the HTML cache conformance cases
// (test/system/daemon_html/conformance_cases.json), run against the real
// engine (pagespeed/system/daemon_html_engine.h) on the injected-verdict fake
// library and an in-memory volume, with the real PurgeSet behind the purge
// fence.
//
// The runner asserts what the engine ASKED the library (the request, facts
// and header views it passed, in order), what it wrote, removed and
// notified, and what it asked the port to emit.  It never asserts a policy
// outcome: every verdict is whatever the row injects.  Policy is the
// library's, proven by the library's own runner over the shared policy cases.
//
// Each row is its own test case, named after its id.

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "openssl/sha.h"
#include "pagespeed/kernel/base/json.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/cache/purge_set.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/system/daemon_abi.h"
#include "pagespeed/system/daemon_html_engine.h"
#include "pagespeed/system/daemon_html_purge_observer.h"
#include "pagespeed/system/daemon_serve_arm.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/system/fake_daemon_volume.h"
#include "test/pagespeed/system/injected_verdict_daemon_abi.h"

namespace net_instaweb {
namespace {

const char kCasesPath[] = "/test/system/daemon_html/conformance_cases.json";
const int64 kStartMs = 1790000000000LL;
const char kDefaultHost[] = "www.example.test";

GoogleString Hex(const uint8_t* bytes, size_t n) {
  static const char kDigits[] = "0123456789abcdef";
  GoogleString out;
  for (size_t i = 0; i < n; ++i) {
    out.push_back(kDigits[bytes[i] >> 4]);
    out.push_back(kDigits[bytes[i] & 0x0F]);
  }
  return out;
}

void Sha256(const GoogleString& s, uint8_t out[kPsHtmlHashBytes]) {
  SHA256(reinterpret_cast<const uint8_t*>(s.data()), s.size(), out);
}

// A row's symbolic site name as a site id.
GoogleString SiteId(const GoogleString& name) {
  uint8_t h[kPsHtmlHashBytes];
  Sha256(StrCat("site:", name), h);
  return Hex(h, sizeof(h));
}

const char* const kStoreReasons[] = {"none",
                                     "method",
                                     "status",
                                     "incomplete",
                                     "auth_request",
                                     "cookie_request",
                                     "set_cookie",
                                     "private",
                                     "no_store",
                                     "no_lifetime",
                                     "vary",
                                     "headers",
                                     "nonce_csp",
                                     "nonce_link",
                                     "content_type",
                                     "encoding",
                                     "size",
                                     "url",
                                     "personalisation_signal",
                                     "no_transform",
                                     "mode"};

const char* const kServeReasons[] = {"fresh",
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

template <size_t N>
int IndexOf(const char* const (&names)[N], const GoogleString& name) {
  for (size_t i = 0; i < N; ++i) {
    if (name == names[i]) return static_cast<int>(i);
  }
  return -1;
}

int RcOf(const Json::Value& v) {
  const GoogleString rc = v.asString();
  if (rc == "ok") return kPsOk;
  if (rc == "not_found") return kPsErrNotFound;
  if (rc == "io_error") return 2;  // PS_ERR_IO
  if (rc == "corrupted") return 3;
  if (rc == "invalid_arg") return kPsErrInvalidArg;
  return 99;
}

// The Cache-Control bits a row names (PS_CC_ORIGIN_*).
uint16_t CcFlagsOf(const Json::Value& names) {
  uint16_t out = 0;
  for (const Json::Value& n : names) {
    const GoogleString name = n.asString();
    if (name == "no_cache") out |= kPsCcOriginNoCache;
    if (name == "must_revalidate") out |= kPsCcOriginMustRevalidate;
    if (name == "no_store") out |= kPsCcOriginNoStore;
    if (name == "private") out |= kPsCcOriginPrivate;
    if (name == "public") out |= kPsCcOriginPublic;
    if (name == "immutable") out |= kPsCcOriginImmutable;
    if (name == "s_maxage") out |= kPsCcOriginSMaxagePresent;
    if (name == "proxy_revalidate") out |= kPsCcOriginProxyRevalidate;
    if (name == "no_transform") out |= kPsCcOriginNoTransform;
    if (name == "header_present") out |= kPsCcOriginHeaderPresent;
  }
  return out;
}

// The ps_html_store_classify out_flags bits a row names.
uint32_t StoreFlagsOf(const Json::Value& names) {
  uint32_t out = 0;
  for (const Json::Value& n : names) {
    if (n.asString() == "needs_validation") out |= 2u;
    if (n.asString() == "explicit_shared") out |= 8u;
  }
  return out;
}

// One call the engine made, as the runner records it.
struct Call {
  GoogleString fn;
  Json::Value args{Json::objectValue};
  std::vector<std::pair<GoogleString, GoogleString>> headers;
};

class CountingHandler : public MessageHandler {
 public:
  int count() const { return count_; }
  const std::vector<GoogleString>& messages() const { return messages_; }

 protected:
  void MessageSImpl(MessageType, const GoogleString& m) override {
    ++count_;
    messages_.push_back(m);
  }
  void FileMessageSImpl(MessageType, const char*, int,
                        const GoogleString& m) override {
    ++count_;
    messages_.push_back(m);
  }

 private:
  int count_ = 0;
  std::vector<GoogleString> messages_;
};

// The purge state of the process: a real PurgeSet, and the flush file.
class FakePurgeSource : public DaemonHtmlPurgeSource {
 public:
  FakePurgeSource() : purge_set_(new PurgeSet(1 << 20)) {}
  bool IsUrlValidIgnoringGlobal(const GoogleString& url,
                                int64 time_ms) const override {
    return purge_set_->IsValidIgnoringGlobal(url, time_ms);
  }
  int64 GlobalInvalidationTimestampMs() const override {
    return purge_set_->global_invalidation_timestamp_ms();
  }
  bool FlushFileMtimeMs(int64* mtime_ms) const override {
    if (!has_flush_) return false;
    *mtime_ms = flush_ms_;
    return true;
  }
  PurgeSet* purge_set() { return purge_set_.get(); }
  void Wipe() {
    purge_set_ = std::make_unique<PurgeSet>(1 << 20);
    has_flush_ = false;
  }
  void SetFlush(int64 ms) {
    has_flush_ = true;
    flush_ms_ = ms;
  }

 private:
  std::unique_ptr<PurgeSet> purge_set_;
  bool has_flush_ = false;
  int64 flush_ms_ = 0;
};

class FakePort : public DaemonHtmlPort {
 public:
  bool ClearAndEmitAllowlisted(const DaemonHtmlServeResult& result,
                               bool head) override {
    calls.push_back("ClearAndEmitAllowlisted");
    emitted = result;
    emitted_head = head;
    return emit_ok;
  }
  void MarkNoCompress() override { calls.push_back("MarkNoCompress"); }
  bool EmitAsOrigin(const DaemonHtmlServeResult&) override {
    calls.push_back("EmitAsOrigin");
    return false;
  }
  void RemoveClientConditionals() override {
    calls.push_back("RemoveClientConditionals");
  }
  void RunBlocking(DaemonHtmlCommit commit) override {
    calls.push_back("RunBlocking");
    commit.Run();
  }

  std::vector<GoogleString> calls;
  DaemonHtmlServeResult emitted;
  bool emitted_head = false;
  bool emit_ok = true;
};

// The injected-verdict library over the volume: every call recorded with its
// arguments; the policy calls answer the row's verdicts, consumed in order
// per function (and per view for the store classifier); the volume calls go
// to the volume unless a verdict overrides their return code.
class ConformanceAbi : public InjectedVerdictDaemonAbi {
 public:
  ConformanceAbi(FakeDaemonVolume* volume, const Json::Value& verdicts,
                 const Json::Value& library,
                 std::map<GoogleString, GoogleString>* site_names)
      : InjectedVerdictDaemonAbi(volume),
        volume_(volume),
        site_names_(site_names) {
    for (const Json::Value& v : verdicts) {
      GoogleString fn = v["fn"].asString();
      if (fn == "ps_html_store_classify") {
        fn = StrCat(fn, "/", v["view"].asString());
      }
      queues_[fn].push_back(v);
    }
    if (library.isMember("minor")) minor_ = library["minor"].asInt();
    if (library.isMember("set_bound")) {
      set_bound_ = library["set_bound"].asBool();
    }
    if (library.isMember("html_mode")) {
      html_mode_ = HtmlMode(library["html_mode"].asString());
    }
    if (library.isMember("agent_entitled")) {
      agent_entitled_ = library["agent_entitled"].asBool() ? 1 : 0;
    }
  }

  std::vector<Call>& calls() { return calls_; }
  void BeginCapture() { store_classify_calls_ = 0; }

  static int HtmlMode(const GoogleString& name) {
    if (name == "off") return kPsHtmlModeOff;
    if (name == "markdown-static") return kPsHtmlModeMarkdownStatic;
    if (name == "markdown-rendered") return kPsHtmlModeMarkdownRendered;
    if (name == "full") return kPsHtmlModeFull;
    return -1;
  }

  int VersionMinor() const override { return minor_; }
  bool HasHtmlServeSet() const override {
    return minor_ >= kHtmlServeSetMinor && set_bound_;
  }
  GoogleString HtmlServeSetUnavailableReason() const override {
    if (HasHtmlServeSet()) return GoogleString();
    return minor_ < kHtmlServeSetMinor
               ? StrCat("the optimizer library reports C API 1.",
                        IntegerToString(minor_),
                        "; the HTML serve set needs 1.14")
               : GoogleString(
                     "the optimizer library lacks ps_html_serve_classify");
  }

  int HtmlStoreClassify(int mode, const char* method, const char* url_path,
                        uint32_t request_flags, int status,
                        const char* const* names, const char* const* values,
                        size_t count, uint64_t body_bytes, int body_complete,
                        const char* body_prefix, size_t body_prefix_len,
                        uint64_t max_body_bytes, uint32_t now_seconds,
                        int* out_reason, uint32_t* out_lifetime_s,
                        uint32_t* out_flags, uint16_t* out_cc_flags,
                        int* out_sidecar, char* detail_buf,
                        size_t detail_capacity,
                        size_t* out_detail_len) const override {
    const char* view = store_classify_calls_++ % 2 == 0 ? "store" : "final";
    Call c = Rec("ps_html_store_classify");
    c.args["view"] = view;
    c.args["mode"] = mode;
    c.args["status"] = status;
    c.args["request.authorization"] = (request_flags & 1u) != 0;
    c.args["request.authenticated_user"] = (request_flags & 2u) != 0;
    c.args["request.cookie"] = (request_flags & 4u) != 0;
    c.args["request.no_store"] = (request_flags & 8u) != 0;
    c.args["body_bytes"] = static_cast<Json::UInt64>(body_bytes);
    for (size_t i = 0; i < count; ++i) {
      c.headers.emplace_back(names[i], values[i]);
    }
    calls_.push_back(c);
    Json::Value v = Pop(StrCat("ps_html_store_classify/", view));
    GoogleString action = v.isNull() ? "refuse" : v["action"].asString();
    int a = kPsHtmlStoreActionRefuse;
    if (action == "store") a = kPsHtmlStoreActionStore;
    if (action == "store_no_notify") a = kPsHtmlStoreActionStoreNoNotify;
    if (action == "would_store") a = kPsHtmlStoreActionWouldStore;
    if (action == "would_store_no_notify") {
      a = kPsHtmlStoreActionWouldStoreNoNotify;
    }
    *out_reason = IndexOf(kStoreReasons, v.isNull() ? GoogleString("mode")
                                                    : v["reason"].asString());
    *out_lifetime_s = v.isNull() ? 0 : v["lifetime_s"].asUInt();
    *out_flags = v.isNull() ? 0 : StoreFlagsOf(v["flags"]);
    *out_cc_flags = v.isNull() ? 0 : CcFlagsOf(v["cc_flags"]);
    *out_sidecar = 0;
    const GoogleString detail = v.isNull() ? "" : v["detail"].asString();
    *out_detail_len = detail.size();
    if (detail_capacity > 0) {
      const size_t n = std::min(detail.size(), detail_capacity - 1);
      memcpy(detail_buf, detail.data(), n);
      detail_buf[n] = '\0';
    }
    return a;
  }

  const char* HtmlStoreReasonName(int reason) const override {
    if (reason < 0 || static_cast<size_t>(reason) >= std::size(kStoreReasons)) {
      return nullptr;
    }
    return kStoreReasons[reason];
  }

  int HtmlServeClassify(const PsHtmlServeRequest* request,
                        const PsHtmlStoredFacts* facts,
                        PsHtmlServeVerdict* out) const override {
    Call c = Rec("ps_html_serve_classify");
    c.args["request.struct_size"] =
        static_cast<Json::UInt64>(request->struct_size);
    c.args["request.method"] = request->method;
    c.args["request.head"] = GoogleString(request->method) == "HEAD";
    c.args["request.accept"] = Str(request->accept);
    c.args["request.cache_control"] = Str(request->cache_control);
    c.args["request.pragma"] = Str(request->pragma);
    c.args["request.if_none_match"] = Str(request->if_none_match);
    const uint32_t f = request->request_flags;
    c.args["request.authorization"] =
        (f & kPsHtmlServeRequestHasAuthorization) != 0;
    c.args["request.authenticated_user"] =
        (f & kPsHtmlServeRequestAuthenticatedUser) != 0;
    c.args["request.cookie"] = (f & kPsHtmlServeRequestHasCookie) != 0;
    c.args["request.range"] = (f & kPsHtmlServeRequestHasRange) != 0;
    c.args["request.if_range"] = (f & kPsHtmlServeRequestHasIfRange) != 0;
    c.args["request.origin"] = (f & kPsHtmlServeRequestHasOrigin) != 0;
    c.args["request.now_seconds"] = request->now_seconds;
    c.args["stored.present"] = (facts->flags & kPsHtmlFactsPresent) != 0;
    c.args["stored.purged"] = (facts->flags & kPsHtmlFactsPurged) != 0;
    c.args["stored.binding_live"] =
        (facts->flags & kPsHtmlFactsBindingLive) != 0;
    c.args["stored.variant_present"] =
        (facts->flags & kPsHtmlFactsVariantPresent) != 0;
    c.args["stored.withheld"] = (facts->flags & kPsHtmlFactsWithheld) != 0;
    c.args["stored.stored_at"] = facts->stored_at;
    calls_.push_back(c);
    Json::Value v = Pop("ps_html_serve_classify");
    PsHtmlServeVerdict verdict = {};
    verdict.struct_size = sizeof(verdict);
    const GoogleString action =
        v.isNull() ? "fall_through" : v["action"].asString();
    if (action == "serve_html") verdict.action = kPsHtmlServeActionServeHtml;
    if (action == "serve_markdown") {
      verdict.action = kPsHtmlServeActionServeMarkdown;
    }
    if (action == "validate") verdict.action = kPsHtmlServeActionValidate;
    verdict.reason =
        IndexOf(kServeReasons,
                v.isNull() ? GoogleString("absent") : v["reason"].asString());
    verdict.enforce = 1;
    verdict.wants_markdown = v.isNull() || v["wants_markdown"].asBool() ? 1 : 0;
    verdict.age_s = v.isMember("age_s") ? v["age_s"].asUInt() : 0;
    const_cast<ConformanceAbi*>(this)->mutable_injection()->verdict = verdict;
    return InjectedVerdictDaemonAbi::HtmlServeClassify(request, facts, out);
  }

  const char* HtmlServeReasonName(int reason) const override {
    if (reason < 0 || static_cast<size_t>(reason) >= std::size(kServeReasons)) {
      return nullptr;
    }
    return kServeReasons[reason];
  }

  int HtmlStoreWriteParams(int action, uint32_t lifetime_s,
                           uint32_t store_flags, uint16_t cc_flags,
                           int sidecar_stored, int64_t capture_start_ms,
                           const char* const* age_lines, size_t age_count,
                           uint64_t content_length, const char* ct,
                           PsWriteParams* params) const override {
    Call c = Rec("ps_html_store_write_params");
    c.args["action"] = action;
    c.args["lifetime_s"] = lifetime_s;
    c.args["capture_start_ms"] = static_cast<Json::Int64>(capture_start_ms);
    c.args["sidecar_stored"] = sidecar_stored;
    c.args["store_flags"] = store_flags;
    c.args["cc_flags"] = cc_flags;
    Json::Value ages(Json::arrayValue);
    for (size_t i = 0; i < age_count; ++i) ages.append(age_lines[i]);
    c.args["age"] = ages;
    calls_.push_back(c);
    Json::Value v = Pop("ps_html_store_write_params");
    HtmlServeInjection* inj =
        const_cast<ConformanceAbi*>(this)->mutable_injection();
    inj->write_params_rc = v.isMember("rc") && RcOf(v["rc"]) != kPsOk ? -1 : 0;
    inj->write_params_inserted_at =
        v.isMember("inserted_at_s") ? v["inserted_at_s"].asUInt() : 0;
    return InjectedVerdictDaemonAbi::HtmlStoreWriteParams(
        action, lifetime_s, store_flags, cc_flags, sidecar_stored,
        capture_start_ms, age_lines, age_count, content_length, ct, params);
  }

  // The library's facts rule, applied to the volume's read results.
  int HtmlStoredFactsFromReads(const void* original, const void* markdown,
                               const void* withheld, int sidecar_present,
                               PsHtmlStoredFacts* out) const override {
    Call c = Rec("ps_html_stored_facts_from_reads");
    c.args["original"] = original != nullptr;
    c.args["markdown"] = markdown != nullptr;
    c.args["withheld"] = withheld != nullptr;
    c.args["sidecar_present"] = sidecar_present;
    calls_.push_back(c);
    if (out == nullptr || out->struct_size < sizeof(PsHtmlStoredFacts)) {
      return -1;
    }
    PsHtmlStoredFacts f = {};
    f.struct_size = sizeof(f);
    const auto* o = static_cast<const FakeVolumeEntry*>(original);
    const auto* m = static_cast<const FakeVolumeEntry*>(markdown);
    if (o != nullptr) {
      f.flags |= kPsHtmlFactsPresent;
      f.stored_at = o->inserted_at;
      f.lifetime_s = o->max_age;
      if (o->has_hash) memcpy(f.binding_hash, o->hash, kPsHtmlHashBytes);
      if ((o->cc_flags & kPsCcOriginNoTransform) != 0) {
        f.flags |= kPsHtmlFactsNoTransform;
      }
      if ((o->flags & kPsFlagHeadersSidecarRequired) != 0) {
        f.flags |= kPsHtmlFactsSidecarRequired;
      }
      if (sidecar_present != 0) f.flags |= kPsHtmlFactsSidecarPresent;
    }
    if (m != nullptr) f.flags |= kPsHtmlFactsVariantPresent;
    if (o != nullptr && withheld != nullptr) {
      f.flags |= kPsHtmlFactsWithheld;
      f.withheld_reason = kPsHtmlWithheldBelowFloor;
    } else if (o != nullptr && m != nullptr && o->has_hash && m->has_hash &&
               memcmp(o->hash, m->hash, kPsHtmlHashBytes) == 0) {
      f.flags |= kPsHtmlFactsBindingLive;
    }
    *out = f;
    return 0;
  }

  int HtmlMarkdownEtag(const uint8_t* binding, const uint8_t* markdown,
                       char* buf, size_t capacity,
                       size_t* out_len) const override {
    calls_.push_back(Rec("ps_html_markdown_etag"));
    Json::Value v = Pop("ps_html_markdown_etag");
    HtmlServeInjection* inj =
        const_cast<ConformanceAbi*>(this)->mutable_injection();
    inj->etag = v.isMember("etag") ? v["etag"].asString()
                                   : GoogleString("W/\"psm-0123456789abcdef\"");
    inj->etag_rc = 0;
    return InjectedVerdictDaemonAbi::HtmlMarkdownEtag(binding, markdown, buf,
                                                      capacity, out_len);
  }

  int IfNoneMatchMatchesWeak(const char* inm, const char* etag) const override {
    Call c = Rec("ps_if_none_match_matches_weak");
    c.args["if_none_match"] = Str(inm);
    c.args["etag"] = Str(etag);
    calls_.push_back(c);
    Json::Value v = Pop("ps_if_none_match_matches_weak");
    const_cast<ConformanceAbi*>(this)->mutable_injection()->inm_match =
        !v.isNull() && v["match"].asBool() ? 1 : 0;
    return InjectedVerdictDaemonAbi::IfNoneMatchMatchesWeak(inm, etag);
  }

  int CacheWriteHeadersSidecarSite(void* cache,
                                   const PsSidecarWriteParams* params,
                                   const char* const* names,
                                   const char* const* values, size_t count,
                                   int* out_verdict,
                                   int* out_stored) const override {
    Call c = Rec("ps_cache_write_headers_sidecar_site");
    c.args["url"] = params->url;
    c.args["site"] = SiteName(params->site);
    c.args["paired"] = params->origin_html_hash != nullptr;
    for (size_t i = 0; i < count; ++i) {
      c.headers.emplace_back(names[i], values[i]);
    }
    calls_.push_back(c);
    Json::Value v = Pop("ps_cache_write_headers_sidecar_site");
    if (v.isMember("rc") && RcOf(v["rc"]) != kPsOk) return RcOf(v["rc"]);
    if (v.isMember("stored") && v["stored"].asInt() == 0) {
      // The library's gate kept no block.
      if (out_verdict != nullptr) *out_verdict = 1;
      if (out_stored != nullptr) *out_stored = 0;
      return kPsOk;
    }
    return volume_->CacheWriteHeadersSidecarSite(
        cache, params, names, values, count, out_verdict, out_stored);
  }

  int HeadersSidecarRead(void* cache, const char* url, const char* host,
                         const char* scheme, const char* site,
                         void** out) const override {
    Call c = Rec("ps_headers_sidecar_read");
    c.args["url"] = url;
    calls_.push_back(c);
    return volume_->HeadersSidecarRead(cache, url, host, scheme, site, out);
  }
  size_t HeaderBlockCount(const void* b) const override {
    return volume_->HeaderBlockCount(b);
  }
  int HeaderBlockGet(const void* b, size_t i, const char** n, size_t* nl,
                     const char** v, size_t* vl) const override {
    return volume_->HeaderBlockGet(b, i, n, nl, v, vl);
  }
  const uint8_t* HeaderBlockPairingHash(const void* b) const override {
    return volume_->HeaderBlockPairingHash(b);
  }
  int HeaderBlockPairing(const void* b, const uint8_t* h) const override {
    calls_.push_back(Rec("ps_header_block_pairing"));
    return volume_->HeaderBlockPairing(b, h);
  }
  void HeaderBlockFree(void* b) const override { volume_->HeaderBlockFree(b); }

  int CacheRemoveSite(void* cache, const char* url, const char* host,
                      const char* scheme, const char* site) const override {
    Call c = Rec("ps_cache_remove_site");
    c.args["url"] = url;
    c.args["site"] = SiteName(site);
    calls_.push_back(c);
    Json::Value v = Pop("ps_cache_remove_site");
    if (v.isMember("rc") && RcOf(v["rc"]) != kPsOk) return RcOf(v["rc"]);
    return volume_->CacheRemoveSite(cache, url, host, scheme, site);
  }

  int CacheWriteSitePurgeEpoch(void* cache, const char* site,
                               uint64_t epoch_s) const override {
    Call c = Rec("ps_cache_write_site_purge_epoch");
    c.args["site"] = SiteName(site);
    c.args["epoch_s"] = static_cast<Json::UInt64>(epoch_s);
    calls_.push_back(c);
    Json::Value v = Pop("ps_cache_write_site_purge_epoch");
    if (v.isMember("rc") && RcOf(v["rc"]) != kPsOk) return RcOf(v["rc"]);
    return volume_->CacheWriteSitePurgeEpoch(cache, site, epoch_s);
  }

  int CacheReadSitePurgeEpoch(void* cache, const char* site,
                              uint64_t* out) const override {
    Call c = Rec("ps_cache_read_site_purge_epoch");
    c.args["site"] = SiteName(site);
    calls_.push_back(c);
    Json::Value v = Pop("ps_cache_read_site_purge_epoch");
    if (v.isMember("rc") && RcOf(v["rc"]) != kPsOk) return RcOf(v["rc"]);
    return volume_->CacheReadSitePurgeEpoch(cache, site, out);
  }

  int ReadSharedConfigHtmlMode(const char* path) const override {
    calls_.push_back(Rec("ps_read_shared_config_html_mode"));
    Json::Value v = Pop("ps_read_shared_config_html_mode");
    return v.isNull() ? html_mode_ : HtmlMode(v["value"].asString());
  }

  int ReadSharedConfigAgentEntitled(const char* path) const override {
    calls_.push_back(Rec("ps_read_shared_config_agent_entitled"));
    Json::Value v = Pop("ps_read_shared_config_agent_entitled");
    return v.isNull() ? agent_entitled_ : (v["value"].asBool() ? 1 : 0);
  }

  int WantsAgentMarkdown(const char* accept) const override {
    Call c = Rec("ps_wants_agent_markdown");
    c.args["accept"] = Str(accept);
    calls_.push_back(c);
    Json::Value v = Pop("ps_wants_agent_markdown");
    if (!v.isNull()) return v["value"].asBool() ? 1 : 0;
    return accept != nullptr && GoogleString(accept).find("text/markdown") !=
                                    GoogleString::npos
               ? 1
               : 0;
  }

  int CacheWriteOriginalSite(void* cache, const char* url, const char* host,
                             const char* scheme, const char* site,
                             const PsWriteParams* params,
                             void** out) const override {
    Call c = Rec("ps_cache_write_original_site");
    c.args["url"] = url;
    c.args["hostname"] = host;
    c.args["site"] = SiteName(site);
    c.args["inserted_at_s"] = params->cache_inserted_at;
    c.args["lifetime_s"] = params->origin_max_age;
    c.args["hashed"] = params->origin_html_hash != nullptr;
    calls_.push_back(c);
    return RecordingDaemonAbi::CacheWriteOriginalSite(cache, url, host, scheme,
                                                      site, params, out);
  }

  int WriteClose(void* handle) const override {
    calls_.push_back(Rec("ps_write_close"));
    return RecordingDaemonAbi::WriteClose(handle);
  }

  int CacheReadAlternateSite(void* cache, const char* url, const char* host,
                             const char* scheme, const char* site, uint8_t id,
                             void** out) const override {
    Call c = Rec("ps_cache_read_alternate_site");
    c.args["url"] = url;
    c.args["alternate"] = id == kPsSentinelOriginal           ? "original"
                          : id == kPsSentinelAgentMarkdown    ? "markdown"
                          : id == kPsSentinelMarkdownWithheld ? "withheld"
                                                              : "other";
    calls_.push_back(c);
    return RecordingDaemonAbi::CacheReadAlternateSite(cache, url, host, scheme,
                                                      site, id, out);
  }

  int CacheReadBestSite(void* cache, const char* url, const char* host,
                        const char* scheme, const char* site, uint32_t mask,
                        void** out) const override {
    calls_.push_back(Rec("ps_cache_read_best_site"));
    return RecordingDaemonAbi::CacheReadBestSite(cache, url, host, scheme, site,
                                                 mask, out);
  }

  int NotifyWorker(const char* socket,
                   const PsNotifyParams* params) const override {
    Call c = Rec("ps_notify_worker_ex");
    c.args["url"] = params->url;
    c.args["agent_request"] = params->agent_request != 0;
    c.args["content_type"] = params->content_type;
    c.args["notify_flags"] = params->notify_flags;
    calls_.push_back(c);
    return RecordingDaemonAbi::NotifyWorker(socket, params);
  }

 private:
  static Json::Value Str(const char* s) {
    return s == nullptr ? Json::Value() : Json::Value(s);
  }
  static Call Rec(const char* fn) {
    Call c;
    c.fn = fn;
    return c;
  }
  GoogleString SiteName(const char* site) const {
    if (site == nullptr) return "(null)";
    auto it = site_names_->find(site);
    return it == site_names_->end() ? GoogleString(site) : it->second;
  }
  Json::Value Pop(const GoogleString& fn) const {
    auto it = queues_.find(fn);
    if (it == queues_.end() || it->second.empty()) return Json::Value();
    Json::Value v = it->second.front();
    it->second.erase(it->second.begin());
    return v;
  }

  FakeDaemonVolume* volume_;
  std::map<GoogleString, GoogleString>* site_names_;
  mutable std::map<GoogleString, std::vector<Json::Value>> queues_;
  mutable std::vector<Call> calls_;
  mutable int store_classify_calls_ = 0;
  int minor_ = 14;
  bool set_bound_ = true;
  int html_mode_ = kPsHtmlModeMarkdownStatic;
  int agent_entitled_ = 1;
};

struct Row {
  GoogleString id;
  Json::Value body;  // the row's engine body
};

const Json::Value& Cases() {
  static const Json::Value* cases = [] {
    auto* root = new Json::Value;
    std::ifstream in(StrCat(GTestSrcDir(), kCasesPath).c_str(),
                     std::ios::binary);
    std::stringstream buf;
    buf << in.rdbuf();
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(buf.str());
    CHECK(Json::parseFromStream(builder, stream, root, &errors)) << errors;
    return root;
  }();
  return *cases;
}

// One row, run.
class EngineRowRunner : public DaemonHtmlEngine::TestHooks {
 public:
  explicit EngineRowRunner(const Json::Value& row)
      : engine_body_(row["engine"]),
        bodies_(Cases()["bodies"]),
        thread_system_(Platform::CreateThreadSystem()),
        timer_(new NullMutex, kStartMs),
        stats_(thread_system_.get()),
        limiter_(/*test_key=*/0x5eed) {
    RewriteStats::InitStats(&stats_);
    default_site_ = SiteId("default");
    site_names_[default_site_] = "default";
    volume_.set_now_seconds(kStartMs / 1000);
    abi_ =
        std::make_unique<ConformanceAbi>(&volume_, engine_body_["verdicts"],
                                         engine_body_["library"], &site_names_);
    FillVolume();
    ConfigureEngine();
  }

  void Run() {
    CreateEngine();
    int index = 0;
    for (const Json::Value& step : engine_body_["steps"]) {
      SCOPED_TRACE(StrCat("step ", IntegerToString(index++), ": ",
                          step["kind"].asString()));
      RunStep(step);
    }
  }

  void AtPoint(const char* point) override { RunDuring(point); }

 private:
  // ---- setup

  GoogleString Body(const Json::Value& ref) const {
    return ref.isNull() ? GoogleString() : bodies_[ref.asString()].asString();
  }

  void FillVolume() {
    std::map<GoogleString, bool> paired;
    for (const Json::Value& e : engine_body_["volume"]) {
      const GoogleString kind = e["kind"].asString();
      if (kind == "epoch") {
        const GoogleString site = e["site"].asString() == "default"
                                      ? default_site_
                                      : SiteFor(e["site"].asString());
        volume_.epochs()[site] = e["epoch_s"].asUInt64();
        continue;
      }
      const GoogleString url = e["url"].asString();
      const GoogleString key =
          FakeDaemonVolume::Key(default_site_.c_str(), url.c_str());
      if (kind == "original") {
        FakeVolumeEntry entry;
        entry.alternate_id = kPsSentinelOriginal;
        entry.body = Body(e["body_ref"]);
        entry.inserted_at = e["stored_at_s"].asUInt();
        entry.max_age = e["lifetime_s"].asUInt();
        entry.cc_flags = CcFlagsOf(e["cc_flags"]);
        for (const Json::Value& f : e["flags"]) {
          if (f.asString() == "headers_classified") {
            entry.flags |= kPsFlagHeadersClassified;
          }
        }
        entry.has_hash = true;
        Sha256(entry.body, entry.hash);
        paired[key] = e["paired"].asBool();
        volume_.entries()[{key, kPsSentinelOriginal}] = entry;
      }
    }
    for (const Json::Value& e : engine_body_["volume"]) {
      const GoogleString kind = e["kind"].asString();
      if (kind == "epoch" || kind == "original") continue;
      const GoogleString url = e["url"].asString();
      const GoogleString key =
          FakeDaemonVolume::Key(default_site_.c_str(), url.c_str());
      const FakeVolumeEntry* original =
          volume_.Find(default_site_, url, kPsSentinelOriginal);
      uint8_t page_hash[kPsHtmlHashBytes];
      Sha256(Body(Json::Value("page")), page_hash);
      const uint8_t* parent = original != nullptr ? original->hash : page_hash;
      if (kind == "sidecar") {
        FakeVolumeSidecar s;
        for (const Json::Value& line : e["headers"]) {
          s.lines.emplace_back(line[0].asString(), line[1].asString());
        }
        s.has_hash = true;
        if (original == nullptr || paired[key]) {
          memcpy(s.hash, parent, kPsHtmlHashBytes);
        } else {
          Sha256("a header block from another store", s.hash);
        }
        volume_.sidecars()[key] = s;
      } else if (kind == "markdown") {
        FakeVolumeEntry entry;
        entry.alternate_id = kPsSentinelAgentMarkdown;
        entry.body = e.isMember("body_ref") ? Body(e["body_ref"])
                                            : Body(Json::Value("page_md"));
        entry.content_type = kPsContentOther;
        entry.origin_ct = "text/markdown; charset=utf-8";
        entry.has_hash = true;
        memcpy(entry.hash, parent, kPsHtmlHashBytes);
        volume_.entries()[{key, kPsSentinelAgentMarkdown}] = entry;
      } else if (kind == "withheld") {
        FakeVolumeEntry entry;
        entry.alternate_id = kPsSentinelMarkdownWithheld;
        entry.has_hash = true;
        memcpy(entry.hash, parent, kPsHtmlHashBytes);
        volume_.entries()[{key, kPsSentinelMarkdownWithheld}] = entry;
      }
    }
  }

  GoogleString SiteFor(const GoogleString& name) {
    if (name == "default") return default_site_;
    const GoogleString id = SiteId(name);
    site_names_[id] = name;
    return id;
  }

  void ConfigureEngine() {
    const Json::Value& c = engine_body_["config"];
    config_.mode = DaemonHtmlMode::kServeVariants;
    if (c.isMember("mode")) {
      const GoogleString m = c["mode"].asString();
      config_.mode = m == "off"           ? DaemonHtmlMode::kOff
                     : m == "report-only" ? DaemonHtmlMode::kReportOnly
                                          : DaemonHtmlMode::kServeVariants;
    }
    config_.agent_optimize =
        !c.isMember("agent_optimize") || c["agent_optimize"].asBool();
    if (c.isMember("max_bytes")) config_.max_bytes = c["max_bytes"].asInt64();
    if (c.isMember("host_names")) {
      const Json::Value& names = c["host_names"];
      config_.host_names.primary = names[0].asString();
      for (Json::ArrayIndex i = 0; i < names.size(); ++i) {
        config_.host_names.aliases.push_back(names[i].asString());
      }
    } else {
      config_.host_names.primary = kDefaultHost;
    }
    if (c.isMember("budget_bytes")) {
      budget_.set_capacity(c["budget_bytes"].asInt64());
    }
    config_.site = default_site_;
    for (const Json::Value& s : c["sites"]) {
      config_.epoch_sites.push_back(SiteFor(s.asString()));
    }
  }

  void CreateEngine() {
    engine_.reset();
    DaemonHtmlEngine::Deps deps;
    deps.substrate.abi = abi_.get();
    deps.substrate.cache = &volume_;
    deps.substrate.cache_path = "/var/cache/pagespeed";
    deps.substrate.notify_socket = "/run/pagespeed/worker.sock";
    deps.purge_source = &source_;
    deps.statistics = &stats_;
    deps.timer = &timer_;
    deps.handler = &handler_;
    deps.limiter = &limiter_;
    deps.budget = &budget_;
    engine_ = std::make_unique<DaemonHtmlEngine>(config_, deps);
    engine_->set_test_hooks(this);
    engine_->oracle()->set_recheck_disabled_for_testing(recheck_disabled_);
  }

  // ---- ops

  int64 Now() const { return timer_.NowMs(); }

  void ApplyPending(bool all) {
    for (auto it = pending_url_.begin(); it != pending_url_.end();) {
      if (all || Now() >= it->visible_at_ms) {
        for (const GoogleString& key : DaemonHtmlPurgeKeys(it->url)) {
          source_.purge_set()->Put(key, it->at_ms);
        }
        it = pending_url_.erase(it);
      } else {
        ++it;
      }
    }
    if (all) {
      for (int64 at_ms : pending_global_) {
        source_.purge_set()->UpdateGlobalInvalidationTimestampMs(at_ms);
      }
      pending_global_.clear();
    }
  }

  std::vector<GoogleString> EpochSites() const {
    if (config_.epoch_sites.empty()) return {default_site_};
    return config_.epoch_sites;
  }

  void RunOp(const Json::Value& op) {
    const GoogleString name = op.getMemberNames()[0];
    const Json::Value& arg = op[name];
    if (name == "advance_clock") {
      timer_.AdvanceMs(arg["ms"].asInt64());
      volume_.set_now_seconds(Now() / 1000);
      ApplyPending(false);
    } else if (name == "purge_url") {
      const int64 at = arg.isMember("at_ms") ? arg["at_ms"].asInt64() : Now();
      if (arg.isMember("visible_at_ms") &&
          arg["visible_at_ms"].asInt64() > Now()) {
        pending_url_.push_back(
            {arg["url"].asString(), at, arg["visible_at_ms"].asInt64()});
      } else {
        for (const GoogleString& key :
             DaemonHtmlPurgeKeys(arg["url"].asString())) {
          source_.purge_set()->Put(key, at);
        }
      }
    } else if (name == "purge_global") {
      const int64 at = arg.isMember("at_ms") ? arg["at_ms"].asInt64() : Now();
      const GoogleString source = arg["source"].asString();
      if (source == "admin") {
        source_.purge_set()->UpdateGlobalInvalidationTimestampMs(at);
        engine_->OnAdminPurge(
            StrCat("https://", kDefaultHost, "/*"), true, at,
            [this](bool ok, const GoogleString&) { admin_ok_ = ok; });
      } else if (source == "flush_file") {
        source_.SetFlush(at);
      } else {  // other_process: its epoch file write, not yet seen here.
        for (const GoogleString& site : EpochSites()) {
          uint64_t& e = volume_.epochs()[site];
          e = std::max<uint64_t>(e, static_cast<uint64_t>(at / 1000));
        }
        pending_global_.push_back(at);
      }
    } else if (name == "flush_file") {
      source_.SetFlush(arg["mtime_ms"].asInt64());
    } else if (name == "wipe_file_cache") {
      source_.Wipe();
      pending_url_.clear();
      pending_global_.clear();
    } else if (name == "create_engine") {
      CreateEngine();
    } else if (name == "make_visible") {
      ApplyPending(true);
    } else if (name == "disable_recheck") {
      recheck_disabled_ = true;
      engine_->oracle()->set_recheck_disabled_for_testing(true);
    } else if (name == "budget") {
      budget_.set_capacity(arg["bytes"].asInt64());
    } else {
      ADD_FAILURE() << "unknown op " << name;
    }
  }

  void RunDuring(const GoogleString& point) {
    if (during_ == nullptr) return;
    for (const Json::Value& d : *during_) {
      if (d["point"].asString() != point) continue;
      for (const Json::Value& op : d["do"]) RunOp(op);
    }
  }

  // ---- steps

  DaemonHtmlRequestFacts Facts(const Json::Value& r) const {
    DaemonHtmlRequestFacts f;
    f.method = r.get("method", "GET").asString();
    f.scheme = r.get("scheme", "https").asString();
    f.host = r.get("host", kDefaultHost).asString();
    f.path = r.get("path", "/page").asString();
    f.site = default_site_;
    f.url_agreed = r.get("url_agreed", true).asBool();
    f.options_enabled_and_allowed =
        r.get("options_enabled_and_allowed", true).asBool();
    f.capture_start_ms =
        r.get("capture_start_ms", Json::Int64(Now())).asInt64();
    for (const Json::Value& a : r["accept"])
      f.accept_lines.push_back(a.asString());
    f.has_cookie = r.get("cookie", false).asBool();
    f.has_authorization = r.get("authorization", false).asBool();
    f.authenticated_user = r.get("authenticated_user", false).asBool();
    if (r.isMember("cache_control")) {
      f.has_cache_control = true;
      f.cache_control = r["cache_control"].asString();
    }
    if (r.isMember("pragma")) {
      f.has_pragma = true;
      f.pragma = r["pragma"].asString();
    }
    if (r.isMember("if_none_match")) {
      f.has_if_none_match = true;
      f.if_none_match = r["if_none_match"].asString();
    }
    if (r.isMember("if_modified_since")) {
      f.has_if_modified_since = true;
      f.if_modified_since = r["if_modified_since"].asString();
    }
    f.has_if_match = r.get("if_match", false).asBool();
    f.has_if_unmodified_since = r.get("if_unmodified_since", false).asBool();
    f.has_if_range = r.get("if_range", false).asBool();
    f.has_range = r.get("range", false).asBool();
    f.has_origin = r.get("origin", false).asBool();
    return f;
  }

  static DaemonHtmlHeaderLines Lines(const Json::Value& lines) {
    DaemonHtmlHeaderLines out;
    for (const Json::Value& l : lines) {
      out.emplace_back(l[0].asString(), l[1].asString());
    }
    return out;
  }

  void RunStep(const Json::Value& step) {
    const size_t calls_before = abi_->calls().size();
    const size_t notifies_before = abi_->notifies().size();
    const int logs_before = handler_.count();
    const int writes_before = volume_.writes_closed();
    std::map<GoogleString, int64> counters_before = Counters();
    port_ = FakePort();
    purge_set_added_.clear();
    admin_ok_ = true;
    serve_ = DaemonHtmlServeResult();
    during_ = &step["during"];

    for (const Json::Value& op : step["pre"]) RunOp(op);
    const GoogleString kind = step["kind"].asString();
    const Json::Value& request = step["request"];
    if (kind == "capture") {
      RunCapture(request, step["response"]);
    } else if (kind == "serve") {
      serve_ = engine_->Serve(Facts(request), &port_);
    } else if (kind == "poll_tick") {
      engine_->OnPurgePollTick();
    } else if (kind == "stats_tick") {
      engine_->OnStatisticsTick();
    } else if (kind == "admin_purge") {
      const GoogleString url = request["url"].asString();
      const bool global = !url.empty() && url[url.size() - 1] == '*';
      if (global) {
        source_.purge_set()->UpdateGlobalInvalidationTimestampMs(Now());
      } else {
        for (const GoogleString& key : DaemonHtmlPurgeKeys(url)) {
          source_.purge_set()->Put(key, Now());
          purge_set_added_.push_back(key);
        }
      }
      engine_->OnAdminPurge(
          url, global, Now(),
          [this](bool ok, const GoogleString&) { admin_ok_ = ok; });
    } else {
      ADD_FAILURE() << "unknown step kind " << kind;
    }
    during_ = nullptr;

    const std::vector<Call> calls(abi_->calls().begin() + calls_before,
                                  abi_->calls().end());
    const std::vector<RecordedNotify> notifies(
        abi_->notifies().begin() + notifies_before, abi_->notifies().end());
    Check(step["expect"], calls, notifies, handler_.count() - logs_before,
          volume_.writes_closed() - writes_before, counters_before);
  }

  void RunCapture(const Json::Value& request, const Json::Value& response) {
    abi_->BeginCapture();
    const DaemonHtmlRequestFacts facts = Facts(request);
    std::unique_ptr<DaemonHtmlRecording> rec =
        engine_->StartRecording(facts, &port_);
    RunDuring("before_send");
    const DaemonHtmlHeaderLines origin = Lines(response["origin"]);
    const DaemonHtmlHeaderLines module = Lines(response["module"]);
    DaemonHtmlHeaderLines store_point = origin;
    for (const auto& l : Lines(response["store_delta"]))
      store_point.push_back(l);
    for (const auto& l : module) store_point.push_back(l);
    DaemonHtmlHeaderLines final_point = origin;
    for (const auto& l : Lines(response["final_delta"]))
      final_point.push_back(l);
    for (const auto& l : module) final_point.push_back(l);
    const GoogleString body = Body(response["body_ref"]);
    if (rec != nullptr) {
      rec->set_status(response.get("status", 200).asInt());
      rec->SetOriginHeaders(origin);
      rec->SetBaselineHeaders(origin);
      rec->SetModuleHeaders(module);
      rec->NoteStoreHeaders(store_point);
      // In pieces, as a server delivers it.
      for (size_t i = 0; i < body.size(); i += 100) {
        rec->AddBody(body.data() + i, std::min<size_t>(100, body.size() - i));
      }
      rec->NoteFinalHeaders(final_point);
    }
    RunDuring("after_send");
    if (rec != nullptr) rec->Finish(response.get("aborted", false).asBool());
  }

  std::map<GoogleString, int64> Counters() {
    std::map<GoogleString, int64> out;
    for (const char* const* c = RewriteStats::kDaemonHtmlEngineCounters;
         *c != nullptr; ++c) {
      out[*c] = stats_.GetVariable(*c)->Get();
    }
    for (const char* const* r = RewriteStats::kDaemonMarkdownFallthroughReasons;
         *r != nullptr; ++r) {
      const GoogleString name = StrCat("daemon_markdown_fallthrough_", *r);
      out[name] = stats_.GetVariable(name)->Get();
    }
    return out;
  }

  // ---- expectations

  static bool HeaderMatches(const std::pair<GoogleString, GoogleString>& line,
                            const GoogleString& spec) {
    const size_t colon = spec.find(": ");
    if (colon == GoogleString::npos) {
      return StringCaseEqual(line.first, spec);
    }
    return StringCaseEqual(line.first, spec.substr(0, colon)) &&
           line.second == spec.substr(colon + 2);
  }

  bool ArgsMatch(const Call& call, const Json::Value& args) const {
    for (const GoogleString& key : args.getMemberNames()) {
      const Json::Value& want = args[key];
      if (key == "headers_include" || key == "headers_exclude") {
        for (const Json::Value& spec : want) {
          bool found = false;
          for (const auto& line : call.headers) {
            if (HeaderMatches(line, spec.asString())) found = true;
          }
          if (found != (key == "headers_include")) return false;
        }
        continue;
      }
      if (!call.args.isMember(key)) return false;
      const Json::Value& got = call.args[key];
      if (want.isNumeric() && got.isNumeric()) {
        if (want.asInt64() != got.asInt64()) return false;
      } else if (key == "age") {
        if (want != got) return false;
      } else if (want != got) {
        return false;
      }
    }
    return true;
  }

  static GoogleString Describe(const std::vector<Call>& calls) {
    GoogleString out;
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    for (const Call& c : calls) {
      StrAppend(&out, "  ", c.fn, " ", Json::writeString(w, c.args));
      if (!c.headers.empty()) {
        StrAppend(&out, " headers:");
        for (const auto& h : c.headers) {
          StrAppend(&out, " [", h.first, ": ", h.second, "]");
        }
      }
      StrAppend(&out, "\n");
    }
    return out;
  }

  void Check(const Json::Value& expect, const std::vector<Call>& calls,
             const std::vector<RecordedNotify>& notifies, int log_lines,
             int writes, const std::map<GoogleString, int64>& before) {
    const GoogleString trace = StrCat("calls in the step:\n", Describe(calls));
    SCOPED_TRACE(trace);
    if (expect.isMember("calls")) {
      size_t at = 0;
      for (const Json::Value& want : expect["calls"]) {
        const GoogleString fn = want["fn"].asString();
        bool found = false;
        for (; at < calls.size(); ++at) {
          if (calls[at].fn == fn && ArgsMatch(calls[at], want["args"])) {
            found = true;
            ++at;
            break;
          }
        }
        EXPECT_TRUE(found) << "expected call " << fn << " "
                           << want["args"].toStyledString();
      }
    }
    for (const Json::Value& fn : expect["calls_exclude"]) {
      for (const Call& c : calls) {
        EXPECT_NE(fn.asString(), c.fn) << "call not expected";
      }
    }
    for (const Json::Value& want : expect["port_calls"]) {
      EXPECT_NE(
          port_.calls.end(),
          std::find(port_.calls.begin(), port_.calls.end(), want.asString()))
          << "port call " << want.asString();
    }
    for (const Json::Value& want : expect["port_calls_exclude"]) {
      EXPECT_EQ(
          port_.calls.end(),
          std::find(port_.calls.begin(), port_.calls.end(), want.asString()))
          << "port call " << want.asString();
    }
    if (expect.isMember("stored")) {
      if (expect["stored"].empty()) {
        EXPECT_EQ(0, writes) << "nothing may be stored in this step";
      }
      for (const Json::Value& want : expect["stored"]) {
        const FakeVolumeEntry* e = volume_.Find(
            default_site_, want["url"].asString(), kPsSentinelOriginal);
        ASSERT_NE(nullptr, e) << "stored " << want["url"].asString();
        if (want.isMember("lifetime_s")) {
          EXPECT_EQ(want["lifetime_s"].asUInt(), e->max_age);
        }
        if (want.isMember("stored_at_s")) {
          EXPECT_EQ(want["stored_at_s"].asUInt(), e->inserted_at);
        }
        if (want.isMember("cc_flags")) {
          EXPECT_EQ(CcFlagsOf(want["cc_flags"]), e->cc_flags) << "cc_flags";
        }
      }
    }
    for (const Json::Value& want : expect["removed"]) {
      bool found = false;
      for (const Call& c : calls) {
        if (c.fn == "ps_cache_remove_site" &&
            c.args["url"].asString() == want["url"].asString()) {
          found = true;
        }
      }
      EXPECT_TRUE(found) << "removed " << want["url"].asString();
      EXPECT_EQ(nullptr, volume_.Find(default_site_, want["url"].asString(),
                                      kPsSentinelOriginal));
    }
    if (expect.isMember("notify")) {
      ASSERT_EQ(expect["notify"].size(), notifies.size()) << "notifications";
      for (Json::ArrayIndex i = 0; i < notifies.size(); ++i) {
        const Json::Value& want = expect["notify"][i];
        const RecordedNotify& got = notifies[i];
        if (want.isMember("content")) {
          EXPECT_EQ(want["content"].asString() == "html" ? kPsContentHtml : -2,
                    got.content_type);
        }
        if (want.isMember("agent_request")) {
          EXPECT_EQ(want["agent_request"].asBool() ? 1 : 0, got.agent_request);
        }
        if (want.isMember("flags")) {
          uint32_t flags = 0;
          for (const Json::Value& f : want["flags"]) {
            if (f.asString() == "from_module") flags |= kPsNotifyFlagFromModule;
            if (f.asString() == "host_vouched") {
              flags |= kPsNotifyFlagHostVouched;
            }
            if (f.asString() == "html_stored") flags |= kPsNotifyFlagHtmlStored;
          }
          EXPECT_EQ(flags, got.notify_flags);
        }
        EXPECT_EQ(default_site_, got.site);
      }
    }
    if (expect.isMember("counters")) {
      const std::map<GoogleString, int64> after = Counters();
      for (const GoogleString& name : expect["counters"].getMemberNames()) {
        ASSERT_TRUE(after.count(name) == 1) << "unknown counter " << name;
        EXPECT_EQ(expect["counters"][name].asInt64(),
                  after.at(name) - before.at(name))
            << name;
      }
    }
    if (expect.isMember("serve")) CheckServe(expect["serve"]);
    if (expect.isMember("log_lines")) {
      EXPECT_EQ(expect["log_lines"].asInt(), log_lines)
          << JoinCollection(handler_.messages(), "\n");
    }
    if (expect.isMember("admin_response")) {
      EXPECT_EQ(expect["admin_response"].asString(),
                admin_ok_ ? "ok" : "error");
    }
    if (expect.isMember("purge_set_added")) {
      std::vector<GoogleString> want;
      for (const Json::Value& k : expect["purge_set_added"]) {
        want.push_back(k.asString());
      }
      EXPECT_EQ(want, purge_set_added_);
    }
    if (expect.isMember("volume_epoch_s")) {
      EXPECT_EQ(expect["volume_epoch_s"].asUInt64(),
                volume_.epochs()[default_site_]);
    }
    if (expect.isMember("budget_in_use")) {
      EXPECT_EQ(expect["budget_in_use"].asInt64(), budget_.in_use());
    }
  }

  void CheckServe(const Json::Value& want) {
    const bool final = serve_.kind == DaemonHtmlServeResult::kFinal;
    EXPECT_EQ(want["kind"].asString(), final ? "final" : "fall_through");
    if (want.isMember("reason")) {
      EXPECT_EQ(want["reason"].asString(), serve_.reason);
    }
    if (want.isMember("status")) {
      EXPECT_EQ(want["status"].asInt(), serve_.status);
    }
    if (want.isMember("headers_exact")) {
      const Json::Value& exact = want["headers_exact"];
      // The complete set: every line's name is listed, with its count.
      for (const auto& line : port_.emitted.headers) {
        bool listed = false;
        for (const GoogleString& name : exact.getMemberNames()) {
          if (StringCaseEqual(name, line.first)) listed = true;
        }
        EXPECT_TRUE(listed) << "unexpected header " << line.first;
      }
      for (const GoogleString& name : exact.getMemberNames()) {
        int count = 0;
        GoogleString value;
        for (const auto& line : port_.emitted.headers) {
          if (StringCaseEqual(name, line.first)) {
            ++count;
            value = line.second;
          }
        }
        EXPECT_EQ(exact[name]["count"].asInt(), count) << name;
        if (exact[name].isMember("value")) {
          GoogleString v = exact[name]["value"].asString();
          if (HasPrefixString(v, "$len:")) {
            v = Integer64ToString(Body(Json::Value(v.substr(5))).size());
          }
          EXPECT_EQ(v, value) << name;
        }
      }
    }
    if (want.isMember("body_bytes")) {
      EXPECT_EQ(want["body_bytes"].asUInt64(), port_.emitted.body.size());
    }
  }

  struct PendingUrl {
    GoogleString url;
    int64 at_ms;
    int64 visible_at_ms;
  };

  const Json::Value& engine_body_;
  const Json::Value& bodies_;
  std::unique_ptr<ThreadSystem> thread_system_;
  MockTimer timer_;
  SimpleStats stats_;
  CountingHandler handler_;
  DaemonHealNotifyLimiter limiter_;
  DaemonHtmlByteBudget budget_;
  FakeDaemonVolume volume_;
  FakePurgeSource source_;
  std::map<GoogleString, GoogleString> site_names_;
  GoogleString default_site_;
  std::unique_ptr<ConformanceAbi> abi_;
  DaemonHtmlEngine::Config config_;
  std::unique_ptr<DaemonHtmlEngine> engine_;
  bool recheck_disabled_ = false;
  FakePort port_;
  DaemonHtmlServeResult serve_;
  const Json::Value* during_ = nullptr;
  std::vector<PendingUrl> pending_url_;
  std::vector<int64> pending_global_;
  std::vector<GoogleString> purge_set_added_;
  bool admin_ok_ = true;
};

class EngineRowTest : public testing::Test {
 public:
  explicit EngineRowTest(Json::ArrayIndex index) : index_(index) {}
  void TestBody() override {
    EngineRowRunner runner(Cases()["rows"][index_]);
    runner.Run();
  }

 private:
  Json::ArrayIndex index_;
};

// Registers one test per engine row before main runs them.
int RegisterRows() {
  const Json::Value& rows = Cases()["rows"];
  int registered = 0;
  for (Json::ArrayIndex i = 0; i < rows.size(); ++i) {
    bool engine = false;
    for (const Json::Value& r : rows[i]["runners"]) {
      if (r.asString() == "engine") engine = true;
    }
    if (!engine) continue;
    GoogleString name = rows[i]["id"].asString();
    for (char& c : name) {
      if (c == '-') c = '_';
    }
    testing::RegisterTest(
        "DaemonHtmlEngineRows", name.c_str(), nullptr, nullptr, __FILE__,
        __LINE__, [i]() -> testing::Test* { return new EngineRowTest(i); });
    ++registered;
  }
  return registered;
}

const int kRegisteredRows = RegisterRows();

// Every engine row runs: none is skipped, and the file has them.
TEST(DaemonHtmlEngineRowsMeta, EveryEngineRowIsRegistered) {
  int engine_rows = 0;
  for (const Json::Value& row : Cases()["rows"]) {
    for (const Json::Value& r : row["runners"]) {
      if (r.asString() == "engine") ++engine_rows;
    }
  }
  EXPECT_GT(engine_rows, 0);
  EXPECT_EQ(engine_rows, kRegisteredRows);
}

}  // namespace
}  // namespace net_instaweb
