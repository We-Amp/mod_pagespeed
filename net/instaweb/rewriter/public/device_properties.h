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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_DEVICE_PROPERTIES_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_DEVICE_PROPERTIES_H_

#include <vector>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/gtest_prod.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/user_agent_matcher.h"

namespace net_instaweb {

class RequestHeaders;

// This class keeps track of the device properties of the client, which are
// for the most part learned from the UserAgent string.
class DeviceProperties {
 public:
  explicit DeviceProperties(UserAgentMatcher* matcher);
  virtual ~DeviceProperties();

  void SetUserAgent(const StringPiece& user_agent_string);
  // Set device-based properties that are capture in the request headers
  // (eg. the Accept: header).
  void ParseRequestHeaders(const RequestHeaders& request_headers);
  bool SupportsImageInlining() const;
  bool SupportsLazyloadImages() const;
  bool SupportsCriticalCss() const;
  bool SupportsCriticalImagesBeacon() const;
  bool SupportsJsDefer(bool enable_mobile) const;
  // SupportsWebpInPlace indicates we saw an Accept: image/webp header.
  // Strictly Accept-driven: the UA-derived WebP verdict is excluded,
  // so a guessed capability never masquerades as an observed Accept header.
  // Since #640 the in-place path itself is request-independent and consults no
  // WebP capability at all; the remaining consumers are device logging and
  // RequestContext::accepts_webp_via_accept_header (the Vary: Accept
  // cache-validity check in OptionsAwareHTTPCacheCallback::IsCacheValid).
  bool SupportsWebpInPlace() const;
  // SupportsWebpRewrittenUrls indicates that the device can handle webp so long
  // as the url changes - either we know this based on user agent, or we got an
  // Accept header.  We can't tell a proxy cache to distinguish this case using
  // Vary: accept in the result headers, as we can't guarantee we'll see such a
  // header, ever.  So we need to Vary: user-agent or cache-control: private,
  // and thus restrict it to rewritten urls.  This is the only WebP accessor
  // that still consults the user agent, and it grants the lossy tier only.
  bool SupportsWebpRewrittenUrls() const;
  // Lossless/alpha and animated WebP gate solely on the Accept: image/webp
  // header, exactly like SupportsWebpInPlace above and the AVIF accessors
  // below: a client that advertises image/webp is taken to support every WebP
  // flavour.  No user-agent allow list is consulted.
  bool SupportsWebpLosslessAlpha() const;
  bool SupportsWebpAnimated() const;
  // AVIF capability accessors. Unlike WebP, AVIF has no "legacy no-Accept" UA
  // population to allow-list: support is strictly driven by the
  // Accept: image/avif header parsed in ParseRequestHeaders, so every level
  // gates solely on accepts_avif_ and none of these consult the UA matcher.
  // These are pure pre-decode request capabilities; the per-image
  // AVIF-vs-WebP-vs-original format choice happens at encode time, not here.
  bool SupportsAvifInPlace() const;
  bool SupportsAvifRewrittenUrls() const;
  bool SupportsAvifLosslessAlpha() const;
  bool SupportsAvifAnimated() const;
  bool IsBot() const;
  // Records the outcome of Web Bot Auth (RFC 9421 HTTP message signature)
  // verification for this request. Pass true only for a request whose signature
  // actually verified -- a signed agent or a registered verified bot. Pass
  // false, or never call this at all, for everything else.
  //
  // Consequences of a true verdict, i.e. everything IsBot() gates:
  // add_instrumentation is disabled outright, the critical-image and
  // critical-CSS beacons are suppressed, lazyload_images is disabled,
  // the defer_javascript family is disabled (js_defer_disabled, js_disable,
  // defer_iframe, fix_reflow and the support_noscript fallback all gate on
  // SupportsJsDefer), background fetches are skipped when
  // DisableBackgroundFetchesForBots is on (default off), the design record WebP
  // user-agent fallback declines to grant WebP from the UA
  // (ApplyUserAgentWebpFallback in device_properties.cc), and the request is
  // logged as a bot by LogDeviceInfo.
  //
  // The override is deliberately one-directional: a valid signature is a
  // cryptographic assertion of bot-ness, and never an assertion of humanity.
  // "No signature material present" and "verification failed" are both far too
  // common among ordinary bots to be read as evidence of a human, so a false
  // verdict simply falls through to the user-agent heuristic. IsBot() can
  // therefore only ever move from false to true because of this call.
  //
  // Timing caveat: unlike the other consumers above, which read IsBot() at
  // rewrite time, the WebP fallback reads it during ParseRequestHeaders --
  // and the nginx port can only apply this verdict AFTER SetRequestHeaders
  // (ps_apply_webbotauth_verdict in ngx_pagespeed.cc; calling earlier would
  // see the verdict discarded when SetRequestHeaders recreates
  // RequestProperties). The fallback therefore sees the verdict as of parse
  // time, and a verdict recorded later is NOT retroactive on an
  // already-granted fallback. So the honest blast-radius statement is: a
  // late true verdict withholds beacons, lazyload and JS-defer as intended,
  // but a verified agent asserting a Safari-16+/Firefox-132+ UA may still be
  // served rewritten .webp URLs for that request. That residue is bounded --
  // it never serves anything broken, only a format the asserted UA decodes --
  // and it is still STRICTER than the Accept path, which grants WebP on
  // "Accept: image/webp" with no IsBot() gate at all.
  //
  // Takes a plain bool rather than the webbotauth verdict enum on purpose: the
  // port binding collapses the enum, so this layer keeps no dependency on
  // pagespeed/kernel/webbotauth, and ports that do not implement Web Bot Auth
  // simply never call it.
  void SetWebBotAuthVerdict(bool signature_verified_agent);
  bool AcceptsGzip() const;
  UserAgentMatcher::DeviceType GetDeviceType() const;
  bool IsMobile() const { return GetDeviceType() == UserAgentMatcher::kMobile; }
  bool IsTablet() const { return GetDeviceType() == UserAgentMatcher::kTablet; }

