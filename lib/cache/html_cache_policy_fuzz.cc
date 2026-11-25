// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Fuzz harness for the HTML cache policy (lib/cache/html_cache_policy.h) and
// its decision line formatter (lib/cache/html_cache_decision.h).  Both run on
// untrusted input: response headers and a body prefix an origin chose, and a
// path a client chose.  Build optimized with a sanitizer so release behaviour
// is what gets exercised and AddressSanitizer is the memory-safety oracle:
//
//     bazel build -c opt --config=asan //lib/cache:html_cache_policy_fuzz
//
// One source, two build modes (mirrors lib/css/css_minify_fuzz.cc):
//   * default                       -> deterministic main() that replays the
//                                      embedded seeds and then every file
//                                      named on the command line.  Hermetic;
//                                      CI-friendly.  `--dump_corpus=DIR`
//                                      instead writes the embedded seed set
//                                      as seed-NN files into DIR (created if
//                                      missing) and exits.
//   * -DHTML_CACHE_POLICY_LIBFUZZER -> exposes LLVMFuzzerTestOneInput for
//                                      coverage-guided discovery when linked
//                                      with -fsanitize=fuzzer.
// A third mode, -DHTML_CACHE_POLICY_FUZZ_NO_MAIN, emits neither entry point:
// it is how the seed-replay test links the same oracle without libFuzzer.
//
// INPUT FORMAT.  One blob drives all the parsers.  It is split at the first
// blank line (`\n\n`) into a header block and a body prefix.  The header block
// is lines of `name: value` (a trailing CR is dropped; a line without a colon
// or with an empty name is dropped).  A line whose name starts with `:` is a
// harness control line, never a response header: `:method`, `:path`,
// `:status`, `:authorization`, `:authenticated`, `:cookie`, `:no-store`,
// `:req-max-age`, `:req-min-fresh`, `:incomplete`, `:body-bytes` and `:now`
// set the request, status, body facts and receipt time that a header block
// cannot carry.  `:req-accept`, `:req-cache-control`, `:req-range`,
// `:req-origin` and `:serve-method` set the request the serve verdict sees.
//
// WHAT RUNS.  (1) ClassifyHtmlForStore over the parsed request, headers and
// body prefix.  (2) FormatHtmlCacheDecisionLine over the header lines taken
// as key/value pairs (unknown keys are expected to be refused, not to crash)
// and then over a canonical field set built from the store verdict the way a
// front end would build it.  (3) ParseHttpDateSeconds over every header
// value.  (4) ClassifyHtmlForServe, in every mode, over facts derived from
// the store verdict.  (5) ClassifyHtmlForServe over the blob's serve request
// (Accept, Cache-Control, Range, Origin, method) against a live copy, a
// no-transform parent and a withheld copy, and a serve-side decision line
// built from each verdict.
//
// ORACLES.  Crash-free, with the sanitizer as the memory-safety backstop, and
// these invariants, each of which aborts on violation:
//   * a verdict that stores has an explicit lifetime or a validator, no
//     private/no-store flag, and only Accept-Encoding in its Vary tokens;
//   * THE DIFFERENTIAL ORACLE: whatever this policy stores, the gates that
//     already guard the shared cache accept too (VaryUncacheable is false,
//     ClassifyForHeadersSidecar is not a fall-through, no Set-Cookie, no
//     no-store or private directive).  The policy may be stricter than the
//     existing gates, never looser;
//   * a verdict's reason is in the closed set, its detail is a bounded token,
//     and the reason code built from them always fits the decision line's
//     `reason` grammar (as does the sidecar code built from the detail);
//   * no formatted line contains a line break, and a canonical field set
//     always formats (a fact the grammar cannot carry is left out, never
//     bent to fit);
//   * a stale entry is never served, in any mode, an entry that cannot meet
//     the request's min-fresh is never served, and report-only never
//     enforces, never returns a storing action and never returns a serve
//     action;
//   * a request with Range, If-Range or Origin, or a method other than GET
//     or HEAD, is never served; Markdown is never served to a request that
//     did not prefer it, for a no-transform parent or for a withheld copy;
//   * a serve-side decision line built from a serve verdict always formats
//     and the C API's gate admits it.

#include "lib/cache/html_cache_policy_fuzz.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "lib/cache/headers_sidecar.h"
#include "lib/cache/html_cache_decision.h"
#include "lib/cache/html_cache_policy.h"
#include "lib/cache/origin_cache_control.h"
#include "lib/cache/vary_storability.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"

