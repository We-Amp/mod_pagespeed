// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Helpers for filters that write CSS into a <style> element of the page
// being rewritten. See pagespeed/kernel/html/inline_style_text.h.

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_INLINE_STYLE_UTIL_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_INLINE_STYLE_UTIL_H_

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/inline_style_text.h"

namespace net_instaweb {

class HtmlCharactersNode;
class HtmlElement;
class RewriteDriver;

// Follows the charset a page declares in meta tags, in the order a filter
// sees the page. Only the first meta tag that declares a charset counts, and
// only when its start tag begins within the first 1024 bytes of the
// document; a later one, or one further in, declares nothing.
class InlineStyleMetaCharset {
 public:
  // Call at the start of the document.
  void Clear() {
    seen_ = false;
    charset_.clear();
  }
  // Call for every start element, in order.
  void NoteElement(const HtmlElement& element);
  // The declared charset, or empty when the meta tags seen so far declare
  // none that counts.
  StringPiece charset() const { return charset_; }

 private:
  bool seen_ = false;
  GoogleString charset_;
};

// The mode for CSS written into the document `driver` is parsing, at the
// point a filter has reached. kHtml only for an HTML document (known from
// finalized response headers or, when the server never finalizes them, from
// the type the parse was started with) whose declared charset is one
// InlineStyleModeForCharset() accepts. The declared charset is the one in the
// response headers or, when they have none, `meta.charset()`, from the meta
// tags the calling filter has seen so far. A document that starts with a byte
// order mark other than UTF-8's is kStrict. Everything else is kStrict. Call
// it on the HTML thread and keep the result.
InlineStyleMode InlineStyleModeForDocument(RewriteDriver* driver,
                                           const InlineStyleMetaCharset& meta);

// Appends `text` as the contents of `style`. The only way the filters that
// create a <style> element from a stylesheet add its text.
HtmlCharactersNode* AppendInlineStyleText(RewriteDriver* driver,
                                          HtmlElement* style,
                                          const InlineStyleText& text);

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_INLINE_STYLE_UTIL_H_
