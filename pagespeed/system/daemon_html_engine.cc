// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/daemon_html_engine.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>

#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "openssl/sha.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/system/daemon_abi.h"
#include "pagespeed/system/daemon_html_decision_log.h"
#include "pagespeed/system/daemon_notify_params.h"
#include "pagespeed/system/daemon_serve_arm.h"
#include "pagespeed/system/site_cache_fragment.h"

namespace net_instaweb {

namespace {

// ps_html_store_classify's request_flags (PS_API 1.12), hand-copied for the
// reason daemon_abi.h gives.
constexpr uint32_t kStoreRequestHasAuthorization = 1u;
constexpr uint32_t kStoreRequestAuthenticatedUser = 2u;
constexpr uint32_t kStoreRequestHasCookie = 4u;
constexpr uint32_t kStoreRequestNoStore = 8u;

// ps_html_store_classify's out_flags bits carried into the stored entry:
// needs-validation from either view, explicit-shared only from both.
constexpr uint32_t kStoreFlagNeedsValidation = 2u;
constexpr uint32_t kStoreFlagExplicitShared = 8u;
// ps_html_store_classify's sidecar-evaluated out flag, and its sidecar
// verdicts, for the decision line.
constexpr uint32_t kStoreFlagSidecarEvaluated = 16u;
constexpr int kSidecarEligible = 0;
constexpr int kSidecarFallThrough = 1;
constexpr int kSidecarNever = 2;

// The library that formats vhost_added (and the serve-side lines).
constexpr int kVhostAddedMinor = 14;
// The first library whose decision-line grammar admits the request_header
// reason; an older one would refuse the whole line.
constexpr int kRequestHeaderReasonMinor = 15;

// The Cache-Control bits that widen what a cache may do with an entry: kept
// only when both views carry them.  Every other bit restricts (or only
// records that a line was present) and is kept when either view carries it.
constexpr uint16_t kPermissiveCcFlags =
    kPsCcOriginPublic | kPsCcOriginImmutable | kPsCcOriginSMaxagePresent;

uint16_t MergeCcFlags(uint16_t a, uint16_t b) {
  return static_cast<uint16_t>(((a | b) & ~kPermissiveCcFlags) |
                               (a & b & kPermissiveCcFlags));
}

uint32_t MergeStoreFlags(uint32_t store, uint32_t final_flags) {
  uint32_t out =
      store & ~(kStoreFlagNeedsValidation | kStoreFlagExplicitShared);
  out |= (store | final_flags) & kStoreFlagNeedsValidation;
  out |= store & final_flags & kStoreFlagExplicitShared;
  return out;
}

// What the body classifier scans; the rest is counted, not kept twice.
constexpr size_t kBodyPrefixBytes = size_t{64} * 1024;

// The refusal-removal set: keys, and how long a key stays in it.
constexpr size_t kRecentlyRemovedMax = 1024;
constexpr int64 kRecentlyRemovedSeconds = 60;

// The keys watched for a header block that stays unpaired.
constexpr size_t kUnpairedMax = 1024;

// The ignore-listed cookie values one request may carry and still count as
// anonymous: the echo check reads every one, so a request with more is
// blocking (the library fails closed past the caller's array, and so does
// the engine).
constexpr size_t kIgnorableCookieValuesMax = 32;
// The echo check's floor for a value worth looking for.
constexpr size_t kCookieEchoMinBytes = 8;
// The longest name a decision line's detail carries (the library's grammar).
constexpr size_t kDetailMaxBytes = 128;

constexpr char kAfterCommit[] = "after_commit";
constexpr char kAfterRecheck[] = "after_recheck";

// The response-side refusals that remove a stored entry (a 200 the origin
// now marks as not storable).  Everything else -- a status other than 404 or
// 410, a request-side reason, a lifetime or content rule -- leaves the entry.
const char* const kRemovingReasons[] = {"private", "no_store", "set_cookie",
                                        "vary", "headers"};

// The fall-through reasons that are the stored entry's own record (C API
// 1.15): counted under daemon_html_fallthrough_ for either body.
const char* const kEntryRecordReasons[] = {"legacy_entry", "policy_mismatch",
                                           "credentialed"};

// The header lines a stored page is never answered with: a cookie, which the
// store refuses, and the validators, which a hit never replays (no 304 is
// produced for HTML, and a validator would let a client revalidate the
// rewritten page against the origin's).  A header block holding one is a
// defect; the hit is refused at emit.  A coding or a range would not match
// the stored body, which is always whole and never compressed.
const char* const kForbiddenOnHit[] = {
    "Set-Cookie",       "Set-Cookie2",      "ETag",
    "Last-Modified",    "Content-Encoding", "Content-Range",
    "Transfer-Encoding"};

// The lines a hit carries from the stored entry itself, never from the
// header block.
const char* const kHitOwnLines[] = {"Content-Type", "Content-Length", "Age"};

// The sampled would-serve pages remembered for the body comparison.
constexpr size_t kShadowWouldServeMax = 256;

// The response lines whose value a stored entry represents itself, or that
// describe this transfer only: never part of the header block compared by
// the re-store skip.  Date, Age and Expires are recomputed per response by
// common server configurations (mod_expires), so an otherwise identical
// response differs in them.  Server and Accept-Ranges are stamped per
// response by the server and never kept in a block.  Cache-Control and
// Content-Type are compared through the stored entry's own fields instead.
const char* const kNotInHeaderBlock[] = {
    "Content-Type",  "Content-Length", "Content-Encoding", "Transfer-Encoding",
    "Cache-Control", "ETag",           "Last-Modified",    "Date",
    "Age",           "Expires",        "Connection",       "Keep-Alive",
    "Server",        "Accept-Ranges"};

// Lines a compressor or the server's framing changes on the way out: not a
// change of the response for the final view.
const char* const kTransportLines[] = {"Content-Encoding", "Content-Length",
                                       "Transfer-Encoding"};

// The lines the server's own configuration can add after the store view and
// still refuse the store with: what decides whether, and for how long, a
// shared cache may keep the page, and what makes the page per-client.  Any
// other line the server adds there is not stored (the header block comes
// from the store view) and is not part of a Markdown answer either, so it
// is left out of the final view the store is classified on.  (Report-only
// classifies the whole final view, as it always has.)
const char* const kFinalViewPolicyLines[] = {"Cache-Control",
                                             "Pragma",
                                             "Expires",
                                             "Date",
                                             "Age",
                                             "Set-Cookie",
                                             "Set-Cookie2",
                                             "Vary",
                                             "Content-Type",
                                             "Content-Encoding",
                                             "Content-Range",
                                             "Content-Location",
                                             "Content-Security-Policy",
                                             "Link",
                                             "WWW-Authenticate",
                                             "Proxy-Authenticate",
                                             "Authentication-Info",
                                             "Proxy-Authentication-Info",
                                             "Clear-Site-Data",
                                             "Surrogate-Control",
                                             "CDN-Cache-Control",
                                             "X-Accel-Expires"};

// A policy line by name: one of the above, or any name ending in
// "Cache-Control" (a targeted cache control such as CDN-Cache-Control).
bool IsFinalViewPolicyLine(StringPiece name) {
  const StringPiece kSuffix("Cache-Control");
  if (name.size() >= kSuffix.size() &&
      StringCaseEqual(name.substr(name.size() - kSuffix.size()), kSuffix)) {
    return true;
  }
  for (const char* n : kFinalViewPolicyLines) {
    if (StringCaseEqual(name, n)) return true;
  }
  return false;
}

template <size_t N>
bool NameIn(StringPiece name, const char* const (&names)[N]) {
  for (const char* n : names) {
    if (StringCaseEqual(name, n)) return true;
  }
  return false;
}

template <size_t N>
bool ValueIn(StringPiece value, const char* const (&values)[N]) {
  for (const char* v : values) {
    if (value == v) return true;
  }
  return false;
}

bool SameLine(const std::pair<GoogleString, GoogleString>& a,
              const std::pair<GoogleString, GoogleString>& b) {
  return StringCaseEqual(a.first, b.first) && a.second == b.second;
}

bool IsVary(const std::pair<GoogleString, GoogleString>& line) {
  return StringCaseEqual(line.first, "Vary");
}

// `a` minus `b`, one line for one line, in a's order.
DaemonHtmlHeaderLines Minus(const DaemonHtmlHeaderLines& a,
                            const DaemonHtmlHeaderLines& b) {
  std::vector<bool> used(b.size(), false);
  DaemonHtmlHeaderLines out;
  for (const auto& line : a) {
    bool matched = false;
    for (size_t i = 0; i < b.size(); ++i) {
      if (!used[i] && SameLine(line, b[i])) {
        used[i] = true;
        matched = true;
        break;
      }
    }
    if (!matched) out.push_back(line);
  }
  return out;
}

DaemonHtmlHeaderLines WithoutVary(const DaemonHtmlHeaderLines& lines) {
  DaemonHtmlHeaderLines out;
  for (const auto& line : lines) {
    if (!IsVary(line)) out.push_back(line);
  }
  return out;
}

// The Vary tokens of every Vary line, in order, each once (case-insensitive).
std::vector<GoogleString> VaryTokens(const DaemonHtmlHeaderLines& lines) {
  std::vector<GoogleString> tokens;
  for (const auto& line : lines) {
    if (!IsVary(line)) continue;
    StringPieceVector parts;
    SplitStringPieceToVector(line.second, ",", &parts, true);
    for (StringPiece part : parts) {
      TrimWhitespace(&part);
      if (part.empty()) continue;
      bool seen = false;
      for (const GoogleString& t : tokens) {
        if (StringCaseEqual(t, part)) {
          seen = true;
          break;
        }
      }
      if (!seen) tokens.emplace_back(part.data(), part.size());
    }
  }
  return tokens;
}

bool HasToken(const std::vector<GoogleString>& tokens, StringPiece token) {
  for (const GoogleString& t : tokens) {
    if (StringCaseEqual(t, token)) return true;
  }
  return false;
}

std::vector<GoogleString> TokensMinus(const std::vector<GoogleString>& a,
                                      const std::vector<GoogleString>& b) {
  std::vector<GoogleString> out;
  for (const GoogleString& t : a) {
    if (!HasToken(b, t)) out.push_back(t);
  }
  return out;
}

// An entity tag with a compressor's "-gzip" suffix taken off.
GoogleString WithoutGzipSuffix(const GoogleString& etag) {
  static const char kSuffix[] = "-gzip\"";
  const size_t n = sizeof(kSuffix) - 1;
  if (etag.size() > n && etag.compare(etag.size() - n, n, kSuffix) == 0) {
    return StrCat(etag.substr(0, etag.size() - n), "\"");
  }
  return etag;
}

// One header view: the origin's lines plus the change from the baseline to
// `at_point`, without the module's own lines; for the final view also
// without transport changes.
DaemonHtmlHeaderLines BuildView(const DaemonHtmlHeaderLines& origin,
                                const DaemonHtmlHeaderLines& baseline,
                                const DaemonHtmlHeaderLines& at_point,
                                const DaemonHtmlHeaderLines& module,
                                bool final_view) {
  // Nothing changed after the baseline: the origin's lines as they came, in
  // their order.
  if (at_point.size() == baseline.size() &&
      std::equal(at_point.begin(), at_point.end(), baseline.begin(),
                 SameLine)) {
    return origin;
  }
  DaemonHtmlHeaderLines added = Minus(
      Minus(WithoutVary(at_point), WithoutVary(baseline)), WithoutVary(module));
  DaemonHtmlHeaderLines removed =
      Minus(WithoutVary(baseline), WithoutVary(at_point));
  if (final_view) {
    auto transport = [](const std::pair<GoogleString, GoogleString>& l) {
      return NameIn(l.first, kTransportLines);
    };
    added.erase(std::remove_if(added.begin(), added.end(), transport),
                added.end());
    removed.erase(std::remove_if(removed.begin(), removed.end(), transport),
                  removed.end());
    // A compressor's "-gzip" entity tag stands for the origin's own.
    DaemonHtmlHeaderLines kept;
    for (const auto& line : added) {
      if (StringCaseEqual(line.first, "ETag")) {
        const GoogleString plain = WithoutGzipSuffix(line.second);
        bool origin_tag = false;
        for (const auto& o : origin) {
          if (StringCaseEqual(o.first, "ETag") && o.second == plain) {
            origin_tag = true;
          }
        }
        if (origin_tag) {
          removed = Minus(removed, {{"ETag", plain}});
          continue;
        }
      }
      kept.push_back(line);
    }
    added.swap(kept);
  }

  DaemonHtmlHeaderLines view = Minus(WithoutVary(origin), removed);
  for (const auto& line : added) view.push_back(line);

  // Vary, compared as a set of tokens.
  const std::vector<GoogleString> origin_tokens = VaryTokens(origin);
  const std::vector<GoogleString> base_tokens = VaryTokens(baseline);
  const std::vector<GoogleString> point_tokens = VaryTokens(at_point);
  const std::vector<GoogleString> module_tokens =
      TokensMinus(VaryTokens(module), origin_tokens);
  std::vector<GoogleString> add_tokens =
      TokensMinus(TokensMinus(point_tokens, base_tokens), module_tokens);
  if (final_view) {
    add_tokens = TokensMinus(add_tokens, {"Accept-Encoding"});
  }
  const std::vector<GoogleString> removed_tokens =
      TokensMinus(base_tokens, point_tokens);
  std::vector<GoogleString> tokens = TokensMinus(origin_tokens, removed_tokens);
  for (const GoogleString& t : add_tokens) {
    if (!HasToken(tokens, t)) tokens.push_back(t);
  }
  if (removed_tokens.empty() && add_tokens.empty()) {
    for (const auto& line : origin) {
      if (IsVary(line)) view.push_back(line);
    }
  } else if (!tokens.empty()) {
    view.emplace_back("Vary", JoinCollection(tokens, ", "));
  }
  return view;
}

// The final view as a store is classified on: without the lines the server
// added after the store view whose names are not policy lines (see
// kFinalViewPolicyLines).
DaemonHtmlHeaderLines FinalViewForStore(
    const DaemonHtmlHeaderLines& final_view,
    const DaemonHtmlHeaderLines& store_view) {
  DaemonHtmlHeaderLines out;
  for (const auto& line : final_view) {
    bool in_store = false;
    for (const auto& s : store_view) {
      if (StringCaseEqual(s.first, line.first)) {
        in_store = true;
        break;
      }
    }
    if (in_store || IsFinalViewPolicyLine(line.first)) {
      out.push_back(line);
    }
  }
  return out;
}

// Whether a request path can name a volume entry: a path that starts with
// "/".  Anything else is neither stored, served nor removed, so a cache key
// always splits back into its parts exactly (VolumeUrl).
bool IsVolumePath(StringPiece path) { return !path.empty() && path[0] == '/'; }

// The URL argument of the volume calls for a cache key
// ("scheme://host[:port]/path?query", built by CacheKey from a path that
// IsVolumePath accepts, and a host with no "/"): its path and query.  The
// volume keys an entry by that, the host name, the scheme and the site, as
// the optimizer worker and this module's other volume readers and writers
// do; the full URL is what the purge fence is checked under.
GoogleString VolumeUrl(const GoogleString& url) {
  const size_t sep = url.find("://");
  if (sep == GoogleString::npos) return url;
  const size_t path = url.find('/', sep + 3);
  return path == GoogleString::npos ? GoogleString() : url.substr(path);
}

// The key of the engine's own per-entry sets: the entry's volume identity,
// so one entry reached through two ports is one key.
GoogleString VolumeIdentity(const GoogleString& url,
                            const GoogleString& hostname,
                            const GoogleString& scheme) {
  return StrCat(scheme, "\n", hostname, "\n", VolumeUrl(url));
}

// The header block the re-store skip compares: what a stored entry does not
// already represent.
DaemonHtmlHeaderLines HeaderBlockOf(const DaemonHtmlHeaderLines& lines) {
  DaemonHtmlHeaderLines out;
  for (const auto& line : lines) {
    if (!NameIn(line.first, kNotInHeaderBlock)) out.push_back(line);
  }
  return out;
}

bool SameMultiset(const DaemonHtmlHeaderLines& a,
                  const DaemonHtmlHeaderLines& b) {
  return a.size() == b.size() && Minus(a, b).empty();
}

// Whether a Cache-Control value carries `directive` as a directive name.
bool HasDirective(StringPiece value, StringPiece directive) {
  StringPieceVector parts;
  SplitStringPieceToVector(value, ",", &parts, true);
  for (StringPiece part : parts) {
    const size_t eq = part.find('=');
    if (eq != StringPiece::npos) part = part.substr(0, eq);
    TrimWhitespace(&part);
    if (StringCaseEqual(part, directive)) return true;
  }
  return false;
}

GoogleString FirstValue(const DaemonHtmlHeaderLines& lines, StringPiece name) {
  for (const auto& line : lines) {
    if (StringCaseEqual(line.first, name)) return line.second;
  }
  return GoogleString();
}

void Sha256(const GoogleString& body, uint8_t out[kPsHtmlHashBytes]) {
  SHA256(reinterpret_cast<const uint8_t*>(body.data()), body.size(), out);
}

bool IsZeroHash(const uint8_t* hash) {
  if (hash == nullptr) return true;
  for (size_t i = 0; i < kPsHtmlHashBytes; ++i) {
    if (hash[i] != 0) return false;
  }
  return true;
}

// Whether a request header line's name is `name`, compared as the library's
// guard compares names: case-insensitively, with '_' read as '-'.
bool HeaderNameIs(StringPiece line_name, StringPiece name) {
  if (line_name.size() != name.size()) return false;
  for (size_t i = 0; i < name.size(); ++i) {
    const char a = line_name[i] == '_' ? '-' : LowerChar(line_name[i]);
    const char b = name[i] == '_' ? '-' : LowerChar(name[i]);
    if (a != b) return false;
  }
  return true;
}

// An RFC 9110 token: what a decision line's detail may carry.
bool IsDetailToken(StringPiece name) {
  if (name.empty() || name.size() > kDetailMaxBytes) return false;
  for (const char c : name) {
    const bool tchar =
        (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        StringPiece("!#$%&'*+-.^_`|~").find(c) != StringPiece::npos;
    if (!tchar) return false;
  }
  return true;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// `in` with every %XX escape decoded once; anything else is kept as it is.
GoogleString PercentDecoded(StringPiece in) {
  GoogleString out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size() && HexValue(in[i + 1]) >= 0 &&
        HexValue(in[i + 2]) >= 0) {
      out.push_back(
          static_cast<char>(HexValue(in[i + 1]) * 16 + HexValue(in[i + 2])));
      i += 2;
    } else {
      out.push_back(in[i]);
    }
  }
  return out;
}

// The name of the cookie whose value starts at `offset` of a Cookie line:
// the text between the pair's start and its "=", trimmed.
GoogleString CookieNameBefore(StringPiece line, size_t offset) {
  if (offset > line.size()) return GoogleString();
  size_t eq = line.rfind('=', offset == 0 ? 0 : offset - 1);
  if (eq == StringPiece::npos || eq >= offset) return GoogleString();
  size_t start = line.rfind(';', eq);
  start = start == StringPiece::npos ? 0 : start + 1;
  StringPiece name = line.substr(start, eq - start);
  TrimWhitespace(&name);
  return name.as_string();
}

// The index of the first of `needles` that occurs in `haystack`, or -1: one
// pass over the haystack, each position tried only against the needles
// that start with its byte.
int FirstNeedleIn(StringPiece haystack,
                  const std::vector<GoogleString>& needles) {
  std::vector<int> by_byte[256];
  for (size_t n = 0; n < needles.size(); ++n) {
    if (!needles[n].empty()) {
      by_byte[static_cast<unsigned char>(needles[n][0])].push_back(
          static_cast<int>(n));
    }
  }
  for (size_t i = 0; i < haystack.size(); ++i) {
    for (const int n : by_byte[static_cast<unsigned char>(haystack[i])]) {
      const GoogleString& needle = needles[n];
      if (haystack.size() - i >= needle.size() &&
          haystack.substr(i, needle.size()) == needle) {
        return n;
      }
    }
  }
  return -1;
}

// Whether the body or a stored header value repeats the value of an
// ignore-listed cookie, with that cookie's name in *name.
bool EchoedCookie(
    const std::vector<std::pair<GoogleString, GoogleString>>& cookies,
    const GoogleString& body, const DaemonHtmlHeaderLines& headers,
    GoogleString* name) {
  std::vector<GoogleString> needles;
  std::vector<size_t> owner;
  for (size_t c = 0; c < cookies.size(); ++c) {
    const GoogleString& value = cookies[c].second;
    const GoogleString decoded = PercentDecoded(value);
    for (const GoogleString* form : {&value, &decoded}) {
      if (form->size() < kCookieEchoMinBytes) continue;
      if (form == &decoded && decoded == value) continue;
      needles.push_back(*form);
      owner.push_back(c);
    }
  }
  if (needles.empty()) return false;
  auto scan = [&](StringPiece text) {
    int n = FirstNeedleIn(text, needles);
    if (n < 0 && text.find('%') != StringPiece::npos) {
      n = FirstNeedleIn(PercentDecoded(text), needles);
    }
    return n;
  };
  int n = scan(body);
  for (size_t h = 0; n < 0 && h < headers.size(); ++h) {
    n = scan(headers[h].second);
  }
  if (n < 0) return false;
  *name = cookies[owner[n]].first;
  return true;
}

const char* CStrOrNull(bool present, const GoogleString& value) {
  return present ? value.c_str() : nullptr;
}

void RunOn(DaemonHtmlBlockingRunner* runner, std::function<void()> work) {
  if (runner == nullptr) {
    work();
  } else {
    runner->RunBlocking(DaemonHtmlCommit(std::move(work)));
  }
}

// A byte-budget reservation, released when its last owner goes.
class BudgetHold {
 public:
  BudgetHold(DaemonHtmlByteBudget* budget, int64 bytes)
      : budget_(budget), bytes_(bytes) {}
  ~BudgetHold() {
    if (budget_ != nullptr && bytes_ > 0) budget_->Release(bytes_);
  }
  BudgetHold(const BudgetHold&) = delete;
  BudgetHold& operator=(const BudgetHold&) = delete;

