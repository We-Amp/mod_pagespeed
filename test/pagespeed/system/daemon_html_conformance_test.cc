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
#include <concepts>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

#include "gtest/gtest-spi.h"
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
#include "pagespeed/system/daemon_html_agent_class.h"
#include "pagespeed/system/daemon_html_engine.h"
#include "pagespeed/system/daemon_html_purge_observer.h"
#include "pagespeed/system/daemon_serve_arm.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/kernel/http/agent_vectors.h"
#include "test/pagespeed/system/fake_daemon_volume.h"
#include "test/pagespeed/system/injected_verdict_daemon_abi.h"

#ifdef DAEMON_HTML_CONFORMANCE_ON_STUB
#include "test/pagespeed/system/daemon_stub_test_util.h"
#endif

namespace net_instaweb {
namespace {

const char kCasesPath[] = "/test/system/daemon_html/conformance_cases.json";
const int64 kStartMs = 1790000000000LL;
const char kDefaultHost[] = "www.example.test";

// ---- How the runner reports what it cannot do.
//
// A failure of a row is one of three kinds, told apart by its prefix, so a
// pending row is held to "fails on behaviour, or on an entry point or
// counter the serve path is pinned to add" (EveryPendingRowRunsAndStillFails):
//   kRunnerError      the runner cannot run the row as written (an unknown
//                     key, name or value): never allowed;
//   kEntryPointAbsent an entry point of kServePathEntryPoints the engine does
//                     not declare yet;
//   kCounterAbsent    a counter of kServePathCounters not registered yet.
// Every other failure is the engine's behaviour.
const char kRunnerError[] = "RUNNER: ";
const char kEntryPointAbsent[] = "ENTRY POINT NOT DECLARED YET: ";
const char kCounterAbsent[] = "COUNTER NOT REGISTERED YET: ";

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

const char* const kStoreReasons[] = {
    "none", "method", "status", "incomplete", "auth_request", "cookie_request",
    "set_cookie", "private", "no_store", "no_lifetime", "vary", "headers",
    "nonce_csp", "nonce_link", "content_type", "encoding", "size", "url",
    "personalisation_signal", "no_transform", "mode",
    // 1.15
    "request_header", "path"};

const char* const kServeReasons[] = {
    "fresh", "mode", "auth_request", "cookie_request", "client_no_cache",
    "needs_validation", "stale", "headers_not_reproducible", "sidecar_missing",
    "purged", "absent", "binding_stale", "parent_stale", "method", "range",
    "cors", "no_transform", "withheld", "skew",
    // 1.15
    "scope", "capped", "conditional", "credentialed", "legacy_entry",
    "policy_mismatch", "request_header", "path"};

template <size_t N>
int IndexOf(const char* const (&names)[N], const GoogleString& name) {
  for (size_t i = 0; i < N; ++i) {
    if (name == names[i]) return static_cast<int>(i);
  }
  return -1;
}

int RcOf(const Json::Value& v) {
  if (v.isIntegral()) return v.asInt();  // 0 is ok
  const GoogleString rc = v.asString();
  if (rc == "ok") return kPsOk;
  if (rc == "not_found") return kPsErrNotFound;
  if (rc == "io_error" || rc == "io") return 2;  // PS_ERR_IO
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

// The ps_html_store_classify out_flags bits a row names (PS_HTML_STORE_FLAG_*);
// a name outside the set is a row error.
uint32_t StoreFlagsOf(const Json::Value& names) {
  static const std::pair<const char*, uint32_t> kFlags[] = {
      {"lifetime_explicit", 1u},
      {"needs_validation", 2u},
      {"has_validators", 4u},
      {"explicit_shared", 8u},
      {"sidecar_evaluated", 16u},
      {"cc_evaluated", 32u},
      {"request_anonymous", kPsHtmlStoreFlagRequestAnonymous}};
  uint32_t out = 0;
  for (const Json::Value& n : names) {
    bool known = false;
    for (const auto& [name, bit] : kFlags) {
      if (n.asString() == name) {
        out |= bit;
        known = true;
      }
    }
    EXPECT_TRUE(known) << kRunnerError << "unknown store flag " << n.asString();
  }
  return out;
}

// A withheld marker's reason as a row names it (PS_HTML_WITHHELD_*).
int WithheldReasonOf(const Json::Value& name) {
  static const std::pair<const char*, int> kReasons[] = {
      {"below_floor", kPsHtmlWithheldBelowFloor},
      {"too_large", kPsHtmlWithheldTooLarge},
      {"encoded", kPsHtmlWithheldEncoded},
      {"hash_mismatch", kPsHtmlWithheldHashMismatch},
      {"extract_failed", kPsHtmlWithheldExtractFailed}};
  for (const auto& [n, reason] : kReasons) {
    if (name.asString() == n) return reason;
  }
  ADD_FAILURE() << kRunnerError << "unknown withheld reason "
                << name.asString();
  return kPsHtmlWithheldBelowFloor;
}

// A policy stamp as a row writes it: 16 hex digits (or a number).
uint64_t StampOf(const Json::Value& v) {
  if (v.isIntegral()) return v.asUInt64();
  uint64_t out = 0;
  const GoogleString s = v.asString();
  EXPECT_TRUE(!s.empty() && s.size() <= 16) << "stamp " << s;
  for (const char c : s) {
    out <<= 4;
    if (c >= '0' && c <= '9') {
      out |= static_cast<uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      out |= static_cast<uint64_t>(c - 'a' + 10);
    } else {
      ADD_FAILURE() << kRunnerError << "stamp " << s
                    << " is not lower-case hex";
    }
  }
  return out;
}

// A policy stamp as the recorder renders it, so a row can name it.
GoogleString StampName(uint64_t stamp) {
  static const char kDigits[] = "0123456789abcdef";
  GoogleString out(16, '0');
  for (int i = 15; i >= 0; --i, stamp >>= 4) out[i] = kDigits[stamp & 0xF];
  return out;
}

// The stamp the fake's ps_html_policy_stamp returns when a row names none.
const char kDefaultPolicyStamp[] = "5a17c0de00000001";

// Whether a config names any setting of the cached-HTML serve path; such a
// row must state its library's minor (RunnerShape).  Without one a row runs
// on the 1.14 library the earlier rows were written against.
bool NamesServeCachedHtmlSettings(const Json::Value& config) {
  for (const char* key : {"serve_scope", "max_age_s", "credentialed",
                          "ignore_cookies", "ignore_cookie", "policy_stamp"}) {
    if (config.isMember(key)) return true;
  }
  return config.get("mode", "").asString() == "serve-cached-html";
}

// The recorder below writes every field of these structs, so a row's
// "exact" expectation covers the whole struct.  A field added to either
// struct must be recorded in ConformanceAbi::HtmlServeClassify before these
// sizes are updated; otherwise exact matching silently stops covering it.
//
// The 1.15 fields are recorded only from a struct of the 1.15 size: a struct
// of the 1.14 size (what the engine sends a 1.14 library) carries none, and a
// row's exact expectation for it is exactly the 1.14 fields.
static_assert(sizeof(PsHtmlServeRequest) == 88,
              "record the new PsHtmlServeRequest field, then update this size");
static_assert(sizeof(PsHtmlStoredFacts) == 104,
              "record the new PsHtmlStoredFacts field, then update this size");

// Every library function the runner records.  A row naming any other in
// calls or calls_exclude would assert nothing, so the runner refuses it.
const char* const kRecordedFns[] = {
    "ps_cache_read_alternate_site", "ps_cache_read_best_site",
    "ps_cache_read_site_purge_epoch", "ps_cache_remove_site",
    "ps_cache_write_headers_sidecar_site", "ps_cache_write_original_site",
    "ps_cache_write_site_purge_epoch", "ps_header_block_pairing",
    "ps_header_block_policy_stamp", "ps_headers_sidecar_read",
    "ps_html_classify_cookies", "ps_html_ignore_cookie_guard",
    "ps_html_markdown_etag", "ps_html_policy_stamp", "ps_html_request_guard",
    "ps_html_serve_classify", "ps_html_store_classify",
    "ps_html_store_write_params",
    // Both forms: the _ex form is recorded
    // under this name, with block set.
    "ps_html_stored_facts_from_reads", "ps_html_unsafe_method_invalidates",
    "ps_if_none_match_matches_weak", "ps_notify_worker_ex",
    "ps_read_shared_config_agent_entitled", "ps_read_shared_config_html_mode",
    "ps_wants_agent_markdown", "ps_write_close"};

// The argument names each recorded call carries, so a row can name only
// what the recorder writes (RunnerShape), and the recorder writes only these
// (checked after every step).  headers_include and headers_exclude match the
// call's header lines.
const std::map<GoogleString, std::set<GoogleString>>& RecordedArgs() {
  static const auto* args = new std::map<GoogleString, std::set<GoogleString>>{
      {"ps_cache_read_alternate_site", {"url", "alternate"}},
      {"ps_cache_read_best_site", {}},
      {"ps_cache_read_site_purge_epoch", {"site"}},
      {"ps_cache_remove_site", {"url", "site"}},
      {"ps_cache_write_headers_sidecar_site",
       {"url", "site", "paired", "format", "policy_stamp", "headers_include",
        "headers_exclude"}},
      {"ps_cache_write_original_site",
       {"url", "hostname", "site", "inserted_at_s", "lifetime_s", "hashed"}},
      {"ps_cache_write_site_purge_epoch", {"site", "epoch_s"}},
      {"ps_header_block_pairing", {}},
      {"ps_header_block_policy_stamp", {}},
      {"ps_headers_sidecar_read", {"url"}},
      {"ps_html_classify_cookies",
       {"cookie_lines", "ignore_builtin", "additions", "capacity"}},
      {"ps_html_ignore_cookie_guard", {"addition"}},
      {"ps_html_markdown_etag", {"binding_hash", "markdown_hash"}},
      {"ps_html_policy_stamp",
       {"credentialed", "ignore_builtin", "list_version", "additions"}},
      {"ps_html_request_guard",
       {"raw_path", "keyed_scheme", "header_names", "header_values",
        "headers_include", "headers_exclude"}},
      {"ps_html_serve_classify",
       {"request.struct_size",
        "request.mode",
        "request.method",
        "request.accept",
        "request.cache_control",
        "request.pragma",
        "request.if_none_match",
        "request.authorization",
        "request.authenticated_user",
        "request.cookie",
        "request.range",
        "request.if_range",
        "request.origin",
        "request.now_seconds",
        "request.serve_scope",
        "request.agent_class",
        "request.html_max_lifetime_s",
        "request.reserved",
        "request.policy_stamp",
        "request.fresh_only",
        "request.html_fallback",
        "request.conditional",
        "request.explicit_public",
        "request.flags_unknown",
        "stored.struct_size",
        "stored.present",
        "stored.needs_validation",
        "stored.explicit_shared",
        "stored.headers_not_reproducible",
        "stored.sidecar_required",
        "stored.sidecar_present",
        "stored.purged",
        "stored.validated_this_request",
        "stored.variant_present",
        "stored.binding_live",
        "stored.no_transform",
        "stored.withheld",
        "stored.store_v2",
        "stored.anonymous_recorded",
        "stored.flags_unknown",
        "stored.stored_at",
        "stored.age_at_store",
        "stored.lifetime_s",
        "stored.render_ttl_s",
        "stored.withheld_reason",
        "stored.binding_hash",
        "stored.markdown_hash",
        "stored.entry_policy_stamp"}},
      {"ps_html_store_classify",
       {"view", "mode", "method", "url_path", "status", "request.authorization",
        "request.authenticated_user", "request.cookie", "request.no_store",
        "request.explicit_public", "request.flags_unknown", "body_bytes",
        "body_complete", "max_body_bytes", "now_seconds", "headers_include",
        "headers_exclude"}},
      {"ps_html_store_write_params",
       {"action", "lifetime_s", "capture_start_ms", "sidecar_stored",
        "store_flags", "store_flags.request_anonymous", "cc_flags", "age"}},
      {"ps_html_stored_facts_from_reads",
       {"original", "markdown", "withheld", "sidecar_present", "block"}},
      {"ps_html_unsafe_method_invalidates", {"method", "status"}},
      {"ps_if_none_match_matches_weak", {"if_none_match", "etag"}},
      {"ps_notify_worker_ex",
       {"url", "agent_request", "content_type", "notify_flags"}},
      {"ps_read_shared_config_agent_entitled", {}},
      {"ps_read_shared_config_html_mode", {}},
      {"ps_wants_agent_markdown", {"accept"}},
      {"ps_write_close", {}}};
  return *args;
}

// The FakePort calls a row may name.
const char* const kPortCalls[] = {"ClearAndEmitAllowlisted", "MarkNoCompress",
                                  "EmitAsOrigin", "RemoveClientConditionals",
                                  "RunBlocking"};

bool IsRecordedFn(const GoogleString& fn) {
  for (const char* f : kRecordedFns) {
    if (fn == f) return true;
  }
  return false;
}

// One call the engine made, as the runner records it.
struct Call {
  GoogleString fn;
  Json::Value args{Json::objectValue};
  std::vector<std::pair<GoogleString, GoogleString>> headers;
};

// ---- The engine entry points the cached-HTML serve path adds.
//
// The rows of that path are written before the engine has every entry point
// they drive.  Each one is looked up at compile time twice: by its full
// signature, which the runner calls once declared, and by name alone, so a
// declaration with another signature is a runner error that says so, never
// "not declared yet".  The set still undeclared is pinned
// (ServePathEntryPointsStillUndeclared).
struct ServePathEntryPoint {
  const char* name;
  const char* signature;
};
const ServePathEntryPoint kServePathEntryPoints[] = {
    {"served_from_cache",
     "bool DaemonHtmlRequestFacts::served_from_cache (the port's mark that "
     "the cache answered this request)"},
    {"request_headers",
     "DaemonHtmlHeaderLines DaemonHtmlRequestFacts::request_headers (every "
     "request header line, unfiltered)"},
    {"raw_path", "GoogleString DaemonHtmlRequestFacts::raw_path (as received)"},
    {"cookie_lines",
     "std::vector<GoogleString> DaemonHtmlRequestFacts::cookie_lines (every "
     "Cookie line's value)"},
    {"internal",
     "bool DaemonHtmlRequestFacts::internal (a subrequest or internal "
     "redirect, which the engine never evaluates)"},
    {"OnUnsafeRequest",
     "void DaemonHtmlEngine::OnUnsafeRequest(const DaemonHtmlRequestFacts&, "
     "int status, const std::vector<GoogleString>& location, const "
     "std::vector<GoogleString>& content_location, DaemonHtmlPort*)"},
    {"ApplyPolicySettings",
     "static bool DaemonHtmlEngine::ApplyPolicySettings(const DaemonAbi&, "
     "Config*, GoogleString* error) (configuration load: guards the "
     "ignore-list additions, computes config->policy_stamp)"},
    {"set_credential_skip_disabled_for_testing",
     "void DaemonHtmlEngine::set_credential_skip_disabled_for_testing(bool)"},
};

const char* EntryPointSignature(StringPiece name) {
  for (const ServePathEntryPoint& e : kServePathEntryPoints) {
    if (name == e.name) return e.signature;
  }
  return nullptr;
}

// Full-signature and name-only probes.
template <typename F>
constexpr bool kSetsServedFromCache =
    requires(F f, bool v) { f.served_from_cache = v; };
template <typename F>
constexpr bool kNamesServedFromCache = requires { &F::served_from_cache; };
template <typename F>
constexpr bool kSetsRequestHeaders =
    requires(F f, DaemonHtmlHeaderLines v) { f.request_headers = v; };
template <typename F>
constexpr bool kNamesRequestHeaders = requires { &F::request_headers; };
template <typename F>
constexpr bool kSetsRawPath = requires(F f, GoogleString v) { f.raw_path = v; };
template <typename F>
constexpr bool kNamesRawPath = requires { &F::raw_path; };
template <typename F>
constexpr bool kSetsCookieLines =
    requires(F f, std::vector<GoogleString> v) { f.cookie_lines = v; };
template <typename F>
constexpr bool kNamesCookieLines = requires { &F::cookie_lines; };
template <typename F>
constexpr bool kSetsInternal = requires(F f, bool v) { f.internal = v; };
template <typename F>
constexpr bool kNamesInternal = requires { &F::internal; };
template <typename E>
constexpr bool kCallsOnUnsafeRequest = requires(
    E e, const DaemonHtmlRequestFacts& f, const std::vector<GoogleString>& l,
    DaemonHtmlPort* p) { e.OnUnsafeRequest(f, 200, l, l, p); };
template <typename E>
constexpr bool kNamesOnUnsafeRequest = requires { &E::OnUnsafeRequest; };
template <typename E>
constexpr bool kCallsApplyPolicySettings =
    requires(const DaemonAbi& abi, typename E::Config* c, GoogleString* err) {
      { E::ApplyPolicySettings(abi, c, err) } -> std::convertible_to<bool>;
    };
template <typename E>
constexpr bool kNamesApplyPolicySettings =
    requires { &E::ApplyPolicySettings; };
template <typename E>
constexpr bool kCallsDisableCredentialSkip =
    requires(E e) { e.set_credential_skip_disabled_for_testing(true); };
template <typename E>
constexpr bool kNamesDisableCredentialSkip =
    requires { &E::set_credential_skip_disabled_for_testing; };

// Whether F and E declare each entry point: by full signature, and by name.
struct EntryPointState {
  bool callable;
  bool named;
};
template <typename F, typename E>
std::map<GoogleString, EntryPointState> EntryPointStates() {
  return {
      {"served_from_cache",
       {kSetsServedFromCache<F>, kNamesServedFromCache<F>}},
      {"request_headers", {kSetsRequestHeaders<F>, kNamesRequestHeaders<F>}},
      {"raw_path", {kSetsRawPath<F>, kNamesRawPath<F>}},
      {"cookie_lines", {kSetsCookieLines<F>, kNamesCookieLines<F>}},
      {"internal", {kSetsInternal<F>, kNamesInternal<F>}},
      {"OnUnsafeRequest", {kCallsOnUnsafeRequest<E>, kNamesOnUnsafeRequest<E>}},
      {"ApplyPolicySettings",
       {kCallsApplyPolicySettings<E>, kNamesApplyPolicySettings<E>}},
      {"set_credential_skip_disabled_for_testing",
       {kCallsDisableCredentialSkip<E>, kNamesDisableCredentialSkip<E>}},
  };
}

// Reports an entry point a row needs that cannot be called: not declared
// yet, or declared with another signature (a runner error).
void EntryPointMissing(const char* name, bool named) {
  if (named) {
    ADD_FAILURE() << kRunnerError << name
                  << " is declared with a signature the runner does not "
                  << "call; expected: " << EntryPointSignature(name);
  } else {
    ADD_FAILURE() << kEntryPointAbsent << name;
  }
}

template <typename F>
bool SetServedFromCache(F* f, bool value) {
  if constexpr (kSetsServedFromCache<F>) {
    f->served_from_cache = value;
    return true;
  }
  return !value;
}

template <typename F>
bool SetRequestHeaders(F* f, const DaemonHtmlHeaderLines& lines) {
  if constexpr (kSetsRequestHeaders<F>) {
    f->request_headers = lines;
    return true;
  }
  return false;
}

template <typename F>
bool SetRawPath(F* f, const GoogleString& raw_path, bool given) {
  if constexpr (kSetsRawPath<F>) {
    f->raw_path = raw_path;
    return true;
  }
  return !given;
}

template <typename F>
bool SetCookieLines(F* f, const std::vector<GoogleString>& lines) {
  if constexpr (kSetsCookieLines<F>) {
    f->cookie_lines = lines;
    return true;
  }
  return lines.empty();
}

template <typename F>
bool SetInternal(F* f, bool value) {
  if constexpr (kSetsInternal<F>) {
    f->internal = value;
    return true;
  }
  return !value;
}

template <typename E>
bool CallOnUnsafeRequest(E* engine, const DaemonHtmlRequestFacts& facts,
                         int status, const std::vector<GoogleString>& location,
                         const std::vector<GoogleString>& content_location,
                         DaemonHtmlPort* port) {
  if constexpr (kCallsOnUnsafeRequest<E>) {
    engine->OnUnsafeRequest(facts, status, location, content_location, port);
    return true;
  }
  return false;
}

// -1 when the engine has no ApplyPolicySettings, else whether it accepted.
template <typename E>
int CallApplyPolicySettings(const DaemonAbi& abi, typename E::Config* config,
                            GoogleString* error) {
  if constexpr (kCallsApplyPolicySettings<E>) {
    return E::ApplyPolicySettings(abi, config, error) ? 1 : 0;
  }
  return -1;
}

template <typename E>
bool DisableCredentialSkip(E* engine) {
  if constexpr (kCallsDisableCredentialSkip<E>) {
    engine->set_credential_skip_disabled_for_testing(true);
    return true;
  }
  return false;
}

// The engine's own types, as the rows drive them.
bool NamedOnEngine(const char* name) {
  return EntryPointStates<DaemonHtmlRequestFacts, DaemonHtmlEngine>()
      .at(name)
      .named;
}

// ---- Counters the serve path registers, named by its design; a row may
// name one before it is registered (kCounterAbsent).  Any other name a row
// gives must be registered.
const char* const kServePathCounters[] = {
    "daemon_html_agent_html_requests",
    "daemon_html_fallthrough_capped",
    "daemon_html_fallthrough_conditional",
    "daemon_html_fallthrough_path",
    "daemon_html_fallthrough_scope",
    "daemon_html_fresh_body_changed",
    "daemon_html_hit_forbidden_header",
    "daemon_html_hit_recorded",
    "daemon_html_invalidate_failed",
    "daemon_html_invalidated_unsafe",
    "daemon_html_served_html",
    "daemon_html_served_html_c1",
    "daemon_html_served_html_c3",
    "daemon_html_served_html_markdown_preference",
    "daemon_html_served_html_unclassed",
    "daemon_html_shadow_sampled",
    "daemon_html_unexpected_validate",
    "daemon_html_would_serve_body_differs",
    "daemon_html_would_serve_html",
};

bool IsServePathCounter(const GoogleString& name) {
  for (const char* c : kServePathCounters) {
    if (name == c) return true;
  }
  // daemon_html_would_fallthrough_<reason>, for every serve reason.
  const GoogleString prefix = "daemon_html_would_fallthrough_";
  return HasPrefixString(name, prefix) &&
         IndexOf(kServeReasons, name.substr(prefix.size())) >= 0;
}

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
  // The as-origin result a port hands to its own chain; false (an
  // emit_fails step) is a port that could not emit it.
  bool EmitAsOrigin(const DaemonHtmlServeResult& result) override {
    calls.push_back("EmitAsOrigin");
    emitted = result;
    return as_origin_ok;
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
  bool as_origin_ok = true;
};

// The injected-verdict library over the volume: every call recorded with its
// arguments; the policy calls answer the row's verdicts, consumed in order
// per function (and per view for the store classifier); the volume calls go
// to the volume unless a verdict overrides their return code.
//
// With `stub` set, the same calls are recorded the same way, but every one
// is answered by the stub library (a real dlopen-ed binding steered by its
// PS_STUB_* settings) instead: the degrade and fault rows run there, so the
// engine's fallback is proven against a real binding as well.
class ConformanceAbi : public InjectedVerdictDaemonAbi {
 public:
  ConformanceAbi(FakeDaemonVolume* volume, const DaemonAbi* stub,
                 const Json::Value& verdicts, const Json::Value& library,
                 std::map<GoogleString, GoogleString>* site_names,
                 std::map<GoogleString, GoogleString> hash_names,
                 int default_minor, uint64_t policy_stamp)
      : InjectedVerdictDaemonAbi(
            stub != nullptr ? stub : static_cast<const DaemonAbi*>(volume)),
        volume_(volume),
        stub_(stub),
        site_names_(site_names),
        hash_names_(std::move(hash_names)),
        minor_(default_minor),
        policy_stamp_(policy_stamp) {
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
    for (const Json::Value& fn : library["missing"]) {
      missing_.push_back(fn.asString());
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
  // Every decision line the engine had formatted, in order.
  const std::vector<GoogleString>& decision_lines() const {
    return decision_lines_;
  }
  // Runs before every ps_cache_remove_site call, with the removed URL.
  void set_on_remove(std::function<void(const GoogleString&)> f) {
    on_remove_ = std::move(f);
  }

  static int HtmlMode(const GoogleString& name) {
    if (name == "off") return kPsHtmlModeOff;
    if (name == "markdown-static") return kPsHtmlModeMarkdownStatic;
    if (name == "markdown-rendered") return kPsHtmlModeMarkdownRendered;
    if (name == "full") return kPsHtmlModeFull;
    return -1;
  }

  int VersionMinor() const override {
    if (stub_ != nullptr) return stub_->VersionMinor();
    return minor_;
  }
  bool HasHtmlServeSet() const override {
    if (stub_ != nullptr) return stub_->HasHtmlServeSet();
    return minor_ >= kHtmlServeSetMinor && set_bound_;
  }
  // The 1.15 set: bound at minor 15 unless the row leaves a symbol out.
  bool HasHtmlServe15Set() const override {
    if (stub_ != nullptr) return stub_->HasHtmlServe15Set();
    return HasHtmlServeSet() && minor_ >= kHtmlServe15SetMinor &&
           missing_.empty();
  }
  GoogleString HtmlServe15SetUnavailableReason() const override {
    if (stub_ != nullptr) return stub_->HtmlServe15SetUnavailableReason();
    if (HasHtmlServe15Set()) return GoogleString();
    if (minor_ < kHtmlServe15SetMinor) {
      return StrCat("the optimizer library reports C API 1.",
                    IntegerToString(minor_),
                    "; serving cached HTML needs 1.15");
    }
    return StrCat(
        "the optimizer library lacks ",
        missing_.empty() ? GoogleString("the 1.14 set") : missing_.front());
  }
  GoogleString HtmlServeSetUnavailableReason() const override {
    if (stub_ != nullptr) return stub_->HtmlServeSetUnavailableReason();
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
    c.args["method"] = Str(method);
    c.args["url_path"] = Str(url_path);
    c.args["status"] = status;
    // Every bit of request_flags: the four named ones, and any other.
    c.args["request.authorization"] = (request_flags & 1u) != 0;
    c.args["request.authenticated_user"] = (request_flags & 2u) != 0;
    c.args["request.cookie"] = (request_flags & 4u) != 0;
    c.args["request.no_store"] = (request_flags & 8u) != 0;
    uint32_t known = 15u;
    // The 1.15 policy bit, recorded only for a 1.15 library, which alone
    // reads it.
    if (HasHtmlServe15Set()) {
      c.args["request.explicit_public"] =
          (request_flags & kPsHtmlStoreRequestExplicitPublic) != 0;
      known |= kPsHtmlStoreRequestExplicitPublic;
    }
    c.args["request.flags_unknown"] = request_flags & ~known;
    c.args["body_bytes"] = static_cast<Json::UInt64>(body_bytes);
    c.args["body_complete"] = body_complete;
    c.args["max_body_bytes"] = static_cast<Json::UInt64>(max_body_bytes);
    c.args["now_seconds"] = now_seconds;
    for (size_t i = 0; i < count; ++i) {
      c.headers.emplace_back(names[i], values[i]);
    }
    calls_.push_back(c);
    if (stub_ != nullptr) {
      return stub_->HtmlStoreClassify(
          mode, method, url_path, request_flags, status, names, values, count,
          body_bytes, body_complete, body_prefix, body_prefix_len,
          max_body_bytes, now_seconds, out_reason, out_lifetime_s, out_flags,
          out_cc_flags, out_sidecar, detail_buf, detail_capacity,
          out_detail_len);
    }
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
    if (stub_ != nullptr) return stub_->HtmlStoreReasonName(reason);
    if (reason < 0 || static_cast<size_t>(reason) >= std::size(kStoreReasons)) {
      return nullptr;
    }
    return kStoreReasons[reason];
  }

  int HtmlServeClassify(const PsHtmlServeRequest* request,
                        const PsHtmlStoredFacts* facts,
                        PsHtmlServeVerdict* out) const override {
    Call c = Rec("ps_html_serve_classify");
    // Every field of both structs, so a row can assert either exactly.
    // Of the stored facts, the engine itself sets only `purged` (from its
    // purge fence); the rest are what HtmlStoredFactsFromReads returned,
    // which on this fake is the fake's reading of the volume.  A row's
    // exact facts therefore prove the engine passed them on unchanged and
    // set purged, not the library's facts rule.
    c.args["request.struct_size"] =
        static_cast<Json::UInt64>(request->struct_size);
    c.args["request.mode"] = request->mode;
    c.args["request.method"] = Str(request->method);
    c.args["request.accept"] = Str(request->accept);
    c.args["request.cache_control"] = Str(request->cache_control);
    c.args["request.pragma"] = Str(request->pragma);
    c.args["request.if_none_match"] = Str(request->if_none_match);
    const uint32_t f = request->request_flags;
    uint32_t known_request = 0;
    auto request_bit = [&](const char* name, uint32_t mask) {
      c.args[StrCat("request.", name)] = (f & mask) != 0;
      known_request |= mask;
    };
    request_bit("authorization", kPsHtmlServeRequestHasAuthorization);
    request_bit("authenticated_user", kPsHtmlServeRequestAuthenticatedUser);
    request_bit("cookie", kPsHtmlServeRequestHasCookie);
    request_bit("range", kPsHtmlServeRequestHasRange);
    request_bit("if_range", kPsHtmlServeRequestHasIfRange);
    request_bit("origin", kPsHtmlServeRequestHasOrigin);
    c.args["request.now_seconds"] = request->now_seconds;
    // The 1.15 fields and bits, recorded only from a request of the 1.15
    // size: a library reads them from no other.  A bit set in a smaller
    // request shows in flags_unknown.
    if (request->struct_size >= sizeof(PsHtmlServeRequest)) {
      c.args["request.serve_scope"] = request->serve_scope;
      c.args["request.agent_class"] = request->agent_class;
      c.args["request.html_max_lifetime_s"] = request->html_max_lifetime_s;
      c.args["request.reserved"] = request->reserved;
      c.args["request.policy_stamp"] = StampName(request->policy_stamp);
      request_bit("fresh_only", kPsHtmlServeRequestFreshOnly);
      request_bit("html_fallback", kPsHtmlServeRequestHtmlFallback);
      request_bit("conditional", kPsHtmlServeRequestConditional);
      request_bit("explicit_public", kPsHtmlServeRequestExplicitPublic);
    }
    c.args["request.flags_unknown"] = f & ~known_request;
    const uint32_t s = facts->flags;
    uint32_t known = 0;
    auto bit = [&](const char* name, uint32_t mask) {
      c.args[StrCat("stored.", name)] = (s & mask) != 0;
      known |= mask;
    };
    c.args["stored.struct_size"] =
        static_cast<Json::UInt64>(facts->struct_size);
    bit("present", kPsHtmlFactsPresent);
    bit("needs_validation", kPsHtmlFactsNeedsValidation);
    bit("explicit_shared", kPsHtmlFactsExplicitShared);
    bit("headers_not_reproducible", kPsHtmlFactsHeadersNotReproducible);
    bit("sidecar_required", kPsHtmlFactsSidecarRequired);
    bit("sidecar_present", kPsHtmlFactsSidecarPresent);
    bit("purged", kPsHtmlFactsPurged);
    bit("validated_this_request", kPsHtmlFactsValidatedThisRequest);
    bit("variant_present", kPsHtmlFactsVariantPresent);
    bit("binding_live", kPsHtmlFactsBindingLive);
    bit("no_transform", kPsHtmlFactsNoTransform);
    bit("withheld", kPsHtmlFactsWithheld);
    if (facts->struct_size >= sizeof(PsHtmlStoredFacts)) {
      bit("store_v2", kPsHtmlFactsStoreV2);
      bit("anonymous_recorded", kPsHtmlFactsAnonymousRecorded);
    }
    c.args["stored.flags_unknown"] = s & ~known;
    c.args["stored.stored_at"] = facts->stored_at;
    c.args["stored.age_at_store"] = facts->age_at_store;
    c.args["stored.lifetime_s"] = facts->lifetime_s;
    c.args["stored.render_ttl_s"] = facts->render_ttl_s;
    c.args["stored.withheld_reason"] = facts->withheld_reason;
    c.args["stored.binding_hash"] = HashName(facts->binding_hash);
    c.args["stored.markdown_hash"] = HashName(facts->markdown_hash);
    if (facts->struct_size >= sizeof(PsHtmlStoredFacts)) {
      c.args["stored.entry_policy_stamp"] =
          StampName(facts->entry_policy_stamp);
    }
    calls_.push_back(c);
    if (stub_ != nullptr) return stub_->HtmlServeClassify(request, facts, out);
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
    if (stub_ != nullptr) return stub_->HtmlServeReasonName(reason);
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
    c.args["store_flags.request_anonymous"] =
        (store_flags & kPsHtmlStoreFlagRequestAnonymous) != 0;
    c.args["cc_flags"] = cc_flags;
    Json::Value ages(Json::arrayValue);
    for (size_t i = 0; i < age_count; ++i) ages.append(age_lines[i]);
    c.args["age"] = ages;
    calls_.push_back(c);
    if (stub_ != nullptr) {
      return stub_->HtmlStoreWriteParams(
          action, lifetime_s, store_flags, cc_flags, sidecar_stored,
          capture_start_ms, age_lines, age_count, content_length, ct, params);
    }
    Json::Value v = Pop("ps_html_store_write_params");
    HtmlServeInjection* inj =
        const_cast<ConformanceAbi*>(this)->mutable_injection();
    inj->write_params_rc = v.isMember("rc") && RcOf(v["rc"]) != kPsOk ? -1 : 0;
    inj->write_params_inserted_at =
        v.isMember("inserted_at_s") ? v["inserted_at_s"].asUInt() : 0;
    const int rc = InjectedVerdictDaemonAbi::HtmlStoreWriteParams(
        action, lifetime_s, store_flags, cc_flags, sidecar_stored,
        capture_start_ms, age_lines, age_count, content_length, ct, params);
    // A 1.15 library records the entry under the credentialed policy, and
    // whether its request was anonymous (the library's own mapping of its
    // store flag; no policy).
    if (rc == 0 && params != nullptr && HasHtmlServe15Set()) {
      params->flags |= kPsFlagHtmlStoreV2;
      if ((store_flags & kPsHtmlStoreFlagRequestAnonymous) != 0) {
        params->flags |= kPsFlagAnonymousRecorded;
      }
    }
    return rc;
  }

  // The library's facts rule, applied to the volume's read results.  The
  // _ex form is recorded under the same name (with block set), so a row
  // that excludes the facts call excludes both.
  int HtmlStoredFactsFromReads(const void* original, const void* markdown,
                               const void* withheld, int sidecar_present,
                               PsHtmlStoredFacts* out) const override {
    RecordFacts(original, markdown, withheld, sidecar_present, nullptr,
                /*ex=*/false);
    if (stub_ != nullptr) {
      return stub_->HtmlStoredFactsFromReads(original, markdown, withheld,
                                             sidecar_present, out);
    }
    return FactsFromVolume(original, markdown, withheld, sidecar_present,
                           nullptr, out);
  }

  int HtmlStoredFactsFromReadsEx(const void* original, const void* markdown,
                                 const void* withheld, int sidecar_present,
                                 const void* block,
                                 PsHtmlStoredFacts* out) const override {
    RecordFacts(original, markdown, withheld, sidecar_present, block,
                /*ex=*/true);
    if (stub_ != nullptr) {
      return stub_->HtmlStoredFactsFromReadsEx(original, markdown, withheld,
                                               sidecar_present, block, out);
    }
    if (!HasHtmlServe15Set()) return -1;
    return FactsFromVolume(original, markdown, withheld, sidecar_present, block,
                           out);
  }

  void RecordFacts(const void* original, const void* markdown,
                   const void* withheld, int sidecar_present, const void* block,
                   bool ex) const {
    Call c = Rec("ps_html_stored_facts_from_reads");
    c.args["original"] = original != nullptr;
    c.args["markdown"] = markdown != nullptr;
    c.args["withheld"] = withheld != nullptr;
    c.args["sidecar_present"] = sidecar_present;
    if (ex) c.args["block"] = block != nullptr;
    calls_.push_back(c);
  }

  // The facts as the library derives them from what the volume returned;
  // `block` (the _ex form) carries the entry's stamp.
  int FactsFromVolume(const void* original, const void* markdown,
                      const void* withheld, int sidecar_present,
                      const void* block, PsHtmlStoredFacts* out) const {
    if (out == nullptr || out->struct_size < kPsHtmlStoredFactsSize14) {
      return -1;
    }
    // Written as far as the caller's struct reaches, its size kept.
    const size_t out_size = out->struct_size;
    PsHtmlStoredFacts f = {};
    f.struct_size = out_size;
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
      f.withheld_reason =
          static_cast<const FakeVolumeEntry*>(withheld)->withheld_reason;
    } else if (o != nullptr && m != nullptr && o->has_hash && m->has_hash &&
               memcmp(o->hash, m->hash, kPsHtmlHashBytes) == 0) {
      f.flags |= kPsHtmlFactsBindingLive;
    }
    // The 1.15 facts, for a caller whose struct reaches them.
    if (out_size >= sizeof(PsHtmlStoredFacts)) {
      if (o != nullptr && (o->flags & kPsFlagHtmlStoreV2) != 0) {
        f.flags |= kPsHtmlFactsStoreV2;
      }
      if (o != nullptr && (o->flags & kPsFlagAnonymousRecorded) != 0) {
        f.flags |= kPsHtmlFactsAnonymousRecorded;
      }
      const auto* b = static_cast<const FakeVolumeSidecar*>(block);
      if (b != nullptr && b->format >= 3) {
        f.entry_policy_stamp = b->policy_stamp;
      }
    }
    memcpy(out, &f, out_size < sizeof(f) ? out_size : sizeof(f));
    return 0;
  }

  int HtmlMarkdownEtag(const uint8_t* binding, const uint8_t* markdown,
                       char* buf, size_t capacity,
                       size_t* out_len) const override {
    Call c = Rec("ps_html_markdown_etag");
    c.args["binding_hash"] = HashName(binding);
    c.args["markdown_hash"] = HashName(markdown);
    calls_.push_back(c);
    if (stub_ != nullptr) {
      return stub_->HtmlMarkdownEtag(binding, markdown, buf, capacity, out_len);
    }
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
    if (stub_ != nullptr) return stub_->IfNoneMatchMatchesWeak(inm, etag);
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
    c.args["url"] = FakeDaemonVolume::EntryUrl(params->url, params->hostname,
                                               params->scheme);
    c.args["site"] = SiteName(params->site);
    c.args["paired"] = params->origin_html_hash != nullptr;
    // Format 3 (the policy block) exists only in a 1.15-size struct.
    const bool policy_block =
        params->struct_size >= sizeof(PsSidecarWriteParams) &&
        (params->write_flags & kPsSidecarWritePolicyBlock) != 0;
    c.args["format"] = policy_block ? 3 : 2;
    if (policy_block) c.args["policy_stamp"] = StampName(params->policy_stamp);
    for (size_t i = 0; i < count; ++i) {
      c.headers.emplace_back(names[i], values[i]);
    }
    calls_.push_back(c);
    if (stub_ != nullptr) {
      return stub_->CacheWriteHeadersSidecarSite(
          cache, params, names, values, count, out_verdict, out_stored);
    }
    Json::Value v = Pop("ps_cache_write_headers_sidecar_site");
    if (v.isMember("rc") && RcOf(v["rc"]) != kPsOk) return RcOf(v["rc"]);
    if (v.isMember("stored") && v["stored"].asInt() == 0) {
      // The library's gate kept no block.
      if (out_verdict != nullptr) *out_verdict = 1;
      if (out_stored != nullptr) *out_stored = 0;
      return kPsOk;
    }
    return vol()->CacheWriteHeadersSidecarSite(cache, params, names, values,
                                               count, out_verdict, out_stored);
  }

  int HeadersSidecarRead(void* cache, const char* url, const char* host,
                         const char* scheme, const char* site,
                         void** out) const override {
    Call c = Rec("ps_headers_sidecar_read");
    c.args["url"] = FakeDaemonVolume::EntryUrl(url, host, scheme);
    calls_.push_back(c);
    return vol()->HeadersSidecarRead(cache, url, host, scheme, site, out);
  }
  size_t HeaderBlockCount(const void* b) const override {
    return vol()->HeaderBlockCount(b);
  }
  int HeaderBlockGet(const void* b, size_t i, const char** n, size_t* nl,
                     const char** v, size_t* vl) const override {
    return vol()->HeaderBlockGet(b, i, n, nl, v, vl);
  }
  const uint8_t* HeaderBlockPairingHash(const void* b) const override {
    return vol()->HeaderBlockPairingHash(b);
  }
  int HeaderBlockPairing(const void* b, const uint8_t* h) const override {
    calls_.push_back(Rec("ps_header_block_pairing"));
    return vol()->HeaderBlockPairing(b, h);
  }
  void HeaderBlockFree(void* b) const override { vol()->HeaderBlockFree(b); }

  // The stamp the volume's header block carries (format 3 only).
  int HeaderBlockPolicyStamp(const void* block, uint64_t* out) const override {
    calls_.push_back(Rec("ps_header_block_policy_stamp"));
    if (stub_ != nullptr) return stub_->HeaderBlockPolicyStamp(block, out);
    if (!HasHtmlServe15Set() || block == nullptr || out == nullptr) return -1;
    const auto* b = static_cast<const FakeVolumeSidecar*>(block);
    *out = b->format >= 3 ? b->policy_stamp : 0;
    return b->format >= 3 ? 1 : 0;
  }

  int CacheRemoveSite(void* cache, const char* url, const char* host,
                      const char* scheme, const char* site) const override {
    Call c = Rec("ps_cache_remove_site");
    c.args["url"] = FakeDaemonVolume::EntryUrl(url, host, scheme);
    c.args["site"] = SiteName(site);
    calls_.push_back(c);
    if (on_remove_) on_remove_(c.args["url"].asString());
    Json::Value v = Pop("ps_cache_remove_site");
    if (v.isMember("rc") && RcOf(v["rc"]) != kPsOk) return RcOf(v["rc"]);
    return vol()->CacheRemoveSite(cache, url, host, scheme, site);
  }

  int CacheWriteSitePurgeEpoch(void* cache, const char* site,
                               uint64_t epoch_s) const override {
    Call c = Rec("ps_cache_write_site_purge_epoch");
    c.args["site"] = SiteName(site);
    c.args["epoch_s"] = static_cast<Json::UInt64>(epoch_s);
    calls_.push_back(c);
    Json::Value v = Pop("ps_cache_write_site_purge_epoch");
    if (v.isMember("rc") && RcOf(v["rc"]) != kPsOk) return RcOf(v["rc"]);
    return vol()->CacheWriteSitePurgeEpoch(cache, site, epoch_s);
  }

  int CacheReadSitePurgeEpoch(void* cache, const char* site,
                              uint64_t* out) const override {
    Call c = Rec("ps_cache_read_site_purge_epoch");
    c.args["site"] = SiteName(site);
    calls_.push_back(c);
    Json::Value v = Pop("ps_cache_read_site_purge_epoch");
    if (v.isMember("rc") && RcOf(v["rc"]) != kPsOk) return RcOf(v["rc"]);
    return vol()->CacheReadSitePurgeEpoch(cache, site, out);
  }

  int ReadSharedConfigHtmlMode(const char* path) const override {
    calls_.push_back(Rec("ps_read_shared_config_html_mode"));
    if (stub_ != nullptr) return stub_->ReadSharedConfigHtmlMode(path);
    Json::Value v = Pop("ps_read_shared_config_html_mode");
    return v.isNull() ? html_mode_ : HtmlMode(v["value"].asString());
  }

  int ReadSharedConfigAgentEntitled(const char* path) const override {
    calls_.push_back(Rec("ps_read_shared_config_agent_entitled"));
    if (stub_ != nullptr) return stub_->ReadSharedConfigAgentEntitled(path);
    Json::Value v = Pop("ps_read_shared_config_agent_entitled");
    return v.isNull() ? agent_entitled_ : (v["value"].asBool() ? 1 : 0);
  }

  int WantsAgentMarkdown(const char* accept) const override {
    Call c = Rec("ps_wants_agent_markdown");
    c.args["accept"] = Str(accept);
    calls_.push_back(c);
    if (stub_ != nullptr) return stub_->WantsAgentMarkdown(accept);
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
    c.args["url"] = FakeDaemonVolume::EntryUrl(url, host, scheme);
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
    c.args["url"] = FakeDaemonVolume::EntryUrl(url, host, scheme);
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

  // ---- the 1.15 request policy.  Each records its call and answers the
  // row's verdict; with none, the answer of a library with no policy to
  // apply (no cookie, nothing refused, nothing invalidated).  Unbound
  // (minor 14, or a symbol the row leaves out), each answers as the binding
  // does then.

  int HtmlRequestGuard(const char* raw_path, const char* const* names,
                       const char* const* values, size_t count,
                       const char* keyed_scheme,
                       PsHtmlRequestGuard* out) const override {
    Call c = Rec("ps_html_request_guard");
    c.args["raw_path"] = Str(raw_path);
    c.args["keyed_scheme"] = Str(keyed_scheme);
    Json::Value n(Json::arrayValue);
    Json::Value v(Json::arrayValue);
    for (size_t i = 0; i < count; ++i) {
      n.append(Str(names[i]));
      v.append(Str(values[i]));
      c.headers.emplace_back(Str(names[i]).asString(),
                             Str(values[i]).asString());
    }
    c.args["header_names"] = n;
    c.args["header_values"] = v;
    calls_.push_back(c);
    if (stub_ != nullptr) {
      return stub_->HtmlRequestGuard(raw_path, names, values, count,
                                     keyed_scheme, out);
    }
    if (!HasHtmlServe15Set() || out == nullptr ||
        out->struct_size < sizeof(PsHtmlRequestGuard)) {
      return -1;
    }
    const Json::Value want = Pop("ps_html_request_guard");
    if (want.isMember("rc")) return RcOf(want["rc"]);
    const GoogleString reason = want.get("reason", "none").asString();
    out->reason = reason == "request_header" ? kPsHtmlGuardRequestHeader
                  : reason == "path"         ? kPsHtmlGuardPath
                                             : kPsHtmlGuardNone;
    EXPECT_TRUE(reason == "none" || out->reason != kPsHtmlGuardNone)
        << kRunnerError << "unknown guard reason " << reason;
    out->header_index = want.get("header_index", -1).asInt();
    out->xfp = want.get("xfp", false).asBool() ? 1 : 0;
    return 0;
  }

  int HtmlClassifyCookies(const char* const* lines, size_t line_count,
                          int ignore_builtin, const char* const* additions,
                          size_t addition_count,
                          PsHtmlCookieSpan* ignorable_values,
                          size_t ignorable_capacity,
                          PsHtmlCookieClass* out) const override {
    Call c = Rec("ps_html_classify_cookies");
    Json::Value l(Json::arrayValue);
    for (size_t i = 0; i < line_count; ++i) l.append(Str(lines[i]));
    Json::Value a(Json::arrayValue);
    for (size_t i = 0; i < addition_count; ++i) a.append(Str(additions[i]));
    c.args["cookie_lines"] = l;
    c.args["ignore_builtin"] = ignore_builtin != 0;
    c.args["additions"] = a;
    c.args["capacity"] = static_cast<Json::UInt64>(ignorable_capacity);
    calls_.push_back(c);
    if (stub_ != nullptr) {
      return stub_->HtmlClassifyCookies(
          lines, line_count, ignore_builtin, additions, addition_count,
          ignorable_values, ignorable_capacity, out);
    }
    if (!HasHtmlServe15Set() || out == nullptr ||
        out->struct_size < sizeof(PsHtmlCookieClass)) {
      return -1;
    }
    const Json::Value want = Pop("ps_html_classify_cookies");
    if (want.isMember("rc")) return RcOf(want["rc"]);
    // With no verdict: no lines is no cookie, and any line is blocking (a
    // fake that knows no list fails closed), naming the first cookie.
    const GoogleString cls =
        want.isMember("class")
            ? want["class"].asString()
            : GoogleString(line_count == 0 ? "none" : "blocking");
    out->cookie_class = cls == "blocking"         ? kPsHtmlCookiesBlocking
                        : cls == "ignorable_only" ? kPsHtmlCookiesIgnorableOnly
                                                  : kPsHtmlCookiesNone;
    EXPECT_TRUE(cls == "none" || out->cookie_class != kPsHtmlCookiesNone)
        << kRunnerError << "unknown cookie class " << cls;
    out->malformed = want.get("malformed", false).asBool() ? 1 : 0;
    out->first_blocking = PsHtmlCookieSpan{0, 0, 0};
    if (out->cookie_class == kPsHtmlCookiesBlocking) {
      GoogleString name = want.get("first_blocking", "").asString();
      if (name.empty() && line_count > 0) {
        const GoogleString first = Str(lines[0]).asString();
        name = first.substr(0, first.find('='));
      }
      out->first_blocking = SpanOf(lines, line_count, name);
    }
    // The values, as spans into the caller's lines, as far as its array
    // reaches; the count is every value (ignorable_count may say more).
    const Json::Value& values = want["ignorable_values"];
    size_t n = 0;
    for (const Json::Value& value : values) {
      if (n < ignorable_capacity && ignorable_values != nullptr) {
        ignorable_values[n] = SpanOf(lines, line_count, value.asString());
      }
      ++n;
    }
    out->ignorable_count =
        std::max<size_t>(n, want.get("ignorable_count", 0).asUInt64());
    return 0;
  }

  int HtmlIgnoreCookieGuard(const char* addition,
                            int* out_warning) const override {
    Call c = Rec("ps_html_ignore_cookie_guard");
    c.args["addition"] = Str(addition);
    calls_.push_back(c);
    if (stub_ != nullptr) {
      return stub_->HtmlIgnoreCookieGuard(addition, out_warning);
    }
    if (!HasHtmlServe15Set() || addition == nullptr) return -1;
    const Json::Value want = Pop("ps_html_ignore_cookie_guard");
    if (out_warning != nullptr) {
      *out_warning = want.get("warning", false).asBool() ? 1 : 0;
    }
    return want.get("accepted", true).asBool() ? 1 : 0;
  }

  int HtmlPolicyStamp(int credentialed, int ignore_builtin,
                      uint32_t list_version, const char* const* additions,
                      size_t addition_count, uint64_t* out) const override {
    Call c = Rec("ps_html_policy_stamp");
    c.args["credentialed"] =
        credentialed == kPsHtmlCredentialedExplicitPublic  ? "explicit-public"
        : credentialed == kPsHtmlCredentialedAnonymousOnly ? "anonymous-only"
                                                           : "unknown";
    c.args["ignore_builtin"] = ignore_builtin != 0;
    c.args["list_version"] = list_version;
    Json::Value a(Json::arrayValue);
    for (size_t i = 0; i < addition_count; ++i) a.append(Str(additions[i]));
    c.args["additions"] = a;
    calls_.push_back(c);
    if (stub_ != nullptr) {
      return stub_->HtmlPolicyStamp(credentialed, ignore_builtin, list_version,
                                    additions, addition_count, out);
    }
    if (!HasHtmlServe15Set() || out == nullptr) return -1;
    const Json::Value want = Pop("ps_html_policy_stamp");
    if (want.isMember("rc")) return RcOf(want["rc"]);
    *out = want.isMember("value") ? StampOf(want["value"]) : policy_stamp_;
    return 0;
  }

  int HtmlUnsafeMethodInvalidates(const char* method,
                                  int status) const override {
    Call c = Rec("ps_html_unsafe_method_invalidates");
    c.args["method"] = Str(method);
    c.args["status"] = status;
    calls_.push_back(c);
    if (stub_ != nullptr) {
      return stub_->HtmlUnsafeMethodInvalidates(method, status);
    }
    if (!HasHtmlServe15Set()) return 0;
    return Pop("ps_html_unsafe_method_invalidates").get("value", 0).asInt();
  }

  // Every decision line the engine queues: formatted as the library would,
  // one JSON object in the order given, and kept for decision_lines.
  int HtmlDecisionFormat(const char* const* keys, const char* const* values,
                         size_t count, char* buf, size_t capacity,
                         size_t* out_len) const override {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    GoogleString line = "{";
    for (size_t i = 0; i < count; ++i) {
      if (i > 0) line.push_back(',');
      StrAppend(&line, Json::writeString(w, Json::Value(keys[i])), ":",
                Json::writeString(w, Json::Value(values[i])));
    }
    line.push_back('}');
    decision_lines_.push_back(line);
    if (stub_ != nullptr) {
      return stub_->HtmlDecisionFormat(keys, values, count, buf, capacity,
                                       out_len);
    }
    if (buf == nullptr || out_len == nullptr || capacity <= line.size()) {
      return -1;
    }
    memcpy(buf, line.c_str(), line.size() + 1);
    *out_len = line.size();
    return 0;
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
  // The volume calls go to the in-memory volume, or to the stub library.
  const DaemonAbi* vol() const {
    return stub_ != nullptr ? stub_ : static_cast<const DaemonAbi*>(volume_);
  }
  // A hash as the row names it: "zero", "sha256:<body>" for the SHA-256 of a
  // fixture body, otherwise its hex.
  Json::Value HashName(const uint8_t* hash) const {
    if (hash == nullptr) return Json::Value();
    bool zero = true;
    for (size_t i = 0; i < kPsHtmlHashBytes; ++i) zero = zero && hash[i] == 0;
    if (zero) return "zero";
    const GoogleString hex = net_instaweb::Hex(hash, kPsHtmlHashBytes);
    auto it = hash_names_.find(hex);
    return it == hash_names_.end() ? Json::Value(hex)
                                   : Json::Value(StrCat("sha256:", it->second));
  }
  static Json::Value Str(const char* s) {
    return s == nullptr ? Json::Value() : Json::Value(s);
  }
  // Where `text` first occurs in the caller's lines, as a span.
  static PsHtmlCookieSpan SpanOf(const char* const* lines, size_t count,
                                 const GoogleString& text) {
    for (size_t i = 0; i < count && !text.empty(); ++i) {
      const char* at =
          lines[i] == nullptr ? nullptr : strstr(lines[i], text.c_str());
      if (at != nullptr) {
        return PsHtmlCookieSpan{i, static_cast<size_t>(at - lines[i]),
                                text.size()};
      }
    }
    // The runner cannot build the verdict: until the facts carry the Cookie
    // lines the engine has none to pass; after that, the row names a value
    // its own request does not hold.
    if (!kSetsCookieLines<DaemonHtmlRequestFacts>) {
      EntryPointMissing("cookie_lines", NamedOnEngine("cookie_lines"));
    } else {
      ADD_FAILURE() << kRunnerError << "the verdict names " << text
                    << ", which no Cookie line the engine passed holds";
    }
    return PsHtmlCookieSpan{0, 0, 0};
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
  const DaemonAbi* stub_;
  std::map<GoogleString, GoogleString>* site_names_;
  std::map<GoogleString, GoogleString> hash_names_;
  mutable std::map<GoogleString, std::vector<Json::Value>> queues_;
  mutable std::vector<Call> calls_;
  mutable std::vector<GoogleString> decision_lines_;
  std::function<void(const GoogleString&)> on_remove_;
  mutable int store_classify_calls_ = 0;
  int minor_;
  uint64_t policy_stamp_;
  std::vector<GoogleString> missing_;
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

// The SHA-256 of every fixture body, hex -> body name.
std::map<GoogleString, GoogleString> BodyHashNames() {
  std::map<GoogleString, GoogleString> out;
  const Json::Value& bodies = Cases()["bodies"];
  for (const GoogleString& name : bodies.getMemberNames()) {
    uint8_t h[kPsHtmlHashBytes];
    Sha256(bodies[name].asString(), h);
    out[Hex(h, sizeof(h))] = name;
  }
  return out;
}

// How a row runs on the stub library instead of the fake: which build of it,
// and the PS_STUB_* settings that make it answer as the row's library and
// verdicts say.
struct StubSetup {
  const char* library;
  std::vector<std::pair<const char*, const char*>> env;
};

#ifdef DAEMON_HTML_CONFORMANCE_ON_STUB
// Every PS_STUB_* setting a stub row may set; each is cleared around a row.
const char* const kStubEnv[] = {"PS_STUB_MINOR",
                                "PS_STUB_HTML_MODE",
                                "PS_STUB_AGENT_ENTITLED",
                                "PS_STUB_HTML_ACTION",
                                "PS_STUB_HTML_REASON",
                                "PS_STUB_SIDECAR_WRITE_STATUS",
                                "PS_STUB_REMOVE_STATUS",
                                "PS_STUB_EPOCH_WRITE_STATUS",
                                "PS_STUB_EPOCH_READ_STATUS"};
#endif

// One row, run.
class EngineRowRunner : public DaemonHtmlEngine::TestHooks {
 public:
  explicit EngineRowRunner(const Json::Value& row,
                           const StubSetup* stub = nullptr)
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
    const DaemonAbi* stub_abi = nullptr;
    if (stub != nullptr) {
      stub_abi = LoadStub(*stub);
      if (stub_abi == nullptr) return;
      ExpectStubMatchesRow(*stub_abi, engine_body_["library"]);
      // The stub answers; the row's injected verdicts are not used.
      if (!engine_body_["volume"].empty()) {
        ADD_FAILURE() << kRunnerError << "a stub row cannot hold a volume";
      }
    }
    const Json::Value& config = engine_body_["config"];
    abi_ = std::make_unique<ConformanceAbi>(
        &volume_, stub_abi,
        stub_abi != nullptr ? Json::Value(Json::arrayValue)
                            : engine_body_["verdicts"],
        engine_body_["library"], &site_names_, BodyHashNames(),
        kHtmlServeSetMinor,  // a row of the 1.15 settings states its own
        StampOf(config.get("policy_stamp", kDefaultPolicyStamp)));
    abi_->set_on_remove([this](const GoogleString& url) {
      // Whether the request target was fenced when its removal began.
      if (!fence_at_remove_.has_value() && engine_ != nullptr) {
        fence_at_remove_ = engine_->oracle()->Purged(url, FenceProbeTime());
      }
    });
    if (RowExpects("decision_lines")) {
      // The lines are read from the formatter; the log needs a directory.
      log_dir_ = StrCat(GTestTempDir(), "/daemon_html_conformance_log");
    }
    FillVolume();
    ConfigureEngine();
  }

  ~EngineRowRunner() {
    engine_.reset();
#ifdef DAEMON_HTML_CONFORMANCE_ON_STUB
    if (stub_abi_ != nullptr && stub_cache_ != nullptr) {
      stub_abi_->CacheClose(stub_cache_);
    }
    for (const char* name : kStubEnv) SetEnv(name, nullptr);
#endif
  }

  bool ready() const { return abi_ != nullptr; }

  void Run() {
    // A row that starts with a configure step loads its configuration
    // there, once: the library's guard and stamp verdicts it injects are
    // for that load, not for one before it.
    const Json::Value& steps = engine_body_["steps"];
    if (steps.empty() || steps[0].get("kind", "").asString() != "configure") {
      CreateEngine();
    }
    int index = 0;
    for (const Json::Value& step : engine_body_["steps"]) {
      SCOPED_TRACE(StrCat("step ", IntegerToString(index++), ": ",
                          step["kind"].asString()));
      RunStep(step);
    }
  }

  void AtPoint(const char* point) override { RunDuring(point); }

 private:
  friend class RunnerShape;

#ifdef DAEMON_HTML_CONFORMANCE_ON_STUB
  static void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value == nullptr ? "" : value);
#else
    if (value == nullptr) {
      unsetenv(name);
    } else {
      setenv(name, value, 1);
    }
#endif
  }

  const DaemonAbi* LoadStub(const StubSetup& stub) {
    for (const char* name : kStubEnv) SetEnv(name, nullptr);
    for (const auto& kv : stub.env) SetEnv(kv.first, kv.second);
    GoogleString error;
    stub_abi_.reset(LoadDaemonAbi(StubLibraryPath(stub.library), &error));
    if (stub_abi_ == nullptr) {
      ADD_FAILURE() << kRunnerError
                    << "the stub library did not load: " << error;
      return nullptr;
    }
    alignas(std::max_align_t) static unsigned char
        config_storage[kCacheConfigProbeBytes];
    PsCacheConfig* config = reinterpret_cast<PsCacheConfig*>(config_storage);
    stub_abi_->CacheConfigInit(config);
    if (stub_abi_->CacheOpen(config, &stub_cache_) != kPsOk) {
      ADD_FAILURE() << kRunnerError << "the stub library opened no cache";
      return nullptr;
    }
    return stub_abi_.get();
  }
#else
  const DaemonAbi* LoadStub(const StubSetup&) {
    ADD_FAILURE() << kRunnerError
                  << "this build does not run rows on the stub library";
    return nullptr;
  }
#endif

  bool on_stub() const { return stub_abi_ != nullptr; }

  // The stub's PS_STUB_* settings must make it the library the row
  // describes; a wrong mapping would turn a degrade row into a normal one.
  static void ExpectStubMatchesRow(const DaemonAbi& abi,
                                   const Json::Value& library) {
    const int minor = library.get("minor", 14).asInt();
    const bool bound =
        minor >= kHtmlServeSetMinor && library.get("set_bound", true).asBool();
    EXPECT_EQ(minor, abi.VersionMinor()) << "stub minor";
    EXPECT_EQ(bound, abi.HasHtmlServeSet()) << "stub serve set";
    if (!bound) return;
    const int mode = ConformanceAbi::HtmlMode(
        library.get("html_mode", "markdown-static").asString());
    EXPECT_EQ(mode, abi.ReadSharedConfigHtmlMode("/unused")) << "stub mode";
    EXPECT_EQ(library.get("agent_entitled", true).asBool() ? 1 : 0,
              abi.ReadSharedConfigAgentEntitled("/unused"))
        << "stub agent-optimize";
  }

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
        if (e.isMember("content_type")) {
          entry.origin_ct = e["content_type"].asString();
        }
        // The 1.15 entry record; an entry that names neither has both (the
        // earlier rows describe such entries).
        if (e.get("store_v2", true).asBool()) {
          entry.flags |= kPsFlagHtmlStoreV2;
        }
        if (e.get("anonymous_recorded", true).asBool()) {
          entry.flags |= kPsFlagAnonymousRecorded;
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
        s.format = e.get("format", 2).asInt();
        // The block is the one source of an entry's stamp.
        if (e.isMember("policy_stamp")) {
          s.policy_stamp = StampOf(e["policy_stamp"]);
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
        entry.withheld_reason =
            WithheldReasonOf(e.get("reason", "below_floor"));
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
                     : m == "serve-cached-html"
                         ? DaemonHtmlMode::kServeCachedHtml
                         : DaemonHtmlMode::kServeVariants;
    }
    // The cached-HTML settings, as a port fills them from its directives.
    // The policy stamp is not among them: configuration load computes it
    // (ApplyPolicySettings, through the fake's ps_html_policy_stamp).
    if (c.isMember("serve_scope")) {
      config_.serve_scope = c["serve_scope"].asString() == "all"
                                ? kPsHtmlServeScopeAll
                                : kPsHtmlServeScopeAgents;
    }
    if (c.isMember("max_age_s")) config_.max_age_s = c["max_age_s"].asUInt();
    if (c.isMember("credentialed")) {
      config_.credentialed = c["credentialed"].asString() == "explicit-public"
                                 ? kPsHtmlCredentialedExplicitPublic
                                 : kPsHtmlCredentialedAnonymousOnly;
    }
    if (c.isMember("ignore_cookies")) {
      config_.ignore_cookies = c["ignore_cookies"].asBool();
    }
    for (const Json::Value& name : c["ignore_cookie"]) {
      config_.ignore_cookie.push_back(name.asString());
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
    // Configuration load, as every port runs it before creating the engine.
    DaemonHtmlEngine::Config config = config_;
    config_error_.clear();
    policy_settings_ = CallApplyPolicySettings<DaemonHtmlEngine>(
        *abi_, &config, &config_error_);
    DaemonHtmlEngine::Deps deps;
    deps.substrate.abi = abi_.get();
    deps.substrate.cache = on_stub() ? stub_cache_ : &volume_;
    deps.substrate.cache_path = "/var/cache/pagespeed";
    deps.substrate.notify_socket = "/run/pagespeed/worker.sock";
    deps.purge_source = &source_;
    deps.statistics = &stats_;
    deps.timer = &timer_;
    deps.handler = &handler_;
    deps.limiter = &limiter_;
    deps.budget = &budget_;
    deps.log_dir = log_dir_;
    engine_ = std::make_unique<DaemonHtmlEngine>(config, deps);
    engine_->set_test_hooks(this);
    engine_->oracle()->set_recheck_disabled_for_testing(recheck_disabled_);
    if (credential_skip_disabled_) DisableCredentialSkipOrFail();
  }

  void DisableCredentialSkipOrFail() {
    credential_skip_disabled_ = true;
    if (!DisableCredentialSkip(engine_.get())) {
      EntryPointMissing(
          "set_credential_skip_disabled_for_testing",
          NamedOnEngine("set_credential_skip_disabled_for_testing"));
    }
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
    } else if (name == "disable_credential_skip") {
      DisableCredentialSkipOrFail();
    } else if (name == "unsafe") {
      // Another request's unsafe method, answered while this step runs.
      Json::Value request(Json::objectValue);
      request["method"] = arg["method"];
      const GoogleString url = arg["url"].asString();
      const size_t scheme_end = url.find("://");
      const size_t path_at = url.find('/', scheme_end + 3);
      request["scheme"] = url.substr(0, scheme_end);
      request["host"] = url.substr(scheme_end + 3, path_at - scheme_end - 3);
      request["path"] = path_at == GoogleString::npos ? GoogleString("/")
                                                      : url.substr(path_at);
      Json::Value response(Json::objectValue);
      response["status"] = arg["status"];
      RunUnsafe(request, response);
    } else {
      ADD_FAILURE() << kRunnerError << "unknown op " << name;
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

  // The request's facts, as a port fills them, and its agent class, which a
  // port computes once per client request (agent_class absent: classified
  // from user_agent and signature through the port's slot).
  DaemonHtmlRequestFacts Facts(const Json::Value& r) {
    for (const GoogleString& key : r.getMemberNames()) {
      EXPECT_EQ(1u, RequestKeys().count(key))
          << kRunnerError << "unknown request key " << key;
    }
    DaemonHtmlRequestFacts f = BaseFacts(r);
    f.has_authorization =
        f.has_authorization || r.get("proxy_authorization", false).asBool();
    // One list of request header lines, from either key.
    DaemonHtmlHeaderLines lines;
    for (const char* key : {"headers", "request_headers"}) {
      for (const Json::Value& line : r[key]) {
        lines.emplace_back(line[0].asString(), line[1].asString());
      }
    }
    const GoogleString both = HeaderNamedUnderBothKeys(r);
    EXPECT_TRUE(both.empty())
        << kRunnerError << both
        << " is named under both headers and request_headers";
    for (const auto& [name, value] : lines) {
      const char* marker = DaemonHtmlAjaxMarker(name, value);
      if (marker != nullptr && f.ajax_marker.empty()) f.ajax_marker = marker;
    }
    // Rows of the earlier serve path give `headers` for the AJAX marker
    // alone; only a row that names request_headers needs the facts field.
    if (!SetRequestHeaders(&f, lines) && r.isMember("request_headers")) {
      EntryPointMissing("request_headers", NamedOnEngine("request_headers"));
    }
    if (!SetRawPath(&f, r.get("raw_path", "").asString(),
                    r.isMember("raw_path"))) {
      EntryPointMissing("raw_path", NamedOnEngine("raw_path"));
    }
    std::vector<GoogleString> cookie_lines;
    for (const Json::Value& line : r["cookie_lines"]) {
      cookie_lines.push_back(line.asString());
    }
    f.has_cookie = f.has_cookie || !cookie_lines.empty();
    if (!SetCookieLines(&f, cookie_lines)) {
      EntryPointMissing("cookie_lines", NamedOnEngine("cookie_lines"));
    }
    // An internal request is handed to the engine, marked, and the engine
    // must decline it itself.
    if (!SetInternal(&f, r.get("internal", false).asBool())) {
      EntryPointMissing("internal", NamedOnEngine("internal"));
    }
    if (!SetServedFromCache(&f, r.get("served_from_cache", false).asBool())) {
      EntryPointMissing("served_from_cache",
                        NamedOnEngine("served_from_cache"));
    }
    if (r.isMember("agent_class")) {
      f.agent_class = r["agent_class"].asInt();
    } else {
      f.agent_class = ClassifyAgent(r);
    }
    agent_class_ = f.agent_class;
    return f;
  }

  // The port's once-per-request slot: a re-routed request (an internal
  // redirect) reads the class the first hook kept and classifies nothing.
  int ClassifyAgent(const Json::Value& r) {
    const GoogleString sig = r.get("signature", "").asString();
    const DaemonHtmlAgentSignature signature =
        sig == "signed_agent" || sig == "verified_bot"
            ? DaemonHtmlAgentSignature::kVerified
        : sig == "not_ready" ? DaemonHtmlAgentSignature::kNotReady
        : sig == "failed"    ? DaemonHtmlAgentSignature::kFailed
                             : DaemonHtmlAgentSignature::kNone;
    EXPECT_TRUE(sig.empty() || signature != DaemonHtmlAgentSignature::kNone)
        << kRunnerError << "unknown signature " << sig;
    const GoogleString ua = r.get("user_agent", "").asString();
    DaemonHtmlAgentClassSlot slot;
    slot.Resolve(ua, signature, &engine_->agent_class_counters());
    if (r.get("internal_redirect", false).asBool()) {
      slot.Resolve(ua, signature, &engine_->agent_class_counters());
    }
    return slot.agent_class();
  }

  // The name of a header line given under both request-header keys, or "".
  static GoogleString HeaderNamedUnderBothKeys(const Json::Value& r) {
    for (const Json::Value& a : r["headers"]) {
      for (const Json::Value& b : r["request_headers"]) {
        if (StringCaseEqual(a[0].asString(), b[0].asString())) {
          return a[0].asString();
        }
      }
    }
    return GoogleString();
  }

  // Every request key the runner reads; any other is a row error, never
  // silently ignored.
  static const std::set<GoogleString>& RequestKeys() {
    static const auto* keys = new std::set<GoogleString>{
        "method", "scheme", "host", "path", "url_agreed",
        "options_enabled_and_allowed", "capture_start_ms", "accept", "cookie",
        "authorization", "authenticated_user", "cache_control", "pragma",
        "if_none_match", "if_modified_since", "if_match", "if_unmodified_since",
        "if_range", "range", "origin", "headers",
        // The cached-HTML serve path.
        "agent_class", "user_agent", "signature", "internal_redirect",
        "internal", "served_from_cache", "cookie_lines", "proxy_authorization",
        "request_headers", "raw_path"};
    return *keys;
  }

  DaemonHtmlRequestFacts BaseFacts(const Json::Value& r) const {
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
    // Either flag is given as present (true) or by the line's value.
    auto present = [&r](const char* key) {
      const Json::Value& v = r[key];
      return v.isBool() ? v.asBool() : !v.isNull();
    };
    f.has_if_match = present("if_match");
    f.has_if_unmodified_since = present("if_unmodified_since");
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
    std::map<GoogleString, int64> counters_before =
        Counters(step["expect"]["counters"]);
    const size_t decision_lines_before = abi_->decision_lines().size();
    port_ = FakePort();
    purge_set_added_.clear();
    admin_ok_ = true;
    serve_ = DaemonHtmlServeResult();
    during_ = &step["during"];
    agent_class_ = -1;
    recording_started_.reset();
    fence_at_remove_.reset();
    unsafe_targets_.clear();
    fence_map_size_max_ = 0;

    for (const Json::Value& op : step["pre"]) RunOp(op);
    const GoogleString kind = step["kind"].asString();
    const Json::Value& request = step["request"];
    step_request_ = request;
    const int repeat = step.get("repeat", 1).asInt();
    EXPECT_GE(repeat, 1) << kRunnerError << "repeat is below 1";
    for (int i = 0; i < repeat; ++i) {
      if (kind == "capture") {
        RunCapture(request, step["response"]);
      } else if (kind == "serve" || kind == "emit_fails") {
        port_.as_origin_ok = kind == "serve";
        serve_ = engine_->Serve(Facts(request), &port_);
      } else if (kind == "begin_recording") {
        // The recording start a port makes at its capture point.
        std::unique_ptr<DaemonHtmlRecording> rec =
            engine_->StartRecording(Facts(request), &port_);
        recording_started_ = rec != nullptr;
      } else if (kind == "configure") {
        CreateEngine();
      } else if (kind == "unsafe") {
        RunUnsafe(request, step["response"]);
      } else if (kind == "unsafe_many") {
        RunUnsafeMany(request, step.get("count", 0).asInt());
      } else {
        RunOtherStep(kind, request);
      }
    }
    during_ = nullptr;

    const std::vector<Call> calls(abi_->calls().begin() + calls_before,
                                  abi_->calls().end());
    const std::vector<RecordedNotify> notifies(
        abi_->notifies().begin() + notifies_before, abi_->notifies().end());
    int writes = volume_.writes_closed() - writes_before;
    if (on_stub()) {
      // No volume to look in: every write the engine closed is a call.
      writes = static_cast<int>(
          std::count_if(calls.begin(), calls.end(), [](const Call& c) {
            return c.fn == "ps_write_close" ||
                   c.fn == "ps_cache_write_original_site";
          }));
    }
    // The recorder writes only the names RecordedArgs lists.
    for (const Call& c : calls) {
      auto it = RecordedArgs().find(c.fn);
      if (it == RecordedArgs().end()) {
        ADD_FAILURE() << kRunnerError << c.fn << " has no RecordedArgs entry";
        continue;
      }
      for (const GoogleString& key : c.args.getMemberNames()) {
        if (it->second.count(key) == 0) {
          ADD_FAILURE() << kRunnerError << c.fn << " records " << key
                        << ", which RecordedArgs does not list";
        }
      }
    }
    const std::vector<GoogleString> decision_lines(
        abi_->decision_lines().begin() + decision_lines_before,
        abi_->decision_lines().end());
    Check(step["expect"], calls, notifies, handler_.count() - logs_before,
          writes, counters_before);
    CheckServePath(step["expect"], calls, decision_lines, counters_before);
  }

  void RunOtherStep(const GoogleString& kind, const Json::Value& request) {
    if (kind == "poll_tick") {
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
      ADD_FAILURE() << kRunnerError << "unknown step kind " << kind;
    }
  }

  // The lines of a Location or Content-Location value: one line or a list.
  static std::vector<GoogleString> LinesOf(const Json::Value& v) {
    std::vector<GoogleString> out;
    if (v.isString()) out.push_back(v.asString());
    for (const Json::Value& line : v.isArray() ? v : Json::Value()) {
      out.push_back(line.asString());
    }
    return out;
  }

  // The response to an unsafe method, handed to the engine as a port does.
  // False when the engine has no entry point for it.
  bool RunUnsafe(const Json::Value& request, const Json::Value& response) {
    const DaemonHtmlRequestFacts facts = Facts(request);
    if (!CallOnUnsafeRequest(engine_.get(), facts,
                             response.get("status", 0).asInt(),
                             LinesOf(response["location"]),
                             LinesOf(response["content_location"]), &port_)) {
      EntryPointMissing("OnUnsafeRequest", NamedOnEngine("OnUnsafeRequest"));
      return false;
    }
    return true;
  }

  // `count` unsafe requests to distinct targets (the request path with
  // "-<n>" appended), with the fence map's size read back as it grows.
  void RunUnsafeMany(const Json::Value& request, int count) {
    EXPECT_GT(count, 0) << kRunnerError << "unsafe_many count";
    Json::Value response(Json::objectValue);
    response["status"] = 200;
    for (int i = 0; i < count; ++i) {
      Json::Value r = request;
      r["path"] = StrCat(request.get("path", "/page").asString(), "-",
                         IntegerToString(i));
      unsafe_targets_.push_back(
          StrCat(r.get("scheme", "https").asString(), "://",
                 r.get("host", kDefaultHost).asString(), r["path"].asString()));
      if (!RunUnsafe(r, response)) return;
      // Sampled as the map fills (every 256, and around its bound).
      if (i % 256 == 255 || (i >= 4090 && i <= 4100) || i == count - 1) {
        fence_map_size_max_ = std::max(fence_map_size_max_, FencesKept());
      }
    }
  }

  // A time for a fence probe: an entry stored the second before now.
  uint32 FenceProbeTime() const {
    return static_cast<uint32>(Now() / 1000 - 1);
  }

  // How many of the step's unsafe targets are fenced now.
  int FencesKept() {
    int kept = 0;
    for (const GoogleString& url : unsafe_targets_) {
      if (engine_->oracle()->Purged(url, FenceProbeTime())) ++kept;
    }
    return kept;
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
      if (response.isMember("split_at")) {
        // Two pieces, split where the row says.
        const size_t at =
            std::min<size_t>(response["split_at"].asUInt64(), body.size());
        rec->AddBody(body.data(), at);
        rec->AddBody(body.data() + at, body.size() - at);
      } else {
        // In pieces, as a server delivers it.
        for (size_t i = 0; i < body.size(); i += 100) {
          rec->AddBody(body.data() + i, std::min<size_t>(100, body.size() - i));
        }
      }
      rec->NoteFinalHeaders(final_point);
    }
    RunDuring("after_send");
    if (rec != nullptr) rec->Finish(response.get("aborted", false).asBool());
  }

  // Every engine counter, and any other registered counter `named` names
  // (a counter the serve path registers outside the two lists).
  std::map<GoogleString, int64> Counters(
      const Json::Value& named = Json::Value()) {
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
    for (const GoogleString& name :
         named.isObject() ? named.getMemberNames() : Json::Value::Members()) {
      Variable* v = stats_.FindVariable(name);
      if (v != nullptr) out[name] = v->Get();
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

  // `exact` names struct prefixes ("request", "stored") whose every recorded
  // field the row must list: a field the engine set that the row did not
  // name fails the match.
  bool ArgsMatch(const Call& call, const Json::Value& args,
                 const Json::Value& exact) const {
    for (const Json::Value& prefix : exact) {
      const GoogleString p = StrCat(prefix.asString(), ".");
      for (const GoogleString& key : call.args.getMemberNames()) {
        if (HasPrefixString(key, p) && !args.isMember(key)) return false;
      }
    }
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
    for (const Json::Value& want : expect["calls"]) {
      EXPECT_TRUE(IsRecordedFn(want["fn"].asString()))
          << kRunnerError << want["fn"].asString() << " is never recorded";
    }
    for (const Json::Value& fn : expect["calls_exclude"]) {
      EXPECT_TRUE(IsRecordedFn(fn.asString()))
          << kRunnerError << fn.asString()
          << " is never recorded, so excluding it asserts "
          << "nothing";
    }
    if (expect.isMember("calls")) {
      size_t at = 0;
      for (const Json::Value& want : expect["calls"]) {
        const GoogleString fn = want["fn"].asString();
        bool found = false;
        for (; at < calls.size(); ++at) {
          if (calls[at].fn == fn &&
              ArgsMatch(calls[at], want["args"], want["exact"])) {
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
    if (on_stub()) {
      for (const char* key : {"removed", "volume_epoch_s"}) {
        if (expect.isMember(key)) {
          ADD_FAILURE() << kRunnerError << key
                        << " needs the in-memory volume, not the stub";
        }
      }
      if (expect.isMember("stored") && !expect["stored"].empty()) {
        ADD_FAILURE() << kRunnerError
                      << "a stored entry needs the in-memory volume";
      }
    }
    if (expect.isMember("stored")) {
      if (expect["stored"].empty()) {
        EXPECT_EQ(0, writes) << "nothing may be stored in this step";
      }
      for (const Json::Value& want : expect["stored"]) {
        if (on_stub()) break;
        EXPECT_EQ("original", want.get("kind", "original").asString())
            << kRunnerError << "a stored entry is an original";
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
        // The 1.15 entry record.
        if (want.isMember("store_v2")) {
          EXPECT_EQ(want["store_v2"].asBool(),
                    (e->flags & kPsFlagHtmlStoreV2) != 0)
              << "store_v2";
        }
        if (want.isMember("anonymous_recorded")) {
          EXPECT_EQ(want["anonymous_recorded"].asBool(),
                    (e->flags & kPsFlagAnonymousRecorded) != 0)
              << "anonymous_recorded";
        }
      }
    }
    for (const Json::Value& want : expect["removed"]) {
      if (on_stub()) break;
      // A removed entry is its url, or {url}.
      const GoogleString url =
          want.isString() ? want.asString() : want["url"].asString();
      bool found = false;
      for (const Call& c : calls) {
        if (c.fn == "ps_cache_remove_site" && c.args["url"].asString() == url) {
          found = true;
        }
      }
      EXPECT_TRUE(found) << "removed " << url;
      EXPECT_EQ(nullptr, volume_.Find(default_site_, url, kPsSentinelOriginal));
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
      const std::map<GoogleString, int64> after = Counters(expect["counters"]);
      for (const GoogleString& name : expect["counters"].getMemberNames()) {
        if (after.count(name) == 0) {
          if (IsServePathCounter(name)) {
            ADD_FAILURE() << kCounterAbsent << name;
          } else {
            ADD_FAILURE() << kRunnerError << "unknown counter " << name;
          }
          continue;
        }
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
    if (expect.isMember("volume_epoch_s") && !on_stub()) {
      if (expect["volume_epoch_s"].isNull()) {
        // Never written.
        EXPECT_EQ(0u, volume_.epochs().count(default_site_))
            << "the epoch was written";
      } else {
        EXPECT_EQ(expect["volume_epoch_s"].asUInt64(),
                  volume_.epochs()[default_site_]);
      }
    }
    if (expect.isMember("budget_in_use")) {
      EXPECT_EQ(expect["budget_in_use"].asInt64(), budget_.in_use());
    }
  }

  void CheckServe(const Json::Value& want) {
    const char* kind = serve_.kind == DaemonHtmlServeResult::kFinal ? "final"
                       : serve_.kind == DaemonHtmlServeResult::kAsOrigin
                           ? "as_origin"
                           : "fall_through";
    EXPECT_EQ(want["kind"].asString(), kind);
    if (want.isMember("suppress_recording")) {
      EXPECT_EQ(want["suppress_recording"].asBool(), serve_.suppress_recording)
          << "suppress_recording";
    }
    if (want.isMember("body_ref")) {
      EXPECT_EQ(Body(want["body_ref"]), port_.emitted.body) << "body";
    }
    if (want.isMember("header_order")) {
      // The named lines, in the order the port is asked to emit them.
      std::set<GoogleString> names;
      std::vector<GoogleString> wanted;
      for (const Json::Value& n : want["header_order"]) {
        GoogleString name = n.asString();
        LowerString(&name);
        names.insert(name);
        wanted.push_back(name);
      }
      std::vector<GoogleString> got;
      for (const auto& line : port_.emitted.headers) {
        GoogleString name = line.first;
        LowerString(&name);
        if (names.count(name) != 0) got.push_back(name);
      }
      EXPECT_EQ(wanted, got) << "header order";
    }
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

  // The expectations of the cached-HTML serve path.
  void CheckServePath(const Json::Value& expect, const std::vector<Call>& calls,
                      const std::vector<GoogleString>& decision_lines,
                      const std::map<GoogleString, int64>& before) {
    for (const Json::Value& name : expect["headers_absent"]) {
      for (const auto& line : port_.emitted.headers) {
        EXPECT_FALSE(StringCaseEqual(name.asString(), line.first))
            << line.first << " is emitted";
      }
    }
    if (expect.isMember("classify_calls")) {
      EXPECT_EQ(expect["classify_calls"].asInt(),
                std::count_if(calls.begin(), calls.end(),
                              [](const Call& c) {
                                return c.fn == "ps_html_serve_classify";
                              }))
          << "ps_html_serve_classify calls";
    }
    if (expect.isMember("removed_count")) {
      EXPECT_EQ(expect["removed_count"].asInt(),
                std::count_if(calls.begin(), calls.end(),
                              [](const Call& c) {
                                return c.fn == "ps_cache_remove_site";
                              }))
          << "ps_cache_remove_site calls";
    }
    if (expect.isMember("decision_lines")) {
      const GoogleString all = JoinCollection(decision_lines, "\n");
      for (const Json::Value& text : expect["decision_lines"]["contains"]) {
        EXPECT_NE(GoogleString::npos, all.find(text.asString()))
            << "no decision line holds " << text.asString() << "; lines:\n"
            << all;
      }
      for (const Json::Value& text : expect["decision_lines"]["absent"]) {
        EXPECT_EQ(GoogleString::npos, all.find(text.asString()))
            << "a decision line holds " << text.asString();
      }
    }
    if (expect.isMember("recording_started")) {
      ASSERT_TRUE(recording_started_.has_value())
          << kRunnerError << "recording_started needs a begin_recording step";
      EXPECT_EQ(expect["recording_started"].asBool(), *recording_started_)
          << "recording started";
    }
    if (expect.isMember("config_error")) {
      if (policy_settings_ < 0) {
        EntryPointMissing("ApplyPolicySettings",
                          NamedOnEngine("ApplyPolicySettings"));
      } else if (expect["config_error"].isNull()) {
        EXPECT_EQ(1, policy_settings_) << config_error_;
      } else {
        EXPECT_EQ(0, policy_settings_) << "the configuration loaded";
        const GoogleString text = expect["config_error"]["contains"].asString();
        EXPECT_NE(GoogleString::npos, config_error_.find(text))
            << "the error does not name " << text << ": " << config_error_;
      }
    }
    if (expect.isMember("fence_before_remove")) {
      ASSERT_TRUE(fence_at_remove_.has_value()) << "nothing was removed";
      EXPECT_EQ(expect["fence_before_remove"].asBool(), *fence_at_remove_)
          << "the target was fenced when its removal began";
    }
    if (expect.isMember("fences_kept")) {
      EXPECT_EQ(expect["fences_kept"].asInt(), FencesKept()) << "fences kept";
    }
    if (expect.isMember("fence_map_size_max")) {
      EXPECT_GE(expect["fence_map_size_max"].asInt(), fence_map_size_max_)
          << "the fence map grew past its bound";
    }
    for (const char* key : {"oldest_fenced", "newest_fenced"}) {
      if (!expect.isMember(key)) continue;
      ASSERT_FALSE(unsafe_targets_.empty())
          << kRunnerError << key << " needs unsafe_many";
      const GoogleString& url = key == GoogleString("oldest_fenced")
                                    ? unsafe_targets_.front()
                                    : unsafe_targets_.back();
      EXPECT_EQ(expect[key].asBool(),
                engine_->oracle()->Purged(url, FenceProbeTime()))
          << key;
    }
    if (expect.isMember("agent_class")) {
      EXPECT_EQ(expect["agent_class"].asInt(), agent_class_) << "agent class";
    }
    if (expect.isMember("classify_agent_calls")) {
      const std::map<GoogleString, int64> after = Counters();
      int64 classified = 0;
      for (const char* name : {kDaemonHtmlAgentClassC1, kDaemonHtmlAgentClassC3,
                               kDaemonHtmlAgentClassC4}) {
        classified += after.at(name) - before.at(name);
      }
      EXPECT_EQ(expect["classify_agent_calls"].asInt64(), classified)
          << "classifications";
    }
    if (expect.isMember("agent_vectors")) {
      // Every shared vector as the step's request: classified through the
      // port's slot, then served, and the class the engine passes the
      // library is the vector's.
      const AgentVectors vectors = LoadAgentVectors();
      EXPECT_FALSE(vectors.vectors.empty());
      for (const AgentVector& v : vectors.vectors) {
        Json::Value r = step_request_;
        r.removeMember("agent_class");
        r["user_agent"] = v.user_agent;
        const size_t before_calls = abi_->calls().size();
        const DaemonHtmlRequestFacts facts = Facts(r);
        EXPECT_EQ(v.expected_class, facts.agent_class)
            << "vector line " << v.line << ": " << v.user_agent;
        engine_->Serve(facts, &port_);
        int passed = -1;
        for (size_t i = before_calls; i < abi_->calls().size(); ++i) {
          const Call& c = abi_->calls()[i];
          if (c.fn == "ps_html_serve_classify" &&
              c.args.isMember("request.agent_class")) {
            passed = c.args["request.agent_class"].asInt();
            break;
          }
        }
        EXPECT_EQ(v.expected_class, passed)
            << "the class passed to ps_html_serve_classify, vector line "
            << v.line << ": " << v.user_agent;
      }
    }
  }

  // Whether any step of the row expects `key`.
  bool RowExpects(const char* key) const {
    for (const Json::Value& step : engine_body_["steps"]) {
      if (step["expect"].isMember(key)) return true;
    }
    return false;
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
  std::unique_ptr<DaemonAbi> stub_abi_;
  void* stub_cache_ = nullptr;
  // The cached-HTML serve path.
  GoogleString log_dir_;
  int policy_settings_ = -1;  // CallApplyPolicySettings
  GoogleString config_error_;
  bool credential_skip_disabled_ = false;
  int agent_class_ = -1;
  std::optional<bool> recording_started_;
  std::optional<bool> fence_at_remove_;
  std::vector<GoogleString> unsafe_targets_;
  int fence_map_size_max_ = 0;
  Json::Value step_request_;
};

bool NamesEngine(const Json::Value& row) {
  for (const Json::Value& r : row["runners"]) {
    if (r.asString() == "engine") return true;
  }
  return false;
}

// A pending row describes behaviour whose support is not built yet: it is
// never run and never counted as a skip, only counted apart. The change that
// adds the support removes the marker.
[[maybe_unused]] bool IsPendingEngineRow(const Json::Value& row) {
  return NamesEngine(row) && row.isMember("pending");
}

bool IsEngineRow(const Json::Value& row) {
  return NamesEngine(row) && !row.isMember("pending");
}

GoogleString TestName(const Json::Value& row) {
  GoogleString name = row["id"].asString();
  for (char& c : name) {
    if (c == '-') c = '_';
  }
  return name;
}

// What the runner reads of a row, as closed sets: a key or value outside
// them is a row the runner cannot run as written, reported without running
// it.  (daemon_html_conformance_cases_test checks the documented shape; this
// checks that the runner reads every part of it.)
class RunnerShape {
 public:
  using Set = std::set<GoogleString>;

  std::vector<GoogleString> Errors(const Json::Value& row) {
    const Json::Value& e = row["engine"];
    Keys("engine", e, {"config", "library", "volume", "verdicts", "steps"});
    Keys("config", e["config"],
         {"mode", "agent_optimize", "max_bytes", "host_names", "budget_bytes",
          "sites", "serve_scope", "max_age_s", "credentialed", "ignore_cookies",
          "ignore_cookie", "policy_stamp"});
    Value("config mode", e["config"]["mode"],
          {"off", "report-only", "serve-variants", "serve-cached-html"});
    Value("config serve_scope", e["config"]["serve_scope"], {"agents", "all"});
    Value("config credentialed", e["config"]["credentialed"],
          {"anonymous-only", "explicit-public"});
    Keys("library", e["library"],
         {"minor", "set_bound", "html_mode", "agent_entitled", "missing"});
    // A row of the cached-HTML settings states its library: the 1.15 rules
    // and the earlier ones differ, so the minor is never implied.
    if (NamesServeCachedHtmlSettings(e["config"]) &&
        !e["library"]["minor"].isIntegral()) {
      Fail("library: a row that names a cached-HTML setting sets minor");
    }
    for (const Json::Value& v : e["volume"]) Volume(v);
    for (const Json::Value& v : e["verdicts"]) Verdict(v);
    for (const Json::Value& step : e["steps"]) Step(step);
    return errors_;
  }

 private:
  void Fail(const GoogleString& what) { errors_.push_back(what); }

  void Keys(const GoogleString& where, const Json::Value& obj,
            const Set& allowed) {
    if (obj.isNull()) return;
    if (!obj.isObject()) return Fail(StrCat(where, " is not an object"));
    for (const GoogleString& key : obj.getMemberNames()) {
      if (allowed.count(key) == 0) {
        Fail(StrCat(where, ": the runner does not read '", key, "'"));
      }
    }
  }

  void Value(const GoogleString& where, const Json::Value& v,
             const Set& allowed) {
    if (!v.isNull() && (!v.isString() || allowed.count(v.asString()) == 0)) {
      Fail(StrCat(where, ": the runner does not know this value"));
    }
  }

  void Volume(const Json::Value& v) {
    static const auto* kKeys = new std::map<GoogleString, Set>{
        {"original",
         {"kind", "url", "body_ref", "stored_at_s", "lifetime_s", "cc_flags",
          "flags", "paired", "content_type", "store_v2", "anonymous_recorded"}},
        {"sidecar", {"kind", "url", "headers", "format", "policy_stamp"}},
        {"markdown", {"kind", "url", "body_ref"}},
        {"withheld", {"kind", "url", "reason"}},
        {"epoch", {"kind", "site", "epoch_s"}}};
    auto it = kKeys->find(v["kind"].asString());
    if (it == kKeys->end()) return Fail("volume: unknown kind");
    Keys(StrCat("volume ", it->first), v, it->second);
  }

  void Verdict(const Json::Value& v) {
    // Per function, the keys its answer reads (fn is every verdict's).
    static const auto* kKeys = new std::map<GoogleString, Set>{
        {"ps_html_store_classify",
         {"view", "action", "reason", "lifetime_s", "flags", "cc_flags",
          "detail"}},
        {"ps_html_serve_classify",
         {"action", "reason", "wants_markdown", "age_s"}},
        {"ps_html_store_write_params", {"rc", "inserted_at_s"}},
        {"ps_html_markdown_etag", {"etag"}},
        {"ps_if_none_match_matches_weak", {"match"}},
        {"ps_cache_write_headers_sidecar_site", {"rc", "stored"}},
        {"ps_cache_remove_site", {"rc"}},
        {"ps_cache_write_site_purge_epoch", {"rc"}},
        {"ps_cache_read_site_purge_epoch", {"rc"}},
        {"ps_read_shared_config_html_mode", {"value"}},
        {"ps_read_shared_config_agent_entitled", {"value"}},
        {"ps_wants_agent_markdown", {"value"}},
        {"ps_html_request_guard", {"rc", "reason", "header_index", "xfp"}},
        {"ps_html_classify_cookies",
         {"rc", "class", "first_blocking", "ignorable_values",
          "ignorable_count", "malformed"}},
        {"ps_html_ignore_cookie_guard", {"accepted", "warning"}},
        {"ps_html_policy_stamp", {"rc", "value"}},
        {"ps_html_unsafe_method_invalidates", {"value"}}};
    const GoogleString fn = v["fn"].asString();
    auto it = kKeys->find(fn);
    if (it == kKeys->end()) {
      return Fail(StrCat("verdicts: the fake answers no ", fn));
    }
    Set keys = it->second;
    keys.insert("fn");
    Keys(StrCat("verdict ", fn), v, keys);
  }

  void Ops(const GoogleString& where, const Json::Value& ops) {
    static const Set kOps = {"purge_url",     "purge_global",
                             "flush_file",    "wipe_file_cache",
                             "create_engine", "advance_clock",
                             "make_visible",  "disable_recheck",
                             "budget",        "disable_credential_skip",
                             "unsafe"};
    for (const Json::Value& op : ops) {
      if (!op.isObject() || op.size() != 1 ||
          kOps.count(op.getMemberNames()[0]) == 0) {
        Fail(StrCat(where, ": an op the runner does not run"));
      }
    }
  }

  void Step(const Json::Value& step) {
    const GoogleString kind = step["kind"].asString();
    Keys("step", step,
         {"kind", "pre", "request", "response", "during", "expect", "repeat",
          "count"});
    Value(
        "step kind", step["kind"],
        {"capture", "serve", "poll_tick", "stats_tick", "admin_purge", "unsafe",
         "unsafe_many", "emit_fails", "begin_recording", "configure"});
    if (kind == "admin_purge") {
      Keys("admin_purge request", step["request"], {"url"});
    } else {
      Keys("request", step["request"], EngineRowRunnerRequestKeys());
    }
    if (kind == "unsafe") {
      Keys("unsafe response", step["response"],
           {"status", "location", "content_location"});
    } else {
      Keys("response", step["response"],
           {"status", "origin", "store_delta", "final_delta", "module",
            "body_ref", "aborted", "split_at"});
    }
    Ops("pre", step["pre"]);
    for (const Json::Value& d : step["during"]) {
      Keys("during", d, {"point", "do"});
      Ops("during do", d["do"]);
    }
    const Json::Value& ex = step["expect"];
    Keys("expect", ex,
         {"calls",
          "calls_exclude",
          "port_calls",
          "port_calls_exclude",
          "stored",
          "removed",
          "notify",
          "counters",
          "serve",
          "log_lines",
          "admin_response",
          "purge_set_added",
          "volume_epoch_s",
          "budget_in_use",
          "headers_absent",
          "classify_calls",
          "decision_lines",
          "recording_started",
          "config_error",
          "fence_before_remove",
          "removed_count",
          "fence_map_size_max",
          "fences_kept",
          "oldest_fenced",
          "newest_fenced",
          "agent_class",
          "classify_agent_calls",
          "agent_vectors"});
    Keys("serve", ex["serve"],
         {"kind", "status", "reason", "headers_exact", "body_bytes", "body_ref",
          "header_order", "suppress_recording"});
    Value("serve kind", ex["serve"]["kind"],
          {"fall_through", "final", "as_origin"});
    Keys("decision_lines", ex["decision_lines"], {"contains", "absent"});
    if (ex["config_error"].isObject()) {
      Keys("config_error", ex["config_error"], {"contains"});
    }
    for (const Json::Value& call : ex["calls"]) {
      Keys("call", call, {"fn", "args", "exact"});
      const GoogleString fn = call["fn"].asString();
      auto it = RecordedArgs().find(fn);
      if (it == RecordedArgs().end()) {
        Fail(StrCat("calls: ", fn, " is never recorded"));
        continue;
      }
      const Json::Value& args = call["args"];
      for (const GoogleString& key :
           args.isObject() ? args.getMemberNames() : Json::Value::Members()) {
        if (it->second.count(key) == 0) {
          Fail(StrCat("calls: ", fn, " never records '", key, "'"));
        }
      }
    }
    for (const Json::Value& fn : ex["calls_exclude"]) {
      if (RecordedArgs().count(fn.asString()) == 0) {
        Fail(StrCat("calls_exclude: ", fn.asString(), " is never recorded"));
      }
    }
    for (const char* key : {"port_calls", "port_calls_exclude"}) {
      for (const Json::Value& name : ex[key]) {
        if (std::find(std::begin(kPortCalls), std::end(kPortCalls),
                      name.asString()) == std::end(kPortCalls)) {
          Fail(StrCat(key, ": the port has no ", name.asString()));
        }
      }
    }
    const Json::Value& counters = ex["counters"];
    for (const GoogleString& name : counters.isObject()
                                        ? counters.getMemberNames()
                                        : Json::Value::Members()) {
      if (!IsRegisteredCounter(name) && !IsServePathCounter(name)) {
        Fail(StrCat("counters: ", name,
                    " is neither registered nor a counter the serve path "
                    "adds (kServePathCounters)"));
      }
    }
    for (const Json::Value& stored : ex["stored"]) {
      Keys("stored", stored,
           {"kind", "url", "lifetime_s", "stored_at_s", "cc_flags", "store_v2",
            "anonymous_recorded"});
    }
    for (const Json::Value& removed : ex["removed"]) {
      if (!removed.isString()) Keys("removed", removed, {"url"});
    }
    for (const Json::Value& notify : ex["notify"]) {
      Keys("notify", notify, {"content", "agent_request", "flags"});
    }
    const Json::Value& exact = ex["serve"]["headers_exact"];
    for (const GoogleString& name :
         exact.isObject() ? exact.getMemberNames() : Json::Value::Members()) {
      Keys(StrCat("headers_exact ", name), exact[name], {"count", "value"});
    }
  }

  // A counter the engine registers today.
  static bool IsRegisteredCounter(const GoogleString& name) {
    for (const char* const* c = RewriteStats::kDaemonHtmlEngineCounters;
         *c != nullptr; ++c) {
      if (name == *c) return true;
    }
    for (const char* const* r = RewriteStats::kDaemonMarkdownFallthroughReasons;
         *r != nullptr; ++r) {
      if (name == StrCat("daemon_markdown_fallthrough_", *r)) return true;
    }
    for (const char* const* r = RewriteStats::kDaemonHtmlFallthroughReasons;
         *r != nullptr; ++r) {
      if (name == StrCat("daemon_html_fallthrough_", *r)) return true;
    }
    for (const char* const* r = RewriteStats::kDaemonHtmlRefusedReasons;
         *r != nullptr; ++r) {
      if (name == StrCat("daemon_html_refused_", *r)) return true;
    }
    return false;
  }

  // The request keys EngineRowRunner reads (defined after it).
  static const Set& EngineRowRunnerRequestKeys();

  std::vector<GoogleString> errors_;
};

const RunnerShape::Set& RunnerShape::EngineRowRunnerRequestKeys() {
  return EngineRowRunner::RequestKeys();
}

class EngineRowTest : public testing::Test {
 public:
  EngineRowTest(Json::ArrayIndex index, const StubSetup* stub)
      : index_(index), stub_(stub) {}
  void TestBody() override {
    const Json::Value& row = Cases()["rows"][index_];
    for (const GoogleString& e : RunnerShape().Errors(row)) ADD_FAILURE() << e;
    EngineRowRunner runner(row, stub_);
    if (!runner.ready()) return;  // The failure is already recorded.
    runner.Run();
  }

 private:
  Json::ArrayIndex index_;
  const StubSetup* stub_;
};

#ifndef DAEMON_HTML_CONFORMANCE_ON_STUB

// Set (to anything) to also register every pending row as its own test, in
// DaemonHtmlEnginePendingRows, to work on the support it waits for.  Unset in
// every lane, where pending rows run only in the check below.
const char kRunPendingRowsEnv[] = "DAEMON_HTML_RUN_PENDING_ROWS";

// Registers one test per engine row before main runs them.
int RegisterRows() {
  const Json::Value& rows = Cases()["rows"];
  const bool pending_too = getenv(kRunPendingRowsEnv) != nullptr;
  int registered = 0;
  for (Json::ArrayIndex i = 0; i < rows.size(); ++i) {
    const bool pending = IsPendingEngineRow(rows[i]);
    if (!IsEngineRow(rows[i]) && !(pending && pending_too)) continue;
    testing::RegisterTest(
        pending ? "DaemonHtmlEnginePendingRows" : "DaemonHtmlEngineRows",
        TestName(rows[i]).c_str(), nullptr, nullptr, __FILE__, __LINE__,
        [i]() -> testing::Test* { return new EngineRowTest(i, nullptr); });
    if (!pending) ++registered;
  }
  return registered;
}

const int kRegisteredRows = RegisterRows();

// Every pending row is read in full and run to its end, and still fails for
// an allowed reason only: (a) the engine's behaviour (an expectation), or
// (b) an entry point of kServePathEntryPoints not declared yet, or a counter
// of kServePathCounters not registered yet.  A runner error (a key, name or
// value the runner cannot run, kRunnerError) fails this check, and so does a
// pending row that already passes: its marker must go, so a failing row
// cannot hide behind one.  The failures themselves are the support the row
// waits for and are not reported here; run the row on its own
// (kRunPendingRowsEnv) to see them.
TEST(DaemonHtmlEngineRowsMeta, EveryPendingRowRunsAndStillFails) {
  int pending = 0;
  int behaviour_rows = 0;    // at least one behaviour failure
  int declaration_rows = 0;  // only entry point or counter failures
  std::map<GoogleString, int> waiting_on;  // rows per entry point or counter
  for (const Json::Value& row : Cases()["rows"]) {
    if (!IsPendingEngineRow(row)) continue;
    ++pending;
    const GoogleString id = row["id"].asString();
    SCOPED_TRACE(id);
    const std::vector<GoogleString> shape = RunnerShape().Errors(row);
    for (const GoogleString& e : shape) ADD_FAILURE() << e;
    if (!shape.empty()) continue;
    testing::TestPartResultArray results;
    GoogleString thrown;
    {
      testing::ScopedFakeTestPartResultReporter reporter(
          testing::ScopedFakeTestPartResultReporter::
              INTERCEPT_ONLY_CURRENT_THREAD,
          &results);
      try {
        EngineRowRunner runner(row);
        if (runner.ready()) runner.Run();
      } catch (const std::exception& e) {
        thrown = e.what();
      }
    }
    if (!thrown.empty()) {
      // A value of the wrong type: the runner cannot read the row.
      ADD_FAILURE() << id << ": " << thrown;
      continue;
    }
    bool behaviour = false;
    bool declaration = false;
    std::set<GoogleString> names;
    for (int i = 0; i < results.size(); ++i) {
      const GoogleString message = results.GetTestPartResult(i).message();
      size_t at = message.find(kRunnerError);
      if (at != GoogleString::npos) {
        ADD_FAILURE() << id << ": " << message.substr(at);
        continue;
      }
      bool declared_later = false;
      for (const char* prefix : {kEntryPointAbsent, kCounterAbsent}) {
        at = message.find(prefix);
        if (at == GoogleString::npos) continue;
        declared_later = true;
        GoogleString name = message.substr(at + strlen(prefix));
        name = name.substr(0, name.find_first_of(" \n"));
        names.insert(name);
      }
      if (declared_later) {
        declaration = true;
      } else {
        behaviour = true;
      }
    }
    for (const GoogleString& name : names) ++waiting_on[name];
    if (behaviour) {
      ++behaviour_rows;
    } else if (declaration) {
      ++declaration_rows;
    } else {
      ADD_FAILURE() << id << " passes: remove its pending marker";
    }
  }
  EXPECT_EQ(pending, behaviour_rows + declaration_rows);
  std::printf(
      "pending engine rows: %d run, %d still fail (%d on behaviour, %d only "
      "on an entry point or counter not added yet)\n",
      pending, behaviour_rows + declaration_rows, behaviour_rows,
      declaration_rows);
  for (const auto& [name, rows] : waiting_on) {
    std::printf("  waiting on %s: %d rows\n", name.c_str(), rows);
  }
}

// The entry points the engine does not declare yet, pinned: declaring one
// changes this set in the same change, and a declaration whose signature the
// runner cannot call fails here instead of reading as "not declared yet".
TEST(DaemonHtmlEngineRowsMeta, ServePathEntryPointsStillUndeclared) {
  const std::set<GoogleString> kExpectedUndeclared = {
      "served_from_cache", "internal", "OnUnsafeRequest"};
  std::set<GoogleString> undeclared;
  for (const auto& [name, state] :
       EntryPointStates<DaemonHtmlRequestFacts, DaemonHtmlEngine>()) {
    EXPECT_NE(nullptr, EntryPointSignature(name)) << name;
    EXPECT_FALSE(state.named && !state.callable)
        << name << " is declared with a signature the runner does not call; "
        << "expected: " << EntryPointSignature(name);
    if (!state.named) undeclared.insert(name);
  }
  EXPECT_EQ(kExpectedUndeclared, undeclared);
  EXPECT_EQ(
      std::size(kServePathEntryPoints),
      (EntryPointStates<DaemonHtmlRequestFacts, DaemonHtmlEngine>().size()));
}

// The serve path's entry points are called once declared, and reported
// missing until then.
TEST(DaemonHtmlEngineRowsMeta, ServePathEntryPointsAreUsedOnceDeclared) {
  struct Declared {
    bool served_from_cache = false;
    DaemonHtmlHeaderLines request_headers;
    GoogleString raw_path;
    std::vector<GoogleString> cookie_lines;
    bool internal = false;
  };
  struct Undeclared {};
  struct Engine {
    struct Config {
      uint64_t policy_stamp = 0;
    };
    void OnUnsafeRequest(const DaemonHtmlRequestFacts&, int status,
                         const std::vector<GoogleString>&,
                         const std::vector<GoogleString>&, DaemonHtmlPort*) {
      unsafe_status = status;
    }
    static bool ApplyPolicySettings(const DaemonAbi&, Config* config,
                                    GoogleString*) {
      config->policy_stamp = 7;
      return true;
    }
    void set_credential_skip_disabled_for_testing(bool v) { skip_off = v; }
    int unsafe_status = 0;
    bool skip_off = false;
  };
  struct OldEngine {
    struct Config {};
  };
  Declared d;
  EXPECT_TRUE(SetServedFromCache(&d, true));
  EXPECT_TRUE(d.served_from_cache);
  EXPECT_TRUE(SetRequestHeaders(&d, {{"X-A", "1"}}));
  EXPECT_EQ(1u, d.request_headers.size());
  EXPECT_TRUE(SetRawPath(&d, "/a;b", true));
  EXPECT_EQ("/a;b", d.raw_path);
  EXPECT_TRUE(SetCookieLines(&d, {"a=b"}));
  EXPECT_EQ(1u, d.cookie_lines.size());
  EXPECT_TRUE(SetInternal(&d, true));
  EXPECT_TRUE(d.internal);
  EXPECT_FALSE(
      (EntryPointStates<Undeclared, OldEngine>().at("internal").named));
  EXPECT_TRUE((EntryPointStates<Declared, Engine>().at("internal").callable));
  // Declared by name with another signature: named, not callable.
  struct Drifted {
    int cookie_lines = 0;
  };
  EXPECT_TRUE(
      (EntryPointStates<Drifted, OldEngine>().at("cookie_lines").named));
  EXPECT_FALSE(
      (EntryPointStates<Drifted, OldEngine>().at("cookie_lines").callable));
  Undeclared u;
  EXPECT_FALSE(SetServedFromCache(&u, true));
  EXPECT_TRUE(SetServedFromCache(&u, false));  // nothing to tell
  EXPECT_FALSE(SetRequestHeaders(&u, {}));
  EXPECT_FALSE(SetRawPath(&u, "/a", true));
  EXPECT_TRUE(SetRawPath(&u, "", false));
  EXPECT_FALSE(SetCookieLines(&u, {"a=b"}));

  FakeDaemonVolume volume;
  Engine e;
  EXPECT_TRUE(
      CallOnUnsafeRequest(&e, DaemonHtmlRequestFacts(), 303, {}, {}, nullptr));
  EXPECT_EQ(303, e.unsafe_status);
  Engine::Config config;
  GoogleString error;
  EXPECT_EQ(1, CallApplyPolicySettings<Engine>(volume, &config, &error));
  EXPECT_EQ(7u, config.policy_stamp);
  EXPECT_TRUE(DisableCredentialSkip(&e));
  EXPECT_TRUE(e.skip_off);
  OldEngine old;
  EXPECT_FALSE(CallOnUnsafeRequest(&old, DaemonHtmlRequestFacts(), 303, {}, {},
                                   nullptr));
  OldEngine::Config old_config;
  EXPECT_EQ(-1,
            CallApplyPolicySettings<OldEngine>(volume, &old_config, &error));
  EXPECT_FALSE(DisableCredentialSkip(&old));
}

// Every engine row runs: none is skipped, and the file has them.
TEST(DaemonHtmlEngineRowsMeta, EveryEngineRowIsRegistered) {
  int engine_rows = 0;
  int pending_rows = 0;
  for (const Json::Value& row : Cases()["rows"]) {
    if (IsEngineRow(row)) ++engine_rows;
    if (IsPendingEngineRow(row)) ++pending_rows;
  }
  EXPECT_GT(engine_rows, 0);
  EXPECT_EQ(engine_rows, kRegisteredRows);
  // Pinned, so a row cannot leave the run unnoticed: marking a row pending,
  // or adding one, changes these in the same change.
  EXPECT_EQ(326, kRegisteredRows);
  EXPECT_EQ(151, pending_rows);
  std::printf("engine rows: %d registered, %d pending\n", kRegisteredRows,
              pending_rows);
}

#else  // DAEMON_HTML_CONFORMANCE_ON_STUB

// The degrade and fault rows, rerun on the stub library: a real binding,
// loaded through LoadDaemonAbi, answering as each row's library and
// verdicts describe.  Only the expectations that do not read the in-memory
// volume apply here; the same rows also run on the fake.
const std::map<GoogleString, StubSetup>& StubRows() {
  static const auto* rows = new std::map<GoogleString, StubSetup>{
      {"degrade-library-minor-12",
       {"libdaemon_stub_html_serve.so", {{"PS_STUB_MINOR", "12"}}}},
      {"degrade-symbol-missing",
       {"libdaemon_stub_html_serve_partial.so", {{"PS_STUB_MINOR", "14"}}}},
      {"degrade-worker-mode-full",
       {"libdaemon_stub_html_serve.so",
        {{"PS_STUB_MINOR", "14"}, {"PS_STUB_HTML_MODE", "3"}}}},
      {"degrade-worker-mode-off",
       {"libdaemon_stub_html_serve.so",
        {{"PS_STUB_MINOR", "14"}, {"PS_STUB_HTML_MODE", "0"}}}},
      {"degrade-worker-agent-optimize-off",
       {"libdaemon_stub_html_serve.so",
        {{"PS_STUB_MINOR", "14"}, {"PS_STUB_AGENT_ENTITLED", "0"}}}},
      // Both views store (action 1); the header block write fails (PS_ERR_IO).
      {"store-sidecar-write-failure-stores-nothing",
       {"libdaemon_stub_html_serve.so",
        {{"PS_STUB_MINOR", "14"},
         {"PS_STUB_HTML_ACTION", "1"},
         {"PS_STUB_SIDECAR_WRITE_STATUS", "2"}}}},
      {"epoch-write-failure-fails-admin-purge",
       {"libdaemon_stub_html_serve.so",
        {{"PS_STUB_MINOR", "14"}, {"PS_STUB_EPOCH_WRITE_STATUS", "2"}}}},
      {"url-purge-remove-failure-fails-response",
       {"libdaemon_stub_html_serve.so",
        {{"PS_STUB_MINOR", "14"}, {"PS_STUB_REMOVE_STATUS", "2"}}}},
  };
  return *rows;
}

int RegisterStubRows() {
  const Json::Value& rows = Cases()["rows"];
  int registered = 0;
  for (Json::ArrayIndex i = 0; i < rows.size(); ++i) {
    auto it = StubRows().find(rows[i]["id"].asString());
    if (it == StubRows().end() || !IsEngineRow(rows[i])) continue;
    const StubSetup* stub = &it->second;
    testing::RegisterTest(
        "DaemonHtmlEngineStubRows", TestName(rows[i]).c_str(), nullptr, nullptr,
        __FILE__, __LINE__,
        [i, stub]() -> testing::Test* { return new EngineRowTest(i, stub); });
    ++registered;
  }
  return registered;
}

const int kRegisteredStubRows = RegisterStubRows();

// Every stub setup names an engine row of the file, and every degrade-* row
// has one: a new degrade row cannot miss the stub run.  (The two
// worker-*-within-a-tick rows change the worker's state between steps over a
// stored page, which needs the fake's volume and sequenced answers; they run
// on the fake only.)
TEST(DaemonHtmlEngineStubRowsMeta, EveryDegradeRowRunsOnTheStub) {
  std::set<GoogleString> degrade;
  for (const Json::Value& row : Cases()["rows"]) {
    if (IsEngineRow(row) && HasPrefixString(row["id"].asString(), "degrade-")) {
      degrade.insert(row["id"].asString());
    }
  }
  EXPECT_FALSE(degrade.empty());
  for (const GoogleString& id : degrade) {
    EXPECT_EQ(1u, StubRows().count(id)) << id << " has no stub setup";
  }
  EXPECT_EQ(static_cast<int>(StubRows().size()), kRegisteredStubRows)
      << "a stub setup names no engine row";
}

#endif  // DAEMON_HTML_CONFORMANCE_ON_STUB

}  // namespace
}  // namespace net_instaweb