  enum ImageQualityPreference {
    // Server uses its own default image quality.
    kImageQualityDefault,
    // The request asks for low image quality.
    kImageQualityLow,
    // The request asks for medium image quality.
    kImageQualityMedium,
    // The request asks for high image quality.
    kImageQualityHigh,
  };
  static const int kMediumScreenWidthThreshold = 720;
  static const int kLargeScreenWidthThreshold = 1500;
  bool ForbidWebpInlining() const;

  bool RequestsSaveData() const;
  bool HasViaHeader() const;

 private:
  friend class ImageRewriteTest;
  friend class RequestProperties;

  GoogleString user_agent_;
  GoogleString accept_header_;
  UserAgentMatcher* ua_matcher_;

  mutable LazyBool supports_critical_css_;
  mutable LazyBool supports_image_inlining_;
  mutable LazyBool supports_js_defer_;
  mutable LazyBool supports_lazyload_images_;
  mutable LazyBool requests_save_data_;
  // Grants WebP from the user-agent string when the Accept header did not.
  // Precondition: accepts_webp_ == kFalse.
  void ApplyUserAgentWebpFallback();

  mutable LazyBool accepts_webp_;
  // True when accepts_webp_ was set to kTrue by ApplyUserAgentWebpFallback
  // rather than by an "Accept: image/webp" request header.  Every
  // WebP capability accessor may consult accepts_webp_ freely; the one thing
  // this bit forbids is SupportsWebpInPlace(), which must report only an
  // observed Accept header, never a user-agent guess.  It is also what lets
  // SetUserAgent withdraw a grant the previous user-agent string earned.
  mutable LazyBool webp_ua_derived_;
  // Whether the request carried "Accept: image/avif". Mirrors accepts_webp_:
  // ctor-initialized to kNotSet, set once in ParseRequestHeaders, and (like
  // accepts_webp_) NOT reset in SetUserAgent, since AVIF support is a property
  // of the request headers, not of the user-agent string.
  mutable LazyBool accepts_avif_;
  // Whether Web Bot Auth verification succeeded for this request. Like
  // accepts_avif_ above, this is a property of the request headers rather than
  // of the user-agent string, so SetUserAgent must NOT reset it.
  //
  // IsBot() consults this ahead of the is_bot_ memo, so IsBot() specifically is
  // order-independent with respect to SetUserAgent and SetWebBotAuthVerdict.
  // That guarantee stops at IsBot(): its callers memoise their OWN results
  // around it (supports_lazyload_images_ and supports_js_defer_ here, their
  // RequestProperties twins, and SupportsCriticalImagesBeacon
  // via SupportsImageInlining), so a verdict that arrives after one of those has
  // first been read is not retroactive. Set the verdict before any consumer
  // runs. The nginx port does (SetRequestHeaders, then the verdict, both before
  // the driver starts parsing); any port wiring this up later must too. One
  // consumer is structurally out of reach of that rule: the design record WebP
  // fallback reads IsBot() inside ParseRequestHeaders itself, i.e. before any
  // port can call SetWebBotAuthVerdict -- see the timing caveat at
  // SetWebBotAuthVerdict above for the (bounded) consequence.
  bool webbotauth_verified_agent_;
  mutable LazyBool accepts_gzip_;
  mutable LazyBool supports_webp_rewritten_urls_;
  mutable LazyBool is_bot_;
  mutable LazyBool is_mobile_user_agent_;
  mutable LazyBool supports_split_html_;
  mutable LazyBool supports_flush_early_;
  const std::vector<int>* preferred_webp_qualities_;
  const std::vector<int>* preferred_jpeg_qualities_;
  // Used to lazily set device_type_.
  mutable LazyBool device_type_set_;
  mutable UserAgentMatcher::DeviceType device_type_;
  mutable LazyBool has_via_header_;

  DeviceProperties(const DeviceProperties&) = delete;
  DeviceProperties& operator=(const DeviceProperties&) = delete;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_DEVICE_PROPERTIES_H_