 private:
  DaemonHtmlByteBudget* budget_;
  int64 bytes_;
};

// Frees the read results and the header block of one serve.
class ServeReads {
 public:
  explicit ServeReads(const DaemonAbi* abi) : abi_(abi) {}
  ~ServeReads() {
    if (block != nullptr) abi_->HeaderBlockFree(block);
    for (void* r : {original, markdown, withheld, coded[0], coded[1]}) {
      if (r != nullptr) abi_->ReadFree(r);
    }
  }
  ServeReads(const ServeReads&) = delete;
  ServeReads& operator=(const ServeReads&) = delete;

  void* original = nullptr;
  void* markdown = nullptr;
  void* withheld = nullptr;
  void* block = nullptr;
  // The coded copies read (1.16), at most two.
  void* coded[2] = {nullptr, nullptr};

 private:
  const DaemonAbi* abi_;
};

}  // namespace

const char* DaemonHtmlAjaxMarker(StringPiece name, StringPiece value) {
  TrimWhitespace(&value);
  if (StringCaseEqual(name, "X-Requested-With")) {
    // Anywhere in the value, as an origin matching the token anywhere
    // (Rails' xhr?) reads it: a value a server joined from repeated lines
    // (httpd, IIS) and "XMLHttpRequest, x" are both a script.  Another
    // value (an Android WebView sends the app's package name) is not.
    if (FindIgnoreCase(value, "XMLHttpRequest") != StringPiece::npos) {
      return "X-Requested-With";
    }
    return nullptr;
  }
  if (value.empty()) return nullptr;
  if (StringCaseEqual(name, "X-MicrosoftAjax")) return "X-MicrosoftAjax";
  if (StringCaseEqual(name, "X-Prototype-Version")) {
    return "X-Prototype-Version";
  }
  return nullptr;
}

GoogleString DaemonHtmlKeyedScheme(StringPiece connection_scheme,
                                   const DaemonHtmlHeaderLines& request_headers,
                                   bool respect_forwarded_proto) {
  if (respect_forwarded_proto) {
    for (const auto& [name, value] : request_headers) {
      if (!StringCaseEqual(name, "X-Forwarded-Proto")) continue;
      if (StringCaseEqual(value, "http")) return "http";
      if (StringCaseEqual(value, "https")) return "https";
      break;  // Only the first line counts, as for the URL.
    }
  }
  return connection_scheme.as_string();
}

namespace {

// RFC 3986 section 5.2.4.
GoogleString RemoveDotSegments(StringPiece path) {
  GoogleString in = path.as_string();
  GoogleString out;
  auto drop_last_segment = [&out] {
    const size_t slash = out.rfind('/');
    out.resize(slash == GoogleString::npos ? 0 : slash);
  };
  while (!in.empty()) {
    if (in.compare(0, 3, "../") == 0) {
      in.erase(0, 3);
    } else if (in.compare(0, 2, "./") == 0 || in.compare(0, 3, "/./") == 0) {
      // "./" goes; "/./" becomes "/".
      in.erase(0, 2);
    } else if (in == "/.") {
      in = "/";
    } else if (in.compare(0, 4, "/../") == 0) {
      in.erase(0, 3);
      drop_last_segment();
    } else if (in == "/..") {
      in = "/";
      drop_last_segment();
    } else if (in == "." || in == "..") {
      in.clear();
    } else {
      const size_t next = in.find('/', in[0] == '/' ? 1 : 0);
      const size_t n = next == GoogleString::npos ? in.size() : next;
      out.append(in, 0, n);
      in.erase(0, n);
    }
  }
  return out;
}

// The scheme of an absolute reference ("https" for "https://h/p"), or empty.
GoogleString ReferenceScheme(StringPiece ref) {
  if (ref.empty() || !IsAsciiAlphaNumeric(ref[0]) ||
      (ref[0] >= '0' && ref[0] <= '9')) {
    return GoogleString();
  }
  for (size_t i = 1; i < ref.size(); ++i) {
    const char c = ref[i];
    if (c == ':') return ref.substr(0, i).as_string();
    if (!IsAsciiAlphaNumeric(c) && c != '+' && c != '-' && c != '.') break;
  }
  return GoogleString();
}

}  // namespace

GoogleString DaemonHtmlResolveReference(StringPiece scheme, StringPiece host,
                                        StringPiece path_and_query,
                                        StringPiece reference) {
  StringPiece ref = reference;
  TrimWhitespace(&ref);
  const size_t hash = ref.find('#');
  if (hash != StringPiece::npos) ref = ref.substr(0, hash);
  GoogleString ref_scheme = ReferenceScheme(ref);
  GoogleString authority = host.as_string();
  GoogleString out_scheme = scheme.as_string();
  if (!ref_scheme.empty()) {
    LowerString(&ref_scheme);
    if (ref_scheme != "http" && ref_scheme != "https") return GoogleString();
    out_scheme = ref_scheme;
    ref.remove_prefix(ref_scheme.size() + 1);
    // An http(s) URL has an authority; "http:path" names no entry.
    if (!HasPrefixString(ref, "//")) return GoogleString();
  }
  bool own_authority = false;
  if (HasPrefixString(ref, "//")) {
    ref.remove_prefix(2);
    const size_t end = ref.find_first_of("/?");
    authority = ref.substr(0, end).as_string();
    ref = end == StringPiece::npos ? StringPiece() : ref.substr(end);
    own_authority = true;
  }
  const size_t q = ref.find('?');
  const StringPiece ref_path = ref.substr(0, q);
  const StringPiece ref_query =
      q == StringPiece::npos ? StringPiece() : ref.substr(q);
  const size_t base_q = path_and_query.find('?');
  const StringPiece base_path = path_and_query.substr(0, base_q);
  const StringPiece base_query = base_q == StringPiece::npos
                                     ? StringPiece()
                                     : path_and_query.substr(base_q);
  GoogleString path;
  StringPiece query = ref_query;
  if (own_authority || (!ref_path.empty() && ref_path[0] == '/')) {
    path = RemoveDotSegments(ref_path);
  } else if (ref_path.empty()) {
    // The request's own path, with the reference's query or its own.
    path = base_path.as_string();
    if (q == StringPiece::npos) query = base_query;
  } else {
    // Merged with the request path's directory (section 5.2.3).
    const size_t slash = base_path.rfind('/');
    const GoogleString merged =
        StrCat(slash == StringPiece::npos ? StringPiece("/")
                                          : base_path.substr(0, slash + 1),
               ref_path);
    path = RemoveDotSegments(merged);
  }
  if (path.empty()) path = "/";
  return StrCat(out_scheme, "://", authority, path, query);
}

GoogleString DaemonHtmlRawRequestPath(StringPiece target) {
  if (target.empty()) return "/";
  if (target[0] != '/') {
    const size_t scheme_end = target.find("://");
    const size_t path_start = scheme_end == StringPiece::npos
                                  ? StringPiece::npos
                                  : target.find_first_of("/?", scheme_end + 3);
    if (path_start == StringPiece::npos) return "/";
    target.remove_prefix(path_start);
    if (target[0] == '?') return StrCat("/", target);
  }
  return target.as_string();
}

// ---------------------------------------------------------------------------
// DaemonHtmlPurgeOracle

DaemonHtmlPurgeOracle::DaemonHtmlPurgeOracle(
    const DaemonHtmlPurgeSource* source, Timer* timer)
    : source_(source), timer_(timer) {}

int64 DaemonHtmlPurgeOracle::ObserveLocked() {
  if (source_ == nullptr) return clamped_global_ms_;
  const int64 raw = source_->GlobalInvalidationTimestampMs();
  if (raw != last_raw_global_ms_) {
    last_raw_global_ms_ = raw;
    if (raw >= 0) {
      clamped_global_ms_ =
          std::max(clamped_global_ms_, std::min(raw, timer_->NowMs()));
    }
  }
  return clamped_global_ms_;
}

int64 DaemonHtmlPurgeOracle::Observe() {
  std::lock_guard<std::mutex> lock(mutex_);
  ObserveLocked();
  int64 mtime_ms = 0;
  if (source_ != nullptr && source_->FlushFileMtimeMs(&mtime_ms) &&
      mtime_ms != last_raw_flush_ms_) {
    last_raw_flush_ms_ = mtime_ms;
    clamped_global_ms_ =
        std::max(clamped_global_ms_, std::min(mtime_ms, timer_->NowMs()));
  }
  return clamped_global_ms_;
}

void DaemonHtmlPurgeOracle::NoteGlobalPurge(int64 purge_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  clamped_global_ms_ =
      std::max(clamped_global_ms_, std::min(purge_ms, timer_->NowMs()));
}

void DaemonHtmlPurgeOracle::SetSiteEpoch(bool read_ok, uint64 epoch_s) {
  std::lock_guard<std::mutex> lock(mutex_);
  site_epoch_ok_ = read_ok;
  if (read_ok) site_epoch_s_ = std::max(site_epoch_s_, epoch_s);
}

void DaemonHtmlPurgeOracle::RaiseSiteEpoch(uint64 epoch_s) {
  std::lock_guard<std::mutex> lock(mutex_);
  site_epoch_s_ = std::max(site_epoch_s_, epoch_s);
}

void DaemonHtmlPurgeOracle::NoteUrlPurge(const GoogleString& url,
                                         int64 purge_ms) {
  // A purge time is never later than now (as for the global purge).
  if (timer_ != nullptr) purge_ms = std::min(purge_ms, timer_->NowMs());
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = local_url_purges_.find(url);
  if (it != local_url_purges_.end()) {
    it->second = std::max(it->second, purge_ms);
    return;
  }
  if (local_url_purges_.size() >= kLocalUrlPurgesMax) {
    auto oldest = local_url_purges_.begin();
    for (auto i = local_url_purges_.begin(); i != local_url_purges_.end();
         ++i) {
      if (i->second < oldest->second) oldest = i;
    }
    // The purge set may not have polled the evicted purge in yet: every
    // entry stored at or before it stays fenced, whatever its URL.  The
    // evicted purge is the oldest remembered, so this covers nothing newer
    // than a purge still in the map.
    local_url_floor_ms_ = std::max(local_url_floor_ms_, oldest->second);
    local_url_purges_.erase(oldest);
  }
  local_url_purges_[url] = purge_ms;
}

bool DaemonHtmlPurgeOracle::Purged(const GoogleString& url,
                                   uint32 stored_at_s) {
  std::lock_guard<std::mutex> lock(mutex_);
  const int64 global_ms = ObserveLocked();
  if (!site_epoch_ok_) return true;
  if (stored_at_s <= site_epoch_s_) return true;
  const int64 stored_ms = static_cast<int64>(stored_at_s) * Timer::kSecondMs;
  if (global_ms >= 0 && stored_ms <= global_ms) return true;
  if (stored_ms <= local_url_floor_ms_) return true;
  const auto local = local_url_purges_.find(url);
  if (local != local_url_purges_.end() && stored_ms <= local->second) {
    return true;
  }
  if (UnsafeFencedLocked(url, stored_ms)) return true;
  return source_ != nullptr &&
         !source_->IsUrlValidIgnoringGlobal(url, stored_ms);
}

void DaemonHtmlPurgeOracle::SetUnsafeRetentionCap(uint32 cap_s) {
  std::lock_guard<std::mutex> lock(mutex_);
  unsafe_cap_s_ = cap_s;
}

int64 DaemonHtmlPurgeOracle::UnsafeRetentionMsLocked() const {
  constexpr int64 kFloorS = 60;
  // No cap: the library keeps a stored page's own lifetime, unclamped, so
  // no age is safe to drop a fence at; it goes only when the map is full.
  if (unsafe_cap_s_ == 0) return std::numeric_limits<int64>::max();
  return 2 * std::max<int64>(unsafe_cap_s_, kFloorS) * Timer::kSecondMs;
}

GoogleString DaemonHtmlPurgeOracle::UnsafeFenceKey(const GoogleString& url) {
  // scheme://host[:port]/path?query, as CacheKey builds it (host lowercased):
  // the entry is the scheme, the host without its port, and path and query.
  GoogleString identity = url;
  const size_t sep = url.find("://");
  if (sep != GoogleString::npos) {
    const size_t path = url.find('/', sep + 3);
    GoogleString host =
        url.substr(sep + 3, path == GoogleString::npos ? GoogleString::npos
                                                       : path - sep - 3);
    if (!host.empty() && host[0] == '[') {
      const size_t close = host.find(']');
      if (close != GoogleString::npos) host.resize(close + 1);
    } else {
      const size_t colon = host.rfind(':');
      if (colon != GoogleString::npos) host.resize(colon);
    }
    identity =
        StrCat(url.substr(0, sep), "\n", host, "\n",
               path == GoogleString::npos ? GoogleString() : url.substr(path));
  }
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const uint8_t*>(identity.data()), identity.size(),
         digest);
  return GoogleString(reinterpret_cast<const char*>(digest), sizeof(digest));
}

void DaemonHtmlPurgeOracle::NoteUnsafeInvalidation(const GoogleString& url,
                                                   int64 fence_ms) {
  if (timer_ != nullptr) fence_ms = std::min(fence_ms, timer_->NowMs());
  const GoogleString key = UnsafeFenceKey(url);
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = unsafe_fences_.find(key);
  if (it != unsafe_fences_.end()) {
    // A newer fence for the same key: it moves to the back of the order.
    fence_ms = std::max(fence_ms, it->second->second);
    unsafe_order_.erase(it->second);
    unsafe_fences_.erase(it);
  }
  // Expired fences go first, then, when the map is still full, the oldest:
  // that key keeps only the bound every other process has.
  const int64 now_ms = timer_ != nullptr ? timer_->NowMs() : fence_ms;
  const int64 retention_ms = UnsafeRetentionMsLocked();
  while (!unsafe_order_.empty() &&
         (now_ms - unsafe_order_.front().second >= retention_ms ||
          unsafe_order_.size() >= kUnsafeFencesMax)) {
    unsafe_fences_.erase(unsafe_order_.front().first);
    unsafe_order_.pop_front();
  }
  unsafe_order_.emplace_back(key, fence_ms);
  unsafe_fences_[key] = std::prev(unsafe_order_.end());
}

bool DaemonHtmlPurgeOracle::UnsafeFencedLocked(const GoogleString& url,
                                               int64 stored_ms) {
  if (unsafe_fences_.empty()) return false;
  auto it = unsafe_fences_.find(UnsafeFenceKey(url));
  if (it == unsafe_fences_.end()) return false;
  const int64 fence_ms = it->second->second;
  const int64 now_ms = timer_ != nullptr ? timer_->NowMs() : fence_ms;
  if (now_ms - fence_ms >= UnsafeRetentionMsLocked()) {
    unsafe_order_.erase(it->second);
    unsafe_fences_.erase(it);
    return false;
  }
  // At or before: an entry stored in the second of the unsafe request is
  // fenced, as for a purge.
  return stored_ms <= fence_ms;
}

bool DaemonHtmlPurgeOracle::UnsafeFenced(const GoogleString& url,
                                         uint32 stored_at_s) {
  std::lock_guard<std::mutex> lock(mutex_);
  return UnsafeFencedLocked(url,
                            static_cast<int64>(stored_at_s) * Timer::kSecondMs);
}

size_t DaemonHtmlPurgeOracle::unsafe_fences_size() {
  std::lock_guard<std::mutex> lock(mutex_);
  return unsafe_fences_.size();
}

// ---------------------------------------------------------------------------
// DaemonHtmlByteBudget

bool DaemonHtmlByteBudget::TryReserve(int64 bytes) {
  const int64 capacity = capacity_.load(std::memory_order_relaxed);
  int64 used = in_use_.load(std::memory_order_relaxed);
  do {
    if (used + bytes > capacity) return false;
  } while (!in_use_.compare_exchange_weak(used, used + bytes,
                                          std::memory_order_relaxed));
  return true;
}

DaemonHtmlByteBudget* ProcessDaemonHtmlByteBudget() {
  static DaemonHtmlByteBudget* const budget = new DaemonHtmlByteBudget;
  return budget;
}

GoogleString DaemonMarkdownCompressDemands::Digest(const GoogleString& key) {
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const uint8_t*>(key.data()), key.size(), digest);
  return GoogleString(reinterpret_cast<const char*>(digest), sizeof(digest));
}

bool DaemonMarkdownCompressDemands::Admit(const GoogleString& key) {
  const GoogleString digest = Digest(key);
  std::lock_guard<std::mutex> lock(mutex_);
  if (capacity_ == 0 || !keys_.insert(digest).second) return false;
  order_.push_back(digest);
  while (order_.size() > capacity_) {
    keys_.erase(order_.front());
    order_.pop_front();
  }
  return true;
}

void DaemonMarkdownCompressDemands::Forget(const GoogleString& key) {
  const GoogleString digest = Digest(key);
  std::lock_guard<std::mutex> lock(mutex_);
  if (keys_.erase(digest) == 0) return;
  for (auto it = order_.begin(); it != order_.end(); ++it) {
    if (*it == digest) {
      order_.erase(it);
      break;
    }
  }
}

size_t DaemonMarkdownCompressDemands::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return keys_.size();
}

DaemonMarkdownCompressDemands* ProcessDaemonMarkdownCompressDemands() {
  static DaemonMarkdownCompressDemands* const demands =
      new DaemonMarkdownCompressDemands;
  return demands;
}

