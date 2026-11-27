// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// This helper evaluates a CSP string; it does not care where the string came
// from. The worker feeds it both <meta http-equiv> policies and the origin's
// response-header policies (read from the URL's headers sidecar). Also
// deferred: gating InjectAsyncCssLoader's external <script> on script-src.

// PageSpeed 2.0 - CSP inline-injection policy helper
//
// Shared, side-effect-free predicates that answer a single question: given a
// Content-Security-Policy string, is the browser going to HONOR an inline
// <style> / <script> element that we inject? If not, injecting it is wasted
// (the browser drops it) and — worse for critical CSS — swapping a
// render-blocking external sheet for a dropped inline copy causes FOUC. The
// filter uses these to decide whether to inject at all.

#ifndef PAGESPEED_LIB_HTML_CSP_INLINE_POLICY_H_
#define PAGESPEED_LIB_HTML_CSP_INLINE_POLICY_H_

#include <string_view>

namespace net_instaweb {

// Returns true if an INLINE <style> element would be honored under `csp`,
// resolving the CSP3 fallback chain style-src-elem -> style-src -> default-src.
//
// `csp` may be a single policy or a comma-separated policy LIST (header-list
// semantics, which browsers also apply to a list arriving in one <meta> tag):
// each member is evaluated as an independent policy and EVERY member must
// allow the inline element.
//
// Semantics (see .cc for the token rules):
//   - empty `csp`                                        -> true  (no policy)
//   - governing directive ABSENT from the chain          -> true
//   - directive present WITHOUT 'unsafe-inline'          -> false
//   - directive present WITH 'unsafe-inline' AND a
//     nonce/hash source (which makes the browser IGNORE
//     'unsafe-inline')                                   -> false
//   - directive present WITH 'unsafe-inline' and NO
//     neutralizing nonce/hash                            -> true
bool InlineStyleAllowed(std::string_view csp);

// Same as InlineStyleAllowed but for an inline <script>, resolving the chain
// script-src-elem -> script-src -> default-src.
bool InlineScriptAllowed(std::string_view csp);

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_HTML_CSP_INLINE_POLICY_H_