namespace pagespeed::fuzz {
namespace {

// ASCII case-insensitive header-name match.
bool NameIs(std::string_view name, std::string_view lower) {
  if (name.size() != lower.size()) return false;
  for (size_t i = 0; i < name.size(); ++i) {
    const auto u = static_cast<unsigned char>(name[i]);
    const char c =
        (u >= 'A' && u <= 'Z') ? static_cast<char>(u | 0x20) : name[i];
    if (c != lower[i]) return false;
  }
  return true;
}

[[noreturn]] void Violation(const char* what) {
  std::fprintf(stderr, "html cache policy fuzz oracle violation: %s\n", what);
  std::abort();
}

struct ParsedBlob {
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::string method = "GET";
  std::string path = "/index.html";
  int status = 200;
  bool authorization = false;
  bool authenticated = false;
  bool cookie = false;
  bool no_store = false;
  int64_t req_max_age = -1;
  int64_t req_min_fresh = -1;
  bool incomplete = false;
  uint64_t body_bytes = 0;
  bool body_bytes_given = false;
  uint32_t now = 784111777;
  // The serve request.
  std::optional<std::string> req_accept;
  std::optional<std::string> req_cache_control;
  bool req_range = false;
  bool req_origin = false;
  std::string serve_method = "GET";
};

uint64_t ParseUnsigned(std::string_view v) {
  uint64_t out = 0;
  for (char c : v) {
    if (c < '0' || c > '9') break;
    if (out > (UINT64_MAX - 9) / 10) return UINT64_MAX;
    out = out * 10 + static_cast<uint64_t>(c - '0');
  }
  return out;
}

ParsedBlob ParseBlob(std::string_view blob) {
  ParsedBlob parsed;
  std::string_view head = blob;
  const size_t split = blob.find("\n\n");
  if (split != std::string_view::npos) {
    head = blob.substr(0, split);
    parsed.body = std::string(blob.substr(split + 2));
  }
  while (!head.empty()) {
    const size_t eol = head.find('\n');
    std::string_view line = head.substr(0, eol);
    head = eol == std::string_view::npos ? std::string_view()
                                         : head.substr(eol + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    const bool control = !line.empty() && line.front() == ':';
    const size_t colon = line.find(':', control ? 1 : 0);
    if (colon == std::string_view::npos || colon == 0 ||
        (control && colon == 1)) {
      continue;
    }
    std::string_view name = line.substr(0, colon);
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.remove_prefix(1);
    }
    if (!control) {
      parsed.headers.emplace_back(std::string(name), std::string(value));
    } else if (name == ":method") {
      parsed.method = std::string(value);
    } else if (name == ":path") {
      parsed.path = std::string(value);
    } else if (name == ":status") {
      parsed.status = static_cast<int>(ParseUnsigned(value) & 0xFFFF);
    } else if (name == ":authorization") {
      parsed.authorization = true;
    } else if (name == ":authenticated") {
      parsed.authenticated = true;
    } else if (name == ":cookie") {
      parsed.cookie = true;
    } else if (name == ":no-store") {
      parsed.no_store = true;
    } else if (name == ":req-max-age") {
      parsed.req_max_age = static_cast<int64_t>(ParseUnsigned(value));
    } else if (name == ":req-min-fresh") {
      parsed.req_min_fresh = static_cast<int64_t>(ParseUnsigned(value));
    } else if (name == ":incomplete") {
      parsed.incomplete = true;
    } else if (name == ":body-bytes") {
      parsed.body_bytes = ParseUnsigned(value);
      parsed.body_bytes_given = true;
    } else if (name == ":now") {
      parsed.now = static_cast<uint32_t>(ParseUnsigned(value));
    } else if (name == ":req-accept") {
      parsed.req_accept = std::string(value);
    } else if (name == ":req-cache-control") {
      parsed.req_cache_control = std::string(value);
    } else if (name == ":req-range") {
      parsed.req_range = true;
    } else if (name == ":req-origin") {
      parsed.req_origin = true;
    } else if (name == ":serve-method") {
      parsed.serve_method = std::string(value);
    }
  }
  return parsed;
}

std::vector<HeaderField> Fields(const ParsedBlob& parsed) {
  std::vector<HeaderField> fields;
  fields.reserve(parsed.headers.size());
  for (const auto& [name, value] : parsed.headers) {
    fields.push_back({name, value});
  }
  return fields;
}

HtmlStoreVerdict Classify(const ParsedBlob& parsed) {
  const std::vector<HeaderField> fields = Fields(parsed);
  HtmlStoreRequest request;
  request.method = parsed.method;
  request.url_path = parsed.path;
  request.has_authorization = parsed.authorization;
  request.authenticated_user = parsed.authenticated;
  request.has_cookie = parsed.cookie;
  request.request_no_store = parsed.no_store;
  HtmlStoreResponse response;
  response.status = parsed.status;
  response.headers = fields;
  response.body_bytes =
      parsed.body_bytes_given ? parsed.body_bytes : parsed.body.size();
  response.body_complete = !parsed.incomplete;
  response.body_prefix = parsed.body;
  HtmlStorePolicy policy;
  policy.now_seconds = parsed.now;
  policy.mode = HtmlCacheMode::kReportOnly;
  if (ClassifyHtmlForStore(request, response, policy).stores()) {
    Violation("report-only returned a storing action");
  }
  policy.mode = HtmlCacheMode::kOff;
  if (ClassifyHtmlForStore(request, response, policy).action !=
      HtmlStoreAction::kRefuse) {
    Violation("off did not refuse");
  }
  policy.mode = HtmlCacheMode::kServeVariants;
  return ClassifyHtmlForStore(request, response, policy);
}

void CheckStoreVerdict(const ParsedBlob& parsed, const HtmlStoreVerdict& v) {
  if (static_cast<uint8_t>(v.reason) >= kHtmlStoreReasonCount) {
    Violation("store reason out of range");
  }
  if (v.detail.size() > kMaxHtmlStoreDetailBytes) Violation("detail too long");
  if (!v.detail.empty() && !IsHttpToken(v.detail)) {
    Violation("detail is not a token");
  }
  // What a front end passes straight into the formatter must always fit.
  if (!HtmlCacheDecisionValueFits("reason", v.reason_code())) {
    Violation("reason code outside the decision line grammar");
  }
  if (v.reason == HtmlStoreReason::kHeaders && !v.detail.empty() &&
      !HtmlCacheDecisionValueFits("sidecar", "fall_through:" + v.detail)) {
    Violation("sidecar code outside the decision line grammar");
  }
  if (v.stores()) {
    if (!v.lifetime_explicit && !v.has_validators) {
      Violation("stored with neither a lifetime nor a validator");
    }
    if (v.needs_validation && !v.has_validators) {
      Violation("stored a must-revalidate response with no validator");
    }
    if ((v.cc_flags & (AlternateMetadata::kCCOriginPrivate |
                       AlternateMetadata::kCCOriginNoStore)) != 0) {
      Violation("stored a private or no-store response");
    }
    for (const std::string& token : v.vary_tokens) {
      if (token != "accept-encoding") Violation("stored a varying response");
    }
    if (!v.sidecar.swap_eligible()) Violation("stored a non-reproducible set");

    // The differential oracle: the existing gates, asked independently over
    // the same headers, must accept everything this policy stores.
    std::string combined_vary;
    bool sets_cookie = false;
    OriginCacheControl cc;
    for (const auto& [name, value] : parsed.headers) {
      if (NameIs(name, "vary")) {
        if (!combined_vary.empty()) combined_vary.push_back(',');
        combined_vary.append(value);
      } else if (NameIs(name, "set-cookie") || NameIs(name, "set-cookie2")) {
        sets_cookie = true;
      } else if (NameIs(name, "cache-control")) {
        AccumulateOriginCacheControl(value, &cc);
      }
    }
    if (VaryUncacheable(combined_vary)) {
      Violation("stored a response the Vary gate refuses");
    }
    if (ClassifyForHeadersSidecar(Fields(parsed)).verdict !=
        SidecarVerdict::kSwapEligible) {
      Violation("stored a response the sidecar gate refuses");
    }
    if (sets_cookie) Violation("stored a response that sets a cookie");
    if ((cc.cc_flags & (AlternateMetadata::kCCOriginPrivate |
                        AlternateMetadata::kCCOriginNoStore)) != 0) {
      Violation("stored a response the directive gate refuses");
    }
  }
}

// A stored-entry view of a store verdict, aged past its lifetime.
void CheckServe(const ParsedBlob& parsed, const HtmlStoreVerdict& v) {
  StoredHtmlFacts facts;
  facts.present = true;
  facts.stored_at = 1000;
  facts.lifetime_s = v.lifetime_s;
  facts.needs_validation = v.needs_validation;
  facts.explicit_shared = v.explicit_shared;
  facts.variant_present = true;
  facts.binding_live = true;
  for (HtmlCacheMode mode :
       {HtmlCacheMode::kOff, HtmlCacheMode::kReportOnly,
        HtmlCacheMode::kServeVariants, HtmlCacheMode::kServeCachedHtml}) {
    for (bool markdown : {false, true}) {
      HtmlServeRequest request;
      request.mode = mode;
      request.wants_markdown = markdown;
      // One second past expiry (or simply later, for lifetime 0).  Computed
      // in 64 bits; a lifetime that saturates has no representable "later",
      // so the staleness check is skipped for it.
      const uint64_t later =
          uint64_t{facts.stored_at} + uint64_t{v.lifetime_s} + 1;
      if (later <= UINT32_MAX) {
        request.now_seconds = static_cast<uint32_t>(later);
        const HtmlServeVerdict stale = ClassifyHtmlForServe(request, facts);
        if (stale.action == HtmlServeAction::kServeHtml ||
            stale.action == HtmlServeAction::kServeMarkdown) {
          Violation("served a stale entry");
        }
        if ((mode == HtmlCacheMode::kOff ||
             mode == HtmlCacheMode::kReportOnly) &&
            stale.enforce) {
          Violation("a non-enforcing mode enforced");
        }
      }
      // A request that wants more freshness than the entry has at all is
      // never served from it, whatever the other request values are (RFC
      // 9111 section 5.2.1.3: age + min_fresh must not exceed the lifetime).
      request.now_seconds = facts.stored_at;
      request.max_age = parsed.req_max_age;
      request.min_fresh = parsed.req_min_fresh;
      if (parsed.req_min_fresh > static_cast<int64_t>(v.lifetime_s)) {
        const HtmlServeVerdict wanted = ClassifyHtmlForServe(request, facts);
        if (wanted.action == HtmlServeAction::kServeHtml ||
            wanted.action == HtmlServeAction::kServeMarkdown) {
          Violation("served an entry that cannot meet min-fresh");
        }
      }
      const HtmlServeVerdict fresh = ClassifyHtmlForServe(request, facts);
      if (mode == HtmlCacheMode::kReportOnly &&
          fresh.action != HtmlServeAction::kFallThrough) {
        Violation("report-only returned a serve action");
      }
      if (static_cast<uint8_t>(fresh.reason) >= kHtmlServeReasonCount) {
        Violation("serve reason out of range");
      }
      if ((mode == HtmlCacheMode::kOff || mode == HtmlCacheMode::kReportOnly) &&
          fresh.enforce) {
        Violation("a non-enforcing mode enforced");
      }
    }
  }
}

void CheckLine(const std::optional<std::string>& line);

// The serve verdicts for the blob's own serve request: against a live copy,
// a no-transform parent and a withheld copy, in both serve modes.
std::vector<HtmlServeVerdict> ServeRequestVerdicts(const ParsedBlob& parsed) {
  HtmlServeRequest request;
  request.wants_markdown =
      parsed.req_accept.has_value() && WantsAgentMarkdown(*parsed.req_accept);
  request.method_cacheable =
      parsed.serve_method == "GET" || parsed.serve_method == "HEAD";
  request.has_range = parsed.req_range;
  request.has_origin = parsed.req_origin;
  request.has_cookie = parsed.cookie;
  request.has_authorization = parsed.authorization;
  if (parsed.req_cache_control.has_value()) {
    ApplyHtmlRequestCacheControl(*parsed.req_cache_control, &request);
  }
  request.now_seconds = parsed.now;
  StoredHtmlFacts live;
  live.present = true;
  live.stored_at = parsed.now;
  live.lifetime_s = 300;
  live.variant_present = true;
  live.binding_live = true;
  std::vector<HtmlServeVerdict> out;
  for (HtmlCacheMode mode :
       {HtmlCacheMode::kServeVariants, HtmlCacheMode::kServeCachedHtml}) {
    request.mode = mode;
    for (int shape = 0; shape < 3; ++shape) {
      StoredHtmlFacts facts = live;
      facts.no_transform = shape == 1;
      facts.withheld = shape == 2;
      const HtmlServeVerdict v = ClassifyHtmlForServe(request, facts);
      const bool served = v.action == HtmlServeAction::kServeHtml ||
                          v.action == HtmlServeAction::kServeMarkdown;
      if (served && (request.has_range || request.has_origin ||
                     !request.method_cacheable)) {
        Violation("served a request the cache never answers");
      }
      if (v.action == HtmlServeAction::kServeMarkdown &&
          (!request.wants_markdown || facts.no_transform || facts.withheld)) {
        Violation("served Markdown that may not be served");
      }
      if (static_cast<uint8_t>(v.reason) >= kHtmlServeReasonCount) {
        Violation("serve reason out of range");
      }
      out.push_back(v);
    }
  }
  return out;
}

// A serve-side line from each verdict, built the way a front end builds it.
void FormatServeLines(const ParsedBlob& parsed,
                      const std::vector<HtmlServeVerdict>& verdicts) {
  for (const HtmlServeVerdict& v : verdicts) {
    const std::string age = std::to_string(v.age_s);
    const std::vector<HtmlCacheDecisionField> fields = {
        {"ts_ms", "1700000000000"},
        {"host", "fuzz.example"},
        {"mode", HtmlCacheModeName(HtmlCacheMode::kServeVariants)},
        {"action", HtmlServeActionName(v.action)},
        {"reason", HtmlServeReasonName(v.reason)},
        {"request", parsed.req_accept.has_value() ? "accept_markdown" : "none"},
        {"variant", "markdown"},
        {"binding",
         v.reason == HtmlServeReason::kBindingStale ? "stale" : "live"},
        {"age_s", age},
    };
    if (!HtmlCacheDecisionCApiAdmits(fields)) {
      Violation("the C API gate refused a serve-side line");
    }
    const std::optional<std::string> line = FormatHtmlCacheDecisionLine(fields);
    if (!line.has_value()) Violation("a serve-side line was refused");
    CheckLine(line);
  }
}

void CheckLine(const std::optional<std::string>& line) {
  if (!line.has_value()) return;
  if (line->empty() || line->front() != '{' || line->back() != '}') {
    Violation("decision line is not an object");
  }
  if (line->find_first_of("\r\n") != std::string::npos) {
    Violation("decision line contains a line break");
  }
}

// The directive names a verdict's Cache-Control bits stand for, as the
// decision line's `cc` grammar takes them.
std::string CacheControlNames(uint16_t flags) {
  struct Bit {
    uint16_t bit;
    std::string_view name;
  };
  static constexpr Bit kBits[] = {
      {AlternateMetadata::kCCOriginImmutable, "immutable"},
      {AlternateMetadata::kCCOriginMustRevalidate, "must-revalidate"},
      {AlternateMetadata::kCCOriginNoCache, "no-cache"},
      {AlternateMetadata::kCCOriginNoStore, "no-store"},
      {AlternateMetadata::kCCOriginNoTransform, "no-transform"},
      {AlternateMetadata::kCCOriginPrivate, "private"},
      {AlternateMetadata::kCCOriginProxyRevalidate, "proxy-revalidate"},
      {AlternateMetadata::kCCOriginPublic, "public"},
      {AlternateMetadata::kCCOriginSMaxagePresent, "s-maxage"},
  };
  std::string out;
  for (const Bit& b : kBits) {
    if ((flags & b.bit) == 0) continue;
    if (!out.empty()) out.push_back(',');
    out.append(b.name);
  }
  return out;
}

void FormatOne(const ParsedBlob& parsed, const HtmlStoreVerdict& v) {
  // The header lines as key/value pairs: mostly unknown keys, and values
  // outside every grammar, which must be refused cleanly.
  std::vector<HtmlCacheDecisionField> raw;
  raw.reserve(parsed.headers.size());
  for (const auto& [name, value] : parsed.headers) raw.push_back({name, value});
  CheckLine(FormatHtmlCacheDecisionLine(raw));
  (void)HtmlCacheDecisionDedupKey(raw);

  // The canonical field set for this verdict, each value built the way a
  // front end builds it.  A fact the grammar cannot carry (a path with a
  // query, a raw Vary token that is not a token) is left out rather than
  // bent to fit: an absent key is how the line says "null".  What is given
  // must format.
  const std::string status = std::to_string(parsed.status);
  const std::string size = std::to_string(parsed.body_bytes);
  const std::string lifetime = std::to_string(v.lifetime_s);
  const std::string before = std::to_string(HtmlCacheFnv1a64(parsed.body));
  const std::string cc = CacheControlNames(v.cc_flags);
  const std::string reason = v.reason_code();
  std::string vary;
  bool vary_fits = !v.vary_tokens.empty();
  for (const std::string& token : v.vary_tokens) {
    if (!IsHttpToken(token)) {
      vary_fits = false;
      break;
    }
    if (!vary.empty()) vary.push_back(',');
    vary.append(token);
  }
  vary_fits = vary_fits && HtmlCacheDecisionValueFits("vary", vary);
  std::string validators;
  bool etag = false;
  bool last_modified = false;
  for (const auto& [name, value] : parsed.headers) {
    if (NameIs(name, "etag")) etag = true;
    if (NameIs(name, "last-modified")) last_modified = true;
  }
  if (etag) validators = "etag";
  if (last_modified)
    validators += validators.empty() ? "last_modified" : ",last_modified";
  if (validators.empty()) validators = "none";
  std::string request_facts;
  if (parsed.authorization) request_facts = "authorization";
  if (parsed.cookie)
    request_facts += request_facts.empty() ? "cookie" : ",cookie";
  if (request_facts.empty()) request_facts = "none";
  std::string sidecar;
  if (v.sidecar_evaluated) {
    switch (v.sidecar.verdict) {
      case SidecarVerdict::kSwapEligible:
        sidecar =
            v.sidecar.payload.empty() ? "eligible_empty" : "eligible_block";
        break;
      case SidecarVerdict::kFallThrough:
        sidecar = "fall_through";
        if (v.reason == HtmlStoreReason::kHeaders && !v.detail.empty()) {
          sidecar.push_back(':');
          sidecar.append(v.detail);
        }
        break;
      case SidecarVerdict::kNeverOptimized:
        sidecar = v.reason == HtmlStoreReason::kNonceLink ? "never:nonce_link"
                                                          : "never:nonce_csp";
        break;
    }
  }
  const std::string_view path = std::string_view(parsed.path).substr(0, 300);
  bool path_fits = !path.empty();
  for (char c : path) {
    const auto u = static_cast<unsigned char>(c);
    if (u <= 0x1F || u == 0x7F || c == '?' || c == '#' || c == '%' ||
        c == ';') {
      path_fits = false;
    }
  }

  std::vector<HtmlCacheDecisionField> canonical = {
      {"ts_ms", "1700000000000"},
      {"host", "fuzz.example"},
      {"mode", HtmlCacheModeName(HtmlCacheMode::kServeVariants)},
      {"action", HtmlStoreActionName(v.action)},
      {"reason", reason},
      {"status", status},
      {"size", size},
      {"lifetime_s", lifetime},
      {"validators", validators},
      {"request", request_facts},
      {"before", before},
  };
  if (path_fits) canonical.push_back({"path", path});
  if (!cc.empty()) canonical.push_back({"cc", cc});
  if (vary_fits) canonical.push_back({"vary", vary});
  if (!sidecar.empty()) canonical.push_back({"sidecar", sidecar});
  if (v.reason == HtmlStoreReason::kPersonalisationSignal) {
    canonical.push_back({"personalisation", v.detail});
  }
  for (const HtmlCacheDecisionField& field : canonical) {
    if (!HtmlCacheDecisionValueFits(field.key, field.value)) {
      Violation("a canonical decision value is outside its grammar");
    }
  }
  const std::optional<std::string> line =
      FormatHtmlCacheDecisionLine(canonical);
  if (!line.has_value()) Violation("canonical decision fields were refused");
  CheckLine(line);
  (void)HtmlCacheDecisionDedupKey(canonical);
}

std::string Blob(std::initializer_list<std::string_view> lines,
                 std::string_view body = "<html><body>ok</body></html>") {
  std::string out;
  for (std::string_view line : lines) {
    out.append(line);
    out.push_back('\n');
  }
  out.push_back('\n');
  out.append(body);
  return out;
}

std::vector<std::string> BuildSeeds() {
  constexpr std::string_view kType = "Content-Type: text/html; charset=utf-8";
  constexpr std::string_view kAge = "Cache-Control: max-age=300";
  std::vector<std::string> seeds;
  // Serve requests.
  seeds.push_back(Blob({":req-accept: text/markdown", kType, kAge}));
  seeds.push_back(
      Blob({":req-accept: text/markdown", ":req-range: 1", kType, kAge}));
  seeds.push_back(
      Blob({":req-accept: text/markdown", ":req-origin: 1", kType, kAge}));
  seeds.push_back(
      Blob({":req-accept: text/markdown", ":serve-method: POST", kType, kAge}));
  seeds.push_back(
      Blob({":req-accept: text/markdown;q=0.5, text/html",
            ":req-cache-control: max-age=abc, No-Cache", kType, kAge}));
  seeds.push_back(Blob({":req-accept: text/markdown",
                        ":req-cache-control: min-fresh=400", kType, kAge}));
  seeds.push_back(Blob({":req-accept: */*", ":serve-method: HEAD",
                        ":req-cache-control: min-fresh=99999999999999999999",
                        kType, kAge}));
  // A storable response.
  seeds.push_back(Blob({kType, kAge, "ETag: \"v1\""}));
  // Request-side facts.
  seeds.push_back(Blob({":no-store: 1", kType, kAge}));
  seeds.push_back(Blob({":req-min-fresh: 9223372036854775807",
                        ":req-max-age: 9223372036854775807", kType, kAge}));
  // Saturating and conflicting lifetimes.
  seeds.push_back(Blob({kType, "Cache-Control: max-age=99999999999"}));
  seeds.push_back(Blob({kType, "Cache-Control: max-age=0",
                        "Cache-Control: max-age=86400", "ETag: \"v1\""}));
  // One per refusal reason, in rule order.
  seeds.push_back(Blob({":method: HEAD", kType, kAge}));
  seeds.push_back(Blob({":status: 404", kType, kAge}));
  seeds.push_back(Blob({":incomplete: 1", kType, kAge}));
  seeds.push_back(Blob({kType, kAge, "Content-Range: bytes 0-9/100"}));
  seeds.push_back(Blob({":path: /search?q=1", kType, kAge}));
  seeds.push_back(
      Blob({":path: /img/a.jpg.pagespeed.ce.HASH.jpg", kType, kAge}));
  seeds.push_back(Blob({"Content-Type: application/xhtml+xml", kAge}));
  seeds.push_back(Blob({kType, kAge, "Content-Encoding: gzip"}));
  seeds.push_back(Blob({":authorization: 1", kType, kAge}));
  seeds.push_back(Blob({kType, kAge, "Set-Cookie: sid=1; HttpOnly"}));
  seeds.push_back(
      Blob({kType, "Cache-Control: private=\"Set-Cookie\", max-age=60"}));
  seeds.push_back(Blob({kType, "Cache-Control: no-store"}));
  seeds.push_back(Blob({":cookie: 1", kType, kAge}));
  seeds.push_back(Blob({kType, kAge, "Vary: *"}));
  seeds.push_back(Blob({kType, kAge, "Vary: Accept-Encoding, Cookie"}));
  seeds.push_back(Blob({kType, kAge, "Vary: \"x\""}));
  seeds.push_back(Blob({kType, kAge, "X-Request-Id: abc123"}));
  seeds.push_back(Blob(
      {kType, kAge, "Content-Security-Policy: script-src 'nonce-r4nd0m'"}));
  seeds.push_back(
      Blob({kType, kAge, "Link: </a.css>; rel=preload; nonce=r4nd0m"}));
  seeds.push_back(Blob({kType}));
  seeds.push_back(Blob({kType, "Cache-Control: no-cache"}));
  seeds.push_back(Blob({":body-bytes: 99999999999", kType, kAge}));
  // Lifetime sources.
  seeds.push_back(Blob({kType, "Date: Sun, 06 Nov 1994 08:49:37 GMT",
                        "Expires: Sun, 06 Nov 1994 08:50:37 GMT"}));
  seeds.push_back(Blob(
      {kType, "Expires: 0", "Last-Modified: Sun, 06 Nov 1994 08:49:37 GMT"}));
  seeds.push_back(
      Blob({kType, "Cache-Control: s-maxage=60, max-age=0", "ETag: W/\"x\""}));
  seeds.push_back(Blob({kType, "Date: Sunday, 06-Nov-94 08:49:37 GMT",
                        "Expires: Sun Nov  6 08:50:37 1994"}));
  seeds.push_back(Blob({kType, kAge, "Vary: ,", "Vary: Accept-Encoding , "}));
  seeds.push_back(Blob({kType,
                        "Cache-Control: max-age=60, "
                        "stale-while-revalidate=600, stale-if-error=86400"}));
  seeds.push_back(Blob({kType, "Cache-Control: max-age=60, no-transform"}));
  // One per personalisation signal.
  for (std::string_view body : {
           "<meta name=\"csrf-token\" content=\"x\">",
           "<meta name=\"csrf-param\" content=\"y\">",
           "<input type=\"hidden\" name=\"_token\" value=\"x\">",
           "<input type=\"hidden\" name=\"csrf_value\" value=\"x\">",
           "<input type=\"hidden\" name=\"authenticity_token\" value=\"x\">",
           "<input type=\"hidden\" name=\"__RequestVerificationToken\">",
           "<input name=\"csrfmiddlewaretoken\" type=\"hidden\" value=\"x\">",
           "<input name=\"_wpnonce\" type=\"hidden\" value=\"x\">",
           "<form method=\"post\" action=\"/account/login\">",
           "<FORM METHOD=POST ACTION=/shop/Checkout>",
       }) {
    seeds.push_back(Blob({kType, kAge}, body));
  }
  seeds.push_back(
      Blob({kType, kAge},
           "<input data-x=\"" + std::string(5000, 'a') + "\" type=hidden>"));
  // Hostile shapes.
  seeds.push_back(Blob({kType, kAge, "X-Big: " + std::string(70000, 'a')}));
  seeds.push_back(Blob({kType, kAge, std::string(300, 'N') + ": v"}));
  seeds.push_back(Blob({kType, kAge}, std::string(70000, '<')));
  seeds.push_back(
      std::string("Date: Sun, 06 Nov 1994 08:49:37 GMT\r\nExpires:\tMon, 07 "
                  "Nov 1994 08:49:37 GMT\r\n\r\n"));
  seeds.push_back(
      std::string("\x01\x02: \xff\xfe\n:status\n: x\n::\nhost: \n\n"));
  seeds.push_back(std::string());
  return seeds;
}

}  // namespace

HtmlStoreVerdict ClassifyHtmlCachePolicyFuzzBlob(std::string_view blob) {
  return Classify(ParseBlob(blob));
}

std::vector<HtmlServeVerdict> ServeHtmlCachePolicyFuzzBlob(
    std::string_view blob) {
  return ServeRequestVerdicts(ParseBlob(blob));
}

void HtmlCachePolicyFuzzOne(std::string_view blob) {
  const ParsedBlob parsed = ParseBlob(blob);
  const HtmlStoreVerdict verdict = Classify(parsed);
  CheckStoreVerdict(parsed, verdict);
  if (parsed.no_store && verdict.stores()) {
    Violation("stored a response to a no-store request");
  }
  CheckServe(parsed, verdict);
  FormatOne(parsed, verdict);
  FormatServeLines(parsed, ServeRequestVerdicts(parsed));
  for (const auto& [name, value] : parsed.headers) {
    (void)ParseHttpDateSeconds(value, parsed.now);
  }
  (void)FindPersonalisationSignal(parsed.body);
}

std::vector<std::pair<std::string, std::string>> HtmlCachePolicyFuzzBlobHeaders(
    std::string_view blob) {
  return ParseBlob(blob).headers;
}

const std::vector<std::string>& HtmlCachePolicyFuzzSeeds() {
  static const std::vector<std::string> seeds = BuildSeeds();
  return seeds;
}

int DumpHtmlCachePolicyFuzzCorpus(const std::string& dir) {
  // Create the directory ourselves: a corpus-seeding step should not depend
  // on the caller having made it.
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    std::fprintf(stderr, "dump_corpus: cannot create %s: %s\n", dir.c_str(),
                 ec.message().c_str());
    return 1;
  }
  size_t written = 0;
  for (const std::string& seed : HtmlCachePolicyFuzzSeeds()) {
    char path[4096];
    std::snprintf(path, sizeof(path), "%s/seed-%02zu", dir.c_str(), written);
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
      std::fprintf(stderr, "dump_corpus: cannot write %s\n", path);
      return 1;
    }
    if (!seed.empty() &&
        std::fwrite(seed.data(), 1, seed.size(), f) != seed.size()) {
      std::fprintf(stderr, "dump_corpus: short write on %s\n", path);
      std::fclose(f);
      return 1;
    }
    if (std::fclose(f) != 0) {
      std::fprintf(stderr, "dump_corpus: close failed on %s\n", path);
      return 1;
    }
    ++written;
  }
  std::fprintf(stderr, "dump_corpus: %zu seeds written to %s\n", written,
               dir.c_str());
  return 0;
}

}  // namespace pagespeed::fuzz

