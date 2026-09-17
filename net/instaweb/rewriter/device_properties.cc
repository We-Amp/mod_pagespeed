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

#include "net/instaweb/rewriter/public/device_properties.h"

#include "base/logging.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/bot_checker.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/user_agent_matcher.h"

namespace net_instaweb {

DeviceProperties::DeviceProperties(UserAgentMatcher* matcher)
    : ua_matcher_(matcher),
      supports_critical_css_(kNotSet),
      supports_image_inlining_(kNotSet),
      supports_js_defer_(kNotSet),
      supports_lazyload_images_(kNotSet),
      requests_save_data_(kNotSet),
      accepts_webp_(kNotSet),
      webp_ua_derived_(kNotSet),
      accepts_avif_(kNotSet),
      webbotauth_verified_agent_(false),
      supports_webp_rewritten_urls_(kNotSet),
      is_bot_(kNotSet),
      is_mobile_user_agent_(kNotSet),
      supports_split_html_(kNotSet),
      supports_flush_early_(kNotSet),
      device_type_set_(kNotSet),
      device_type_(UserAgentMatcher::kDesktop),
      has_via_header_(kNotSet) {}

DeviceProperties::~DeviceProperties() {}

void DeviceProperties::SetUserAgent(const StringPiece& user_agent_string) {
  user_agent_string.CopyToString(&user_agent_);

  // Reset everything determined by user agent.
  supports_critical_css_ = kNotSet;
  supports_image_inlining_ = kNotSet;
  supports_js_defer_ = kNotSet;
  supports_lazyload_images_ = kNotSet;
  supports_webp_rewritten_urls_ = kNotSet;
  is_bot_ = kNotSet;
  is_mobile_user_agent_ = kNotSet;
  supports_split_html_ = kNotSet;
  supports_flush_early_ = kNotSet;

  // accepts_webp_ is deliberately NOT reset here: it records what the request's
  // Accept header said, and replacing the user-agent string cannot invalidate
  // that.  The UA-derived *fallback* verdict, on the other hand, is a
  // pure function of the string we just replaced, so it has to be withdrawn and
  // recomputed.
  //
  // This is what keeps SetUserAgent() and ParseRequestHeaders() order
  // independent.  Deriving WebP capability from the UA would otherwise promote
  // "SetUserAgent before ParseRequestHeaders" -- currently just how
  // RewriteDriver happens to call them -- into a load-bearing invariant that
  // nothing checks and that ParseRequestHeaders' single-call DCHECK cannot
  // enforce.  Instead, whichever of the two runs second establishes the
  // verdict, and callers need not know the order.
  if (webp_ua_derived_ == kTrue) {
    // accepts_webp_ was true only because of the previous user agent.
    accepts_webp_ = kFalse;
  }
  webp_ua_derived_ = kNotSet;
  if (accepts_webp_ == kFalse) {
    // Headers have already been parsed and said no; re-run the fallback against
    // the new user agent.  If they have not been parsed yet (kNotSet),
    // ParseRequestHeaders will run it.
    ApplyUserAgentWebpFallback();
  }
}

// Grants WebP to browsers that decode it but omit "image/webp" from
// the navigation Accept header (Safari 16+, Firefox 132+ -- the Safari floor
// is Version/16, not 14: see the rationale at kWebpNoNavigationAcceptAllowlist
// in user_agent_matcher.cc).  Without this fallback those browsers -- close to
// a fifth of global traffic -- silently keep the original JPEG/PNG; measured
// against the same source image, that is several times the bytes a browser
// that does advertise WebP receives.  Precondition: accepts_webp_ == kFalse,
// i.e. the Accept header has been examined and did not advertise WebP.
//
// Bots are excluded independently of the UA matcher's own crawler denies: a
// crawler that fetches a WebP variant can cache and redistribute it to clients
// that never asked for it, and no bot benefits from the byte savings.
//
// The IsBot() read here is a consumer in the sense of the ordering note at
// webbotauth_verified_agent_ in device_properties.h: the verdict this
// function bakes into webp_ua_derived_ is not retroactive, so a Web Bot Auth
// verdict recorded only after ParseRequestHeaders would not withdraw an
// already-granted fallback.  The existing contract ("set the verdict before
// any consumer runs") covers this; the residual exposure is a verified bot
// presenting a byte-identical Safari UA being served the same rewritten .webp
// URLs real Safari gets.
void DeviceProperties::ApplyUserAgentWebpFallback() {
  if (!IsBot() &&
      ua_matcher_->SupportsWebpButOmitsNavigationAccept(user_agent_)) {
    accepts_webp_ = kTrue;
    webp_ua_derived_ = kTrue;
  } else {
    webp_ua_derived_ = kFalse;
  }
}

void DeviceProperties::ParseRequestHeaders(
    const RequestHeaders& request_headers) {
  DCHECK_EQ(kNotSet, accepts_webp_) << "Double call to ParseRequestHeaders";
  accepts_webp_ = request_headers.HasValue(HttpAttributes::kAccept,
                                           kContentTypeWebp.mime_type())
                      ? kTrue
                      : kFalse;
  webp_ua_derived_ = kFalse;
  if (accepts_webp_ == kFalse) {
    // The Accept header did not advertise WebP. Some browsers decode it anyway
    // and simply never list image types on a navigation request; ask
    // the user agent. See ApplyUserAgentWebpFallback and SupportsWebpInPlace.
    ApplyUserAgentWebpFallback();
  }
  // AVIF is strictly Accept-header-driven; there is no legacy
  // no-Accept UA population, unlike WebP. Match kContentTypeAvif.mime_type()
  // exactly as accepts_webp_ matches kContentTypeWebp above.
  accepts_avif_ = request_headers.HasValue(HttpAttributes::kAccept,
                                           kContentTypeAvif.mime_type())
                      ? kTrue
                      : kFalse;
  accepts_gzip_ = request_headers.HasValue(HttpAttributes::kAcceptEncoding,
                                           HttpAttributes::kGzip)
                      ? kTrue
                      : kFalse;

  const char* save_data_header =
      request_headers.Lookup1(HttpAttributes::kSaveData);
  if (save_data_header != nullptr && StringCaseEqual("on", save_data_header)) {
    requests_save_data_ = kTrue;
  } else {
    requests_save_data_ = kFalse;
  }

  has_via_header_ = request_headers.Has(HttpAttributes::kVia) ? kTrue : kFalse;
}

bool DeviceProperties::AcceptsGzip() const {
  if (accepts_gzip_ == kNotSet) {
    LOG(DFATAL) << "Check of AcceptsGzip before value is set.";
    accepts_gzip_ = kFalse;
  }
  return (accepts_gzip_ == kTrue);
}

bool DeviceProperties::SupportsImageInlining() const {
  if (supports_image_inlining_ == kNotSet) {
    supports_image_inlining_ =
        ua_matcher_->SupportsImageInlining(user_agent_) ? kTrue : kFalse;
  }
  return (supports_image_inlining_ == kTrue);
}

bool DeviceProperties::SupportsLazyloadImages() const {
  if (supports_lazyload_images_ == kNotSet) {
    supports_lazyload_images_ =
        (!IsBot() && ua_matcher_->SupportsLazyloadImages(user_agent_)) ? kTrue
                                                                       : kFalse;
  }
  return (supports_lazyload_images_ == kTrue);
}

bool DeviceProperties::SupportsCriticalCss() const {
  // Currently CriticalSelectorFilter can't deal with IE conditional comments,
  // so we disable ourselves for IE.
  // TODO(morlovich): IE10 in strict mode disables the conditional comments
  // feature; but the strict mode is determined by combination of doctype and
  // X-UA-Compatible, which can come in both meta and header flavors. Once we
  // have a good way of detecting this case, we can enable us for strict IE10.
  if (supports_critical_css_ == kNotSet) {
    supports_critical_css_ = !ua_matcher_->IsIe(user_agent_) ? kTrue : kFalse;
  }
  return (supports_critical_css_ == kTrue);
}

bool DeviceProperties::SupportsCriticalImagesBeacon() const {
  // For now this script has the same user agent requirements as image inlining,
  // however that could change in the future if more advanced JS is used by the
  // beacon. Also disable for bots. See
  // https://github.com/apache/incubator-pagespeed-mod/issues/813.
  return SupportsImageInlining() && !IsBot();
}

// Note that the result of the function is cached as supports_js_defer_. This
// must be cleared before calling the function a second time with a different
// value for allow_mobile.
//
// Bots are excluded, exactly as in SupportsLazyloadImages above. defer_javascript
// retypes every script to text/psajs, which is inert to any client that does not
// run PageSpeed's deferral runtime: an automated client would receive a page whose
// scripts never execute and whose external JavaScript is never even fetched. The
// UA-matcher allowlist consulted below still names Googlebot, Mediapartners-Google
// and Wget by hand; the !IsBot() term overrides all three, deliberately -- see the
// note at kDeferJSAllowlist in user_agent_matcher.cc.
//
// The IsBot() term is inside the memo, so (per the ordering note on
// webbotauth_verified_agent_ in device_properties.h) a Web Bot Auth verdict must be
// recorded before the first consumer reads this; it is not retroactive.
bool DeviceProperties::SupportsJsDefer(bool allow_mobile) const {
  if (supports_js_defer_ == kNotSet) {
    supports_js_defer_ =
        (!IsBot() && ua_matcher_->SupportsJsDefer(user_agent_, allow_mobile))
            ? kTrue
            : kFalse;
  }
  return (supports_js_defer_ == kTrue);
}

bool DeviceProperties::SupportsWebpInPlace() const {
  // We used to check accepts_webp_ == kNotSet here, but many tests don't bother
  // setting request headers.  So we simply use kNotSet to detect
  // double-initialization above.
  //
  // The UA-derived verdict is excluded here, so this accessor keeps
  // reporting exactly what its name and comment claim: the request advertised
  // "Accept: image/webp".  That is also the whole reason the fallback is
  // tracked in webp_ua_derived_ rather than folded into accepts_webp_
  // invisibly.
  //
  // Nothing on the in-place path consults this anymore: in-place optimization
  // is request-independent, never selects WebP, and serves the same
  // bytes to every client, which is what structurally confines the UA-derived
  // grant to rewritten URLs (the format there is committed in the URL itself,
  // so no Vary honesty question arises).  The remaining consumers are device
  // logging (RequestProperties::LogDeviceInfo), where a guessed capability
  // must not masquerade as an observed Accept header, and the
  // accepts_webp_via_accept_header bit that
  // RewriteDriver::PopulateRequestContext stamps into the RequestContext,
  // which decides whether a cached "Vary: Accept" WebP response is valid
  // as-selected for this request.
  return (accepts_webp_ == kTrue) && (webp_ua_derived_ != kTrue);
}

// The only WebP capability still derived from the user-agent string. It exists
// for the "webp-capable but sends no Accept: image/webp on this request"
// population -- historically the Android browser, see kLegacyWebpAllowlist in
// user_agent_matcher.cc. It grants the lossy tier only; a user agent alone can
// never reach lossless/alpha or animated (see the two accessors below).
bool DeviceProperties::SupportsWebpRewrittenUrls() const {
  if (supports_webp_rewritten_urls_ == kNotSet) {
    if ((accepts_webp_ == kTrue) || ua_matcher_->LegacyWebp(user_agent_)) {
      supports_webp_rewritten_urls_ = kTrue;
    } else {
      supports_webp_rewritten_urls_ = kFalse;
    }
  }
  return (supports_webp_rewritten_urls_ == kTrue);
}

// Lossless/alpha and animated WebP are decided by the Accept header alone, the
// way SupportsWebpInPlace above and all of AVIF below already are. A client
// that advertises image/webp is taken at its word for every WebP flavour: the
// hand-maintained browser-version lists this used to consult had to be updated
// on every browser release, went stale on version-digit rollovers, and could
// never say anything about a browser nobody had enumerated yet.
bool DeviceProperties::SupportsWebpLosslessAlpha() const {
  return (accepts_webp_ == kTrue);
}

bool DeviceProperties::SupportsWebpAnimated() const {
  return (accepts_webp_ == kTrue);
}

// AVIF capability accessors. Unlike the WebP accessors above, none of these
// consult the UA matcher: AVIF has no "legacy no-Accept" UA population to
// allow-list, so support is decided solely by the Accept: image/avif
// header captured in accepts_avif_. All four levels therefore share the same
// gate; the per-image AVIF-vs-WebP-vs-original choice happens at encode time.
bool DeviceProperties::SupportsAvifInPlace() const {
  return (accepts_avif_ == kTrue);
}

bool DeviceProperties::SupportsAvifRewrittenUrls() const {
  return (accepts_avif_ == kTrue);
}

bool DeviceProperties::SupportsAvifLosslessAlpha() const {
  return (accepts_avif_ == kTrue);
}

bool DeviceProperties::SupportsAvifAnimated() const {
  return (accepts_avif_ == kTrue);
}

void DeviceProperties::SetWebBotAuthVerdict(bool signature_verified_agent) {
  // Monotone: only ever raises the flag. A later unsigned or unverifiable
  // request state cannot retract a signature that already verified, and no
  // caller can use this to assert that a client is human.
  webbotauth_verified_agent_ =
      webbotauth_verified_agent_ || signature_verified_agent;
}

bool DeviceProperties::IsBot() const {
  // A verified signature outranks the user-agent string, and is checked ahead
  // of the is_bot_ memo so the answer does not depend on whether IsBot() was
  // called before or after SetWebBotAuthVerdict. This is the only signal that
  // can classify an agent presenting a byte-identical copy of a real browser's
  // user agent, which no user-agent list can do by construction.
  if (webbotauth_verified_agent_) {
    return true;
  }
  if (is_bot_ == kNotSet) {
    is_bot_ = BotChecker::Lookup(user_agent_) ? kTrue : kFalse;
  }
  return (is_bot_ == kTrue);
}

UserAgentMatcher::DeviceType DeviceProperties::GetDeviceType() const {
  if (device_type_set_ == kNotSet) {
    device_type_ = ua_matcher_->GetDeviceTypeForUA(user_agent_);
    device_type_set_ = kTrue;
  }
  return device_type_;
}

// Chrome 36 on iOS devices failed to display inlined WebP image, so inlining
// WebP on these devices is forbidden.
// https://bugs.chromium.org/p/chromium/issues/detail?id=402514
bool DeviceProperties::ForbidWebpInlining() const {
  if (ua_matcher_->IsiOSUserAgent(user_agent_)) {
    int major = kNotSet;
    int minor = kNotSet;
    int build = kNotSet;
    int patch = kNotSet;
    if (ua_matcher_->GetChromeBuildNumber(user_agent_, &major, &minor, &build,
                                          &patch) &&
        (major == 36 || major == 37)) {
      return true;
    }
  }
  return false;
}

bool DeviceProperties::RequestsSaveData() const {
  return (requests_save_data_ == kTrue);
}

bool DeviceProperties::HasViaHeader() const {
  return (has_via_header_ == kTrue);
}

}  // namespace net_instaweb
