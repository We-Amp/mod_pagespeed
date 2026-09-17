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

#ifndef PAGESPEED_KERNEL_HTTP_USER_AGENT_MATCHER_H_
#define PAGESPEED_KERNEL_HTTP_USER_AGENT_MATCHER_H_

#include <map>
#include <memory>
#include <utility>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/fast_wildcard_group.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/util/re2.h"

using std::make_pair;
using std::map;
using std::pair;

namespace net_instaweb {

class RequestHeaders;

// This class contains various user agent based checks.  Currently all of these
// are based on simple wildcard based allow- and blocked-lists.
//
// TODO(sriharis):  Split the functionality here into two: a matcher that
// pulls out all relevant information from UA strings (browser-family, version,
// mobile/tablet/desktop, etc.), and a query interface that can be used by
// clients.
class UserAgentMatcher {
 public:
  static const char kTestUserAgentWebP[];  // webp user agent
  // Note that this must not contain the substring "webp".
  static const char kTestUserAgentNoWebP[];  // non-webp user agent
  static const char kTestUserAgentAvif[];    // avif user agent

  // Fixed underlying type: holding an out-of-range value (e.g. the death
  // test's DeviceType(-1)) is then defined behavior, so the defensive
  // range check in ExperimentSpec::matches_device_type — not UB — is what
  // fires.
  enum DeviceType : int {
    kDesktop,
    kTablet,
    kMobile,
    // This should always be the last type. This is used to mark the size of an
    // array containing various DeviceTypes.
    kEndOfDeviceType
  };

  UserAgentMatcher();
  virtual ~UserAgentMatcher();

  // Before calling IsIe, ask if you're doing the right thing: are you doing
  // something that will mess up IE 11 in standards mode?  Are you in a position
  // where you can't tell what compatibility mode IE 11 is in?  Right now we use
  // this only to force edge compatibility mode and to work around a persistent
  // IE Vary: caching bug.
  bool IsIe(const StringPiece& user_agent) const;

  virtual bool SupportsImageInlining(const StringPiece& user_agent) const;
  bool SupportsLazyloadImages(StringPiece user_agent) const;

  // Returns true if the user agent is known to support the native
  // loading="lazy" attribute on <img> elements. This is a conservative
  // allow-list: Chromium >= 77 (Chrome, Edge, Opera, all of which carry a
  // Chrome/<version> token), Firefox >= 75, and Safari >= 15.4. Unknown user
  // agents return false so that callers can fall back to the script-based
  // lazyload implementation.
  bool SupportsNativeLazyLoading(StringPiece user_agent) const;

  // Returns the DeviceType for the given user agent string.
  virtual DeviceType GetDeviceTypeForUA(const StringPiece& user_agent) const;

  // Returns the DeviceType using the given user agent string and request
  // headers.
  virtual DeviceType GetDeviceTypeForUAAndHeaders(
      const StringPiece& user_agent,
      const RequestHeaders* request_headers) const;

  // Returns a string representing the device_type ("desktop", "tablet", or
  // "mobile").
  static StringPiece DeviceTypeString(DeviceType device_type);

  // Returns the suffix for the given device_type.
  static StringPiece DeviceTypeSuffix(DeviceType device_type);

  bool SupportsJsDefer(const StringPiece& user_agent, bool allow_mobile) const;

  // Returns true if the user agent includes a legacy browser that supports
  // webp, but does not issue Accept:image/webp.  At the moment, this means
  // only Android 4.0+ (excluding Firefox).  This is the ONLY UA-derived WebP
  // signal that remains: which WebP flavours a client can decode (lossy,
  // lossless/alpha, animated) is read off the Accept: image/webp header in
  // DeviceProperties, not from any browser-version list here.
  bool LegacyWebp(const StringPiece& user_agent) const;

  // Returns true if the user agent decodes WebP but is known not to advertise
  // "image/webp" in the Accept header of a navigation request -- Safari 16+
  // and Firefox 132+.  This answers a strictly narrower question
  // than LegacyWebp() above, which covers the old Android population.
  //
  // The verdict is a guess derived from a client-controlled string, so it may
  // only ever shape responses whose representation is named by the URL itself
  // -- rewritten .pagespeed. URLs.  It must never shape a response that a
  // shared cache would file under a request-header key: the request's Accept
  // header did not determine it, and a cache keyed on Accept would hand the
  // result to clients that cannot decode it.  The in-place path, the one
  // place that used to serve per-request bytes from the ORIGINAL URL, is
  // request-independent since #640 and never consults request-derived WebP
  // capability, so this confinement holds structurally; DeviceProperties
  // additionally keeps SupportsWebpInPlace() strictly Accept-driven so the
  // logged capability stays honest.
  bool SupportsWebpButOmitsNavigationAccept(
      const StringPiece& user_agent) const;

  // AVIF support. Like WebP above, AVIF capability is decided from the
  // Accept: image/avif header in DeviceProperties; unlike WebP there is not
  // even a legacy no-Accept UA population, so there is deliberately no
  // allow/block list and no LegacyAvif() analogue. These UA-only
  // queries therefore carry no signal and report false.
  bool SupportsAvifLosslessAlpha(const StringPiece& user_agent) const;
  bool SupportsAvifAnimated(const StringPiece& user_agent) const;

  bool SupportsDnsPrefetch(const StringPiece& user_agent) const;

  virtual bool IsAndroidUserAgent(const StringPiece& user_agent) const;
  virtual bool IsiOSUserAgent(const StringPiece& user_agent) const;

  // Returns false if this is not a Chrome user agent, or parsing the
  // string build number fails.
  virtual bool GetChromeBuildNumber(const StringPiece& user_agent, int* major,
                                    int* minor, int* build, int* patch) const;

  bool UserAgentExceedsChromeAndroidBuildAndPatch(const StringPiece& user_agent,
                                                  int required_build,
                                                  int required_patch) const;

  bool UserAgentExceedsChromeiOSBuildAndPatch(const StringPiece& user_agent,
                                              int required_build,
                                              int required_patch) const;

  bool UserAgentExceedsChromeBuildAndPatch(const StringPiece& user_agent,
                                           int required_build,
                                           int required_patch) const;

  bool SupportsMobilization(StringPiece user_agent) const;

 private:
  FastWildcardGroup supports_image_inlining_;
  FastWildcardGroup supports_lazyload_images_;
  FastWildcardGroup defer_js_allowlist_;
  FastWildcardGroup defer_js_mobile_allowlist_;
  FastWildcardGroup legacy_webp_;
  FastWildcardGroup webp_no_navigation_accept_;
  FastWildcardGroup supports_dns_prefetch_;
  FastWildcardGroup mobile_user_agents_;
  FastWildcardGroup tablet_user_agents_;
  FastWildcardGroup ie_user_agents_;
  FastWildcardGroup mobilization_user_agents_;

  const RE2 chrome_version_pattern_;
  std::unique_ptr<RE2> known_devices_pattern_;
  mutable map<GoogleString, pair<int, int> > screen_dimensions_map_;

  UserAgentMatcher(const UserAgentMatcher&) = delete;
  UserAgentMatcher& operator=(const UserAgentMatcher&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_HTTP_USER_AGENT_MATCHER_H_