void DaemonHtmlByteBudget::Release(int64 bytes) {
  in_use_.fetch_sub(bytes, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Counters

struct DaemonHtmlEngine::Counters {
  explicit Counters(Statistics* s)
      : stored(s->GetVariable("daemon_html_stored")),
        refused_budget(s->GetVariable("daemon_html_refused_budget")),
        refused_url_disagree(
            s->GetVariable("daemon_html_refused_url_disagree")),
        refused_request_header(
            s->GetVariable("daemon_html_refused_request_header")),
        refused_auth(s->GetVariable("daemon_html_refused_auth")),
        upgrade_restores(s->GetVariable("daemon_html_upgrade_restores")),
        served_credentialed_refused(
            s->GetVariable("daemon_html_served_credentialed_refused")),
        sidecar_write_failed(
            s->GetVariable("daemon_html_sidecar_write_failed")),
        sidecar_unpaired_stale(
            s->GetVariable("daemon_html_sidecar_unpaired_stale")),
        purge_race_refused(s->GetVariable("daemon_html_purge_race_refused")),
        removed(s->GetVariable("daemon_html_removed")),
        removed_on_refusal(s->GetVariable("daemon_html_removed_on_refusal")),
        restore_skipped(s->GetVariable("daemon_html_restore_skipped")),
        epoch_write_failed(s->GetVariable("daemon_html_epoch_write_failed")),
        purge_remove_failed(s->GetVariable("daemon_html_purge_remove_failed")),
        serve_unavailable(s->GetVariable("daemon_html_serve_unavailable")),
        serve_cached_html_unavailable(
            s->GetVariable("daemon_html_serve_cached_html_unavailable")),
        notify_sent(s->GetVariable("daemon_html_notify_sent")),
        notify_limited(s->GetVariable("daemon_html_notify_limited")),
        markdown_served(s->GetVariable("daemon_markdown_served")),
        markdown_not_modified(s->GetVariable("daemon_markdown_not_modified")),
        served_without_preference(
            s->GetVariable("daemon_markdown_served_without_preference")),
        transport_conflict(
            s->GetVariable("daemon_markdown_transport_conflict")),
        served_br(s->GetVariable("daemon_markdown_served_br")),
        served_gzip(s->GetVariable("daemon_markdown_served_gzip")),
        served_identity(s->GetVariable("daemon_markdown_served_identity")),
        encoded_copy_missing(
            s->GetVariable("daemon_markdown_encoded_copy_missing")),
        encoded_copy_unbound(
            s->GetVariable("daemon_markdown_encoded_copy_unbound")),
        compress_demand_sent(
            s->GetVariable("daemon_markdown_compress_demand_sent")),
        coded_fallback(s->GetVariable("daemon_markdown_coded_fallback")),
        evaluated(s->GetVariable("daemon_html_evaluated")),
        would_store(s->GetVariable("daemon_html_would_store")),
        would_store_no_notify(
            s->GetVariable("daemon_html_would_store_no_notify")),
        would_store_bytes(s->GetVariable("daemon_html_would_store_bytes")),
        refused(s->GetVariable("daemon_html_refused")),
        refused_other(s->GetVariable("daemon_html_refused_other")),
        unavailable(s->GetVariable("daemon_html_unavailable")),
        classify_failed(s->GetVariable("daemon_html_classify_failed")),
        log_dropped(s->GetVariable("daemon_html_log_dropped")),
        served_html(s->GetVariable("daemon_html_served_html")),
        served_html_c1(s->GetVariable("daemon_html_served_html_c1")),
        served_html_c3(s->GetVariable("daemon_html_served_html_c3")),
        served_html_markdown_preference(
            s->GetVariable("daemon_html_served_html_markdown_preference")),
        served_html_unclassed(
            s->GetVariable("daemon_html_served_html_unclassed")),
        agent_html_requests(s->GetVariable("daemon_html_agent_html_requests")),
        shadow_sampled(s->GetVariable("daemon_html_shadow_sampled")),
        would_serve_html(s->GetVariable("daemon_html_would_serve_html")),
        would_serve_body_differs(
            s->GetVariable("daemon_html_would_serve_body_differs")),
        fresh_body_changed(s->GetVariable("daemon_html_fresh_body_changed")),
        hit_recorded(s->GetVariable("daemon_html_hit_recorded")),
        hit_forbidden_header(
            s->GetVariable("daemon_html_hit_forbidden_header")),
        unexpected_validate(s->GetVariable("daemon_html_unexpected_validate")),
        invalidated_unsafe(s->GetVariable("daemon_html_invalidated_unsafe")),
        invalidate_failed(s->GetVariable("daemon_html_invalidate_failed")) {
    for (const char* const* reason =
             RewriteStats::kDaemonMarkdownFallthroughReasons;
         *reason != nullptr; ++reason) {
      fallthrough[*reason] =
          s->GetVariable(StrCat("daemon_markdown_fallthrough_", *reason));
    }
    for (const char* const* reason = RewriteStats::kDaemonHtmlRefusedReasons;
         *reason != nullptr; ++reason) {
      refused_by_reason[*reason] =
          s->GetVariable(StrCat("daemon_html_refused_", *reason));
    }
    for (const char* const* reason =
             RewriteStats::kDaemonHtmlFallthroughReasons;
         *reason != nullptr; ++reason) {
      html_fallthrough[*reason] =
          s->GetVariable(StrCat("daemon_html_fallthrough_", *reason));
    }
    for (const char* const* reason =
             RewriteStats::kDaemonHtmlWouldFallthroughReasons;
         *reason != nullptr; ++reason) {
      would_fallthrough[*reason] =
          s->GetVariable(StrCat("daemon_html_would_fallthrough_", *reason));
    }
  }

  Variable* stored;
  Variable* refused_budget;
  Variable* refused_url_disagree;
  Variable* refused_request_header;
  Variable* refused_auth;
  Variable* upgrade_restores;
  Variable* served_credentialed_refused;
  Variable* sidecar_write_failed;
  Variable* sidecar_unpaired_stale;
  Variable* purge_race_refused;
  Variable* removed;
  Variable* removed_on_refusal;
  Variable* restore_skipped;
  Variable* epoch_write_failed;
  Variable* purge_remove_failed;
  Variable* serve_unavailable;
  Variable* serve_cached_html_unavailable;
  Variable* notify_sent;
  Variable* notify_limited;
  Variable* markdown_served;
  Variable* markdown_not_modified;
  Variable* served_without_preference;
  Variable* transport_conflict;
  // 1.16: the coding of each Markdown 200, the coded copies that could not
  // be sent, and the compress demands that went out.
  Variable* served_br;
  Variable* served_gzip;
  Variable* served_identity;
  Variable* encoded_copy_missing;
  Variable* encoded_copy_unbound;
  Variable* compress_demand_sent;
  // Coded answers a port replaced by the plain copy (a body filter).
  Variable* coded_fallback;
  std::map<GoogleString, Variable*> fallthrough;
  // The report's, in every mode.
  Variable* evaluated;
  Variable* would_store;
  Variable* would_store_no_notify;
  Variable* would_store_bytes;
  Variable* refused;
  Variable* refused_other;
  Variable* unavailable;
  Variable* classify_failed;
  Variable* log_dropped;
  std::map<GoogleString, Variable*> refused_by_reason;
  // The entry-record reasons of C API 1.15, both bodies, and the reasons an
  // HTML lookup falls through for (serve-cached-html).
  std::map<GoogleString, Variable*> html_fallthrough;
  // serve-cached-html: stored pages answered, by the request's agent class
  // (the parts sum to served_html), the agent-classed HTML requests
  // evaluated, the shadow sample, and the checks that must stay 0.
  Variable* served_html;
  Variable* served_html_c1;
  Variable* served_html_c3;
  Variable* served_html_markdown_preference;
  Variable* served_html_unclassed;
  Variable* agent_html_requests;
  Variable* shadow_sampled;
  Variable* would_serve_html;
  Variable* would_serve_body_differs;
  Variable* fresh_body_changed;
  Variable* hit_recorded;
  Variable* hit_forbidden_header;
  Variable* unexpected_validate;
  // Unsafe-method invalidation.
  Variable* invalidated_unsafe;
  Variable* invalidate_failed;
  std::map<GoogleString, Variable*> would_fallthrough;
};

// What the request policy says about one request, before any read.  Filled
// only with a library that binds C API 1.15 (`applies`); below it the
// earlier rules run unchanged.
struct DaemonHtmlEngine::RequestPolicy {
  bool applies = false;
  // A request credential (Authorization, Proxy-Authorization or an
  // authenticated user).
  bool auth = false;
  // The request guard's refusal (request_header or path), or empty, with
  // the header it is about.
  GoogleString guard_reason;
  GoogleString guard_detail;
  // The refusal is an X-Forwarded-Proto that does not name the keyed scheme,
  // or an X-Original-URL (the header IIS URL Rewrite adds to a rewritten
  // request) that differs from the keyed target.
  bool xfp = false;
  bool rewritten_url = false;
  // The request carries a cookie the ignore list in force does not cover,
  // and the name the refusal names (a cookie, malformed or capacity).
  bool blocking_cookie = false;
  GoogleString cookie_detail;
  // explicit-public: a blocking cookie goes to the library, not refused here.
  bool cookie_refused = false;
  // The ignore-listed cookies, name and value, for the echo check.
  std::vector<std::pair<GoogleString, GoogleString>> ignorable_cookies;

  // The refusal before any read, credentials first, then the guard, then
  // the cookies; empty when the request may be read.  With
  // `skip_credentials` (tests only) the credential refusals do not count.
  void Verdict(bool skip_credentials, GoogleString* reason,
               GoogleString* detail) const {
    reason->clear();
    detail->clear();
    if (auth && !skip_credentials) {
      *reason = "auth_request";
    } else if (!guard_reason.empty()) {
      *reason = guard_reason;
      *detail = guard_detail;
    } else if (cookie_refused && !skip_credentials) {
      *reason = "cookie_request";
      *detail = cookie_detail;
    }
  }
};

// One view's classification.
struct DaemonHtmlEngine::StoreVerdict {
  int action = kPsHtmlStoreActionRefuse;
  GoogleString reason;
  GoogleString detail;
  uint32_t lifetime_s = 0;
  uint32_t flags = 0;
  uint16_t cc_flags = 0;
  int sidecar = 0;
  GoogleString content_type;

  bool stores() const {
    return action == kPsHtmlStoreActionStore ||
           action == kPsHtmlStoreActionStoreNoNotify;
  }
  // A store or, in report-only, a would-store.
  bool would_store() const {
    return stores() || action == kPsHtmlStoreActionWouldStore ||
           action == kPsHtmlStoreActionWouldStoreNoNotify;
  }
  bool no_notify() const {
    return action == kPsHtmlStoreActionStoreNoNotify ||
           action == kPsHtmlStoreActionWouldStoreNoNotify;
  }
};

// ---------------------------------------------------------------------------
// DaemonHtmlRecording

DaemonHtmlRecording::DaemonHtmlRecording(DaemonHtmlEngine* engine,
                                         const DaemonHtmlRequestFacts& facts,
                                         DaemonHtmlPort* port, bool may_store)
    : engine_(engine), facts_(facts), port_(port), may_store_(may_store) {}

DaemonHtmlRecording::~DaemonHtmlRecording() { ReleaseBudget(); }

void DaemonHtmlRecording::ReleaseBudget() {
  if (reserved_ > 0 && engine_->deps_.budget != nullptr) {
    engine_->deps_.budget->Release(reserved_);
  }
  reserved_ = 0;
}

void DaemonHtmlRecording::SetOriginHeaders(DaemonHtmlHeaderLines lines) {
  origin_ = std::move(lines);
}

void DaemonHtmlRecording::SetBaselineHeaders(DaemonHtmlHeaderLines lines) {
  baseline_ = std::move(lines);
  has_baseline_ = true;
}

void DaemonHtmlRecording::SetModuleHeaders(DaemonHtmlHeaderLines lines) {
  module_ = std::move(lines);
}

void DaemonHtmlRecording::NoteStoreHeaders(DaemonHtmlHeaderLines lines) {
  store_lines_ = std::move(lines);
  has_store_lines_ = true;
}

void DaemonHtmlRecording::NoteFinalHeaders(DaemonHtmlHeaderLines lines) {
  if (has_final_lines_) return;  // Only the first counts.
  final_lines_ = std::move(lines);
  has_final_lines_ = true;
}

void DaemonHtmlRecording::AddBody(const char* data, size_t size) {
  if (finished_ || size == 0) return;
  body_bytes_ += size;
  if (!may_store_) {
    // Only classified, never stored: the prefix the body scan reads is all
    // that is kept, and it is not held against the budget.
    if (body_.size() < kBodyPrefixBytes) {
      body_.append(data, std::min(size, kBodyPrefixBytes - body_.size()));
    }
    return;
  }
  if (budget_refused_ || over_size_) return;
  const int64 max_bytes = engine_->config_.max_bytes;
  size_t take = size;
  if (static_cast<int64>(body_.size() + size) > max_bytes) {
    // Over the size cap: the library refuses it by the byte count; keep
    // only what the body scan needs.
    over_size_ = true;
    take = body_.size() < kBodyPrefixBytes
               ? std::min(size, kBodyPrefixBytes - body_.size())
               : 0;
  }
  if (take == 0) return;
  DaemonHtmlByteBudget* budget = engine_->deps_.budget;
  if (budget != nullptr && !budget->TryReserve(static_cast<int64>(take))) {
    budget_refused_ = true;
    ReleaseBudget();
    GoogleString().swap(body_);
    return;
  }
  reserved_ += static_cast<int64>(take);
  body_.append(data, take);
}

void DaemonHtmlRecording::Finish(bool aborted) {
  if (finished_) return;
  finished_ = true;
  engine_->FinishRecording(this, aborted);
  ReleaseBudget();
}

// ---------------------------------------------------------------------------
// DaemonHtmlEngine

DaemonHtmlEngine::DaemonHtmlEngine(Config config, Deps deps)
    : config_(std::move(config)),
      deps_(std::move(deps)),
      oracle_(deps_.purge_source, deps_.timer),
      counters_(new Counters(deps_.statistics)),
      agent_class_counters_(deps_.statistics) {
  if (!deps_.log_dir.empty()) {
    log_ = DaemonHtmlDecisionLog::ForLogDir(deps_.log_dir);
  }
  if (config_.epoch_sites.empty()) {
    config_.epoch_sites.push_back(config_.site);
  }
  oracle_.SetUnsafeRetentionCap(config_.max_age_s);
  ReadConditions(/*log_change=*/false);
  ReadSiteEpoch();
  ObservePurgeState();
}

DaemonHtmlEngine::~DaemonHtmlEngine() = default;

void* DaemonHtmlEngine::Cache() const {
  if (deps_.substrate.cache_source) return deps_.substrate.cache_source();
  return deps_.substrate.cache;
}

bool DaemonHtmlEngine::serving() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return set_bound_ && worker_markdown_ && worker_agent_entitled_;
}

DaemonHtmlMode DaemonHtmlEngine::effective_mode() const {
  if (config_.mode != DaemonHtmlMode::kServeCachedHtml) return config_.mode;
  std::lock_guard<std::mutex> lock(mutex_);
  return set15_bound_ ? DaemonHtmlMode::kServeCachedHtml
                      : DaemonHtmlMode::kServeVariants;
}

void DaemonHtmlEngine::CacheKey(const DaemonHtmlRequestFacts& facts,
                                GoogleString* url, GoogleString* hostname) {
  GoogleString host;
  for (const char c : facts.host) host.push_back(LowerChar(c));
  GoogleString port;
  if (!host.empty() && host[0] != '[') {
    const size_t colon = host.rfind(':');
    if (colon != GoogleString::npos) {
      port = host.substr(colon);
      host.resize(colon);
    }
  } else {
    const size_t close = host.find(']');
    if (close != GoogleString::npos && close + 1 < host.size()) {
      port = host.substr(close + 1);
      host.resize(close + 1);
    }
  }
  if ((facts.scheme == "https" && port == ":443") ||
      (facts.scheme == "http" && port == ":80")) {
    port.clear();
  }
  *hostname = host;
  *url = StrCat(facts.scheme, "://", host, port, facts.path);
}

void DaemonHtmlEngine::ReadConditions(bool log_change) {
  const DaemonAbi* abi = deps_.substrate.abi;
  const bool set = abi != nullptr && abi->HasHtmlServeSet();
  ReadServe15Condition(log_change);
  bool markdown = false;
  bool entitled = false;
  int codings = -1;
  GoogleString reason;
  if (!set) {
    reason = abi == nullptr ? GoogleString("no optimizer library is loaded")
                            : abi->HtmlServeSetUnavailableReason();
  } else if (config_.mode < DaemonHtmlMode::kServeVariants ||
             !config_.agent_optimize) {
    // Nothing to serve here: the worker's state is not read at all.
    reason = "this configuration does not serve";
  } else if (abi->HasHtmlServe15Set() && config_.policy_stamp == 0) {
    // Fails closed: with C API 1.15 every stored page and every lookup
    // carries the stamp of the credentialed policy, and a configuration
    // that was not applied (ApplyPolicySettings) has none.
    reason =
        "the credentialed policy of this configuration has no stamp (its "
        "settings were not applied)";
  } else {
    const char* path = deps_.substrate.cache_path.c_str();
    const int mode = abi->ReadSharedConfigHtmlMode(path);
    markdown = mode == kPsHtmlModeMarkdownStatic ||
               mode == kPsHtmlModeMarkdownRendered;
    entitled = abi->ReadSharedConfigAgentEntitled(path) == 1;
    codings = abi->HasHtmlServe16Set()
                  ? abi->ReadSharedConfigMarkdownCodings(path)
                  : -1;
    if (!markdown) {
      reason =
          "the optimizer worker's html_mode is not markdown-static or "
          "markdown-rendered";
    } else if (!entitled) {
      reason = "the optimizer worker reports agent-optimize off";
    }
  }
  bool log = false;
  bool now_serving = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool was_serving =
        set_bound_ && worker_markdown_ && worker_agent_entitled_;
    now_serving = set && markdown && entitled;
    const bool changed =
        was_serving != now_serving || reason != unavailable_reason_;
    set_bound_ = set;
    worker_markdown_ = markdown;
    worker_agent_entitled_ = entitled;
    set16_bound_ = set && abi->HasHtmlServe16Set();
    markdown_codings_ = codings;
    unavailable_reason_ = reason;
    if (changed) {
      unavailable_logged_ = false;
      if (log_change) {
        log = true;
        unavailable_logged_ = !now_serving;
      }
    }
  }
  if (log && deps_.handler != nullptr) {
    if (now_serving) {
      deps_.handler->Message(
          kInfo,
          "DaemonHtmlCache: the optimizer library and worker allow serving; "
          "storing pages and serving their Markdown copies");
    } else {
      deps_.handler->Message(
          kWarning,
          "DaemonHtmlCache: serving is unavailable (%s); running "
          "report-only",
          reason.c_str());
    }
  }
}

void DaemonHtmlEngine::ReadServe15Condition(bool log_change) {
  const DaemonAbi* abi = deps_.substrate.abi;
  const bool set15 = abi != nullptr && abi->HasHtmlServe15Set();
  const GoogleString reason =
      set15            ? GoogleString()
      : abi == nullptr ? GoogleString("no optimizer library is loaded")
                       : abi->HtmlServe15SetUnavailableReason();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (set15 != set15_bound_ || reason != set15_reason_) {
      set15_logged_ = false;
    }
    set15_bound_ = set15;
    set15_reason_ = reason;
  }
  if (log_change) NoteServe15Unavailable();
}

void DaemonHtmlEngine::NoteServe15Unavailable() {
  const DaemonAbi* abi = deps_.substrate.abi;
  // Only a configuration that stores and serves is affected; a library
  // without the 1.14 set is reported by the serving conditions instead.
  if (config_.mode < DaemonHtmlMode::kServeVariants || abi == nullptr ||
      !abi->HasHtmlServeSet()) {
    return;
  }
  // serve-variants with the credentialed policy and the ignore list at their
  // defaults runs exactly as it did before 1.15: nothing the operator set
  // goes unapplied.
  const bool ignore_settings_set =
      !config_.ignore_cookies || !config_.ignore_cookie.empty();
  if (config_.mode != DaemonHtmlMode::kServeCachedHtml &&
      config_.credentialed == kPsHtmlCredentialedAnonymousOnly &&
      !ignore_settings_set) {
    return;
  }
  GoogleString reason;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (set15_bound_ || set15_logged_) return;
    set15_logged_ = true;
    reason = set15_reason_;
  }
  if (deps_.handler == nullptr) return;
  // A library that reports 1.15 but lacks part of the set applies its own
  // 1.15 store and serve rules; only an older one keeps the earlier rule.
  const bool reports_15 = abi->VersionMinor() >= kHtmlServe15SetMinor;
  if (config_.mode == DaemonHtmlMode::kServeCachedHtml) {
    deps_.handler->Message(
        kWarning,
        "DaemonHtmlCache serve-cached-html is unavailable (%s); running "
        "serve-variants, which serves the Markdown copies of stored pages "
        "only, %s",
        reason.c_str(),
        reports_15 ? "with the library's own cookie rule"
                   : "with the earlier cookie rule and no check of the "
                     "stored entry's record; every cookie, an analytics "
                     "cookie included, counts as a cookie");
  } else {
    deps_.handler->Message(
        kWarning,
        "DaemonHtmlCache: %s; %s, and DaemonHtmlCacheCredentialed, "
        "DaemonHtmlCacheIgnoreCookies and DaemonHtmlCacheIgnoreCookie have "
        "no effect",
        reason.c_str(),
        reports_15 ? "serve-variants runs with the library's own cookie rule"
                   : "serve-variants keeps the earlier cookie rule (every "
                     "cookie, an analytics cookie included, counts as a "
                     "cookie, and a page the origin marks public is stored "
                     "and served for a request with a cookie)");
  }
}

void DaemonHtmlEngine::NoteForwardedProtoMismatch() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (xfp_logged_) return;
    xfp_logged_ = true;
  }
  if (deps_.handler == nullptr) return;
  deps_.handler->Message(
      kWarning,
      "DaemonHtmlCache: a request carried an X-Forwarded-Proto that is not "
      "the scheme this server keys its pages on, so no page is stored or "
      "served for it (Markdown copies included); behind a TLS-terminating "
      "proxy that every request passes and that sets the header itself, set "
      "RespectXForwardedProto on");
}

bool DaemonHtmlEngine::ApplyPolicySettings(const DaemonAbi& abi, Config* config,
                                           GoogleString* error) {
  // The additions as a set, so the stamp and the classification see the
  // same list whatever the order or repetition of the lines.
  std::vector<GoogleString> additions = config->ignore_cookie;
  std::sort(additions.begin(), additions.end());
  additions.erase(std::unique(additions.begin(), additions.end()),
                  additions.end());
  config->ignore_cookie = additions;
  config->policy_stamp = 0;
  // Below 1.15 the settings have no effect, as before.
  if (!abi.HasHtmlServe15Set()) return true;
  for (const GoogleString& addition : additions) {
    int warning = 0;
    const int accepted = abi.HtmlIgnoreCookieGuard(addition.c_str(), &warning);
    if (accepted != 1) {
      if (error != nullptr) {
        *error = StrCat(
            "DaemonHtmlCacheIgnoreCookie ", addition,
            " is refused: an addition must not be * alone, must have at "
            "least 3 characters before a trailing *, and must not match or "
            "cover a session cookie name, which is never ignored");
      }
      return false;
    }
  }
  std::vector<const char*> names;
  names.reserve(additions.size());
  for (const GoogleString& addition : additions) {
    names.push_back(addition.c_str());
  }
  uint64_t stamp = 0;
  if (abi.HtmlPolicyStamp(config->credentialed, config->ignore_cookies ? 1 : 0,
                          kPsHtmlCookieListVersion, names.data(), names.size(),
                          &stamp) != 0 ||
      stamp == 0) {
    if (error != nullptr) {
      *error =
          "DaemonHtmlCache: the optimizer library could not compute the "
          "stamp of the credentialed policy";
    }
    return false;
  }
  config->policy_stamp = stamp;
  return true;
}

