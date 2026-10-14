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

#include "net/instaweb/rewriter/public/strip_subresource_hints_filter.h"

#include "net/instaweb/rewriter/dependencies.pb.h"
#include "net/instaweb/rewriter/public/dependency_tracker.h"
#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/http/google_url.h"

namespace net_instaweb {

StripSubresourceHintsFilter::StripSubresourceHintsFilter(RewriteDriver* driver)
    : driver_(driver),
      delete_element_(nullptr),
      remove_script_(false),
      remove_style_(false),
      remove_image_(false),
      remove_any_(false) {}

StripSubresourceHintsFilter::~StripSubresourceHintsFilter() {}

void StripSubresourceHintsFilter::StartDocument() {
  const RewriteOptions* options = driver_->options();
  remove_script_ = driver_->can_modify_urls() && !options->js_preserve_urls();
  remove_style_ = driver_->can_modify_urls() && !options->css_preserve_urls();
  remove_image_ = driver_->can_modify_urls() && !options->image_preserve_urls();
  remove_any_ = remove_script_ || remove_style_ || remove_image_;
  delete_element_ = nullptr;

  // The previous request's dependency record lists the modules the JS filter
  // rewrote, under their rewritten URLs. Decode those back to the original
  // URLs so a modulepreload hint can be matched against them. No record (or
  // no dependencies cohort) means nothing is stripped.
  rewritten_module_urls_.clear();
  const Dependencies* deps = driver_->dependency_tracker()->read_in_info();
  if (remove_script_ && deps != nullptr) {
    for (int i = 0, n = deps->module_dependency_size(); i < n; ++i) {
      GoogleUrl recorded(deps->module_dependency(i).url());
      StringVector originals;
      if (recorded.IsWebValid() && driver_->DecodeUrl(recorded, &originals)) {
        for (const GoogleString& original : originals) {
          rewritten_module_urls_.insert(original);
        }
      }
    }
  }

  // TODO(jefftk): replace can_modify_urls() with can_modify_js_urls(),
  // can_modify_css_urls(), and can_modify_image_urls() so that if you, say,
  // disable all js-modifying filters then js hints won't be stripped.  Right
  // now to get this behavior you need to explicitly turn on PreserveJsUrls.
}

bool StripSubresourceHintsFilter::ShouldStrip(HtmlElement* element) {
  // Strip:
  //   <link rel=subresource href=...>       regardless
  //   <link rel=preload as=script href=...> unless preserve scripts
  //   <link rel=modulepreload href=...>     only for a module the previous
  //                                         request recorded as rewritten
  //   <link rel=preload as=style href=...>  unless preserve styles
  //   <link rel=preload as=image href=...>  unless preserve images
  //
  // Don't strip other kinds of rel=preload hints, because we don't change
  // their urls, so existing ones are still valid.
  if (remove_any_ && !delete_element_ &&
      element->keyword() == HtmlName::kLink) {
    const char* rel_value = element->AttributeValue(HtmlName::kRel);
    if (rel_value != nullptr) {
      if (StringCaseEqual(rel_value, "subresource")) {
        return true;
      } else if (StringCaseEqual(rel_value, "modulepreload")) {
        // Only module script elements are rewritten, never import
        // specifiers, so most modulepreload hints (one per imported chunk)
        // stay valid. Strip only a hint for a module the previous request
        // recorded as rewritten. as defaults to script; other values
        // (e.g. worker) aren't rewritten.
        const char* as_value = element->AttributeValue(HtmlName::kAs);
        const char* href = element->AttributeValue(HtmlName::kHref);
        if (!remove_script_ || href == nullptr ||
            (as_value != nullptr && !StringCaseEqual(as_value, "script"))) {
          return false;
        }
        GoogleUrl resolved(driver_->decoded_base_url(), href);
        if (!resolved.IsWebValid()) {
          return false;
        }
        GoogleString key;
        resolved.Spec().CopyToString(&key);
        return rewritten_module_urls_.count(key) > 0;
      } else if (StringCaseEqual(rel_value, "preload")) {
        const char* as_value = element->AttributeValue(HtmlName::kAs);
        if (as_value != nullptr) {
          return ((remove_script_ && StringCaseEqual(as_value, "script")) ||
                  (remove_style_ && StringCaseEqual(as_value, "style")) ||
                  (remove_image_ && StringCaseEqual(as_value, "image")));
        }
      }
    }
  }
  return false;
}

void StripSubresourceHintsFilter::StartElement(HtmlElement* element) {
  if (ShouldStrip(element)) {
    const RewriteOptions* options = driver_->options();
    const char* resource_url = element->AttributeValue(HtmlName::kHref);
    if (!resource_url) {
      // There's either no href attr, or one that we can't decode (utf8 etc).
      // One way this could happen is if we have a url-encoded utf8 url in an
      // img tag and a utf8 encoded url in the subresource tag.  Delete the
      // link to be on the safe side.
      driver_->DeleteNode(element);
    } else {
      const GoogleUrl& base_url = driver_->decoded_base_url();
      GoogleUrl resolved_resource_url(base_url, resource_url);
      if (options->IsAllowed(resolved_resource_url.Spec()) &&
          options->domain_lawyer()->IsDomainAuthorized(base_url,
                                                       resolved_resource_url)) {
        driver_->DeleteNode(element);
      }
    }
  }
}

}  // namespace net_instaweb
