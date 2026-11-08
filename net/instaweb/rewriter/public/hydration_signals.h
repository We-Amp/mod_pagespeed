// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Recognizes a response whose HTML a client-side framework will hydrate:
// compare against what it rendered on the server, and redo the work in the
// browser if they differ. Each signal comes from a response header or a parsed
// node, never from the raw bytes, so text that merely mentions a marker (in a
// <pre>, say) does not count. A false negative leaves the page rewritten as
// usual; a false positive costs that page its CSS savings.

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_HYDRATION_SIGNALS_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_HYDRATION_SIGNALS_H_

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class HtmlElement;
class ResponseHeaders;

bool HydrationSignalInHeaders(const ResponseHeaders& headers);
bool HydrationSignalInElement(const HtmlElement& element);
// contents is the text between <!-- and -->.
bool HydrationSignalInComment(StringPiece contents);
// contents is the text of a <script> element without src.
bool HydrationSignalInInlineScript(StringPiece contents);

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_HYDRATION_SIGNALS_H_