void DaemonHtmlEngine::EvaluateRequestPolicy(
    const DaemonHtmlRequestFacts& facts, RequestPolicy* out) {
  const DaemonAbi* abi = deps_.substrate.abi;
  *out = RequestPolicy();
  if (abi == nullptr || !abi->HasHtmlServe15Set()) return;
  out->applies = true;
  auto refuse = [out](const char* reason, GoogleString detail) {
    if (!out->guard_reason.empty()) return;  // The first one is the verdict.
    out->guard_reason = reason;
    out->guard_detail = std::move(detail);
  };
  // Credentials the cache never keys on: never stored, never looked up.  The
  // header lines are read here as well as the port's facts, so every port
  // counts both names, and a Cookie2 line (which no ignore list covers)
  // blocks whatever the Cookie lines hold.
  bool cookie2 = false;
  for (const auto& line : facts.request_headers) {
    if (HeaderNameIs(line.first, "Authorization") ||
        HeaderNameIs(line.first, "Proxy-Authorization")) {
      out->auth = true;
    } else if (HeaderNameIs(line.first, "Cookie2")) {
      cookie2 = true;
    }
  }
  if (facts.has_authorization || facts.authenticated_user) out->auth = true;

  // The headers the cache key does not contain, and the shape of the path.
  std::vector<const char*> names;
  std::vector<const char*> values;
  names.reserve(facts.request_headers.size());
  values.reserve(facts.request_headers.size());
  for (const auto& [name, value] : facts.request_headers) {
    names.push_back(name.c_str());
    values.push_back(value.c_str());
  }
  // A port that did not record the target as received gives the keyed
  // path, which the guard reads no less strictly.
  const GoogleString& raw_path =
      facts.raw_path.empty() ? facts.path : facts.raw_path;
  PsHtmlRequestGuard guard = {};
  guard.struct_size = sizeof(guard);
  guard.header_index = -1;
  if (abi->HtmlRequestGuard(raw_path.c_str(), names.data(), values.data(),
                            names.size(), facts.scheme.c_str(), &guard) != 0) {
    // Fails closed: a request the guard could not judge is not read.
    refuse("request_header", GoogleString());
  } else if (guard.reason == kPsHtmlGuardPath) {
    refuse("path", GoogleString());
  } else if (guard.reason != kPsHtmlGuardNone) {
    GoogleString name;
    if (guard.header_index >= 0 && static_cast<size_t>(guard.header_index) <
                                       facts.request_headers.size()) {
      name = facts.request_headers[guard.header_index].first;
    }
    if (out->guard_reason.empty()) {
      out->xfp = guard.xfp != 0;
      out->rewritten_url = HeaderNameIs(name, "X-Original-URL");
    }
    refuse("request_header", name);
  }

  // The library accepts an X-Original-URL that equals the target the guard
  // is given.  A port that keys the page on a normalised reading of the
  // target (Apache: the decoded, slash-merged path) hands the guard the raw
  // target, so the header is refused here whenever the two differ.
  if (!facts.raw_path.empty() && facts.raw_path != facts.path) {
    for (const auto& line : facts.request_headers) {
      if (HeaderNameIs(line.first, "X-Original-URL")) {
        if (out->guard_reason.empty()) out->rewritten_url = true;
        refuse("request_header", line.first);
        break;
      }
    }
  }

  // The port's own reading of the AJAX markers stays a second reader: the
  // guard reads them too, and a refusal never rests on one reader alone.
  if (!facts.ajax_marker.empty()) {
    refuse("request_header", facts.ajax_marker);
  }

  // The cookies: none, only ignore-listed ones, or a blocking one.
  if (facts.has_cookie || cookie2) {
    GoogleString detail;
    if (cookie2 || facts.cookie_lines.empty()) {
      // A Cookie2 line, or a cookie the port could not show: blocking.
      out->blocking_cookie = true;
    } else {
      std::vector<const char*> lines;
      lines.reserve(facts.cookie_lines.size());
      for (const GoogleString& line : facts.cookie_lines) {
        lines.push_back(line.c_str());
      }
      std::vector<const char*> additions;
      additions.reserve(config_.ignore_cookie.size());
      for (const GoogleString& addition : config_.ignore_cookie) {
        additions.push_back(addition.c_str());
      }
      PsHtmlCookieSpan spans[kIgnorableCookieValuesMax] = {};
      PsHtmlCookieClass cls = {};
      cls.struct_size = sizeof(cls);
      auto line_at = [&](const PsHtmlCookieSpan& span, StringPiece* text) {
        if (span.line >= facts.cookie_lines.size()) return false;
        const GoogleString& line = facts.cookie_lines[span.line];
        if (span.offset > line.size() ||
            span.length > line.size() - span.offset) {
          return false;
        }
        *text = StringPiece(line).substr(span.offset, span.length);
        return true;
      };
      if (abi->HtmlClassifyCookies(lines.data(), lines.size(),
                                   config_.ignore_cookies ? 1 : 0,
                                   additions.data(), additions.size(), spans,
                                   kIgnorableCookieValuesMax, &cls) != 0) {
        // Fails closed: a request the classifier could not judge is
        // blocking, with no name to give.
        cls = PsHtmlCookieClass();
        cls.cookie_class = kPsHtmlCookiesBlocking;
      }
      if (cls.cookie_class != kPsHtmlCookiesNone &&
          cls.cookie_class != kPsHtmlCookiesIgnorableOnly) {
        // Blocking, or a class this build does not know.
        out->blocking_cookie = true;
        StringPiece name;
        if (cls.malformed != 0) {
          detail = "malformed";
        } else if (cls.ignorable_count > kIgnorableCookieValuesMax) {
          detail = "capacity";
        } else if (cls.first_blocking.length > 0 &&
                   line_at(cls.first_blocking, &name)) {
          detail = name.as_string();
        }
      } else if (cls.cookie_class == kPsHtmlCookiesIgnorableOnly) {
        if (cls.ignorable_count > kIgnorableCookieValuesMax) {
          // More values than the echo check can read: fails closed.
          out->blocking_cookie = true;
          detail = "capacity";
        } else {
          for (size_t i = 0; i < cls.ignorable_count; ++i) {
            StringPiece value;
            if (!line_at(spans[i], &value)) {
              out->blocking_cookie = true;
              break;
            }
            out->ignorable_cookies.emplace_back(
                CookieNameBefore(facts.cookie_lines[spans[i].line],
                                 spans[i].offset),
                value.as_string());
          }
        }
      }
    }
    if (out->blocking_cookie) {
      out->ignorable_cookies.clear();
      // explicit-public lets the library decide on the response instead
      // (public or s-maxage); it never admits Authorization.
      out->cookie_detail = detail;
      out->cookie_refused =
          config_.credentialed != kPsHtmlCredentialedExplicitPublic;
    }
  }
}

void DaemonHtmlEngine::NoteRequestRefused(const DaemonHtmlRequestFacts& facts,
                                          const RequestPolicy& policy,
                                          const GoogleString& reason,
                                          const GoogleString& detail,
                                          const char* mode) {
  if (reason == "auth_request") {
    counters_->refused_auth->Add(1);
  } else if (reason == "request_header") {
    counters_->refused_request_header->Add(1);
  } else {
    auto found = counters_->refused_by_reason.find(reason);
    (found == counters_->refused_by_reason.end() ? counters_->refused_other
                                                 : found->second)
        ->Add(1);
  }
  if (reason == "request_header" && policy.xfp) NoteForwardedProtoMismatch();
  if (reason == "request_header" && policy.rewritten_url) {
    NoteRewrittenUrlRefused();
  }
  // A report-only line takes no serve key, the detail among them.
  const bool report_only = mode != nullptr && strcmp(mode, "report-only") == 0;
  LogServeSide("refuse", reason, facts.host, facts.path, 0, 0, 0, nullptr, -1,
               mode, report_only ? GoogleString() : detail);
}

void DaemonHtmlEngine::NoteRewrittenUrlRefused() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rewritten_url_logged_) return;
    rewritten_url_logged_ = true;
  }
  if (deps_.handler == nullptr) return;
  deps_.handler->Message(
      kWarning,
      "DaemonHtmlCache: a request carried an X-Original-URL that differs from "
      "the request target the page is keyed on, so no page is stored or "
      "served for it (Markdown copies included); a value equal to the target, "
      "as IIS URL Rewrite sends on a rewritten request, is accepted");
}

const char* DaemonHtmlEngine::ServeModeName() const {
  return effective_mode() == DaemonHtmlMode::kServeCachedHtml
             ? "serve-cached-html"
             : "serve-variants";
}

void DaemonHtmlEngine::NoteUnavailable() {
  GoogleString reason;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unavailable_logged_) return;
    unavailable_logged_ = true;
    reason = unavailable_reason_;
  }
  if (deps_.handler != nullptr) {
    deps_.handler->Message(
        kWarning,
        "DaemonHtmlCache: serving is unavailable (%s); running report-only",
        reason.c_str());
  }
}

void DaemonHtmlEngine::ReadSiteEpoch() {
  const DaemonAbi* abi = deps_.substrate.abi;
  bool bound = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    bound = set_bound_;
  }
  if (!bound) return;
  void* cache = Cache();
  if (cache == nullptr) return;
  uint64_t epoch_s = 0;
  const int rc =
      abi->CacheReadSitePurgeEpoch(cache, config_.site.c_str(), &epoch_s);
  oracle_.SetSiteEpoch(rc == kPsOk, rc == kPsOk ? epoch_s : 0);
}

void DaemonHtmlEngine::WriteEpoch(void* cache, uint64 epoch_s, bool* all_ok) {
  const DaemonAbi* abi = deps_.substrate.abi;
  for (const GoogleString& site : config_.epoch_sites) {
    const int rc = abi->CacheWriteSitePurgeEpoch(cache, site.c_str(), epoch_s);
    if (rc != kPsOk) {
      *all_ok = false;
      counters_->epoch_write_failed->Add(1);
    }
  }
}

void DaemonHtmlEngine::ObservePurgeState() {
  const int64 global_ms = oracle_.Observe();
  if (global_ms < 0) return;
  bool bound = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    bound = set_bound_;
  }
  if (!bound || config_.mode < DaemonHtmlMode::kServeVariants) {
    return;
  }
  void* cache = Cache();
  if (cache == nullptr) return;
  {
    // A replaced volume has none of the epochs written to the one before.
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache != epoch_cache_) {
      epoch_cache_ = cache;
      epoch_written_s_ = -1;
    }
    if (global_ms / Timer::kSecondMs <= epoch_written_s_) return;
  }
  const uint64 epoch_s = static_cast<uint64>(global_ms / Timer::kSecondMs);
  RunOn(deps_.runner, [this, cache, epoch_s] {
    bool all_ok = true;
    WriteEpoch(cache, epoch_s, &all_ok);
    if (all_ok) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        epoch_written_s_ =
            std::max(epoch_written_s_, static_cast<int64>(epoch_s));
      }
      oracle_.RaiseSiteEpoch(epoch_s);
    }
  });
}

void DaemonHtmlEngine::OnPurgePollTick() {
  ReadConditions(/*log_change=*/true);
  ObservePurgeState();
}

void DaemonHtmlEngine::OnStatisticsTick() {
  ObservePurgeState();
  ReadSiteEpoch();
  std::map<GoogleString, Unpaired> watched;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!set_bound_) return;
    watched = unpaired_;
  }
  if (watched.empty()) return;
  void* cache = Cache();
  if (cache == nullptr) return;
  RunOn(deps_.runner, [this, cache, watched] {
    const DaemonAbi* abi = deps_.substrate.abi;
    const int64 now_s = deps_.timer->NowMs() / Timer::kSecondMs;
    for (const auto& [identity, key] : watched) {
      const GoogleString& url = key.url;
      ServeReads reads(abi);
      bool done = true;
      if (abi->CacheReadAlternateSite(cache, VolumeUrl(url).c_str(),
                                      key.hostname.c_str(), key.scheme.c_str(),
                                      config_.site.c_str(), kPsSentinelOriginal,
                                      &reads.original) != kPsOk) {
        reads.original = nullptr;
      }
      if (reads.original != nullptr &&
          abi->HeadersSidecarRead(cache, VolumeUrl(url).c_str(),
                                  key.hostname.c_str(), key.scheme.c_str(),
                                  config_.site.c_str(),
                                  &reads.block) == kPsOk &&
          reads.block != nullptr &&
          abi->HeaderBlockPairing(reads.block,
                                  abi->ReadOriginHtmlHash(reads.original)) ==
              kPsSidecarUnpaired) {
        const int64 stored_at = abi->ReadCacheInsertedAt(reads.original);
        const int64 lifetime = abi->ReadOriginMaxAge(reads.original);
        if (now_s - stored_at > lifetime) {
          counters_->sidecar_unpaired_stale->Add(1);
        } else {
          done = false;  // Still inside its lifetime: watch it further.
        }
      }
      if (done) {
        std::lock_guard<std::mutex> lock(mutex_);
        unpaired_.erase(identity);
      }
    }
  });
}

void DaemonHtmlEngine::TrackUnpaired(const GoogleString& url,
                                     const GoogleString& hostname,
                                     const GoogleString& scheme) {
  const GoogleString key = VolumeIdentity(url, hostname, scheme);
  std::lock_guard<std::mutex> lock(mutex_);
  if (unpaired_.size() >= kUnpairedMax && unpaired_.count(key) == 0) return;
  unpaired_[key] = Unpaired{url, hostname, scheme};
}

bool DaemonHtmlEngine::RecentlyRemoved(const GoogleString& key, int64 now_s) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = recently_removed_.find(key);
  if (it == recently_removed_.end()) return false;
  if (now_s - it->second >= kRecentlyRemovedSeconds || now_s < it->second) {
    recently_removed_.erase(it);
    return false;
  }
  return true;
}

void DaemonHtmlEngine::NoteRemoved(const GoogleString& key, int64 now_s) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (recently_removed_.size() >= kRecentlyRemovedMax) {
    // Drop the expired keys first, then the oldest.
    for (auto it = recently_removed_.begin(); it != recently_removed_.end();) {
      if (now_s - it->second >= kRecentlyRemovedSeconds) {
        it = recently_removed_.erase(it);
      } else {
        ++it;
      }
    }
    if (recently_removed_.size() >= kRecentlyRemovedMax) {
      auto oldest = recently_removed_.begin();
      for (auto it = recently_removed_.begin(); it != recently_removed_.end();
           ++it) {
        if (it->second < oldest->second) oldest = it;
      }
      recently_removed_.erase(oldest);
    }
  }
  recently_removed_[key] = now_s;
}

void DaemonHtmlEngine::ForgetRemoved(const GoogleString& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  recently_removed_.erase(key);
}

void DaemonHtmlEngine::Hook(const char* point) {
  if (test_hooks_ != nullptr) test_hooks_->AtPoint(point);
}

void DaemonHtmlEngine::CountFallThrough(const GoogleString& reason) {
  // The stored entry's own record, for either body.
  if (ValueIn(reason, kEntryRecordReasons)) {
    CountHtmlFallThrough(reason);
    return;
  }
  auto it = counters_->fallthrough.find(reason);
  if (it == counters_->fallthrough.end()) {
    it = counters_->fallthrough.find("other");
  }
  it->second->Add(1);
}

void DaemonHtmlEngine::CountHtmlFallThrough(const GoogleString& reason) {
  auto html = counters_->html_fallthrough.find(reason);
  if (html != counters_->html_fallthrough.end()) html->second->Add(1);
}

void DaemonHtmlEngine::CountUnavailable() { counters_->unavailable->Add(1); }

void DaemonHtmlEngine::NoteHitPathInvariant(const char* what) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hit_invariant_logged_) return;
    hit_invariant_logged_ = true;
  }
  if (deps_.handler == nullptr) return;
  deps_.handler->Message(
      kError,
      "DaemonHtmlCache: %s; this must never happen and was refused (logged "
      "once)",
      what);
}

std::unique_ptr<DaemonHtmlRecording> DaemonHtmlEngine::StartRecording(
    const DaemonHtmlRequestFacts& facts, DaemonHtmlPort* port,
    bool* unavailable) {
  if (unavailable != nullptr) *unavailable = false;
  if (config_.mode == DaemonHtmlMode::kOff) return nullptr;
  if (facts.served_from_cache) {
    // The cache answered this request: recording it would store the page
    // again from its own cached bytes and keep it fresh past its lifetime.
    // A port must never start one; refused, counted, logged once.
    counters_->hit_recorded->Add(1);
    NoteHitPathInvariant(
        "a recording was started for a request the cache answered");
    return nullptr;
  }
  if (deps_.substrate.abi == nullptr) {
    if (unavailable != nullptr) *unavailable = true;
    return nullptr;
  }
  const bool serve_mode =
      config_.mode >= DaemonHtmlMode::kServeVariants && config_.agent_optimize;
  bool may_store = false;
  if (serve_mode && config_.mode == DaemonHtmlMode::kServeCachedHtml &&
      effective_mode() != DaemonHtmlMode::kServeCachedHtml) {
    counters_->serve_cached_html_unavailable->Add(1);
  }
  if (serve_mode) NoteServe15Unavailable();
  if (serve_mode) {
    if (serving() && Cache() != nullptr) {
      // Without a port there is nowhere to run the store's blocking work:
      // such a recording only reports.
      may_store = port != nullptr;
    } else {
      counters_->serve_unavailable->Add(1);
      NoteUnavailable();
    }
  }
  if (may_store) {
    // What the engine may store: this site's own requests, on one of its
    // exact names, read the same way by the server and this module.
    if (!IsSiteId(config_.site) || facts.site != config_.site) return nullptr;
    if (!facts.url_agreed) {
      counters_->refused_url_disagree->Add(1);
      LogServeSide("refuse", "url_disagree", facts.host, facts.path, 0, 0, 0,
                   nullptr, -1);
      return nullptr;
    }
    if (!IsVolumePath(facts.path) ||
        !HostExactlyConfigured(facts.host, config_.host_names)) {
      return nullptr;
    }
  } else if (!deps_.substrate.abi->PublishesHtmlStorePolicy()) {
    // A library older than the policy: nothing can be classified.
    if (unavailable != nullptr) *unavailable = true;
    return nullptr;
  }
  RequestPolicy policy;
  EvaluateRequestPolicy(facts, &policy);
  if (policy.applies) {
    // The request policy of C API 1.15, before anything is recorded:
    // credentials, the headers a cache key does not contain (the AJAX
    // markers among them) and the shape of the path.  In report-only (and
    // while serving is unavailable) the guard's refusals refuse as they
    // would when storing, and the credentials reach the library's report
    // through the cookie class, so the report says what serving would do.
    GoogleString reason;
    GoogleString detail;
    policy.Verdict(credential_skip_disabled_ || !may_store, &reason, &detail);
    if (!reason.empty()) {
      NoteRequestRefused(facts, policy, reason, detail,
                         may_store ? ServeModeName() : "report-only");
      return nullptr;
    }
  } else if (!facts.ajax_marker.empty()) {
    // A script's request: the origin may have answered it with a fragment,
    // which must never be kept under the page's key.  In report-only too,
    // so the report says what serving would do.
    counters_->refused_request_header->Add(1);
    if (deps_.substrate.abi->VersionMinor() >= kRequestHeaderReasonMinor) {
      LogServeSide("refuse", "request_header", facts.host, facts.path, 0, 0, 0,
                   nullptr, -1, may_store ? ServeModeName() : "report-only");
    }
    return nullptr;
  }
  std::unique_ptr<DaemonHtmlRecording> recording(
      new DaemonHtmlRecording(this, facts, port, may_store));
  if (policy.applies) {
    // The engine's own reading of the credential lines reaches the
    // library's classification too (the report-only path among them).
    if (policy.auth) recording->facts_.has_authorization = true;
    recording->cookie_classified_ = true;
    recording->blocking_cookie_ = policy.blocking_cookie;
    recording->ignorable_cookies_ = std::move(policy.ignorable_cookies);
  }
  return recording;
}

