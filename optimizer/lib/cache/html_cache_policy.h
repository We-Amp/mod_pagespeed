// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CACHE_HTML_CACHE_POLICY_H_
#define PAGESPEED_LIB_CACHE_HTML_CACHE_POLICY_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lib/cache/headers_sidecar.h"

namespace pagespeed {

// The HTML cacheability policy: whether a text/html response may be stored,
// and whether a stored one may be served, decided as two pure functions.
//
// WHY A LIBRARY OF ITS OWN.  Images, CSS and scripts are cached under a
// freshness evaluator that applies per-class default lifetimes and caps.
// HTML is the class where that is wrong: a page is the response most likely
// to carry per-user state, so the policy for it is deliberately narrower than
// RFC 9111 permits and answers only from what the origin said explicitly.
// This file is that policy, written down once, so every front end that asks
// the question gets the same answer.
//
// SCOPE.  Pure functions of their arguments: no I/O, no globals, no clock
// reads (the caller passes `now_seconds`), no allocation beyond the returned
// strings and vectors.  Nothing here stores, serves or logs anything.  A
// verdict is advice the caller acts on.  In report-only no verdict can be
// acted on: the store side answers only with a kWouldStore action, and the
// serve side always falls through.
//
// COMPOSITION.  Cache-Control is parsed by AccumulateOriginCacheControl, one
// call per header line.  The header set is classified by
// ClassifyForHeadersSidecar, so a response whose headers the cache cannot
// reproduce on a hit is refused here exactly as it is refused there.  This
// policy does NOT use EvaluateFreshness (its per-class defaults and caps are
// what this policy forbids) and does NOT use VaryUncacheable as its own Vary
// gate (that gate admits four request axes; this policy admits one).
//
// THE STORE RULES, in the order they are evaluated.  The first rule a
// response fails is the reason it is refused; cheap facts come first.  The
// error direction of every choice is toward NOT storing: a refusal costs one
// cache entry, whereas storing a response that was personalised replays one
// client's page to another.
//
//   1. Method, status, completeness.  Only a complete 200 response to GET is
//      stored (RFC 9111 §3: a cache stores a response to GET; §3.3 and §3.4
//      concern partial and 206 content, which this policy does not assemble).
//      A Content-Range header marks a partial body and is refused as
//      incomplete.
//   2. Key-safe path.  The path must be non-empty, begin with `/`, carry no
//      `?`, `%`, `#` or `;`, and not be in the rewritten-URL shape (the
//      `.pagespeed.` marker anywhere in it).  The cache key is built from the
//      path as given, so a path that two spellings could reach
//      (percent-encoding, path parameters, a query that selects a different
//      representation) is
//      never stored, and a URL the optimizer itself produced is never recorded
//      as an origin page.  RFC 9111 §2 leaves the choice of cache key to the
//      cache.
//   3. Type and encoding.  Content-Type must be text/html (any parameters),
//      exactly once.  Content-Encoding must be absent or `identity`: the
//      stored bytes are served to clients that did not ask for a coding
//      (RFC 9110 §8.4, §12.5.5).
//   4. No credentials.  A request with Authorization, Proxy-Authorization or
//      an authenticated user is never stored, and no response directive
//      overrides that.  RFC 9111 §3.5 lets a shared cache store such a
//      response only when the response says so with `public`, `s-maxage` or
//      `must-revalidate`; this policy never takes that permission, because a
//      page rendered for a signed-in user is the canonical personalised
//      response.
//   5. Request no-store.  A request whose Cache-Control carried `no-store`
//      (RFC 9111 §5.2.1.5) is never stored: a cache MUST NOT store a response
//      to it.  The reason is kNoStore with the detail `request`.
//   6. No Set-Cookie.  A response that sets a cookie is per-client by
//      construction (RFC 6265 §4.1; RFC 9111 §3 leaves such responses to the
//      cache's judgement).  Set-Cookie2 is treated the same.
//   7. Cache-Control (RFC 9111 §5.2.2).  `private` in either form (bare, or
//      qualified as `private="Set-Cookie"`, §5.2.2.7) is refused: the
//      qualified form restricts only the named fields, but the cheap error is
//      the one this leans toward.  `no-store` (§5.2.2.5) is refused.
//   8. Cookie request.  A request that carried a Cookie header is stored only
//      when the response is explicitly shareable: `public` (§5.2.2.9) or
//      an `s-maxage` above zero (§5.2.2.10, which also makes the response
//      shareable; `s-maxage=0` or a malformed value asserts nothing).  A
//      page that varies on the cookie it was fetched with could otherwise be
//      served to clients holding a different cookie.  See THE COOKIE RULE
//      below.
//   9. Vary (RFC 9110 §12.5.5).  Every Vary line is joined with `, ` and
//      split into lowercase tokens (RFC 9110 §5.3, §5.6.1).  The set must be
//      a subset of {accept-encoding}.  Any other token, including `*` and
//      `accept`, is refused: this policy stores exactly one representation
//      per key and has no way to key on anything else.
//  10. Header reproducibility.  The header set must pass the sidecar gate
//      (headers_sidecar.h).  A nonce in a Content-Security-Policy or a Link
//      is its own reason (kNonceCsp, kNonceLink), because it never becomes
//      storable; every other gap is reported with the offending header name.
//      Unknown headers fail closed.
//  11. Explicit freshness (RFC 9111 §4.2.1).  The lifetime is `s-maxage`
//      (§5.2.2.10), else `max-age` (§5.2.2.1), else Expires minus Date
//      (§5.3).  When Date is absent, unparseable or repeated the caller's receipt time
//      stands in for it (RFC 9110 §6.6.1).  An Expires value that is invalid
//      or already past, or more than one Expires line, means lifetime 0
//      (§5.3).  NO HEURISTIC LIFETIME:
//      §4.2.2 is deliberately not applied, so a response that states no
//      lifetime is stored only if it has a validator (ETag or Last-Modified)
//      and is then always revalidated before use.  Two or more `max-age`
//      (or two or more `s-maxage`) directives, in one line or across lines,
//      conflict; RFC 9111 §4.2.1 allows using the first or treating the
//      response as stale, and this policy does the latter: lifetime 0,
//      always revalidated.  A lifetime of 0 or
//      `no-cache` (§5.2.2.4, either form, treated as the bare directive)
//      likewise requires a validator.
//  12. Size.  The body must not exceed the operator's cap, itself bounded by
//      a hard ceiling of 16 MiB.
//  13. Personalisation signals.  The first 64 KiB of the body is scanned for
//      markers a page carries when it was rendered for one client: CSRF
//      meta tags, hidden CSRF fields, and a POST form aimed at a login,
//      sign-in, checkout or cart action.  The scan is a bounded, ASCII
//      case-insensitive tag walk, not an HTML parser.  The verdict's detail
//      is the signal name.  A tag of interest still open at the 4 KiB tag
//      bound could hide any attribute, so it is a signal itself
//      (`oversized_tag`).  A prefix shorter than the body or the 64 KiB
//      window, whichever is less, could not be scanned and refuses as
//      kIncomplete with the detail `body_prefix`.
//  14. no-transform (RFC 9111 §5.2.2.6).  A response that forbids
//      transformation is stored but the caller is told not to notify the
//      optimizer about it (`kStoreNoNotify`).
//
// THE MODE.  `HtmlStorePolicy::mode` is applied around those rules.  kOff
// refuses every response with kMode.  kReportOnly runs the rules and turns a
// store into kWouldStore or kWouldStoreNoNotify; stores() is false for both.
// Every refusal stays a refusal, a personalisation match included: its reason
// and detail are what measures it.
//
// THE SERVE RULES, in the order they are evaluated.  Again the error
// direction is toward the origin: a fall-through costs one origin fetch.
//
//   1. Mode.  `kOff` and `kReportOnly` always fall through with kMode and
//      `enforce` false: report-only stores nothing, so it has no would-serve
//      answer to give.  `kServeVariants` and `kServeCachedHtml` report with
//      `enforce` true.
//   1a. Request shape.  Only GET and HEAD are answered from the cache
//      (kMethod; RFC 9111 section 4: a cache answers a request whose method
//      it stored the response for, and HEAD from a stored GET, RFC 9110
//      section 9.3.2).  A request with Range or If-Range (kRange: the cache
//      holds no partial content and assembles none) or with Origin (kCors:
//      a cross-origin request expects CORS response headers the stored entry
//      was never asked for) falls through.
//   2. Absent entry.  Nothing stored, nothing to serve.
//   2a. Skew.  An entry stored more than kHtmlServeSkewSeconds after the
//      request's time is not served (kSkew).  Its age cannot be computed, and
//      counting it as age 0 would make a clock step look like freshness.
//   3. Credentials.  A request with Authorization or an authenticated user is
//      never answered from the cache (RFC 9111 §3.5).
//   4. Cookie request.  A request with a Cookie header is answered only from
//      an entry that was explicitly shareable.
//   5. Client no-cache (RFC 9111 §5.2.1.4).  `Cache-Control: no-cache` on the
//      request, or `Pragma: no-cache` when the request carries no
//      Cache-Control at all (§5.4), bypasses the cache.
//   6. Purged.  An entry the caller's purge state covers is not served.
//   7. Header replay.  An entry whose headers cannot be reproduced, or whose
//      stored header block could not be read, is not served.
//   8. Freshness (RFC 9111 §4.2, §4.2.3).  The current age is the age at
//      store plus the time since it was stored.  The caller supplies an
//      initial age already corrected for the origin's Age and the apparent
//      age (§4.2.3); the library adds resident time only.  An entry is fresh when it
//      needs no validation and its age is below its lifetime.  A request
//      `max-age` below the age (§5.2.1.1) or a `min-fresh` the entry cannot
//      meet (§5.2.1.3: the entry must stay fresh for at least that long, so
//      age plus `min-fresh` may not exceed the lifetime) makes it not fresh
//      for that request.  STALE ENTRIES ARE NEVER SERVED: `must-revalidate`,
//      `proxy-revalidate`, `s-maxage`, `stale-while-revalidate` and
//      `stale-if-error` are not consulted, and the request's `max-stale`
//      (§5.2.1.2) is ignored.  A not-fresh entry is validated with the origin
//      in `kServeCachedHtml` and falls through in every other mode.  An entry
//      the origin just confirmed with a 304 (§4.3.4) counts as fresh in
//      `kServeCachedHtml`;
//      `kServeVariants` has no validation step, so there the flag is not
//      consulted and the entry is as fresh as its stored lifetime says.
//   9. Markdown.  A request for the Markdown copy is served it only while the
//      HTML parent is fresh, the copy exists, its recorded parent hash still
//      matches the live content, and the copy is within its own lifetime
//      (age below the smaller of the parent's lifetime and the copy's
//      render lifetime; for a parent the origin confirmed this request, only
//      the render lifetime bounds it).  The bound is the PARENT's age: the
//      copy is not separately limited by its own age, because the binding
//      hash already covers a content change.  A parent stored with
//      `no-transform` (RFC 9111 section 5.2.2.6), or asked for by a request
//      carrying `no-transform` (section 5.2.1.6), never has its copy served
//      (kNoTransform), and a parent whose copy the deriver withheld for this
//      content (`withheld`) falls through with kWithheld; both are checked
//      before the copy itself.  A Markdown request on a parent that
//      is not fresh is its own outcome, `kParentStale` (validated first in
//      `kServeCachedHtml`, a fall-through in `kServeVariants`), not the HTML
//      request's stale or needs-validation reason, so the Markdown path is
//      countable on its own.  A Markdown fall-through never degrades to
//      serving HTML here: the caller serves the origin path.
//  10. HTML.  In `kServeCachedHtml` a fresh entry is served.  In
//      `kServeVariants` the HTML still comes from the origin.

enum class HtmlCacheMode : uint8_t {
  kOff = 0,
  kReportOnly = 1,
  kServeVariants = 2,
  kServeCachedHtml = 3,
};

enum class HtmlStoreAction : uint8_t {
  kRefuse = 0,
  kStore = 1,
  // Store, but do not notify the optimizer (the response said no-transform).
  kStoreNoNotify = 2,
  // Report-only: what would have happened.  Nothing may be written.
  kWouldStore = 3,
  kWouldStoreNoNotify = 4,
};

// One per store rule, in rule order.  The names (HtmlStoreReasonName) are a
// closed set.
enum class HtmlStoreReason : uint8_t {
  kNone = 0,
  kMethod,
  kStatus,
  kIncomplete,
  kAuthRequest,
  kCookieRequest,
  kSetCookie,
  kPrivate,
  kNoStore,
  kNoLifetime,
  kVary,
  kHeaders,
  kNonceCsp,
  kNonceLink,
  kContentType,
  kEncoding,
  kSize,
  kUrl,
  kPersonalisationSignal,
  kNoTransform,
  kMode,
};

inline constexpr uint8_t kHtmlStoreReasonCount = 21;

struct HtmlStoreRequest {
  std::string_view method;          // "GET"; the caller normalises case
  std::string_view url_path;        // path as the cache key would see it
  bool has_authorization = false;   // Authorization or Proxy-Authorization
  bool authenticated_user = false;  // the server authenticated the request
  bool has_cookie = false;          // Cookie or Cookie2 present
  bool request_no_store = false;  // the request carried Cache-Control: no-store
};

struct HtmlStoreResponse {
  int status = 0;
  // The origin's header set, one entry per received line, before any header
  // the server or the caller adds.
  std::span<const HeaderField> headers;
  uint64_t body_bytes = 0;
  bool body_complete = true;  // false for an aborted or partial body
  // Up to kPersonalisationScanBytes of the body; may be empty.
  std::string_view body_prefix;
};

struct HtmlStorePolicy {
  HtmlCacheMode mode = HtmlCacheMode::kOff;
  // The operator's cap.  A value above the 16 MiB hard ceiling is clamped.
  uint64_t max_body_bytes = uint64_t{5} * 1024 * 1024;
  uint32_t now_seconds = 0;  // receipt time (RFC 9110 §6.6.1), for Expires
};

struct HtmlStoreVerdict {
  HtmlStoreAction action = HtmlStoreAction::kRefuse;
  HtmlStoreReason reason = HtmlStoreReason::kNone;
  // The lowercased header name for kHeaders, the token for kVary, the matched
  // signal name for kPersonalisationSignal; empty otherwise.  Header names
  // and Vary tokens are origin-controlled, so the detail is reduced to RFC
  // 9110 token characters (anything else becomes `_`) and capped at
  // kMaxHtmlStoreDetailBytes: it is safe to put on a log line, and a
  // reason_code() built from it always fits the decision line's grammar.
  std::string detail;
  // s-maxage, else max-age, else Expires minus Date; 0 when absent or invalid.
  uint32_t lifetime_s = 0;
  bool lifetime_explicit = false;
  bool needs_validation = false;  // lifetime 0, or no-cache in either form
  bool has_validators = false;    // a non-empty ETag or Last-Modified present
  bool explicit_shared = false;   // public, or an s-maxage above zero
  uint16_t cc_flags = 0;          // the accumulated kCCOrigin* bits
  std::vector<std::string> vary_tokens;  // lowercased, from the Vary lines
  // True once the header set reached the sidecar gate (rule 9); before that
  // `sidecar` holds its default and means nothing.
  bool sidecar_evaluated = false;
  // True once Cache-Control was parsed; before that `cc_flags` means nothing.
  bool cc_evaluated = false;
  SidecarClassification sidecar;  // the full classification, for the caller

