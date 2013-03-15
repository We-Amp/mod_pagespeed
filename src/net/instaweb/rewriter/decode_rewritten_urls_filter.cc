/*
 * Copyright 2013 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "net/instaweb/rewriter/public/decode_rewritten_urls_filter.h"

#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/http/public/log_record.h"
#include "net/instaweb/http/public/logging_proto_impl.h"
#include "net/instaweb/http/public/semantic_type.h"
#include "net/instaweb/rewriter/public/resource_tag_scanner.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

DecodeRewrittenUrlsFilter::~DecodeRewrittenUrlsFilter() {
}

void DecodeRewrittenUrlsFilter::StartElement(HtmlElement* element) {
  semantic_type::Category category;
  HtmlElement::Attribute* attr =  resource_tag_scanner::ScanElement(
      element, driver_, &category);
  if (attr == NULL) {
    return;
  }
  StringPiece url(attr->DecodedValueOrNull());
  if (url.empty() || url.starts_with("data:")) {
    return;
  }
  GoogleUrl gurl(driver_->base_url(), url);
  if (gurl.is_valid()) {
    StringVector decoded_url;
    if (driver_->DecodeUrl(gurl, &decoded_url)) {
      // An encoded URL.
      if (decoded_url.size() == 1) {
        driver_->log_record()->SetRewriterLoggingStatus(
            RewriteOptions::FilterId(RewriteOptions::kDecodeRewrittenUrls),
            RewriterInfo::APPLIED_OK);
        // Replace attr value with decoded URL.
        attr->SetValue(decoded_url.at(0));
      } else {
        // A combined encoded URL.
        // TODO(sriharis):  What can we do?  Creating elements for each
        // constituent (that are other wise identical to 'element')?
        driver_->log_record()->SetRewriterLoggingStatus(
            RewriteOptions::FilterId(RewriteOptions::kDecodeRewrittenUrls),
            RewriterInfo::NOT_APPLIED);
      }
    }
  }
}

}  // namespace net_instaweb