bool DaemonHtmlEngine::Classify(const DaemonHtmlRecording& r,
                                const DaemonHtmlHeaderLines& view, int mode,
                                bool body_complete, StoreVerdict* out) {
  const DaemonAbi* abi = deps_.substrate.abi;
  std::vector<const char*> names;
  std::vector<const char*> values;
  names.reserve(view.size());
  values.reserve(view.size());
  for (const auto& [name, value] : view) {
    names.push_back(name.c_str());
    values.push_back(value.c_str());
  }
  const DaemonHtmlRequestFacts& f = r.facts_;
  uint32_t request_flags = 0;
  if (f.has_authorization) request_flags |= kStoreRequestHasAuthorization;
  if (f.authenticated_user) request_flags |= kStoreRequestAuthenticatedUser;
  // With C API 1.15 only a blocking cookie counts: a request whose every
  // cookie is on the ignore list in force is anonymous.  Below 1.15 every
  // cookie counts, as the earlier rule has it.
  if (r.cookie_classified_ ? r.blocking_cookie_ : f.has_cookie) {
    request_flags |= kStoreRequestHasCookie;
  }
  if (f.has_cache_control && HasDirective(f.cache_control, "no-store")) {
    request_flags |= kStoreRequestNoStore;
  }
  // The explicit-public policy exists only from 1.15; an older library
  // applies its own earlier cookie rule and is not told.
  if (config_.credentialed == kPsHtmlCredentialedExplicitPublic &&
      abi->HasHtmlServe15Set()) {
    request_flags |= kPsHtmlStoreRequestExplicitPublic;
  }
  int reason = 0;
  char detail[160] = {0};
  size_t detail_len = 0;
  const size_t prefix = std::min(r.body_.size(), kBodyPrefixBytes);
  const int action = abi->HtmlStoreClassify(
      mode, f.method.c_str(), f.path.c_str(), request_flags, r.status_,
      names.data(), values.data(), names.size(), r.body_bytes_,
      body_complete ? 1 : 0, r.body_.data(), prefix,
      static_cast<uint64_t>(std::max<int64>(config_.max_bytes, 0)),
      static_cast<uint32_t>(deps_.timer->NowMs() / Timer::kSecondMs), &reason,
      &out->lifetime_s, &out->flags, &out->cc_flags, &out->sidecar, detail,
      sizeof(detail), &detail_len);
  if (action < 0) return false;
  out->action = action;
  const char* name = abi->HtmlStoreReasonName(reason);
  out->reason = name == nullptr ? "" : name;
  out->detail.assign(detail, std::min(detail_len, sizeof(detail) - 1));
  out->content_type = FirstValue(view, "Content-Type");
  return true;
}

void DaemonHtmlEngine::QueueLine(
    const std::vector<std::pair<const char*, GoogleString>>& fields,
    const GoogleString& dedup_key, int64 now_ms, bool request_path) {
  if (log_ == nullptr) return;
  if (request_path) {
    // What the writer dropped and had to say since the last line.
    const int64 dropped = log_->TakeDropped();
    if (dropped > 0) counters_->log_dropped->Add(dropped);
    if (deps_.handler != nullptr) {
      for (const GoogleString& message : log_->TakeMessages()) {
        deps_.handler->Message(kWarning, "%s", message.c_str());
      }
    }
  }
  if (!log_->Queue(*deps_.substrate.abi, fields, dedup_key, now_ms)) {
    counters_->log_dropped->Add(1);
  }
}

void DaemonHtmlEngine::Report(const DaemonHtmlRecording& r,
                              const StoreVerdict& v, bool classified,
                              const DaemonHtmlHeaderLines& store_view,
                              const DaemonHtmlHeaderLines& final_view,
                              bool serve_line) {
  counters_->evaluated->Add(1);
  if (!classified) {
    counters_->classify_failed->Add(1);
    return;
  }
  const bool would_store = v.would_store();
  if (would_store) {
    counters_->would_store->Add(1);
    counters_->would_store_bytes->Add(static_cast<int64>(r.body_bytes_));
    if (v.no_notify()) counters_->would_store_no_notify->Add(1);
  } else {
    counters_->refused->Add(1);
    auto found = counters_->refused_by_reason.find(v.reason);
    (found == counters_->refused_by_reason.end() ? counters_->refused_other
                                                 : found->second)
        ->Add(1);
  }
  // A store is logged when it is written (Commit).
  if (log_ == nullptr || (serve_line && would_store)) return;

  const DaemonAbi* abi = deps_.substrate.abi;
  const DaemonHtmlRequestFacts& f = r.facts_;
  const GoogleString& bare_reason = v.reason;
  const bool has_detail = !v.detail.empty();
  GoogleString reason_code;
  if (!would_store) {
    reason_code = bare_reason;
    if ((bare_reason == "headers" || bare_reason == "vary") && has_detail) {
      reason_code = StrCat(bare_reason, ":", v.detail);
    }
  }
  GoogleString sidecar_code;
  if ((v.flags & kStoreFlagSidecarEvaluated) != 0) {
    if (v.sidecar == kSidecarEligible) {
      sidecar_code = "eligible_block";
    } else if (v.sidecar == kSidecarFallThrough) {
      sidecar_code = bare_reason == "headers" && has_detail
                         ? StrCat("fall_through:", v.detail)
                         : GoogleString("fall_through");
    } else if (v.sidecar == kSidecarNever) {
      if (bare_reason == "nonce_csp") sidecar_code = "never:nonce_csp";
      if (bare_reason == "nonce_link") sidecar_code = "never:nonce_link";
    }
  }
  GoogleString request;
  if (f.has_authorization) request = "authorization";
  if (f.has_cookie) {
    if (!request.empty()) request.push_back(',');
    request.append("cookie");
  }
  if (request.empty()) request = "none";

  const int64 now_ms = deps_.timer->NowMs();
  const GoogleString path = DaemonHtmlLogPath(f.path);
  const GoogleString action = serve_line ? "refuse"
                              : v.action == kPsHtmlStoreActionWouldStoreNoNotify
                                  ? "would_store_no_notify"
                              : would_store ? "would_store"
                                            : "refuse";
  const bool vhost_added_known = abi->VersionMinor() >= kVhostAddedMinor;
  const DaemonHtmlDecisionLog::Fields fields = {
      {"ts_ms", Integer64ToString(now_ms)},
      {"host", DaemonHtmlIsLogHost(f.host) ? f.host : GoogleString()},
      {"path", path},
      {"mode", serve_line ? ServeModeName() : "report-only"},
      {"action", action},
      {"reason", reason_code},
      {"status", IntegerToString(r.status_)},
      {"size", Integer64ToString(static_cast<int64>(r.body_bytes_))},
      {"lifetime_s", Integer64ToString(v.lifetime_s)},
      {"validators", DaemonHtmlLogValidators(final_view)},
      {"cc", DaemonHtmlLogCc(v.cc_flags, v.flags)},
      {"vary", DaemonHtmlLogVary(final_view)},
      {"sidecar", sidecar_code},
      {"request", request},
      {"personalisation",
       bare_reason == "personalisation_signal" ? v.detail : GoogleString()},
      {"vhost_added", vhost_added_known
                          ? DaemonHtmlVhostAdded(store_view, final_view)
                          : GoogleString()},
  };
  QueueLine(fields,
            StrCat(f.host, "\x1f", path, "\x1f", action, "\x1f", reason_code,
                   "\x1f", sidecar_code),
            now_ms, /*request_path=*/true);
}

void DaemonHtmlEngine::LogServeSide(
    const char* action, const GoogleString& reason, const GoogleString& host,
    const GoogleString& path, int status, uint64 size, uint32 lifetime_s,
    const char* variant, int64 age_s, const char* mode,
    const GoogleString& detail, int agent_class, int serve_scope) {
  if (log_ == nullptr) return;
  const int64 now_ms = deps_.timer->NowMs();
  const GoogleString log_path = DaemonHtmlLogPath(path);
  // The name a refusal is about, a token, never a value: only a library
  // whose grammar has the key (C API 1.15) is given one.
  const bool detail_known =
      deps_.substrate.abi->VersionMinor() >= kRequestHeaderReasonMinor &&
      IsDetailToken(detail);
  const DaemonHtmlDecisionLog::Fields fields = {
      {"ts_ms", Integer64ToString(now_ms)},
      {"host", DaemonHtmlIsLogHost(host) ? host : GoogleString()},
      {"path", log_path},
      {"mode", mode != nullptr ? mode : ServeModeName()},
      {"action", action},
      {"reason", reason},
      {"status", status > 0 ? IntegerToString(status) : GoogleString()},
      {"size",
       size > 0 ? Integer64ToString(static_cast<int64>(size)) : GoogleString()},
      {"lifetime_s",
       lifetime_s > 0 ? Integer64ToString(lifetime_s) : GoogleString()},
      {"variant", variant == nullptr ? GoogleString() : GoogleString(variant)},
      {"age_s", age_s >= 0 ? Integer64ToString(age_s) : GoogleString()},
      {"detail", detail_known ? detail : GoogleString()},
      // A served page's scope and the request's agent class, which only a
      // library whose grammar has the keys (C API 1.15) is given.
      {"scope",
       serve_scope >= 0 &&
               deps_.substrate.abi->VersionMinor() >= kRequestHeaderReasonMinor
           ? GoogleString(serve_scope == kPsHtmlServeScopeAll ? "all"
                                                              : "agents")
           : GoogleString()},
      {"agent_class",
       agent_class >= 0 && agent_class <= kPsHtmlAgentClassMax &&
               deps_.substrate.abi->VersionMinor() >= kRequestHeaderReasonMinor
           ? IntegerToString(agent_class)
           : GoogleString()},
  };
  // Only the request path takes what the writer has to say; a commit may
  // run on a pool thread.
  QueueLine(fields,
            StrCat(host, "\x1f", log_path, "\x1f", action, "\x1f", reason),
            now_ms, /*request_path=*/false);
}

void DaemonHtmlEngine::FinishRecording(DaemonHtmlRecording* r, bool aborted) {
  const DaemonAbi* abi = deps_.substrate.abi;
  if (abi == nullptr) return;
  if (r->may_store_) {
    if (aborted) return;
    if (r->budget_refused_) {
      counters_->refused_budget->Add(1);
      LogServeSide("refuse", "budget", r->facts_.host, r->facts_.path,
                   r->status_, r->body_bytes_, 0, nullptr, -1);
      return;
    }
  }
  const DaemonHtmlHeaderLines& baseline =
      r->has_baseline_ ? r->baseline_ : r->origin_;
  const DaemonHtmlHeaderLines& store_point =
      r->has_store_lines_ ? r->store_lines_ : baseline;
  const DaemonHtmlHeaderLines& final_point =
      r->has_final_lines_ ? r->final_lines_ : store_point;
  DaemonHtmlHeaderLines store_view =
      BuildView(r->origin_, baseline, store_point, r->module_, false);
  const DaemonHtmlHeaderLines final_view =
      BuildView(r->origin_, baseline, final_point, r->module_, true);

  const int mode = !r->may_store_ ? kPsHtmlCacheReportOnly
                   : effective_mode() == DaemonHtmlMode::kServeCachedHtml
                       ? kPsHtmlCacheServeCachedHtml
                       : kPsHtmlCacheServeVariants;
  // A report reads whether the body ended; a store needs it whole (an
  // aborted response was never classified for one).
  const bool body_complete = r->may_store_ || (r->body_ended_ && !aborted);
  StoreVerdict store;
  StoreVerdict fin;
  const bool store_ok = Classify(*r, store_view, mode, body_complete, &store);
  const bool final_ok = Classify(
      *r,
      r->may_store_ ? FinalViewForStore(final_view, store_view) : final_view,
      mode, body_complete, &fin);
  if (!r->may_store_) {
    // Report-only: the final view's verdict, the one the report has always
    // read.
    Report(*r, fin, final_ok, store_view, final_view, /*serve_line=*/false);
    return;
  }
  // A body over the cap was not kept whole: never stored, whatever a
  // verdict says (the library refuses it by its byte count), so it is
  // reported as refused for its size.
  if (store_ok && final_ok && r->over_size_ &&
      (store.stores() || fin.stores())) {
    StoreVerdict too_large = fin;
    too_large.action = kPsHtmlStoreActionRefuse;
    too_large.reason = "size";
    too_large.detail.clear();
    Report(*r, too_large, true, store_view, final_view, /*serve_line=*/true);
    return;
  }
  // A store needs both views; the refusal, when there is one, is the view
  // that refused (the store view first).
  Report(*r, store_ok && !store.stores() ? store : fin, store_ok && final_ok,
         store_view, final_view, /*serve_line=*/true);
  if (!store_ok || !final_ok) return;

  GoogleString url;
  GoogleString hostname;
  CacheKey(r->facts_, &url, &hostname);
  if (store.stores() && fin.stores()) {
    StoreVerdict verdict = store;
    if (fin.action == kPsHtmlStoreActionStoreNoNotify) {
      verdict.action = kPsHtmlStoreActionStoreNoNotify;
    }
    verdict.lifetime_s = std::min(store.lifetime_s, fin.lifetime_s);
    // The stored entry takes the more restrictive reading of the two
    // views' cache directives.
    verdict.flags = MergeStoreFlags(store.flags, fin.flags);
    verdict.cc_flags = MergeCcFlags(store.cc_flags, fin.cc_flags);
    const bool agent_request =
        deps_.substrate.abi->WantsAgentMarkdown(
            JoinCollection(r->facts_.accept_lines, ", ").c_str()) == 1;
    // The inbound Age as the origin sent it: a server stage that drops the
    // line does not make the entry younger.
    std::vector<GoogleString> ages;
    for (const auto& [name, value] : r->origin_) {
      if (StringCaseEqual(name, "Age")) ages.push_back(value);
    }
    // The body and its budget move to the commit.  The reservation is owned,
    // so it is released when the commit has run or is dropped unrun.
    std::shared_ptr<BudgetHold> hold =
        std::make_shared<BudgetHold>(deps_.budget, r->reserved_);
    r->reserved_ = 0;
    DaemonHtmlRequestFacts facts = r->facts_;
    GoogleString body;
    body.swap(r->body_);
    r->port_->RunBlocking(
        DaemonHtmlCommit([this, facts = std::move(facts), url, hostname,
                          verdict, store_view = std::move(store_view),
                          ages = std::move(ages), body = std::move(body), hold,
                          agent_request, ignorable = r->ignorable_cookies_] {
          Commit(facts, url, hostname, verdict, store_view, ages, body,
                 agent_request, ignorable);
        }));
    return;
  }
  MaybeRemoveOnRefusal(r->facts_, url, hostname, r->status_, store, fin,
                       r->port_);
}

void DaemonHtmlEngine::Commit(
    const DaemonHtmlRequestFacts& facts, const GoogleString& url,
    const GoogleString& hostname, const StoreVerdict& verdict,
    const DaemonHtmlHeaderLines& store_view,
    const std::vector<GoogleString>& ages, const GoogleString& body,
    bool agent_request,
    const std::vector<std::pair<GoogleString, GoogleString>>&
        ignorable_cookies) {
  const DaemonAbi* abi = deps_.substrate.abi;
  void* cache = Cache();
  if (cache == nullptr) return;
  const char* site = config_.site.c_str();
  const bool policy15 = abi->HasHtmlServe15Set();
  // The record this store gives the entry: recorded under the credentialed
  // policy, anonymously or not, under this vhost's stamp.
  const bool anonymous =
      (verdict.flags & kPsHtmlStoreFlagRequestAnonymous) != 0;
  bool upgrade_restore = false;
  uint8_t hash[kPsHtmlHashBytes];
  Sha256(body, hash);
  const GoogleString identity = VolumeIdentity(url, hostname, facts.scheme);
  // A sampled would-serve page whose origin answer is now recorded: did the
  // origin answer with the bytes the cache would have served?
  CheckShadowBody(identity, hash, facts.capture_start_ms / Timer::kSecondMs);

  // The insert time: computed by the library alone, from the request's
  // start and the inbound Age.  A refused Age stores nothing.
  std::vector<const char*> age_lines;
  age_lines.reserve(ages.size());
  for (const GoogleString& a : ages) age_lines.push_back(a.c_str());
  const GoogleString& content_type = verdict.content_type;
  PsWriteParams params;
  abi->WriteParamsInit(&params);
  if (abi->HtmlStoreWriteParams(
          verdict.action, verdict.lifetime_s, verdict.flags, verdict.cc_flags,
          /*sidecar_stored=*/1, facts.capture_start_ms, age_lines.data(),
          age_lines.size(), body.size(), content_type.c_str(), &params) != 0) {
    return;
  }
  const uint32 inserted_at = params.cache_inserted_at;
  // An unsafe request answered for this key in this process after the
  // capture started: what was captured may already be out of date, and the
  // entry would only be removed again.  Nothing is written.
  if (oracle_.UnsafeFenced(url, inserted_at)) {
    counters_->purge_race_refused->Add(1);
    LogServeSide("purged", "purge_race", facts.host, facts.path, 200,
                 body.size(), verdict.lifetime_s, nullptr, -1);
    return;
  }

  // The re-store skip: only an unchanged, paired, fresh entry with more than
  // half its lifetime left, or a newer one, is kept as it is.
  bool hash_changed = true;
  // The key held a fresh entry with another body: the origin changed the
  // page within its lifetime (counted when this store is kept).
  bool fresh_body_changed = false;
  {
    ServeReads existing(abi);
    if (abi->CacheReadAlternateSite(cache, VolumeUrl(url).c_str(),
                                    hostname.c_str(), facts.scheme.c_str(),
                                    site, kPsSentinelOriginal,
                                    &existing.original) != kPsOk) {
      existing.original = nullptr;
    }
    if (existing.original != nullptr) {
      const uint32 stored_at = abi->ReadCacheInsertedAt(existing.original);
      const uint32 existing_lifetime = abi->ReadOriginMaxAge(existing.original);
      // An entry an unsafe request fenced counts as purged here too: it is
      // never kept as unchanged.
      if (!oracle_.Purged(url, stored_at)) {
        const uint8_t* old_hash = abi->ReadOriginHtmlHash(existing.original);
        const bool same_body = old_hash != nullptr && !IsZeroHash(old_hash) &&
                               memcmp(old_hash, hash, kPsHtmlHashBytes) == 0;
        hash_changed = !same_body;
        const int64 capture_s = facts.capture_start_ms / Timer::kSecondMs;
        fresh_body_changed = !same_body && old_hash != nullptr &&
                             !IsZeroHash(old_hash) &&
                             capture_s >= static_cast<int64>(stored_at) &&
                             capture_s - static_cast<int64>(stored_at) <
                                 static_cast<int64>(existing_lifetime);
        // The key is present again (written here or by a peer): a later
        // refusal must read and remove it, whatever this process removed.
        ForgetRemoved(VolumeIdentity(url, hostname, facts.scheme));
        const char* stored_ct = abi->ReadOriginContentType(existing.original);
        // With C API 1.15 an entry is kept only when its record matches the
        // one this store writes: recorded under the credentialed policy, with
        // the same anonymous state and the same policy stamp.  A pre-1.15
        // entry, a mismatched one or one recorded for another credential
        // state is always written again, so it never stays unservable.
        bool record_matches = true;
        if (policy15) {
          const uint8_t flags = abi->ReadFlags(existing.original);
          const bool v2 = (flags & kPsFlagHtmlStoreV2) != 0;
          upgrade_restore = !v2;
          record_matches =
              v2 && ((flags & kPsFlagAnonymousRecorded) != 0) == anonymous;
          if (record_matches) {
            void* block = nullptr;
            uint64_t stored_stamp = 0;
            record_matches =
                abi->HeadersSidecarRead(cache, VolumeUrl(url).c_str(),
                                        hostname.c_str(), facts.scheme.c_str(),
                                        site, &block) == kPsOk &&
                block != nullptr &&
                abi->HeaderBlockPolicyStamp(block, &stored_stamp) == 1 &&
                stored_stamp == config_.policy_stamp;
            if (block != nullptr) abi->HeaderBlockFree(block);
          }
        }
        bool skip = record_matches && stored_at > inserted_at;
        if (!skip && record_matches && same_body &&
            abi->ReadOriginMaxAge(existing.original) == verdict.lifetime_s &&
            abi->ReadOriginCcFlags(existing.original) == verdict.cc_flags &&
            stored_ct != nullptr && verdict.content_type == stored_ct) {
          const int64 capture_s = facts.capture_start_ms / Timer::kSecondMs;
          const int64 age = capture_s - static_cast<int64>(stored_at);
          if (age >= 0 && age * 2 < static_cast<int64>(verdict.lifetime_s) &&
              abi->HeadersSidecarRead(cache, VolumeUrl(url).c_str(),
                                      hostname.c_str(), facts.scheme.c_str(),
                                      site, &existing.block) == kPsOk &&
              existing.block != nullptr &&
              abi->HeaderBlockPairing(existing.block, old_hash) ==
                  kPsSidecarPaired) {
            DaemonHtmlHeaderLines stored_block;
            const size_t n = abi->HeaderBlockCount(existing.block);
            for (size_t i = 0; i < n; ++i) {
              const char* name = nullptr;
              const char* value = nullptr;
              size_t name_len = 0;
              size_t value_len = 0;
              if (abi->HeaderBlockGet(existing.block, i, &name, &name_len,
                                      &value, &value_len) == kPsOk) {
                stored_block.emplace_back(GoogleString(name, name_len),
                                          GoogleString(value, value_len));
              }
            }
            skip = SameMultiset(HeaderBlockOf(stored_block),
                                HeaderBlockOf(store_view));
          }
        }
        if (skip) {
          counters_->restore_skipped->Add(1);
          return;
        }
      }
    }
  }

  // An ignore-listed cookie's value that the page or a stored header
  // repeats means the origin answered for this visitor: never stored.  Only
  // a store that writes is checked (a skip writes nothing).
  if (!ignorable_cookies.empty()) {
    GoogleString echoed;
    if (EchoedCookie(ignorable_cookies, body, store_view, &echoed)) {
      counters_->refused_by_reason["cookie_echo"]->Add(1);
      LogServeSide("refuse", "cookie_echo", facts.host, facts.path, 200,
                   body.size(), verdict.lifetime_s, nullptr, -1, nullptr,
                   echoed);
      return;
    }
  }

  // The header block first (a failure stores nothing), then the entry.
  std::vector<const char*> names;
  std::vector<const char*> values;
  names.reserve(store_view.size());
  values.reserve(store_view.size());
  for (const auto& [name, value] : store_view) {
    names.push_back(name.c_str());
    values.push_back(value.c_str());
  }
  PsSidecarWriteParams sidecar = {};
  sidecar.struct_size = SidecarWriteParamsSize(*abi);
  if (policy15) {
    // Header block format 3: the policy stamp, which a serve compares, and
    // the origin's Cache-Control and Expires lines.
    sidecar.write_flags = kPsSidecarWritePolicyBlock;
    sidecar.policy_stamp = config_.policy_stamp;
  }
  const GoogleString volume_url = VolumeUrl(url);
  sidecar.url = volume_url.c_str();
  sidecar.hostname = hostname.c_str();
  sidecar.scheme = facts.scheme.c_str();
  sidecar.site = site;
  sidecar.origin_html_hash = hash;
  int sidecar_verdict = 0;
  int sidecar_stored = 0;
  // A paired write keeps a block even with nothing to carry, so a block
  // that was not kept is a refusal: nothing is stored.
  if (abi->CacheWriteHeadersSidecarSite(
          cache, &sidecar, names.data(), values.data(), names.size(),
          &sidecar_verdict, &sidecar_stored) != kPsOk ||
      sidecar_stored == 0) {
    counters_->sidecar_write_failed->Add(1);
    return;
  }
  params.origin_html_hash = hash;
  void* handle = nullptr;
  if (abi->CacheWriteOriginalSite(cache, VolumeUrl(url).c_str(),
                                  hostname.c_str(), facts.scheme.c_str(), site,
                                  &params, &handle) != kPsOk ||
      handle == nullptr) {
    TrackUnpaired(url, hostname, facts.scheme);
    return;
  }
  if (abi->WriteData(handle, body.data(), body.size()) != kPsOk) {
    abi->WriteAbort(handle);
    TrackUnpaired(url, hostname, facts.scheme);
    return;
  }
  if (abi->WriteClose(handle) != kPsOk) {
    TrackUnpaired(url, hostname, facts.scheme);
    return;
  }
  ForgetRemoved(identity);
  Hook(kAfterCommit);

  // The fence: a purge that covers the insert time and became visible while
  // the response was on its way removes what was just written.
  if (!oracle_.recheck_disabled() && oracle_.Purged(url, inserted_at)) {
    const int rc =
        abi->CacheRemoveSite(cache, VolumeUrl(url).c_str(), hostname.c_str(),
                             facts.scheme.c_str(), site);
    if (rc == kPsOk) counters_->removed->Add(1);
    counters_->purge_race_refused->Add(1);
    LogServeSide("purged", "purge_race", facts.host, facts.path, 200,
                 body.size(), verdict.lifetime_s, nullptr, -1);
    return;
  }
  Hook(kAfterRecheck);
  counters_->stored->Add(1);
  if (upgrade_restore) counters_->upgrade_restores->Add(1);
  if (fresh_body_changed) counters_->fresh_body_changed->Add(1);
  LogServeSide("stored", GoogleString(), facts.host, facts.path, 200,
               body.size(), verdict.lifetime_s, nullptr, -1);
  if (verdict.action == kPsHtmlStoreActionStore && hash_changed) {
    Notify(facts, url, hostname, agent_request);
  }
}