  [[nodiscard]] bool stores() const {
    return action == HtmlStoreAction::kStore ||
           action == HtmlStoreAction::kStoreNoNotify;
  }
  // The reason name, with "headers:<name>" and "vary:<token>" carrying the
  // detail.
  [[nodiscard]] std::string reason_code() const;
};

inline constexpr size_t kMaxHtmlStoreDetailBytes = 128;
inline constexpr uint64_t kHtmlStoreHardMaxBodyBytes =
    uint64_t{16} * 1024 * 1024;

HtmlStoreVerdict ClassifyHtmlForStore(const HtmlStoreRequest& request,
                                      const HtmlStoreResponse& response,
                                      const HtmlStorePolicy& policy);

inline constexpr size_t kPersonalisationScanBytes = size_t{64} * 1024;

// Scan the first kPersonalisationScanBytes of `body_prefix` for a
// per-client marker.  Bounded, ASCII case-insensitive, and allocation-free per
// match.  Returns the signal name that matched:
//   meta_csrf_token, meta_csrf_param, hidden_token, hidden_csrf,
//   hidden_authenticity, hidden_request_verification,
//   hidden_csrfmiddlewaretoken, hidden_wpnonce, post_form_login,
//   post_form_checkout, oversized_tag.
// A `<meta>` is a signal when its `name` is csrf-token, csrf-param, _csrf or
// _csrf_header.  An `<input>` that is `type="hidden"` is a signal when its
// `name` is `_token` or `_wpnonce`, begins with `csrf` or `_csrf`, or is
// `authenticity_token`, `__RequestVerificationToken` or
// `csrfmiddlewaretoken`.  A `<form>` whose `method` is `post` is a signal
// when its `action` contains `login`/`signin` (post_form_login) or
// `checkout`/`cart` (post_form_checkout).  The first signal in document order
// wins.  A `<meta>`, `<input>` or `<form>` tag still open after 4 KiB is the
// signal oversized_tag: the attributes past the bound were not read.  The
// store rule applies the same signal to such a tag still open at the scan
// window's edge when the body is longer than the window.
std::optional<std::string_view> FindPersonalisationSignal(
    std::string_view body_prefix);

enum class HtmlServeAction : uint8_t {
  kFallThrough = 0,
  kServeHtml = 1,
  kServeMarkdown = 2,
  kValidate = 3,
};

enum class HtmlServeReason : uint8_t {
  kFresh = 0,
  kMode,
  kAuthRequest,
  kCookieRequest,
  kClientNoCache,
  kNeedsValidation,
  kStale,
  kHeadersNotReproducible,
  kSidecarMissing,
  kPurged,
  kAbsent,
  kBindingStale,
  kParentStale,
  // Appended (C API 1.14).  The numeric values are ABI: append only.
  kMethod,
  kRange,
  kCors,
  kNoTransform,
  kWithheld,
  kSkew,
};

inline constexpr uint8_t kHtmlServeReasonCount = 19;

// How far in the future an entry's stored_at may lie before serve rule 2a
// refuses it.  Covers clock disagreement between the processes that write and
// read one volume.
inline constexpr uint32_t kHtmlServeSkewSeconds = 5;

struct HtmlServeRequest {
  HtmlCacheMode mode = HtmlCacheMode::kOff;
  bool wants_markdown = false;  // Accept resolved to text/markdown by caller
  // Rule 1a.  `method_cacheable` is true for GET and HEAD.
  bool method_cacheable = true;
  bool has_range = false;  // Range or If-Range present
  bool has_origin = false;
  bool has_authorization = false;
  bool authenticated_user = false;
  bool has_cookie = false;
  bool has_cache_control = false;  // the request carried Cache-Control at all
  bool cache_control_no_cache = false;
  bool pragma_no_cache = false;  // counts only when has_cache_control is false
  int64_t max_age = -1;          // request max-age, -1 when absent
  int64_t min_fresh = -1;        // request min-fresh, -1 when absent
  // The request's Cache-Control carried no-transform (RFC 9111 section
  // 5.2.1.6): the Markdown copy is a transformation, so it is not served.
  bool request_no_transform = false;
  uint32_t now_seconds = 0;
};

struct StoredHtmlFacts {
  bool present = false;       // an HTML entry exists for the key
  uint32_t stored_at = 0;     // seconds, when the entry was recorded
  uint32_t age_at_store = 0;  // the origin's Age at record time
  uint32_t lifetime_s = 0;
  bool needs_validation = false;
  // The entry's Cache-Control bits are deliberately NOT an input: no serve
  // rule consults must-revalidate, proxy-revalidate or s-maxage, because a
  // stale entry is never served at all.
  bool explicit_shared = false;
  bool headers_not_reproducible = false;  // carries the not-reproducible bit
  bool sidecar_required = false;          // a header block must be replayed
  bool sidecar_present = false;           // the block was read and parsed
  bool purged = false;  // the caller's purge state covers this entry
  // The origin answered 304 just now.  Consulted in kServeCachedHtml only
  // (rule 8).
  bool validated_this_request = false;
  bool variant_present = false;   // a Markdown copy exists
  bool binding_live = false;      // the copy's parent hash equals live content
  uint32_t render_ttl_s = 86400;  // the copy's own lifetime bound
  // The parent was stored with Cache-Control: no-transform (rule 9).
  bool no_transform = false;
  // The deriver refused to build a copy for this parent's content (rule 9).
  bool withheld = false;
};

// Fold a request's Cache-Control value (every line joined with ", ") into
// `request`: `no-cache` sets cache_control_no_cache, `no-transform` sets
// request_no_transform, `max-age` and `min-fresh` set their fields, and
// has_cache_control is set.  Directive names
// are matched case-insensitively (RFC 9111 section 5.2).  A `max-age` or
// `min-fresh` whose argument is not delta-seconds counts as max-age=0 (only
// an entry of age 0 still answers) rather than guessing; a value too large for the field
// saturates (RFC 9111 section 1.2.2).  Other directives are ignored.
void ApplyHtmlRequestCacheControl(std::string_view cache_control,
                                  HtmlServeRequest* request);

struct HtmlServeVerdict {
  HtmlServeAction action = HtmlServeAction::kFallThrough;
  HtmlServeReason reason = HtmlServeReason::kFresh;
  // False in kOff and kReportOnly, where the action is always a fall-through.
  bool enforce = false;
  // RFC 9111 §4.2.3: age_at_store + max(0, now - stored_at), saturated.
  uint32_t age_s = 0;
  [[nodiscard]] std::string reason_code() const;
};

HtmlServeVerdict ClassifyHtmlForServe(const HtmlServeRequest& request,
                                      const StoredHtmlFacts& stored);

// THE COOKIE RULE, exported.  Store rule 8 and serve rule 4 above call
// HtmlCookieRequestMayShare; a front end that applies its own gates for the
// other rules (the nginx port, the ASP.NET Core package) calls the functions
// below, so every front end answers the cookie question the same way.
//
// A request that carried a Cookie header (Cookie or Cookie2, any value, an
// empty one included) is treated by response class:
//
//   kPage (everything that is not a static asset: HTML, XHTML, JSON, plain
//   text, the Markdown copy, and any unknown or missing Content-Type) — the
//   response may be stored for it, and a stored entry served to it, only when
//   the response is explicitly shareable: `public`, or a single `s-maxage`
//   whose value is a valid delta-seconds greater than zero.  `s-maxage=0`, a
//   malformed value and a repeated `s-maxage` assert nothing.
//
//   kStaticAsset (text/css, JavaScript, image/*) — the response may be stored
//   for it only when the origin stated a freshness lifetime (a max-age or
//   s-maxage above zero, or an Expires in the future): no class default
//   applies to a response fetched with a cookie.  A stored asset is served to
//   a cookie-bearing request as to any other.
//
// A request without a cookie is never affected.

enum class CookieShareClass : uint8_t {
  kPage = 0,
  kStaticAsset = 1,
};

// The class of a response from its Content-Type value: the media type is
// compared case-insensitively, parameters (`; charset=...`) and surrounding
// whitespace ignored.  text/css, application/javascript, text/javascript,
// application/x-javascript, application/ecmascript, text/ecmascript and
// image/* are kStaticAsset; everything else, an empty value included, is
// kPage.
[[nodiscard]] CookieShareClass CookieShareClassForContentType(
    std::string_view content_type);

// `explicit_shared` in the stored Cache-Control vocabulary
// (AlternateMetadata::kCCOrigin* flags plus the parsed s-maxage value):
// `public`, or one s-maxage (kCCOriginSMaxageRepeated clear) with a value
// above zero.  ClassifyHtmlForStore
// computes its own `explicit_shared` with this function.
[[nodiscard]] bool HtmlExplicitlyShared(uint16_t origin_cc_flags,
                                        uint32_t origin_s_maxage);

// Did the origin state a freshness lifetime?  The flags carry
// kCCOriginHeaderPresent (a Cache-Control line, or an Expires the caller
// folded into `origin_max_age`), and max-age or s-maxage is above zero.
[[nodiscard]] bool OriginStatesLifetime(uint16_t origin_cc_flags,
                                        uint32_t origin_max_age,
                                        uint32_t origin_s_maxage);

// The page rule alone: !request_has_cookie || explicit_shared.
[[nodiscard]] bool HtmlCookieRequestMayShare(bool request_has_cookie,
                                             bool explicit_shared);

// May the response to this request be recorded into the shared cache?
[[nodiscard]] bool CookieRequestMayStore(bool request_has_cookie,
                                         CookieShareClass response_class,
                                         bool explicit_shared,
                                         bool origin_states_lifetime);

// May this stored entry answer this request?
[[nodiscard]] bool CookieRequestMayServe(bool request_has_cookie,
                                         CookieShareClass entry_class,
                                         bool explicit_shared);

// RFC 9110 §5.6.2 tchar and token: the one implementation, used by the detail
// sanitiser here, by the decision line's grammar and by the fuzz harness, so
// what the policy emits and what the formatter admits cannot drift apart.
bool IsHttpTokenChar(char c);
bool IsHttpToken(std::string_view s);  // non-empty, every byte a tchar

// Closed-set names.  An out-of-range value yields "unknown".
std::string_view HtmlStoreActionName(HtmlStoreAction action);
std::string_view HtmlStoreReasonName(HtmlStoreReason reason);
std::string_view HtmlServeActionName(HtmlServeAction action);
std::string_view HtmlServeReasonName(HtmlServeReason reason);
// off, report-only, serve-variants, serve-cached-html
std::string_view HtmlCacheModeName(HtmlCacheMode mode);

// Parse an HTTP-date (RFC 9110 §5.6.7) to seconds since the Unix epoch: the
// IMF-fixdate form `Sun, 06 Nov 1994 08:49:37 GMT` and the two obsolete forms
// a recipient must accept, rfc850-date `Sunday, 06-Nov-94 08:49:37 GMT` and
// asctime-date `Sun Nov  6 08:49:37 1994`.  The rfc850 two-digit year is
// resolved against `now_seconds` (the receipt time) as the section says: a
// year that would be more than 50 years in the future is the most recent
// past year with those digits.  Anything else is invalid (nullopt), as is a
// weekday that does not match the date.
std::optional<int64_t> ParseHttpDateSeconds(std::string_view value,
                                            uint32_t now_seconds);

// The closed set of names FindPersonalisationSignal can return, for callers
// that validate or enumerate them.
std::span<const std::string_view> HtmlPersonalisationSignalNames();

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CACHE_HTML_CACHE_POLICY_H_