#if defined(HTML_CACHE_POLICY_FUZZ_NO_MAIN)
// Linked into the seed-replay test, which supplies its own entry point.
#elif defined(HTML_CACHE_POLICY_LIBFUZZER)
// Coverage-guided entry point.  Link with -fsanitize=fuzzer,address.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  pagespeed::fuzz::HtmlCachePolicyFuzzOne(
      std::string_view(reinterpret_cast<const char*>(data), size));
  return 0;
}
#else
int main(int argc, char** argv) {
  if (argc > 1) {
    const std::string_view arg1(argv[1]);
    constexpr std::string_view kDumpPrefix = "--dump_corpus=";
    if (arg1.substr(0, kDumpPrefix.size()) == kDumpPrefix) {
      return pagespeed::fuzz::DumpHtmlCachePolicyFuzzCorpus(
          std::string(arg1.substr(kDumpPrefix.size())));
    }
  }
  size_t n = 0;
  for (const std::string& seed : pagespeed::fuzz::HtmlCachePolicyFuzzSeeds()) {
    pagespeed::fuzz::HtmlCachePolicyFuzzOne(seed);
    ++n;
  }
  for (int i = 1; i < argc; ++i) {
    FILE* f = std::fopen(argv[i], "rb");
    if (f == nullptr) {
      std::fprintf(stderr, "skip: cannot open %s\n", argv[i]);
      continue;
    }
    std::string buf;
    char chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
      buf.append(chunk, got);
    }
    std::fclose(f);
    pagespeed::fuzz::HtmlCachePolicyFuzzOne(buf);
    ++n;
  }
  std::fprintf(stderr,
               "html cache policy fuzz harness: %zu inputs processed clean\n",
               n);
  return 0;
}
#endif