void DaemonHtmlEngine::MaybeRemoveOnRefusal(
    const DaemonHtmlRequestFacts& facts, const GoogleString& url,
    const GoogleString& hostname, int status, const StoreVerdict& store,
    const StoreVerdict& final_view, DaemonHtmlPort* port) {
  const bool clean_request =
      facts.method == "GET" && facts.url_agreed && !facts.has_authorization &&
      !facts.authenticated_user && !facts.has_cookie &&
      !facts.has_if_none_match && !facts.has_if_modified_since &&
      !facts.has_if_match && !facts.has_if_unmodified_since &&
      !facts.has_if_range && !facts.has_range &&
      !(facts.has_cache_control &&
        HasDirective(facts.cache_control, "no-store"));
  if (!clean_request) return;
  // A response-side refusal in either view removes.
  auto removing = [](const StoreVerdict& v) {
    return !v.stores() && ValueIn(v.reason, kRemovingReasons) &&
           v.detail != "request";
  };
  const bool removing_response =
      (status == 200 && (removing(store) || removing(final_view))) ||
      status == 404 || status == 410;
  if (!removing_response) return;
  const int64 now_s = deps_.timer->NowMs() / Timer::kSecondMs;
  const GoogleString scheme = facts.scheme;
  const GoogleString identity = VolumeIdentity(url, hostname, scheme);
  if (RecentlyRemoved(identity, now_s)) return;
  port->RunBlocking(
      DaemonHtmlCommit([this, url, hostname, scheme, identity, now_s] {
        const DaemonAbi* abi = deps_.substrate.abi;
        void* cache = Cache();
        if (cache == nullptr) return;
        const char* site = config_.site.c_str();
        ServeReads existing(abi);
        if (abi->CacheReadAlternateSite(
                cache, VolumeUrl(url).c_str(), hostname.c_str(), scheme.c_str(),
                site, kPsSentinelOriginal, &existing.original) != kPsOk) {
          existing.original = nullptr;
        }
        if (existing.original != nullptr) {
          const int rc =
              abi->CacheRemoveSite(cache, VolumeUrl(url).c_str(),
                                   hostname.c_str(), scheme.c_str(), site);
          if (rc != kPsOk && rc != kPsErrNotFound) return;
          if (rc == kPsOk) counters_->removed->Add(1);
          counters_->removed_on_refusal->Add(1);
        }
        NoteRemoved(identity, now_s);
      }));
}

void DaemonHtmlEngine::Notify(const DaemonHtmlRequestFacts& facts,
                              const GoogleString& url,
                              const GoogleString& hostname,
                              bool agent_request) {
  const int64 now_s = deps_.timer->NowMs() / Timer::kSecondMs;
  if (deps_.limiter != nullptr &&
      !deps_.limiter->AdmitAsk(StrCat("html\n", config_.site, "\n", url,
                                      agent_request ? "\nagent" : "\nplain"),
                               now_s)) {
    counters_->notify_limited->Add(1);
    return;
  }
  ModuleNotifyInput input;
  input.url = url;
  input.hostname = hostname;
  input.scheme = facts.scheme;
  input.site = config_.site;
  input.content_type = kPsContentHtml;
  input.option_context = facts.option_context;
  input.option_signature = facts.option_signature;
  input.html_stored = true;
  input.agent_request = agent_request;
  ModuleNotifyParams built;
  if (!BuildModuleNotifyParams(*deps_.substrate.abi, input, &built)) return;
  if (deps_.substrate.abi->NotifyWorker(deps_.substrate.notify_socket.c_str(),
                                        &built.params()) == kPsOk) {
    counters_->notify_sent->Add(1);
  }
}

// ---------------------------------------------------------------------------
// The serve

// What one serve carries from PrepareServe through Read to FinishServe.
struct DaemonHtmlPendingServe::State {
  explicit State(const DaemonAbi* abi) : reads(abi) {}

  enum Kind {
    kDecided,   // `result` is the answer; nothing to read
    kMarkdown,  // a request that prefers Markdown
    kHtml,      // serve-cached-html: a request without that preference
    kShadow,    // a pre-gated request sampled: read and classified as `all`
  };
  Kind kind = kDecided;
  bool read_done = false;

  DaemonHtmlEngine* engine = nullptr;
  DaemonHtmlRequestFacts facts;
  DaemonHtmlPort* port = nullptr;
  DaemonHtmlServeResult result;
  DaemonHtmlEngine::RequestPolicy policy;
  bool cached_html = false;
  // serve-cached-html on a port that answers with the stored page.
  bool serves_page = false;
  GoogleString accept;
  bool has_accept = false;

  // The volume handle, asked for once, and the request's key.
  void* cache = nullptr;
  GoogleString url;
  GoogleString hostname;

  // What Read found.
  ServeReads reads;
  bool sidecar_present = false;
  bool sidecar_unpaired = false;
  const uint8_t* stamp = nullptr;  // the original's body hash, in `reads`
};

DaemonHtmlPendingServe::DaemonHtmlPendingServe() = default;
DaemonHtmlPendingServe::~DaemonHtmlPendingServe() = default;
DaemonHtmlPendingServe::DaemonHtmlPendingServe(
    DaemonHtmlPendingServe&&) noexcept = default;
DaemonHtmlPendingServe& DaemonHtmlPendingServe::operator=(
    DaemonHtmlPendingServe&&) noexcept = default;

bool DaemonHtmlPendingServe::needs_read() const {
  return state_ != nullptr && state_->kind != State::kDecided &&
         !state_->read_done;
}

void DaemonHtmlPendingServe::Read() {
  if (!needs_read()) return;
  state_->engine->ReadForServe(state_.get());
  state_->read_done = true;
}

DaemonHtmlServeResult DaemonHtmlEngine::Serve(
    const DaemonHtmlRequestFacts& facts, DaemonHtmlPort* port) {
  PendingServe pending = PrepareServe(facts, port);
  pending.Read();
  return FinishServe(std::move(pending));
}

DaemonHtmlPendingServe DaemonHtmlEngine::PrepareServe(
    const DaemonHtmlRequestFacts& facts, DaemonHtmlPort* port) {
  using State = DaemonHtmlPendingServe::State;
  DaemonHtmlPendingServe pending;
  pending.state_ = std::make_unique<State>(deps_.substrate.abi);
  State& s = *pending.state_;
  s.engine = this;
  s.port = port;
  DaemonHtmlServeResult& result = s.result;
  result.reason = "mode";
  // The serve gate: nothing is read before every part of it holds.
  if (config_.mode < DaemonHtmlMode::kServeVariants ||
      !config_.agent_optimize || !IsSiteId(config_.site) ||
      facts.site != config_.site) {
    return pending;
  }
  if (!serving()) {
    NoteUnavailable();
    return pending;
  }
  result.reason = "other";
  // Main requests only: a subrequest or an internal redirect goes on as it
  // is, for either body.
  if (facts.internal) return pending;
  if (!facts.options_enabled_and_allowed || !IsVolumePath(facts.path) ||
      !HostExactlyConfigured(facts.host, config_.host_names)) {
    return pending;
  }
  const DaemonAbi* abi = deps_.substrate.abi;
  s.accept = JoinCollection(facts.accept_lines, ", ");
  s.has_accept = !facts.accept_lines.empty();
  s.cached_html = effective_mode() == DaemonHtmlMode::kServeCachedHtml;
  s.serves_page = s.cached_html && port != nullptr && port->ServesAsOrigin();
  const bool markdown_request = abi->WantsAgentMarkdown(s.accept.c_str()) == 1;
  if (!markdown_request && !s.serves_page) {
    // Only a Markdown request is looked up, unless this is serve-cached-html
    // on a port that answers with the stored page: nothing is read, judged
    // or counted for another one.
    result.reason = "";  // Not a Markdown request at all.
    return pending;
  }
  if (config_.mode == DaemonHtmlMode::kServeCachedHtml && !s.cached_html) {
    // The library lacks the 1.15 set: this runs as serve-variants.
    counters_->serve_cached_html_unavailable->Add(1);
    NoteServe15Unavailable();
  }
  const bool agent_classed =
      facts.agent_class >= 1 && facts.agent_class <= kPsHtmlAgentClassMax;
  if (!markdown_request) {
    // The HTML lookup (serve-cached-html): GET and HEAD only, the URL
    // agreed; nothing counted under the Markdown counters.
    if (facts.method != "GET" && facts.method != "HEAD") {
      result.reason = "method";
      return pending;
    }
    if (!facts.url_agreed) {
      result.reason = "url_disagree";
      return pending;
    }
    if (agent_classed) counters_->agent_html_requests->Add(1);
  } else if (!facts.url_agreed) {
    // Under the Markdown counters only for a Markdown request, so the
    // counter means one.
    result.reason = "url_disagree";
    CountFallThrough(result.reason);
    return pending;
  }
  EvaluateRequestPolicy(facts, &s.policy);
  if (s.policy.applies) {
    // The request policy of C API 1.15, before any read and for both
    // bodies: credentials, the headers a cache key does not contain (the
    // AJAX markers among them) and the shape of the path.
    GoogleString reason;
    GoogleString detail;
    s.policy.Verdict(credential_skip_disabled_, &reason, &detail);
    if (!reason.empty()) {
      NoteRequestRefused(facts, s.policy, reason, detail, nullptr);
      result.reason = reason;
      if (markdown_request) CountFallThrough(result.reason);
      return pending;
    }
  } else if (!facts.ajax_marker.empty()) {
    // A script's request is never answered from the cache: it may want a
    // fragment where the cache holds the page.  (Below 1.15 only a Markdown
    // request reaches here.)
    result.reason = "request_header";
    CountFallThrough(result.reason);
    if (abi->VersionMinor() >= kRequestHeaderReasonMinor) {
      LogServeSide("refuse", "request_header", facts.host, facts.path, 0, 0, 0,
                   nullptr, -1);
    }
    return pending;
  }
  State::Kind kind = markdown_request ? State::kMarkdown : State::kHtml;
  if (!markdown_request && config_.serve_scope != kPsHtmlServeScopeAll &&
      !agent_classed) {
    // The pre-gate: under `agents`, a request that neither prefers Markdown
    // nor comes from a known agent is never read (the library would say
    // `scope`).  It can only skip, never serve.  One in kShadowSampleEvery
    // is read and classified as `all` to measure what `all` would serve,
    // and falls through all the same.
    result.reason = "scope";
    CountHtmlFallThrough(result.reason);
    const uint64 n = pregated_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % kShadowSampleEvery != 0) return pending;
    kind = State::kShadow;
  }
  s.cache = Cache();
  if (s.cache == nullptr) return pending;
  s.facts = facts;
  CacheKey(facts, &s.url, &s.hostname);
  s.kind = kind;
  return pending;
}

void DaemonHtmlEngine::ReadForServe(DaemonHtmlPendingServe::State* s) {
  const DaemonAbi* abi = deps_.substrate.abi;
  const char* site = config_.site.c_str();
  const char* scheme = s->facts.scheme.c_str();
  const GoogleString volume_url = VolumeUrl(s->url);
  auto read = [&](uint8_t id, void** out) {
    if (abi->CacheReadAlternateSite(s->cache, volume_url.c_str(),
                                    s->hostname.c_str(), scheme, site, id,
                                    out) != kPsOk) {
      *out = nullptr;
    }
  };
  // The HTML lookup reads the page alone (and its header block): never the
  // Markdown copy or its marker.
  read(kPsSentinelOriginal, &s->reads.original);
  if (s->kind == DaemonHtmlPendingServe::State::kMarkdown) {
    read(kPsSentinelAgentMarkdown, &s->reads.markdown);
    read(kPsSentinelMarkdownWithheld, &s->reads.withheld);
  }
  s->stamp = s->reads.original == nullptr
                 ? nullptr
                 : abi->ReadOriginHtmlHash(s->reads.original);
  if (s->reads.original != nullptr &&
      abi->HeadersSidecarRead(s->cache, volume_url.c_str(), s->hostname.c_str(),
                              scheme, site, &s->reads.block) == kPsOk &&
      s->reads.block != nullptr) {
    if (abi->HeaderBlockPairing(s->reads.block, s->stamp) != kPsSidecarPaired) {
      s->sidecar_unpaired = true;
    } else {
      s->sidecar_present = true;
    }
  }
}

DaemonHtmlServeResult DaemonHtmlEngine::FinishServe(PendingServe pending) {
  using State = DaemonHtmlPendingServe::State;
  if (pending.state_ == nullptr) return DaemonHtmlServeResult();
  State* s = pending.state_.get();
  if (s->kind == State::kDecided) return s->result;
  // A port that skipped the read: read now, inline.
  if (!s->read_done) pending.Read();
  if (s->sidecar_unpaired) {
    TrackUnpaired(s->url, s->hostname, s->facts.scheme);
    if (s->kind == State::kShadow) return s->result;  // still `scope`
    s->result.reason = "sidecar_unpaired";
    if (s->kind == State::kMarkdown) CountFallThrough(s->result.reason);
    return s->result;
  }
  return s->kind == State::kMarkdown ? FinishMarkdownServe(s)
                                     : FinishHtmlServe(s);
}

void DaemonHtmlEngine::FillServeRequest(const DaemonHtmlRequestFacts& facts,
                                        const RequestPolicy& policy,
                                        bool cached_html, int64 now_s,
                                        PsHtmlServeRequest* request) const {
  const DaemonAbi* abi = deps_.substrate.abi;
  uint32_t request_flags = 0;
  if (facts.has_authorization) {
    request_flags |= kPsHtmlServeRequestHasAuthorization;
  }
  if (facts.authenticated_user) {
    request_flags |= kPsHtmlServeRequestAuthenticatedUser;
  }
  // With C API 1.15 only a blocking cookie counts (see Classify).
  if (policy.applies ? policy.blocking_cookie : facts.has_cookie) {
    request_flags |= kPsHtmlServeRequestHasCookie;
  }
  if (facts.has_range) request_flags |= kPsHtmlServeRequestHasRange;
  if (facts.has_if_range) request_flags |= kPsHtmlServeRequestHasIfRange;
  if (facts.has_origin) request_flags |= kPsHtmlServeRequestHasOrigin;
  if (abi->HasHtmlServe15Set()) {
    if (config_.credentialed == kPsHtmlCredentialedExplicitPublic) {
      request_flags |= kPsHtmlServeRequestExplicitPublic;
    }
    if (cached_html) {
      // Never a validation step here: a not-fresh entry falls through.
      request_flags |= kPsHtmlServeRequestFreshOnly;
      // A conditional as the client sent it (this module's own entity tags
      // included): the library sends it to the origin for HTML.
      if (facts.has_if_none_match || facts.has_if_modified_since ||
          facts.has_if_match || facts.has_if_unmodified_since) {
        request_flags |= kPsHtmlServeRequestConditional;
      }
    }
  }
  *request = PsHtmlServeRequest();
  request->struct_size = HtmlServeRequestSize(*abi);
  // effective_mode() is serve-cached-html only with the 1.15 set bound.
  request->mode =
      cached_html ? kPsHtmlCacheServeCachedHtml : kPsHtmlCacheServeVariants;
  request->method = facts.method.c_str();
  request->cache_control =
      CStrOrNull(facts.has_cache_control, facts.cache_control);
  request->pragma = CStrOrNull(facts.has_pragma, facts.pragma);
  request->if_none_match =
      CStrOrNull(facts.has_if_none_match, facts.if_none_match);
  request->request_flags = request_flags;
  request->now_seconds = static_cast<uint32_t>(now_s);
  request->serve_scope = config_.serve_scope;
  request->agent_class = facts.agent_class;
  // Always sent, so the cap holds whatever the library's default.
  request->html_max_lifetime_s = config_.max_age_s;
  request->policy_stamp = config_.policy_stamp;
}

bool DaemonHtmlEngine::CredentialedForbids(
    const RequestPolicy& policy, const PsHtmlStoredFacts& stored) const {
  if (!policy.applies) return false;
  const bool explicit_public =
      config_.credentialed == kPsHtmlCredentialedExplicitPublic;
  const bool request_credentialed =
      policy.auth || (policy.blocking_cookie && !explicit_public);
  const bool entry_forbidden =
      (stored.flags & kPsHtmlFactsStoreV2) == 0 ||
      (!explicit_public &&
       (stored.flags & kPsHtmlFactsAnonymousRecorded) == 0) ||
      stored.entry_policy_stamp != config_.policy_stamp;
  return request_credentialed || entry_forbidden;
}

