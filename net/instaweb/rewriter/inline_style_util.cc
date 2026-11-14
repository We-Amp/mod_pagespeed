// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "net/instaweb/rewriter/public/inline_style_util.h"

#include "net/instaweb/rewriter/public/common_filter.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/charset_util.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/html/html_node.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

InlineStyleMode InlineStyleModeForDocument(RewriteDriver* driver,
                                           const InlineStyleMetaCharset& meta) {
  bool is_html;
  if (driver->server_context()->response_headers_finalized()) {
    is_html = driver->MimeTypeXhtmlStatus() == RewriteDriver::kIsNotXhtml;
  } else {
    // Some servers never finalize the response headers (MimeTypeXhtmlStatus()
    // then stays unknown) and may not have the content type in them at all.
    // Only a parse started as plain HTML counts as HTML.
    is_html = driver->content_type().type() == ContentType::kHtml;
  }
  if (!is_html) {
    return InlineStyleMode::kStrict;
  }
  // A byte order mark decides the encoding over any declaration.
  const StringPiece bom = driver->document_bom_charset();
  if (!bom.empty() && !StringCaseEqual(bom, kUtf8Charset)) {
    return InlineStyleMode::kStrict;
  }
  const ResponseHeaders* headers = driver->response_headers();
  const GoogleString headers_charset =
      headers == nullptr ? GoogleString() : headers->DetermineCharset();
  return InlineStyleModeForCharset(headers_charset.empty() ? meta.charset()
                                                           : headers_charset);
}

void InlineStyleMetaCharset::NoteElement(const HtmlElement& element) {
  if (seen_ || element.keyword() != HtmlName::kMeta) {
    return;
  }
  GoogleString content, mime_type, charset;
  if (CommonFilter::ExtractMetaTagDetails(element, nullptr, &content,
                                          &mime_type, &charset) &&
      !charset.empty()) {
    seen_ = true;
    if (element.begins_in_first_1024_bytes()) {
      charset_ = charset;
    }
  }
}

HtmlCharactersNode* AppendInlineStyleText(RewriteDriver* driver,
                                          HtmlElement* style,
                                          const InlineStyleText& text) {
  HtmlCharactersNode* node = driver->NewCharactersNode(style, text.text());
  driver->AppendChild(style, node);
  return node;
}

}  // namespace net_instaweb
