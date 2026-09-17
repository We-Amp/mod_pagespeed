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

#include "pagespeed/kernel/http/user_agent_matcher.h"

#include <map>
#include <memory>
#include <utility>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/fast_wildcard_group.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/util/re2.h"

namespace net_instaweb {

const char UserAgentMatcher::kTestUserAgentWebP[] = "test-user-agent-webp";
// Note that this must not contain the substring "webp".
const char UserAgentMatcher::kTestUserAgentNoWebP[] = "test-user-agent-no";
const char UserAgentMatcher::kTestUserAgentAvif[] = "test-user-agent-avif";

class RequestHeaders;

// These are the user-agents of browsers/mobile devices which support
// image-inlining. The data is from "Latest WURFL Repository"(mobile devices)
// and "Web Patch"(browsers) on http://wurfl.sourceforge.net
// The user-agent string for Opera could be in the form of "Opera 7" or
// "Opera/7", we use the wildcard pattern "Opera?7" for this case.
namespace {

const char kGooglePlusUserAgent[] =
    "*Google (+https://developers.google.com/+/web/snippet/)*";

const char* kImageInliningAllowlist[] = {
    "*Android*",
    "*Chrome/*",
    "*Firefox/*",
    "*iPad*",
    "*iPhone*",
    "*iPod*",
    "*itouch*",
    "*Opera*",
    "*Safari*",
    "*Wget*",
    // Allow in ads policy checks to match usual UA behavior.
    "AdsBot-Google*",
    // Plus IE, see use in the code.
    // The following user agents are used only for internal testing
    "google command line rewriter",
    "webp",
    "webp-la",
};
const char* kImageInliningBlockedlist[] = {
    "*Firefox/1.*", "*Firefox/2.*", "*MSIE 5.*", "*MSIE 6.*",
    "*MSIE 7.*",    "*Opera?5*",    "*Opera?6*", kGooglePlusUserAgent};

// Exclude BlackBerry OS 5.0 and older. See
// http://supportforums.blackberry.com/t5/Web-and-WebWorks-Development/How-to-detect-the-BlackBerry-Browser/ta-p/559862
// for details on BlackBerry UAs.
// Exclude all Opera Mini: see bug #1070.
// https://github.com/apache/incubator-pagespeed-mod/issues/1070
const char* kLazyloadImagesBlockedlist[] = {"BlackBerry*CLDC*", "*Opera Mini*",
                                            kGooglePlusUserAgent};

// For defer js we only allow Firefox4+, IE8+, safari and Chrome
// We'll be updating this as and when required.
// The blockedlist is checked first, then if not in there, the allowlist is
// checked.
// Historically this list read "Do allow googlebot, since we run defer js for
// modern browsers." That is no longer the effective policy: DeviceProperties::
// SupportsJsDefer now ANDs in !IsBot(), so *Wget*, *Googlebot* and
// *Mediapartners-Google* below are unreachable-in-effect -- every client that
// matches them is classified a bot by BotChecker and never reaches this list at
// the DeviceProperties layer. A client that does not run PageSpeed's deferral
// runtime cannot use text/psajs markup at all, so crawlers and fetchers now get
// the page as authored. The entries are kept rather than deleted so that this
// remains a policy change at one seam, not a data edit spread across two layers;
// UserAgentMatcher::SupportsJsDefer is still unit-tested at its own layer and
// still answers true for them.
// Note: None of the following should match a mobile UA.
const char* kDeferJSAllowlist[] = {"*Chrome/*", "*Firefox/*", "*Safari*",
                                   // Plus IE, see code below.
                                   // Bot entries: dead in effect, see above.
                                   "*Wget*", "*Googlebot*",
                                   "*Mediapartners-Google*"};
const char* kDeferJSBlockedlist[] = {
    "*Firefox/1.*", "*Firefox/2.*", "*Firefox/3.*", "*MSIE 5.*",
    "*MSIE 6.*",    "*MSIE 7.*",    "*MSIE 8.*",
};
const char* kDeferJSMobileAllowlist[] = {
    "*AppleWebKit/*",
};

// Webp support for most devices should be triggered on Accept:image/webp.
// However we special-case Android 4.0 browsers which are fairly commonly
// used, support webp, and don't send Accept:image/webp.  Very old versions
// of Chrome may support webp without Accept:image/webp, but it is safe to
// ignore them because they are extremely rare.
//
// For legacy webp rewriting, we allowlist Android, but blockedlist
// older versions and Firefox, which includes 'Android' in its UA.
// We do this in 2 stages in order to exclude the following category 1 but
// include category 2.
//  1. Firefox on Android does not support WebP, and it has "Android" and
//     "Firefox" in the user agent.
//  2. Recent Opera support WebP, and some Opera have both "Opera" and
//     "Firefox" in the user agent.
// This list previously also carried "*Firefox/66.*" .. "*Firefox/71.*". Those
// entries never took effect: the open-ended "*Firefox/*" entry in
// kLegacyWebpBlockedlist below is registered after every allow entry and the
// highest matching index wins, so LegacyWebp() was false for every Firefox
// regardless. They are gone; see
// https://github.com/apache/incubator-pagespeed-mod/issues/596 and the
// UserAgentMatcherTest.DoesntSupportWebp assertions that pin the verdict.
const char* kLegacyWebpAllowlist[] = {
    "*Android *",
};

// Based on https://github.com/apache/incubator-pagespeed-mod/issues/978,
// Desktop IE11 will start masquerading as Chrome soon, and according to
// https://groups.google.com/forum/?utm_medium=email&utm_source=footer#!msg/mod-pagespeed-discuss/HYzzdOzJu_k/ftdV8koVgUEJ
// a browser called Midori might (at some point) masquerade as Chrome as well.
const char* kLegacyWebpBlockedlist[] = {
    "*Android 0.*",  "*Android 1.*",  "*Android 2.*",  "*Android 3.*",
    "*Firefox/*",    "*Edge/*",       "*Trident/*",    "*Windows Phone*",
    "*Chrome/*",  // Genuine Chrome always sends Accept: webp.
    "*CriOS/*",   // Paranoia: we should not see Android and CriOS together.
    "*Firefox/?.*",  "*Firefox/1?.*", "*Firefox/2?.*", "*Firefox/3?.*",
    "*Firefox/4?.*", "*Firefox/5?.*", "*Firefox/60.*", "*Firefox/61.*",
    "*Firefox/62.*", "*Firefox/63.*",
    "*Firefox/64.*",  // Firefox versions not webp capables
};

// Lossless/alpha and animated WebP used to be decided from hand-maintained
// browser-version allow/block lists here. They are gone: DeviceProperties now
// reads both capabilities off the "Accept: image/webp" request header alone,
// the way AVIF already did. The lists had to be updated on every browser
// release, went stale on version-digit rollovers, and said nothing at all about
// a browser that had not been enumerated.

// Browsers that decode WebP but do not advertise "image/webp" in the Accept
// header of a *navigation* request.  Two populations:
//
//   Safari 16+   -- decodes WebP, but Safari has never listed image types in
//                   a navigation Accept header.  The floor is 16, NOT 14, and
//                   the distinction is load-bearing: Safari's WebP decode is
//                   OS-gated, and Safari 14/15 on macOS Catalina (10.15) has
//                   no WebP decoder at all -- served a committed ".webp" URL
//                   it renders a broken image, strictly worse than the JPEG
//                   status quo.  Denying by real OS version is impossible
//                   from the UA: every modern macOS freezes the UA's OS token
//                   at "Intel Mac OS X 10_15_7" (anti-fingerprinting), so
//                   Catalina and Sequoia read identically.  Safari 16 is the
//                   first version that cannot run on Catalina, which makes
//                   Version/16 the earliest decoder-safe floor derivable from
//                   the UA alone.
//   Firefox 132+ -- WebP since Firefox 65 on every OS (not OS-gated, so no
//                   Safari-style consideration applies), but Firefox 132
//                   dropped image types from the navigation Accept header
//                   entirely (Bugzilla 1917177).
//
// Together these are roughly a fifth of global page views, and today they
// are served the original image because the HTML request carries no
// "Accept: image/webp".  This group answers only the narrow question "does
// this UA decode WebP even though this navigation Accept did not say so"; it
// is consumed exclusively via the rewritten-URL path in DeviceProperties.  It
// cannot reach the in-place path: in-place optimization is request-independent
// and never consults request-derived WebP capability at all.
//
// Frozen-set safety.  Both allow entries are OPEN-ENDED on the version and the
// floor is enforced entirely by the block list below (registered after every
// allow entry, so the later rule wins -- deny beats allow).  Never the other
// shape: a closed allow pattern like "*Chrome/??.*" (in the now-deleted
// animated-WebP allowlist) silently disabled animated WebP for every Chrome
// >= 100 for over four years, because the set of matching version shapes was
// frozen at authoring time.  The denied range, by contrast, is finite and
// historical, so enumerating it cannot rot.  The corollary is that the block
// list must cover every major below the floor at EVERY digit width,
// single-digit ones included.  The open-ended allow also admits Apple's 2025
// renumbering of Safari to OS-aligned versions ("Version/26." and up).
//
// Safari is identified by the "Version/N... Safari/" idiom, never by "Safari/"
// alone: Chrome, Edge and Opera all carry "Safari/537.36" in their UA and would
// otherwise match.  That idiom is already used for the same reason in
// kInsertDnsPrefetchBlockedlist below.
//
// Known limitation of this shape, and the reason it is worth stating out loud:
// "open-ended allow plus a numeric floor in the deny list" fails OPEN on a
// version token that is not a number at all.  A UA reading "Firefox/wibble"
// matches the allow entry and no floor entry, so it is granted.  Real browsers
// do not emit that, but privacy forks do -- a corpus backtest turned up
// Camoufox ("Firefox/Camoufox Camoufox 140.0"), which happens to be Gecko 140
// and so is granted correctly by accident.  The trade-off is inherent to the
// frozen-set-safe shape; the mitigation is that the resulting grant only ever
// reaches rewritten URLs.  The same class covers a sub-floor digit run glued
// to a letter ("Version/9X2", "Firefox/45Build"): that is not a delimited
// version token, it is a non-numeric token, and it fails open like
// Firefox/wibble does -- with the one load-bearing exception of a digit glued
// directly to "Safari/", which is denied explicitly below.
const char* kWebpNoNavigationAcceptAllowlist[] = {
    // Safari: requires a Version/ token *and* a Safari/ token.  Real Safari
    // (macOS and iOS) always emits both; the Chromium family emits Safari/
    // without Version/, so it cannot match here.  The leading space anchors
    // Version/ as a token: every real Safari UA starts "Mozilla/5.0 ..." and
    // space-precedes "Version/", while without it an alien token merely
    // CONTAINING the substring -- "AppVersion/22.1 ... Safari/601" -- would
    // satisfy the allow.
    "* Version/*Safari/*",
    // Firefox (Gecko).  Open-ended; the 132 floor lives in the block list.
    // Same token anchoring: real Firefox UAs read "... Gecko/NNNNNNNN
    // Firefox/NNN.0", so the leading space costs nothing and stops
    // "MyFirefox/9000"-style tokens from matching.  The deny rows below stay
    // deliberately UNanchored: over-deny is the safe direction, so a floor row
    // is allowed to fire on alien tokens too.
    "* Firefox/*",
};

// Every printable ASCII character that is not a digit, not a letter, and not
// one of the two wildcard metacharacters, plus HTAB (legal in HTTP field
// values).  The constructor crosses this alphabet with the sub-floor
// no-minor-version deny shapes below; see the comment in the middle of
// kWebpNoNavigationAcceptBlockedlist.
const char kWebpNoNavigationAcceptVersionDelimiters[] =
    "\t !\"#$%&'()+,-./:;<=>@[\\]^_`{|}~";

const char* kWebpNoNavigationAcceptBlockedlist[] = {
    // ---- Safari version floor: everything below Safari 16. ----
    // Safari 14 and 15 are denied NOT because they lack a decoder everywhere
    // but because the UA cannot prove they are not running on Catalina, where
    // they have none -- see the Version/16 floor rationale above.
    //
    // Two-digit sub-floor majors 10-15 are denied by per-major CATCH-ALLS:
    // "*Version/10*Safari/*" fires on "Version/10" followed by ANYTHING
    // ("10.1", "10 ", "10;", "10)", glued "10Safari/"), so for these majors
    // the floor holds in every delimiter shape by construction, with nothing
    // to enumerate.  The deliberate over-deny this buys is bounded and
    // documented: the same rows also match three-digit majors 100-159, which
    // have never existed and, under Apple's OS-aligned renumbering (Safari 26
    // shipped alongside macOS 26 in 2025, incrementing yearly), cannot exist
    // before the scheme reaches triple digits around the year 2099.  No real
    // population is lost -- the "never lose a real population" rule is
    // satisfied vacuously -- while single- and two-digit futures (16-19, 26+,
    // 99) are untouched, which is exactly why the same catch-all shape is NOT
    // usable for single-digit majors: "*Version/1*Safari/*" would swallow
    // Safari 16-19 today.
    "*Version/10*Safari/*",
    "*Version/11*Safari/*",
    "*Version/12*Safari/*",
    "*Version/13*Safari/*",
    "*Version/14*Safari/*",
    "*Version/15*Safari/*",
    // Major 0, and anything else starting "Version/0": no real version token
    // has ever begun with a zero, so this catch-all is safe forever and also
    // closes the zero-padded spellings ("Version/09 Safari/") that the
    // single-digit rows below would otherwise misread as two-digit majors.
    "*Version/0*Safari/*",
    // Single-digit sub-floor majors 1-9 (the '?' matches the digit).  The '.'
    // and every other delimiter shape ("Version/9 ", "Version/9;", ...) are
    // registered by the constructor, which crosses
    // kWebpNoNavigationAcceptVersionDelimiters with "*Version/?<delim>*Safari/*"
    // -- see the loop in the UserAgentMatcher constructor.  The one shape that
    // enumeration cannot reach is the digit glued directly to the Safari
    // token, denied here:
    "*Version/?Safari/*",

    // ---- Chromium family. ----
    // These carry "Safari/537.36" for historical reasons, and genuine Chromium
    // browsers always send "Accept: image/webp" anyway, so the UA-derived
    // signal must never fire for them.  Android WebView is the case that makes
    // this load-bearing: it emits "Version/4.0 ... Safari/537.36" and so would
    // otherwise reach the Safari rule.  It is denied three times over -- by
    // "*Chrome/*", by "*Android*", and by the Version/4.0 floor above -- and is
    // therefore classified as NOT matching, which is the conservative verdict:
    // an embedded WebView's decoder support is the host OS's business, not
    // something the UA string can settle.
    "*Chrome/*",
    "*Chromium/*",
    "*CriOS/*",
    "*Edg/*",
    "*EdgA/*",
    "*EdgiOS/*",
    "*Edge/*",
    "*OPR/*",
    "*OPiOS/*",
    "*Opera*",
    "*SamsungBrowser/*",
    "*YaBrowser/*",
    "*UCBrowser/*",
    "*Silk/*",
    "*Silk-Accelerated*",
    "*BB10*",
    "*PlayBook*",
    "*PlayStation*",
    "*Trident/*",
    "*Windows Phone*",
    // Android is denied only in combination with a Safari/ token, i.e. only for
    // the WebKit-shaped rule.  A blanket "*Android*" would also strike Firefox
    // for Android, whose UA is "Android NN; Mobile; rv:NNN.0 Gecko/NNN.0
    // Firefox/NNN.0" -- Gecko, in the target population, and carrying no
    // Safari/ token.  Backtesting a blanket entry against a real-world corpus
    // showed it silently removing every Firefox-on-Android hit.
    "*Android*Safari/*",

    // Firefox for iOS is WebKit-backed, so it does decode WebP -- but its UA
    // carries neither a "Version/" token nor "Firefox/", only "FxiOS/", so it
    // matches no allow entry to begin with.  Denied explicitly as well, so that
    // a future FxiOS release adding a Version/ token cannot silently flip it
    // into the Safari rule.  Net effect: FxiOS is a deliberate false negative.
    "*FxiOS/*",

    // ---- Firefox version floor: everything below Firefox 132. ----
    // Unlike the Safari 10-15 rows, Firefox CANNOT use per-major catch-alls at
    // any width: "*Firefox/13*" would deny Firefox 132+ today, "*Firefox/45*"
    // would deny the real future three-digit major 451 -- precisely the
    // "*Chrome/??.*" rot class this list is built to avoid.  So the Firefox
    // floor is enumerated by digit WIDTH instead, with '?' pinning the digit
    // count: one-or-two-digit majors are all sub-floor (1-99), and the
    // three-digit sub-floor range 100-131 is covered by the 10x/11x/12x
    // prefixes plus exact 130 and 131.  A real "Firefox/132.0" or
    // "Firefox/451.0" fails every row because the character after the pinned
    // digits is another digit, never a delimiter.
    //
    // Only the END-ANCHORED no-minor forms ("...Firefox/45"<end>) live here;
    // every delimited form -- ".", " ", ";", ")", ",", "/", ":", "-" and the
    // rest of the printable non-alphanumeric alphabet -- is registered by the
    // constructor, which crosses kWebpNoNavigationAcceptVersionDelimiters with
    // each of these seven width shapes.  ("*Firefox/??" requires exactly two
    // characters and then end of string, so "Firefox/132" cannot fire it.)
    "*Firefox/?",
    "*Firefox/??",
    "*Firefox/10?",
    "*Firefox/11?",
    "*Firefox/12?",
    "*Firefox/130",
    "*Firefox/131",

    // Gecko forks that carry a "Firefox/NNN" compatibility token with their own
    // release numbering.  They are Gecko and do decode WebP, but their token
    // does not mean what it says, so decline rather than guess.
    "*PaleMoon*",
    "*Waterfox*",
    "*SeaMonkey*",
    "*Iceweasel*",
    "*IceCat*",

    // ---- Crawlers and tooling. ----
    // Applebot is the load-bearing entry: it is the one widely deployed crawler
    // that advertises "Version/N... Safari/" with N above the floor and would
    // otherwise be handed WebP.  The rest are cheap insurance; DeviceProperties
    // applies an independent IsBot() gate on top of this group.
    "*Applebot*",
    "*Googlebot*",
    "*Google-InspectionTool*",
    "*bingbot*",
    "*Bingbot*",
    "*DuckDuckBot*",
    "*YandexBot*",
    "*Baiduspider*",
    "*Slurp*",
    "*AhrefsBot*",
    "*SemrushBot*",
    "*PetalBot*",
    "*facebookexternalhit*",
    "*Twitterbot*",
    "*HeadlessChrome*",
    "*PhantomJS*",
    "*Electron/*",
    "*curl/*",
    "*Wget*",
    "*python-requests*",
    "*Go-http-client*",
};

const char* kInsertDnsPrefetchAllowlist[] = {
    "*Chrome/*",
    "*Firefox/*",
    "*Safari/*",
    // Plus IE, see code below.
    "*Wget*",
};

const char* kInsertDnsPrefetchBlockedlist[] = {
    "*Firefox/1.*",
    "*Firefox/2.*",
    "*Firefox/3.*",
    // Safari indicates version with a separate Version/N.N.N token that appears
    // somewhere before the Safari/ token.  This only started with version 3,
    // but versions before 3 are 10+ years old at this point and won't run on
    // any supported OS.
    "*Version/3.*Safari/*",
    "*Version/4.*Safari/*",
    // 5.0.1+ actually did support it, but that's long obsolete, so don't bother
    // contorting the list to include it.
    "*Version/5.*Safari/*",
    "*MSIE 5.*",
    "*MSIE 6.*",
    "*MSIE 7.*",
    "*MSIE 8.*",
};

// Allowlist used for doing the tablet-user-agent check, which also feeds
// into the device type used for storing properties in the property cache.
const char* kTabletUserAgentAllowlist[] = {
    "*Android*",  // Android tablet has "Android" but not "Mobile". Regexp
                  // checks for UserAgents should first check the mobile
                  // allowlists and blockedlists and only then check the tablet
                  // allowlist for correct results.
    "*iPad*", "*TouchPad*", "*Silk-Accelerated*", "*Kindle Fire*"};

// Allowlist used for doing the mobile-user-agent check, which also feeds
// into the device type used for storing properties in the property cache.
const char* kMobileUserAgentAllowlist[] = {
    "*Mozilla*Android*Mobile*",
    "*iPhone*",
    "*BlackBerry*",
    "*Opera Mobi*",
    "*Opera Mini*",
    "*SymbianOS*",
    "*UP.Browser*",
    "*J-PHONE*",
    "*Profile/MIDP*",
    "*profile/MIDP*",
    "*portalmmm*",
    "*DoCoMo*",
    "*Obigo*",
    "AdsBot-Google-Mobile",
};

// Blockedlist used for doing the mobile-user-agent check.
const char* kMobileUserAgentBlockedlist[] = {
    "*Mozilla*Android*Silk*Mobile*", "*Mozilla*Android*Kindle Fire*Mobile*"};

// Allowlist used for mobilization.
const char* kMobilizationUserAgentAllowlist[] = {
    "*Android*",  "*Chrome/*",     "*Firefox/*", "*iPad*", "*iPhone*",
    "*iPod*",     "*Opera*",       "*Safari*",   "*Wget*",
    "*CriOS/*",    // Chrome for iOS.
    "*Android *",  // Native Android browser (see blockedlist below).
    "*iPhone*",   "AdsBot-Google*"};

// Blockedlist used for doing the mobilization UA check.
const char* kMobilizationUserAgentBlockedlist[] = {
    "*Android 0.*", "*Android 1.*", "*Android 2.*", "*BlackBerry*",
    "*Mozilla*Android*Silk*Mobile*", "*Mozilla*Android*Kindle Fire*Mobile*",
    "*Opera Mobi*", "*Opera Mini*", "*SymbianOS*", "*UP.Browser*", "*J-PHONE*",
    "*Profile/MIDP*", "*profile/MIDP*", "*portalmmm*", "*DoCoMo*", "*Obigo*",
    // TODO(jmaessen): Remove when there's a fix for scroll misbehavior on
    // CriOS.
    "*CriOS/*",      // Chrome for iOS.
    "*GSA*Safari*",  // Google Search Application for iOS.
    // TODO(jmaessen): Remove when there's a fix for page geometry on the native
    // Android browser (the old WebKit browser).
    "*U; Android 3.*", "*U; Android 4.*"};

// IE 11 and later user agent strings are deliberately difficult.  That would be
// great if random pages never put the browser into backward compatibility mode,
// and all the outstanding caching bugs were fixed, but neither is true and so
// we need to be able to spot IE 11 and treat it as IE even though we're not
// supposed to need to do so ever again.  See
// http://blogs.msdn.com/b/ieinternals/archive/2013/09/21/internet-explorer-11-user-agent-string-ua-string-sniffing-compatibility-with-gecko-webkit.aspx
const char* kIeUserAgents[] = {
    "*MSIE *",                // Should match any IE before 11.
    "*rv:11.?) like Gecko*",  // Other revisions (eg 12.0) are FireFox
    "*IE 1*",                 // Initial numeral avoids Samsung UA
    "*Trident/7*",            // Opera sometimes pretends to be earlier Trident
};
const int kIEBefore11Index = 0;

// Match either 'CriOS' (iOS Chrome) or 'Chrome'. ':?' marks a non-capturing
// group.
const char* kChromeVersionPattern =
    "(?:Chrome|CriOS)/(\\d+)\\.(\\d+)\\.(\\d+)\\.(\\d+)";

// Device strings must not include wildcards.
struct Dimension {
  const char* device_name;
  int width;
  int height;
};

const Dimension kKnownScreenDimensions[] = {
    {"Galaxy Nexus", 720, 1280}, {"GT-I9300", 720, 1280},
    {"GT-N7100", 720, 1280},     {"Nexus 4", 768, 1280},
    {"Nexus 10", 1600, 2560},    {"Nexus S", 480, 800},
    {"Xoom", 800, 1280},         {"XT907", 540, 960},
};

}  // namespace

UserAgentMatcher::UserAgentMatcher()
    : chrome_version_pattern_(kChromeVersionPattern) {
  // Initialize FastWildcardGroup for image inlining allowlist & blockedlist.
  for (int i = 0, n = arraysize(kImageInliningAllowlist); i < n; ++i) {
    supports_image_inlining_.Allow(kImageInliningAllowlist[i]);
  }
  for (int i = 0, n = arraysize(kIeUserAgents); i < n; ++i) {
    supports_image_inlining_.Allow(kIeUserAgents[i]);
  }
  for (int i = 0, n = arraysize(kImageInliningBlockedlist); i < n; ++i) {
    supports_image_inlining_.Disallow(kImageInliningBlockedlist[i]);
  }
  for (int i = 0, n = arraysize(kLazyloadImagesBlockedlist); i < n; ++i) {
    supports_lazyload_images_.Disallow(kLazyloadImagesBlockedlist[i]);
  }
  defer_js_allowlist_.Allow(kIeUserAgents[kIEBefore11Index]);
  for (int i = 0, n = arraysize(kDeferJSAllowlist); i < n; ++i) {
    defer_js_allowlist_.Allow(kDeferJSAllowlist[i]);
  }

  // https://github.com/apache/incubator-pagespeed-mod/issues/982
  defer_js_allowlist_.Disallow("* MSIE 9.*");

  for (int i = 0, n = arraysize(kDeferJSBlockedlist); i < n; ++i) {
    defer_js_allowlist_.Disallow(kDeferJSBlockedlist[i]);
  }

  for (int i = 0, n = arraysize(kDeferJSMobileAllowlist); i < n; ++i) {
    defer_js_mobile_allowlist_.Allow(kDeferJSMobileAllowlist[i]);
  }

  // Do the same for webp support.
  for (int i = 0, n = arraysize(kLegacyWebpAllowlist); i < n; ++i) {
    legacy_webp_.Allow(kLegacyWebpAllowlist[i]);
  }
  for (int i = 0, n = arraysize(kLegacyWebpBlockedlist); i < n; ++i) {
    legacy_webp_.Disallow(kLegacyWebpBlockedlist[i]);
  }

  // Allows first, denies second: FastWildcardGroup lets the
  // latest-registered matching rule win, so registering the block list after
  // the allow list is what makes "deny beats allow" true here.
  for (int i = 0, n = arraysize(kWebpNoNavigationAcceptAllowlist); i < n; ++i) {
    webp_no_navigation_accept_.Allow(kWebpNoNavigationAcceptAllowlist[i]);
  }
  for (int i = 0, n = arraysize(kWebpNoNavigationAcceptBlockedlist); i < n;
       ++i) {
    webp_no_navigation_accept_.Disallow(kWebpNoNavigationAcceptBlockedlist[i]);
  }
  // Sub-floor no-minor-version deny rows ("Version/9;Safari/", "Firefox/45)"):
  // the wildcard language has no character classes, so "digit run followed by
  // a delimiter" is spelled out as one deny row per delimiter per width shape.
  // The alphabet is every printable non-alphanumeric ASCII character plus
  // HTAB; only the two wildcard metacharacters '*' and '?' are inexpressible
  // (no quoting exists), an accepted residual since no fabricated version
  // token has been observed using them as delimiters.  Registered after the
  // static block list, which is fine: denies do not compete with denies, only
  // with the allow entries above.  This costs a few hundred extra patterns in
  // the group, built once per UserAgentMatcher, not per request.
  for (const char* d = kWebpNoNavigationAcceptVersionDelimiters; *d != '\0';
       ++d) {
    const GoogleString delim(1, *d);
    // Safari single-digit sub-floor majors 1-9 (0 and 10-15 are handled by
    // catch-all rows in the static list).
    webp_no_navigation_accept_.Disallow(
        StrCat("*Version/?", delim, "*Safari/*"));
    // Firefox sub-floor majors by digit width: 1-9, 10-99, 100-129, 130, 131.
    webp_no_navigation_accept_.Disallow(StrCat("*Firefox/?", delim, "*"));
    webp_no_navigation_accept_.Disallow(StrCat("*Firefox/??", delim, "*"));
    webp_no_navigation_accept_.Disallow(StrCat("*Firefox/10?", delim, "*"));
    webp_no_navigation_accept_.Disallow(StrCat("*Firefox/11?", delim, "*"));
    webp_no_navigation_accept_.Disallow(StrCat("*Firefox/12?", delim, "*"));
    webp_no_navigation_accept_.Disallow(StrCat("*Firefox/130", delim, "*"));
    webp_no_navigation_accept_.Disallow(StrCat("*Firefox/131", delim, "*"));
  }

  for (int i = 0, n = arraysize(kInsertDnsPrefetchAllowlist); i < n; ++i) {
    supports_dns_prefetch_.Allow(kInsertDnsPrefetchAllowlist[i]);
  }
  for (int i = 0, n = arraysize(kIeUserAgents); i < n; ++i) {
    supports_dns_prefetch_.Allow(kIeUserAgents[i]);
  }
  for (int i = 0, n = arraysize(kInsertDnsPrefetchBlockedlist); i < n; ++i) {
    supports_dns_prefetch_.Disallow(kInsertDnsPrefetchBlockedlist[i]);
  }

  for (int i = 0, n = arraysize(kMobileUserAgentAllowlist); i < n; ++i) {
    mobile_user_agents_.Allow(kMobileUserAgentAllowlist[i]);
  }
  for (int i = 0, n = arraysize(kMobileUserAgentBlockedlist); i < n; ++i) {
    mobile_user_agents_.Disallow(kMobileUserAgentBlockedlist[i]);
  }
  for (int i = 0, n = arraysize(kTabletUserAgentAllowlist); i < n; ++i) {
    tablet_user_agents_.Allow(kTabletUserAgentAllowlist[i]);
  }
  for (int i = 0, n = arraysize(kMobilizationUserAgentAllowlist); i < n; ++i) {
    mobilization_user_agents_.Allow(kMobilizationUserAgentAllowlist[i]);
  }
  for (int i = 0, n = arraysize(kMobilizationUserAgentBlockedlist); i < n;
       ++i) {
    mobilization_user_agents_.Disallow(kMobilizationUserAgentBlockedlist[i]);
  }
  for (int i = 0, n = arraysize(kIeUserAgents); i < n; ++i) {
    ie_user_agents_.Allow(kIeUserAgents[i]);
  }
  GoogleString known_devices_pattern_string = "(";
  for (int i = 0, n = arraysize(kKnownScreenDimensions); i < n; ++i) {
    const Dimension& dim = kKnownScreenDimensions[i];
    screen_dimensions_map_[dim.device_name] = make_pair(dim.width, dim.height);
    if (i != 0) {
      StrAppend(&known_devices_pattern_string, "|");
    }
    StrAppend(&known_devices_pattern_string, dim.device_name);
  }
  StrAppend(&known_devices_pattern_string, ")");
  known_devices_pattern_ =
      std::make_unique<re2::RE2>(known_devices_pattern_string);
}

UserAgentMatcher::~UserAgentMatcher() {}

bool UserAgentMatcher::IsIe(const StringPiece& user_agent) const {
  return ie_user_agents_.Match(user_agent, false);
}

bool UserAgentMatcher::SupportsImageInlining(
    const StringPiece& user_agent) const {
  if (user_agent.empty()) {
    return true;
  }
  return supports_image_inlining_.Match(user_agent, false);
}

bool UserAgentMatcher::SupportsLazyloadImages(StringPiece user_agent) const {
  return supports_lazyload_images_.Match(user_agent, true);
}

namespace {

// Parses the version number that immediately follows 'token' in 'user_agent'
// as "<major>[.<minor>...]". Returns true and sets *major and *minor (0 when
// no minor component is present) on success; returns false if 'token' is
// absent or not followed by a digit.
bool ParseVersionAfterToken(StringPiece user_agent, StringPiece token,
                            int* major, int* minor) {
  size_t pos = user_agent.find(token);
  if (pos == StringPiece::npos) {
    return false;
  }
  pos += token.size();
  size_t end = pos;
  while (end < user_agent.size() && user_agent[end] >= '0' &&
         user_agent[end] <= '9') {
    ++end;
  }
  if (end == pos || !StringToInt(user_agent.substr(pos, end - pos), major)) {
    return false;
  }
  *minor = 0;
  if (end < user_agent.size() && user_agent[end] == '.') {
    size_t minor_start = end + 1;
    size_t minor_end = minor_start;
    while (minor_end < user_agent.size() && user_agent[minor_end] >= '0' &&
           user_agent[minor_end] <= '9') {
      ++minor_end;
    }
    if (minor_end > minor_start) {
      StringToInt(user_agent.substr(minor_start, minor_end - minor_start),
                  minor);
    }
  }
  return true;
}

}  // namespace

bool UserAgentMatcher::SupportsNativeLazyLoading(StringPiece user_agent) const {
  int major = 0;
  int minor = 0;
  // Chromium-based browsers (Chrome, Edge, Opera) all carry a
  // "Chrome/<version>" token and support loading="lazy" from 77 on. iOS
  // Chrome (CriOS) is a WebKit shell and intentionally not matched here; it
  // falls through to the conservative default below.
  if (ParseVersionAfterToken(user_agent, "Chrome/", &major, &minor)) {
    return major >= 77;
  }
  if (ParseVersionAfterToken(user_agent, "Firefox/", &major, &minor)) {
    return major >= 75;
  }
  // Safari reports its version in a separate "Version/<major>.<minor>" token
  // ahead of the "Safari/" token; loading="lazy" shipped in 15.4. Chromium
  // user agents also contain "Safari/" but no "Version/" token, and they
  // were already handled above.
  if (user_agent.find("Safari/") != StringPiece::npos &&
      ParseVersionAfterToken(user_agent, "Version/", &major, &minor)) {
    return major > 15 || (major == 15 && minor >= 4);
  }
  // Unknown user agent: fall back to the script-based implementation.
  return false;
}

bool UserAgentMatcher::SupportsDnsPrefetch(
    const StringPiece& user_agent) const {
  return supports_dns_prefetch_.Match(user_agent, false);
}

bool UserAgentMatcher::SupportsJsDefer(const StringPiece& user_agent,
                                       bool allow_mobile) const {
  // TODO(ksimbili): Use IsMobileRequest?
  if (GetDeviceTypeForUA(user_agent) != kDesktop) {
    // TODO(ksimbili): IsMobileUserAgent returns true for tablets too.
    // Fix it when we need to differentiate them.
    return allow_mobile && defer_js_mobile_allowlist_.Match(user_agent, false);
  }
  return user_agent.empty() || defer_js_allowlist_.Match(user_agent, false);
}

bool UserAgentMatcher::LegacyWebp(const StringPiece& user_agent) const {
  return legacy_webp_.Match(user_agent, false);
}

bool UserAgentMatcher::SupportsWebpButOmitsNavigationAccept(
    const StringPiece& user_agent) const {
  return webp_no_navigation_accept_.Match(user_agent, false);
}

// AVIF is strictly Accept-header-driven: there is no legacy UA
// allow-list, so the UA string alone carries no AVIF signal and these always
// report false. Real gating happens via the Accept: image/avif header in
// DeviceProperties. The parameter is intentionally unused.
bool UserAgentMatcher::SupportsAvifLosslessAlpha(
    const StringPiece& /*user_agent*/) const {
  return false;
}

bool UserAgentMatcher::SupportsAvifAnimated(
    const StringPiece& /*user_agent*/) const {
  return false;
}

UserAgentMatcher::DeviceType UserAgentMatcher::GetDeviceTypeForUAAndHeaders(
    const StringPiece& user_agent,
    const RequestHeaders* request_headers) const {
  return GetDeviceTypeForUA(user_agent);
}

bool UserAgentMatcher::IsAndroidUserAgent(const StringPiece& user_agent) const {
  return user_agent.find("Android") != GoogleString::npos;
}

bool UserAgentMatcher::IsiOSUserAgent(const StringPiece& user_agent) const {
  return user_agent.find("iPhone") != GoogleString::npos ||
         user_agent.find("iPad") != GoogleString::npos;
}

bool UserAgentMatcher::GetChromeBuildNumber(const StringPiece& user_agent,
                                            int* major, int* minor, int* build,
                                            int* patch) const {
  return RE2::PartialMatch(StringPieceToRe2(user_agent),
                           chrome_version_pattern_, major, minor, build, patch);
}

// TODO(bharathbhushan): Make sure GetDeviceTypeForUA is called only once per
// http request.
UserAgentMatcher::DeviceType UserAgentMatcher::GetDeviceTypeForUA(
    const StringPiece& user_agent) const {
  if (mobile_user_agents_.Match(user_agent, false)) {
    return kMobile;
  }
  if (tablet_user_agents_.Match(user_agent, false)) {
    return kTablet;
  }
  return kDesktop;
}

StringPiece UserAgentMatcher::DeviceTypeString(DeviceType device_type) {
  StringPiece device_type_suffix = "";
  switch (device_type) {
    case kMobile:
      device_type_suffix = "mobile";
      break;
    case kTablet:
      device_type_suffix = "tablet";
      break;
    case kDesktop:
    case kEndOfDeviceType:
    default:
      device_type_suffix = "desktop";
      break;
  }
  return device_type_suffix;
}

StringPiece UserAgentMatcher::DeviceTypeSuffix(DeviceType device_type) {
  StringPiece device_type_suffix = "";
  switch (device_type) {
    case kMobile:
      device_type_suffix = "@Mobile";
      break;
    case kTablet:
      device_type_suffix = "@Tablet";
      break;
    case kDesktop:
    case kEndOfDeviceType:
    default:
      device_type_suffix = "@Desktop";
      break;
  }
  return device_type_suffix;
}

bool UserAgentMatcher::UserAgentExceedsChromeiOSBuildAndPatch(
    const StringPiece& user_agent, int required_build,
    int required_patch) const {
  // Verify if this is an iOS user agent.
  if (!IsiOSUserAgent(user_agent)) {
    return false;
  }
  return UserAgentExceedsChromeBuildAndPatch(user_agent, required_build,
                                             required_patch);
}

bool UserAgentMatcher::UserAgentExceedsChromeAndroidBuildAndPatch(
    const StringPiece& user_agent, int required_build,
    int required_patch) const {
  // Verify if this is an Android user agent.
  if (!IsAndroidUserAgent(user_agent)) {
    return false;
  }
  return UserAgentExceedsChromeBuildAndPatch(user_agent, required_build,
                                             required_patch);
}

bool UserAgentMatcher::UserAgentExceedsChromeBuildAndPatch(
    const StringPiece& user_agent, int required_build,
    int required_patch) const {
  // By default user agent sniffing is disabled.
  if (required_build == -1 && required_patch == -1) {
    return false;
  }
  int major = -1;
  int minor = -1;
  int parsed_build = -1;
  int parsed_patch = -1;
  if (!GetChromeBuildNumber(user_agent, &major, &minor, &parsed_build,
                            &parsed_patch)) {
    return false;
  }

  if (parsed_build < required_build) {  // NOLINT(bugprone-branch-clone)
    return false;
  } else if (parsed_build == required_build && parsed_patch < required_patch) {
    return false;
  }

  return true;
}

bool UserAgentMatcher::SupportsMobilization(StringPiece user_agent) const {
  return mobilization_user_agents_.Match(user_agent, false);
}

}  // namespace net_instaweb