DaemonHtmlServeResult DaemonHtmlEngine::FinishMarkdownServe(
    DaemonHtmlPendingServe::State* s) {
  const DaemonAbi* abi = deps_.substrate.abi;
  const DaemonHtmlRequestFacts& facts = s->facts;
  DaemonHtmlPort* port = s->port;
  ServeReads& reads = s->reads;
  const GoogleString& url = s->url;
  const GoogleString& hostname = s->hostname;
  const RequestPolicy& policy = s->policy;
  const bool cached_html = s->cached_html;
  DaemonHtmlServeResult result = s->result;
  const bool sidecar_present = s->sidecar_present;
  const uint8_t* stamp = s->stamp;
  auto read = [&](uint8_t id, void** out) {
    if (abi->CacheReadAlternateSite(s->cache, VolumeUrl(url).c_str(),
                                    hostname.c_str(), facts.scheme.c_str(),
                                    config_.site.c_str(), id, out) != kPsOk) {
      *out = nullptr;
    }
  };
  // The codings the request accepts for the copy, most preferred first
  // (C API 1.16): the coded copies read are those it prefers to the plain
  // one.  Without the 1.16 set it is the plain copy alone, as before.
  const bool coded_set = abi->HasHtmlServe16Set();
  bool identity_ok = true;
  bool encoding_none = false;
  std::vector<int> coded_order;
  if (coded_set) {
    std::vector<const char*> lines;
    lines.reserve(facts.accept_encoding_lines.size());
    for (const GoogleString& line : facts.accept_encoding_lines) {
      lines.push_back(line.c_str());
    }
    int order[3] = {kPsMarkdownEncodingIdentity, kPsMarkdownEncodingIdentity,
                    kPsMarkdownEncodingIdentity};
    const int n = abi->MarkdownSelectEncoding(
        lines.empty() ? nullptr : lines.data(), lines.size(), order);
    if (n == 0) {
      identity_ok = false;
      encoding_none = true;
    } else if (n > 0 && n <= 3) {
      identity_ok = false;
      for (int i = 0; i < n && !identity_ok; ++i) {
        if (order[i] == kPsMarkdownEncodingIdentity) {
          identity_ok = true;
        } else if ((order[i] == kPsMarkdownEncodingGzip ||
                    order[i] == kPsMarkdownEncodingBr) &&
                   coded_order.size() < 2) {
          coded_order.push_back(order[i]);
        }
      }
      // The scan stops at identity: no coded copy the request ranks below
      // the plain one is read.
    }
    // A malformed call (-1) reads as the plain copy alone.
    int codings = -1;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      codings = markdown_codings_;
    }
    // A worker that publishes no codings writes no coded copy: none is read.
    if (codings < 0) coded_order.clear();
    // Identity refused and no coding this module reads left: the request
    // falls through rather than get the plain copy.  (Identity stays
    // acceptable for, say, `Accept-Encoding: zstd`.)  The plain copy may
    // still be sent after identity;q=0 when a port must replace a coded
    // answer for a body filter; RFC 9110 12.5.3 permits an answer in no
    // acceptable coding.
    if (!identity_ok && coded_order.empty()) encoding_none = true;
  }

  // Fence and freshness are judged now, at the time this runs.
  const int64 now_s = deps_.timer->NowMs() / Timer::kSecondMs;
  PsHtmlServeRequest request;
  FillServeRequest(facts, policy, cached_html, now_s, &request);
  request.accept = CStrOrNull(s->has_accept, s->accept);
  if (coded_set) {
    if (identity_ok) request.request_flags |= kPsHtmlServeRequestIdentityOk;
    if (encoding_none) {
      request.request_flags |= kPsHtmlServeRequestEncodingNone;
    }
  }

  // One classification per coding asked for: the most preferred coded copy
  // first, then, when the library answers that it is not bound (identity
  // instead, or no acceptable coding), the next one.  Each has its own read
  // and its own facts call, which alone says whether that copy is bound.
  PsHtmlStoredFacts stored = {};
  PsHtmlServeVerdict verdict = {};
  void* coded_read = nullptr;
  // The codings read whose copy could not be sent (neither bound nor
  // recorded not smaller), and whether each was missing.
  std::vector<std::pair<int, bool>> unservable;
  for (size_t attempt = 0;; ++attempt) {
    const int requested = attempt < coded_order.size()
                              ? coded_order[attempt]
                              : kPsMarkdownEncodingIdentity;
    coded_read = nullptr;
    if (requested != kPsMarkdownEncodingIdentity) {
      read(requested == kPsMarkdownEncodingBr ? kPsSentinelMarkdownBr
                                              : kPsSentinelMarkdownGzip,
           &reads.coded[attempt]);
      coded_read = reads.coded[attempt];
    }
    stored = PsHtmlStoredFacts();
    stored.struct_size = HtmlStoredFactsSize(*abi);
    // With C API 1.15 the facts also carry the stamp of the entry's header
    // block, which the library and the check at emit compare, whatever
    // coding is asked for.
    const void* block = sidecar_present ? reads.block : nullptr;
    const int facts_rc =
        coded_set ? abi->HtmlStoredFactsFromReadsCoded(
                        reads.original, reads.markdown, reads.withheld,
                        sidecar_present ? 1 : 0, block, coded_read, requested,
                        &stored)
        : policy.applies ? abi->HtmlStoredFactsFromReadsEx(
                               reads.original, reads.markdown, reads.withheld,
                               sidecar_present ? 1 : 0, block, &stored)
                         : abi->HtmlStoredFactsFromReads(
                               reads.original, reads.markdown, reads.withheld,
                               sidecar_present ? 1 : 0, &stored);
    if (facts_rc != 0) {
      CountFallThrough(result.reason);
      return result;
    }
    if ((stored.flags & kPsHtmlFactsPresent) != 0) {
      if (oracle_.Purged(url, stored.stored_at)) {
        stored.flags |= kPsHtmlFactsPurged;
      }
    }
    if (requested != kPsMarkdownEncodingIdentity) {
      const bool br = requested == kPsMarkdownEncodingBr;
      const uint32_t bound =
          br ? kPsHtmlFactsBrCopyBound : kPsHtmlFactsGzipCopyBound;
      const uint32_t not_smaller =
          br ? kPsHtmlFactsBrNotSmaller : kPsHtmlFactsGzipNotSmaller;
      if ((stored.flags & (bound | not_smaller)) == 0) {
        unservable.emplace_back(requested, coded_read == nullptr);
      }
    }
    request.markdown_encoding = static_cast<uint32_t>(requested);
    // Zeroed before each call: nothing of an earlier verdict is read as
    // this one's.
    memset(&verdict, 0, sizeof(verdict));
    verdict.struct_size = HtmlServeVerdictSize(*abi);
    if (abi->HtmlServeClassify(&request, &stored, &verdict) != 0) {
      CountFallThrough(result.reason);
      return result;
    }
    if (requested == kPsMarkdownEncodingIdentity ||
        attempt + 1 >= coded_order.size()) {
      break;
    }
    const char* name = verdict.action == kPsHtmlServeActionFallThrough
                           ? abi->HtmlServeReasonName(verdict.reason)
                           : nullptr;
    const bool not_bound =
        (verdict.action == kPsHtmlServeActionServeMarkdown &&
         verdict.markdown_encoding == kPsMarkdownEncodingIdentity) ||
        (name != nullptr &&
         StringPiece(name) == StringPiece("no_acceptable_encoding"));
    if (!not_bound) break;
  }
  // The copy was admitted (served in some coding, or refused only for its
  // coding): the coded copies that could not be sent count, and the
  // worker is asked once for them when the plain copy goes out instead.
  const char* final_name = verdict.action == kPsHtmlServeActionFallThrough
                               ? abi->HtmlServeReasonName(verdict.reason)
                               : nullptr;
  const bool refused_coding =
      final_name != nullptr &&
      StringPiece(final_name) == StringPiece("no_acceptable_encoding");
  const bool admitted =
      verdict.action == kPsHtmlServeActionServeMarkdown || refused_coding;
  if (admitted && !unservable.empty()) {
    bool demand = false;
    for (const auto& [coding, missing] : unservable) {
      (missing ? counters_->encoded_copy_missing
               : counters_->encoded_copy_unbound)
          ->Add(1);
      int codings = -1;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        codings = markdown_codings_;
      }
      demand = demand || (codings >= 0 && (codings & (1 << coding)) != 0);
    }
    const bool plain_sent =
        verdict.action == kPsHtmlServeActionServeMarkdown &&
        verdict.markdown_encoding == kPsMarkdownEncodingIdentity;
    if (demand && (plain_sent || refused_coding)) {
      DemandCompress(facts, url, hostname);
    }
  }

  if (verdict.action != kPsHtmlServeActionServeMarkdown) {
    const char* name = abi->HtmlServeReasonName(verdict.reason);
    result.reason = name == nullptr ? "other" : name;
    if (verdict.action != kPsHtmlServeActionFallThrough) {
      // A validation (or the page itself) asked for on the Markdown path
      // while fresh-only is set: never acted on.
      if (cached_html && verdict.action == kPsHtmlServeActionValidate) {
        counters_->unexpected_validate->Add(1);
        NoteHitPathInvariant(
            "the optimizer library asked for a validation the request ruled "
            "out");
      }
      result.reason = "other";
    }
    CountFallThrough(result.reason);
    // The lost copy: a present, fresh page this module's store wrote, with
    // no copy bound to it, asks the worker once per limiter window.
    if ((result.reason == "absent" || result.reason == "binding_stale") &&
        verdict.wants_markdown != 0 && reads.original != nullptr &&
        (stored.flags & kPsHtmlFactsPresent) != 0 &&
        (stored.flags & (kPsHtmlFactsPurged | kPsHtmlFactsWithheld)) == 0 &&
        (abi->ReadFlags(reads.original) & kPsFlagHeadersClassified) != 0 &&
        (abi->ReadOriginCcFlags(reads.original) & kPsCcOriginNoTransform) ==
            0 &&
        abi->ReadContentType(reads.original) == kPsContentHtml &&
        !IsZeroHash(stamp) &&
        now_s - static_cast<int64>(stored.stored_at) <
            static_cast<int64>(stored.lifetime_s)) {
      Notify(facts, url, hostname, /*agent_request=*/true);
    }
    if (!s->serves_page || verdict.action != kPsHtmlServeActionFallThrough) {
      return result;
    }
    // serve-cached-html: once more for the page itself, with the same
    // request and facts and html_fallback set.  The library decides which
    // Markdown reasons leave the page servable and repeats every refusal
    // both bodies share; the engine keeps no list of its own.
    PsHtmlServeRequest html_request = request;
    html_request.request_flags |= kPsHtmlServeRequestHtmlFallback;
    html_request.markdown_encoding = kPsMarkdownEncodingIdentity;
    if (coded_set) {
      // The page is compressed, or not, by the server after the rewriter:
      // the request's codings say nothing about it.
      html_request.request_flags &= ~kPsHtmlServeRequestEncodingNone;
      html_request.request_flags |= kPsHtmlServeRequestIdentityOk;
    }
    PsHtmlServeVerdict html_verdict = {};
    html_verdict.struct_size = HtmlServeVerdictSize(*abi);
    if (abi->HtmlServeClassify(&html_request, &stored, &html_verdict) != 0) {
      return result;
    }
    if (html_verdict.action == kPsHtmlServeActionServeHtml) {
      return EmitStoredPage(s, stored, html_verdict,
                            /*markdown_preference=*/true);
    }
    if (html_verdict.action == kPsHtmlServeActionValidate) {
      counters_->unexpected_validate->Add(1);
      NoteHitPathInvariant(
          "the optimizer library asked for a validation the request ruled "
          "out");
      result.reason = "other";
      return result;
    }
    if (html_verdict.action == kPsHtmlServeActionFallThrough) {
      const char* html_name = abi->HtmlServeReasonName(html_verdict.reason);
      result.reason = html_name == nullptr ? "other" : html_name;
      // The Markdown reason is counted above; only a reason of the page's
      // own is counted again.
      if (!ValueIn(result.reason, kEntryRecordReasons)) {
        CountHtmlFallThrough(result.reason);
      }
    } else {
      result.reason = "other";
    }
    return result;
  }
  // Defence in depth against a library or flag defect: a serve the
  // credentialed policy forbids is never emitted, whatever the verdict.
  if (CredentialedForbids(policy, stored)) {
    // Must never happen: the library said serve where the policy forbids
    // it.  Counted, and logged so a non-zero count can be traced.
    counters_->served_credentialed_refused->Add(1);
    if (deps_.handler != nullptr) {
      deps_.handler->Message(
          kError,
          "DaemonHtmlCache: refused to serve the Markdown copy of %s%s, "
          "which the credentialed policy forbids, although the optimizer "
          "library allowed it",
          DaemonHtmlIsLogHost(facts.host) ? facts.host.c_str() : "",
          DaemonHtmlLogPath(facts.path).c_str());
    }
    LogServeSide("refuse", "credentialed", facts.host, facts.path, 0, 0, 0,
                 nullptr, -1);
    result.reason = "credentialed";
    return result;
  }
  if (verdict.wants_markdown == 0) {
    // The library said serve without a preference: a defect, never sent.
    counters_->served_without_preference->Add(1);
    result.reason = "other";
    CountFallThrough(result.reason);
    return result;
  }
  // The coding to send: identity, or the one copy that was read and found
  // bound.  Any other answer is a defect and is never sent.
  const int coding =
      coded_set ? verdict.markdown_encoding : kPsMarkdownEncodingIdentity;
  const bool coded_body = coding != kPsMarkdownEncodingIdentity;
  if (coded_body && (coded_read == nullptr || static_cast<uint32_t>(coding) !=
                                                  request.markdown_encoding)) {
    result.reason = "other";
    CountFallThrough(result.reason);
    return result;
  }
  const uint8_t* markdown = nullptr;
  size_t markdown_len = 0;
  const uint8_t* plain = nullptr;
  size_t plain_len = 0;
  // Only a port that may send the plain copy in place of the coded one
  // reads it as a standby.
  const bool standby = coded_body && port->MaySendPlainInstead();
  if (standby &&
      (reads.markdown == nullptr ||
       abi->ReadContent(reads.markdown, &plain, &plain_len) != kPsOk ||
       plain_len == 0)) {
    // The plain copy stands behind every coded answer (a port may send it
    // instead): without it nothing is sent.
    result.reason = "absent";
    CountFallThrough(result.reason);
    return result;
  }
  void* const body_read = coded_body ? coded_read : reads.markdown;
  if (body_read == nullptr ||
      abi->ReadContent(body_read, &markdown, &markdown_len) != kPsOk ||
      markdown_len == 0) {
    // Never an empty answer for a copy that cannot be read.
    result.reason = "absent";
    CountFallThrough(result.reason);
    return result;
  }

  char etag[kPsHtmlMarkdownEtagChars + 8] = {0};
  size_t etag_len = 0;
  const bool has_etag =
      (coded_set
           ? abi->HtmlMarkdownEtagCoded(stored.binding_hash,
                                        stored.markdown_hash, coding, etag,
                                        sizeof(etag), &etag_len)
           : abi->HtmlMarkdownEtag(stored.binding_hash, stored.markdown_hash,
                                   etag, sizeof(etag), &etag_len)) == 0 &&
      etag_len > 0;
  const bool not_modified =
      has_etag && facts.has_if_none_match &&
      abi->IfNoneMatchMatchesWeak(facts.if_none_match.c_str(), etag) == 1;

  // One Vary line: the origin's own tokens (from the header block) and
  // Accept, and with the 1.16 set Accept-Encoding on every answer, the plain
  // one too, as the coding depends on it.
  std::vector<GoogleString> own_tokens = {"Accept"};
  if (coded_set) own_tokens.push_back("Accept-Encoding");
  std::vector<GoogleString> vary;
  if (reads.block != nullptr) {
    DaemonHtmlHeaderLines block_vary;
    const size_t n = abi->HeaderBlockCount(reads.block);
    for (size_t i = 0; i < n; ++i) {
      const char* name = nullptr;
      const char* value = nullptr;
      size_t name_len = 0;
      size_t value_len = 0;
      if (abi->HeaderBlockGet(reads.block, i, &name, &name_len, &value,
                              &value_len) == kPsOk &&
          StringCaseEqual(StringPiece(name, name_len), "Vary")) {
        block_vary.emplace_back("Vary", GoogleString(value, value_len));
      }
    }
    vary = TokensMinus(VaryTokens(block_vary), own_tokens);
  }
  for (const GoogleString& token : own_tokens) vary.push_back(token);
  const GoogleString vary_line = JoinCollection(vary, ", ");

  const bool head = facts.method == "HEAD";
  result.kind = DaemonHtmlServeResult::kFinal;
  result.reason = "fresh";
  if (not_modified) {
    result.status = 304;
    result.headers.emplace_back("ETag", etag);
    result.headers.emplace_back("Cache-Control", "private");
    result.headers.emplace_back("Vary", vary_line);
  } else {
    result.status = 200;
    result.headers.emplace_back("Content-Type", "text/markdown; charset=utf-8");
    result.headers.emplace_back("Content-Length",
                                Integer64ToString(markdown_len));
    if (coded_body) {
      result.headers.emplace_back(
          "Content-Encoding", coding == kPsMarkdownEncodingBr ? "br" : "gzip");
    }
    result.headers.emplace_back("Cache-Control", "private");
    result.headers.emplace_back("Vary", vary_line);
    result.headers.emplace_back("X-Robots-Tag", "noindex");
    result.headers.emplace_back("Age", Integer64ToString(verdict.age_s));
    if (has_etag) result.headers.emplace_back("ETag", etag);
    if (!head) {
      result.body.assign(reinterpret_cast<const char*>(markdown), markdown_len);
    }
    if (standby) {
      char plain_etag[kPsHtmlMarkdownEtagChars + 8] = {0};
      size_t plain_etag_len = 0;
      if (abi->HtmlMarkdownEtagCoded(stored.binding_hash, stored.markdown_hash,
                                     kPsMarkdownEncodingIdentity, plain_etag,
                                     sizeof(plain_etag),
                                     &plain_etag_len) == 0 &&
          plain_etag_len > 0) {
        result.plain_etag = plain_etag;
      }
      if (!head) {
        result.plain_body.assign(reinterpret_cast<const char*>(plain),
                                 plain_len);
      }
    }
  }
  port->MarkNoCompress();
  if (!port->ClearAndEmitAllowlisted(result, head)) {
    DaemonHtmlServeResult fall_through;
    fall_through.reason = "other";
    CountFallThrough(fall_through.reason);
    return fall_through;
  }
  // A port that sent the plain copy in place of the coded one.
  const bool replaced =
      coded_body && !not_modified && port->CodedAnswerReplacedByPlain();
  const int sent_coding = replaced ? kPsMarkdownEncodingIdentity : coding;
  if (replaced) counters_->coded_fallback->Add(1);
  if (not_modified) {
    counters_->markdown_not_modified->Add(1);
  } else {
    counters_->markdown_served->Add(1);
    if (coded_set) {
      (sent_coding == kPsMarkdownEncodingBr     ? counters_->served_br
       : sent_coding == kPsMarkdownEncodingGzip ? counters_->served_gzip
                                                : counters_->served_identity)
          ->Add(1);
    }
  }
  LogServeSide(
      "served_markdown", "fresh", facts.host, facts.path, result.status,
      not_modified || head ? 0 : (replaced ? plain_len : markdown_len), 0,
      sent_coding == kPsMarkdownEncodingBr     ? "markdown_br"
      : sent_coding == kPsMarkdownEncodingGzip ? "markdown_gzip"
                                               : "markdown",
      verdict.age_s);
  return result;
}

DaemonHtmlServeResult DaemonHtmlEngine::FinishHtmlServe(
    DaemonHtmlPendingServe::State* s) {
  const DaemonAbi* abi = deps_.substrate.abi;
  const bool shadow = s->kind == DaemonHtmlPendingServe::State::kShadow;
  DaemonHtmlServeResult result = s->result;
  PsHtmlStoredFacts stored = {};
  stored.struct_size = HtmlStoredFactsSize(*abi);
  // The page alone: no Markdown copy and no marker.
  if (abi->HtmlStoredFactsFromReadsEx(
          s->reads.original, nullptr, nullptr, s->sidecar_present ? 1 : 0,
          s->sidecar_present ? s->reads.block : nullptr, &stored) != 0) {
    if (!shadow) result.reason = "other";
    return result;
  }
  if ((stored.flags & kPsHtmlFactsPresent) != 0) {
    if (oracle_.Purged(s->url, stored.stored_at)) {
      stored.flags |= kPsHtmlFactsPurged;
    }
  }
  // Fence and freshness are judged now, at the time this runs.
  const int64 now_s = deps_.timer->NowMs() / Timer::kSecondMs;
  PsHtmlServeRequest request;
  FillServeRequest(s->facts, s->policy, /*cached_html=*/true, now_s, &request);
  request.accept = CStrOrNull(s->has_accept, s->accept);
  request.markdown_encoding = kPsMarkdownEncodingIdentity;
  if (abi->HasHtmlServe16Set()) {
    request.request_flags |= kPsHtmlServeRequestIdentityOk;
  }
  // The shadow sample asks what scope `all` would answer.
  if (shadow) request.serve_scope = kPsHtmlServeScopeAll;
  PsHtmlServeVerdict verdict = {};
  verdict.struct_size = HtmlServeVerdictSize(*abi);
  if (abi->HtmlServeClassify(&request, &stored, &verdict) != 0) {
    if (!shadow) result.reason = "other";
    return result;
  }
  const char* name = abi->HtmlServeReasonName(verdict.reason);
  if (shadow) {
    // Counted and remembered, never answered: still a `scope` fall-through.
    counters_->shadow_sampled->Add(1);
    if (verdict.action == kPsHtmlServeActionServeHtml) {
      counters_->would_serve_html->Add(1);
      NoteShadowWouldServe(
          VolumeIdentity(s->url, s->hostname, s->facts.scheme), s->stamp,
          static_cast<int64>(stored.stored_at) + stored.lifetime_s);
    } else {
      if (verdict.action == kPsHtmlServeActionValidate) {
        counters_->unexpected_validate->Add(1);
        NoteHitPathInvariant(
            "the optimizer library asked for a validation the request ruled "
            "out");
      }
      auto it = counters_->would_fallthrough.find(
          verdict.action == kPsHtmlServeActionFallThrough && name != nullptr
              ? GoogleString(name)
              : GoogleString("other"));
      if (it == counters_->would_fallthrough.end()) {
        it = counters_->would_fallthrough.find("other");
      }
      it->second->Add(1);
    }
    return result;
  }
  if (verdict.action == kPsHtmlServeActionServeHtml) {
    return EmitStoredPage(s, stored, verdict, /*markdown_preference=*/false);
  }
  if (verdict.action == kPsHtmlServeActionValidate) {
    // fresh-only is always set here: the library never validates, so this
    // is a defect, and never acted on.
    counters_->unexpected_validate->Add(1);
    NoteHitPathInvariant(
        "the optimizer library asked for a validation the request ruled out");
    result.reason = "other";
    return result;
  }
  if (verdict.action != kPsHtmlServeActionFallThrough) {
    // A Markdown answer for a request that does not prefer Markdown.
    result.reason = "other";
    return result;
  }
  result.reason = name == nullptr ? "other" : name;
  CountHtmlFallThrough(result.reason);
  return result;
}

DaemonHtmlServeResult DaemonHtmlEngine::EmitStoredPage(
    DaemonHtmlPendingServe::State* s, const PsHtmlStoredFacts& stored,
    const PsHtmlServeVerdict& verdict, bool markdown_preference) {
  const DaemonAbi* abi = deps_.substrate.abi;
  const DaemonHtmlRequestFacts& facts = s->facts;
  DaemonHtmlServeResult fall_through = s->result;
  fall_through.kind = DaemonHtmlServeResult::kFallThrough;
  // Defence in depth against a library or flag defect: a serve the
  // credentialed policy forbids is never emitted, whatever the verdict.
  if (CredentialedForbids(s->policy, stored)) {
    counters_->served_credentialed_refused->Add(1);
    if (deps_.handler != nullptr) {
      deps_.handler->Message(
          kError,
          "DaemonHtmlCache: refused to serve the stored page of %s%s, which "
          "the credentialed policy forbids, although the optimizer library "
          "allowed it",
          DaemonHtmlIsLogHost(facts.host) ? facts.host.c_str() : "",
          DaemonHtmlLogPath(facts.path).c_str());
    }
    LogServeSide("refuse", "credentialed", facts.host, facts.path, 0, 0, 0,
                 nullptr, -1);
    fall_through.reason = "credentialed";
    return fall_through;
  }
  const char* content_type =
      s->reads.original == nullptr
          ? nullptr
          : abi->ReadOriginContentType(s->reads.original);
  const uint8_t* body = nullptr;
  size_t body_len = 0;
  if (s->reads.original == nullptr || s->reads.block == nullptr ||
      !s->sidecar_present ||
      abi->ReadContentType(s->reads.original) != kPsContentHtml ||
      content_type == nullptr || *content_type == '\0' ||
      abi->ReadContent(s->reads.original, &body, &body_len) != kPsOk ||
      body_len == 0) {
    // Never an answer without the page, its type or its header block.
    fall_through.reason = "absent";
    return fall_through;
  }
  DaemonHtmlServeResult result;
  result.kind = DaemonHtmlServeResult::kAsOrigin;
  result.status = 200;
  result.reason = "fresh";
  result.suppress_recording = true;
  // The header block's lines in stored order, the origin's Cache-Control and
  // Expires among them, verbatim.
  bool forbidden = false;
  const size_t n = abi->HeaderBlockCount(s->reads.block);
  for (size_t i = 0; i < n; ++i) {
    const char* name = nullptr;
    const char* value = nullptr;
    size_t name_len = 0;
    size_t value_len = 0;
    if (abi->HeaderBlockGet(s->reads.block, i, &name, &name_len, &value,
                            &value_len) != kPsOk) {
      // A block that cannot be read whole is never answered with.
      fall_through.reason = "headers_not_reproducible";
      return fall_through;
    }
    const StringPiece line_name(name, name_len);
    if (NameIn(line_name, kForbiddenOnHit)) {
      forbidden = true;
      break;
    }
    if (NameIn(line_name, kHitOwnLines)) continue;
    result.headers.emplace_back(GoogleString(name, name_len),
                                GoogleString(value, value_len));
  }
  if (forbidden) {
    // Must never happen: the store keeps none of these.  Never emitted.
    counters_->hit_forbidden_header->Add(1);
    if (deps_.handler != nullptr) {
      deps_.handler->Message(
          kError,
          "DaemonHtmlCache: refused to serve the stored page of %s%s: its "
          "header block holds a line a stored page is never answered with",
          DaemonHtmlIsLogHost(facts.host) ? facts.host.c_str() : "",
          DaemonHtmlLogPath(facts.path).c_str());
    }
    LogServeSide("refuse", "headers", facts.host, facts.path, 0, 0, 0, nullptr,
                 -1);
    fall_through.reason = "headers_not_reproducible";
    return fall_through;
  }
  result.headers.emplace_back("Content-Type", content_type);
  result.headers.emplace_back("Content-Length", Integer64ToString(body_len));
  result.headers.emplace_back("Age", Integer64ToString(verdict.age_s));
  const bool head = facts.method == "HEAD";
  if (!head) result.body.assign(reinterpret_cast<const char*>(body), body_len);
  if (!s->port->EmitAsOrigin(result)) {
    // The port could not hand the page to its chain: a miss.
    fall_through.reason = "other";
    return fall_through;
  }
  counters_->served_html->Add(1);
  const int agent_class = facts.agent_class;
  Variable* by_class =
      markdown_preference ? counters_->served_html_markdown_preference
      : agent_class == kDaemonHtmlAgentClassSigned ? counters_->served_html_c1
      : agent_class == kDaemonHtmlAgentClassToken
          ? counters_->served_html_c3
          : counters_->served_html_unclassed;
  by_class->Add(1);
  LogServeSide("served_html", "fresh", facts.host, facts.path, 200,
               head ? 0 : body_len, stored.lifetime_s, nullptr, verdict.age_s,
               nullptr, GoogleString(), agent_class, config_.serve_scope);
  return result;
}

void DaemonHtmlEngine::NoteShadowWouldServe(const GoogleString& identity,
                                            const uint8_t* stored_hash,
                                            int64 fresh_until_s) {
  if (stored_hash == nullptr || IsZeroHash(stored_hash)) return;
  const GoogleString hash(reinterpret_cast<const char*>(stored_hash),
                          kPsHtmlHashBytes);
  std::lock_guard<std::mutex> lock(mutex_);
  if (shadow_would_serve_.count(identity) == 0) {
    shadow_order_.push_back(identity);
  }
  shadow_would_serve_[identity] = {hash, fresh_until_s};
  while (shadow_order_.size() > kShadowWouldServeMax) {
    shadow_would_serve_.erase(shadow_order_.front());
    shadow_order_.pop_front();
  }
}

void DaemonHtmlEngine::CheckShadowBody(const GoogleString& identity,
                                       const uint8_t* hash, int64 capture_s) {
  GoogleString stored;
  int64 fresh_until_s = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = shadow_would_serve_.find(identity);
    if (it == shadow_would_serve_.end()) return;
    stored = it->second.first;
    fresh_until_s = it->second.second;
    shadow_would_serve_.erase(it);
    for (auto o = shadow_order_.begin(); o != shadow_order_.end(); ++o) {
      if (*o == identity) {
        shadow_order_.erase(o);
        break;
      }
    }
  }
  // Only while the sampled entry would still have been served: a page the
  // origin changed after it expired is no sign of a missed personalisation.
  if (capture_s < fresh_until_s &&
      memcmp(stored.data(), hash, kPsHtmlHashBytes) != 0) {
    counters_->would_serve_body_differs->Add(1);
  }
}

void DaemonHtmlEngine::OnUnsafeRequest(
    const DaemonHtmlRequestFacts& facts, int status,
    const std::vector<GoogleString>& location,
    const std::vector<GoogleString>& content_location, DaemonHtmlPort* port) {
  const DaemonAbi* abi = deps_.substrate.abi;
  // In serve-variants or a later mode, with the 1.15 predicate; a main
  // request of this site, on one of its exact names, whose URL the server
  // and this module read the same way (no key is guessed otherwise).
  if (abi == nullptr || config_.mode < DaemonHtmlMode::kServeVariants ||
      !config_.agent_optimize || !abi->HasHtmlServe15Set() ||
      !IsSiteId(config_.site) || facts.site != config_.site || facts.internal ||
      !HostExactlyConfigured(facts.host, config_.host_names)) {
    return;
  }
  if (!facts.url_agreed || !IsVolumePath(facts.path)) return;
  if (abi->HtmlUnsafeMethodInvalidates(facts.method.c_str(), status) != 1) {
    return;
  }
  // The targets: the request's own key, then up to kUnsafeTargetsMax more
  // from Location and Content-Location, in that order, each same-scheme and
  // on an exactly configured host, normalised as a URL purge is.
  struct Target {
    GoogleString url, hostname, scheme, host, path;
  };
  std::vector<Target> targets;
  {
    Target own;
    CacheKey(facts, &own.url, &own.hostname);
    own.scheme = facts.scheme;
    own.host = facts.host;
    own.path = facts.path;
    targets.push_back(std::move(own));
  }
  auto add_target = [&](const GoogleString& value) {
    if (targets.size() >= kUnsafeTargetsMax + 1) return;
    // Resolved against the request URL first, so relative-path, query-only
    // and dot-segment references name their target; only then is the
    // target checked to be on this scheme and an exactly configured host.
    const GoogleString resolved =
        DaemonHtmlResolveReference(facts.scheme, facts.host, facts.path, value);
    if (resolved.empty()) return;
    DaemonHtmlRequestFacts parts;
    {
      const GoogleString key = DaemonHtmlNormalizePurgeUrl(resolved);
      const size_t sep = key.find("://");
      if (sep == GoogleString::npos) return;
      parts.scheme = key.substr(0, sep);
      if (parts.scheme != facts.scheme) return;
      const size_t path_start = key.find_first_of("/?", sep + 3);
      parts.host = key.substr(sep + 3, path_start == GoogleString::npos
                                           ? GoogleString::npos
                                           : path_start - sep - 3);
      parts.path = path_start == GoogleString::npos ? GoogleString("/")
                                                    : key.substr(path_start);
      if (!HostExactlyConfigured(parts.host, config_.host_names)) return;
    }
    if (!IsVolumePath(parts.path)) return;
    Target t;
    CacheKey(parts, &t.url, &t.hostname);
    for (const Target& have : targets) {
      if (have.url == t.url) return;
    }
    t.scheme = parts.scheme;
    t.host = parts.host;
    t.path = parts.path;
    targets.push_back(std::move(t));
  };
  for (const GoogleString& line : location) add_target(line);
  for (const GoogleString& line : content_location) add_target(line);

  // The fence first, in this process, for every target: a capture of any of
  // them already under way fails its check, and no later serve here reads
  // an entry stored before now.
  const int64 now_ms = deps_.timer->NowMs();
  for (const Target& t : targets) oracle_.NoteUnsafeInvalidation(t.url, now_ms);
  counters_->invalidated_unsafe->Add(1);
  LogServeSide("invalidated", GoogleString(), facts.host, facts.path, status, 0,
               0, nullptr, -1);
  void* cache = Cache();
  // No volume this process can vouch for: nothing is served from one either,
  // and the fence holds.
  if (cache == nullptr) return;
  // Then the removal, which fails closed: a missing entry is a success, any
  // other failure is counted and leaves the fence.
  auto remove = [this, cache, targets] {
    const DaemonAbi* abi = deps_.substrate.abi;
    for (const Target& t : targets) {
      const int rc = abi->CacheRemoveSite(cache, VolumeUrl(t.url).c_str(),
                                          t.hostname.c_str(), t.scheme.c_str(),
                                          config_.site.c_str());
      if (rc == kPsOk) {
        counters_->removed->Add(1);
        LogServeSide("purged", "invalidated", t.host, t.path, 0, 0, 0, nullptr,
                     -1);
      } else if (rc != kPsErrNotFound) {
        counters_->invalidate_failed->Add(1);
      }
    }
  };
  if (port != nullptr) {
    port->RunBlocking(DaemonHtmlCommit(std::move(remove)));
  } else {
    RunOn(deps_.runner, std::move(remove));
  }
}

void DaemonHtmlEngine::DemandCompress(const DaemonHtmlRequestFacts& facts,
                                      const GoogleString& url,
                                      const GoogleString& hostname) {
  DaemonMarkdownCompressDemands* demands =
      deps_.compress_demands != nullptr
          ? deps_.compress_demands
          : ProcessDaemonMarkdownCompressDemands();
  const GoogleString key = StrCat(config_.site, "\n", url);
  if (!demands->Admit(key)) return;
  ModuleNotifyInput input;
  input.url = url;
  input.hostname = hostname;
  input.scheme = facts.scheme;
  input.site = config_.site;
  input.content_type = kPsContentHtml;
  input.option_context = facts.option_context;
  input.option_signature = facts.option_signature;
  input.html_stored = true;
  input.markdown_compress = true;
  input.agent_request = true;
  ModuleNotifyParams built;
  if (BuildModuleNotifyParams(*deps_.substrate.abi, input, &built) &&
      deps_.substrate.abi->NotifyWorker(deps_.substrate.notify_socket.c_str(),
                                        &built.params()) == kPsOk) {
    counters_->compress_demand_sent->Add(1);
    return;
  }
  // Not sent (the worker restarting, say): a later request asks again.
  demands->Forget(key);
}

void DaemonHtmlEngine::NoteTransportConflict() {
  counters_->transport_conflict->Add(1);
}

void DaemonHtmlEngine::OnAdminPurge(StringPiece url, bool global, int64 now_ms,
                                    Done done) {
  bool bound = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    bound = set_bound_;
  }
  if (!bound || config_.mode < DaemonHtmlMode::kServeVariants) {
    done(true, GoogleString());
    return;
  }
  // The key a URL purge names, and the fence for it in this process: noted
  // at once, before anything can fail or wait, so a capture already under
  // way fails its re-check; peer processes see the purge set's record at
  // their next poll.  The fence is per URL as spelled after normalisation
  // (its port included), as the purge set's is; the removal below takes the
  // entry every port of the site shares.
  GoogleString hostname;
  GoogleString cache_url;
  DaemonHtmlRequestFacts parts;
  if (global) {
    oracle_.NoteGlobalPurge(now_ms);
  } else {
    const GoogleString key = DaemonHtmlNormalizePurgeUrl(url);
    const size_t sep = key.find("://");
    if (sep == GoogleString::npos) {
      done(true, GoogleString());
      return;
    }
    parts.scheme = key.substr(0, sep);
    const size_t path_start = key.find_first_of("/?#", sep + 3);
    parts.host = key.substr(sep + 3, path_start == GoogleString::npos
                                         ? GoogleString::npos
                                         : path_start - sep - 3);
    // An empty path is the root ("https://host" names "https://host/").
    parts.path = path_start == GoogleString::npos ? GoogleString("/")
                                                  : key.substr(path_start);
    CacheKey(parts, &cache_url, &hostname);
    oracle_.NoteUrlPurge(cache_url, now_ms);
  }
  void* cache = Cache();
  if (cache == nullptr) {
    // No volume this process can vouch for: nothing is served from one
    // either, and the purge is fenced above.  A process with a handle
    // writes the global epoch on its purge-poll tick.
    done(true, GoogleString());
    return;
  }
  if (global) {
    const uint64 epoch_s = static_cast<uint64>(
        std::min(now_ms, deps_.timer->NowMs()) / Timer::kSecondMs);
    RunOn(deps_.runner, [this, cache, epoch_s, done] {
      bool all_ok = true;
      WriteEpoch(cache, epoch_s, &all_ok);
      if (!all_ok) {
        done(false, "the HTML cache's purge epoch could not be written");
        return;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        epoch_written_s_ =
            std::max(epoch_written_s_, static_cast<int64>(epoch_s));
      }
      oracle_.RaiseSiteEpoch(epoch_s);
      done(true, GoogleString());
    });
    return;
  }
  if (!IsVolumePath(parts.path)) {
    // Names no volume entry; the fence above still holds.
    done(true, GoogleString());
    return;
  }
  RunOn(deps_.runner, [this, cache, cache_url, hostname, host = parts.host,
                       path = parts.path, scheme = parts.scheme, done] {
    const int rc = deps_.substrate.abi->CacheRemoveSite(
        cache, VolumeUrl(cache_url).c_str(), hostname.c_str(), scheme.c_str(),
        config_.site.c_str());
    if (rc == kPsOk || rc == kPsErrNotFound) {
      if (rc == kPsOk) {
        counters_->removed->Add(1);
        LogServeSide("purged", GoogleString(), host, path, 0, 0, 0, nullptr,
                     -1);
      }
      done(true, GoogleString());
      return;
    }
    counters_->purge_remove_failed->Add(1);
    done(false, "the HTML cache's stored copy could not be removed");
  });
}

DaemonHtmlMode DaemonHtmlModeFor(RewriteOptions::DaemonHtmlCacheMode mode) {
  switch (mode) {
    case RewriteOptions::kDaemonHtmlCacheOff:
      return DaemonHtmlMode::kOff;
    case RewriteOptions::kDaemonHtmlCacheReportOnly:
      return DaemonHtmlMode::kReportOnly;
    case RewriteOptions::kDaemonHtmlCacheServeVariants:
      return DaemonHtmlMode::kServeVariants;
    case RewriteOptions::kDaemonHtmlCacheServeCachedHtml:
      return DaemonHtmlMode::kServeCachedHtml;
  }
  return DaemonHtmlMode::kOff;
}

void FillDaemonHtmlServeSettings(const RewriteOptions& options,
                                 DaemonHtmlEngine::Config* config) {
  config->serve_scope = options.daemon_html_cache_serve_scope() ==
                                RewriteOptions::kDaemonHtmlCacheServeScopeAll
                            ? kPsHtmlServeScopeAll
                            : kPsHtmlServeScopeAgents;
  config->max_age_s =
      static_cast<uint32_t>(options.daemon_html_cache_max_age_s());
  config->credentialed =
      options.daemon_html_cache_credentialed() ==
              RewriteOptions::kDaemonHtmlCacheCredentialedExplicitPublic
          ? kPsHtmlCredentialedExplicitPublic
          : kPsHtmlCredentialedAnonymousOnly;
  config->ignore_cookies = options.daemon_html_cache_ignore_cookies();
  config->ignore_cookie.clear();
  StringPieceVector names;
  SplitStringPieceToVector(options.daemon_html_cache_ignore_cookie(), " ",
                           &names, true);
  for (StringPiece name : names)
    config->ignore_cookie.push_back(name.as_string());
}

bool DaemonHtmlCheckIgnoreCookieWithLibrary(StringPiece addition,
                                            GoogleString* error) {
  // Loaded for this one check and released again: only a pure function of
  // the library is called, and the server keeps no handle on it from here
  // (its startup check loads the library on its own).
  GoogleString why;
  const std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(kDefaultDaemonLibraryName, &why));
  if (abi == nullptr || !abi->HasHtmlServe15Set()) return true;
  const GoogleString name = addition.as_string();
  int warning = 0;
  if (abi->HtmlIgnoreCookieGuard(name.c_str(), &warning) == 1) return true;
  if (error != nullptr) {
    *error = StrCat(
        RewriteOptions::kDaemonHtmlCacheIgnoreCookie, " ", name,
        " is refused: an addition must not be * alone, must have at least 3 "
        "characters before a trailing *, and must not match or cover a "
        "session cookie name, which is never ignored");
  }
  return false;
}

bool ApplyDaemonHtmlPolicySettingsForPort(const DaemonAbi* abi,
                                          DaemonHtmlEngine::Config* config,
                                          MessageHandler* handler,
                                          GoogleString* error_out) {
  if (abi == nullptr) return true;
  GoogleString error;
  if (DaemonHtmlEngine::ApplyPolicySettings(*abi, config, &error)) {
    return true;
  }
  if (error_out != nullptr) *error_out = error;
  if (handler != nullptr) {
    handler->Message(kError,
                     "%s; DaemonHtmlCache neither stores nor serves for this "
                     "configuration and runs report-only",
                     error.c_str());
  }
  if (config->mode > DaemonHtmlMode::kReportOnly) {
    config->mode = DaemonHtmlMode::kReportOnly;
  }
  return false;
}

std::unique_ptr<DaemonHtmlEngine> NewDaemonHtmlEngineForPort(
    RewriteOptions::DaemonHtmlCacheMode mode, int64 max_bytes,
    const GoogleString& log_dir, const DaemonAbi* abi, Statistics* statistics,
    Timer* timer, MessageHandler* handler) {
  if (mode == RewriteOptions::kDaemonHtmlCacheOff) return nullptr;
  if (log_dir.empty()) {
    handler->Message(kWarning,
                     "DaemonHtmlCache decision log is off: LogDir is not set");
  }
  DaemonHtmlEngine::Config config;
  // No port stores or serves yet: serve-variants reports, as report-only.
  config.mode = DaemonHtmlMode::kReportOnly;
  config.max_bytes = max_bytes;
  DaemonHtmlEngine::Deps deps;
  deps.substrate.abi = abi;
  deps.statistics = statistics;
  deps.timer = timer;
  deps.handler = handler;
  deps.log_dir = log_dir;
  return std::make_unique<DaemonHtmlEngine>(std::move(config), std::move(deps));
}

}  // namespace net_instaweb
