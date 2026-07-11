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

#include "net/instaweb/rewriter/public/lazyload_images_filter.h"

#include "base/logging.h"
#include "net/instaweb/rewriter/public/critical_images_finder.h"
#include "net/instaweb/rewriter/public/csp.h"
#include "net/instaweb/rewriter/public/request_properties.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "net/instaweb/util/public/fallback_property_page.h"
#include "pagespeed/kernel/base/escaping.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/html/html_node.h"
#include "pagespeed/kernel/http/data_url.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/user_agent_matcher.h"
#include "pagespeed/opt/logging/enums.pb.h"
#include "pagespeed/opt/logging/log_record.h"

namespace net_instaweb {

namespace {

const char kTrue[] = "true";
const char kFalse[] = "false";
const char kJquerySlider[] = "jquery.sexyslider";

}  // namespace

const char LazyloadImagesFilter::kImageOnloadCode[] =
    "pagespeed.lazyLoadImages.loadIfVisibleAndMaybeBeacon(this);";

const char LazyloadImagesFilter::kLoadAllImages[] =
    "pagespeed.lazyLoadImages.loadAllImages();";

const char LazyloadImagesFilter::kOverrideAttributeFunctions[] =
    "pagespeed.lazyLoadImages.overrideAttributeFunctions();";

const char LazyloadImagesFilter::kIsLazyloadScriptInsertedPropertyName[] =
    "is_lazyload_script_inserted";

const char LazyloadImagesFilter::kLazyloadImagesApplied[] =
    "lazyload_images_applied";
const char LazyloadImagesFilter::kLazyloadImagesNativeApplied[] =
    "lazyload_images_native_applied";
const char LazyloadImagesFilter::kLazyloadImagesSkippedCritical[] =
    "lazyload_images_skipped_critical";
const char LazyloadImagesFilter::kLazyloadImagesSkippedCsp[] =
    "lazyload_images_skipped_csp";

LazyloadImagesFilter::LazyloadImagesFilter(RewriteDriver* driver)
    : CommonFilter(driver) {
  Clear();
  blank_image_url_ = GetBlankImageSrc(
      driver->options(), driver->server_context()->static_asset_manager());
  Statistics* stats = driver->server_context()->statistics();
  lazyload_images_applied_ = stats->GetVariable(kLazyloadImagesApplied);
  lazyload_images_native_applied_ =
      stats->GetVariable(kLazyloadImagesNativeApplied);
  lazyload_images_skipped_critical_ =
      stats->GetVariable(kLazyloadImagesSkippedCritical);
  lazyload_images_skipped_csp_ = stats->GetVariable(kLazyloadImagesSkippedCsp);
}
LazyloadImagesFilter::~LazyloadImagesFilter() {}

void LazyloadImagesFilter::InitStats(Statistics* statistics) {
  statistics->AddVariable(kLazyloadImagesApplied);
  statistics->AddVariable(kLazyloadImagesNativeApplied);
  statistics->AddVariable(kLazyloadImagesSkippedCritical);
  statistics->AddVariable(kLazyloadImagesSkippedCsp);
}

void LazyloadImagesFilter::DetermineEnabled(GoogleString* disabled_reason) {
  RewriterHtmlApplication::Status should_apply = ShouldApply(driver());
  set_is_enabled(should_apply == RewriterHtmlApplication::ACTIVE);
  if (should_apply == RewriterHtmlApplication::DISABLED &&
      !ShouldApplyNativeMode(driver()) && !CspPermitsJsMode(driver())) {
    lazyload_images_skipped_csp_->Add(1);
    csp_skip_counted_ = true;
  }
  driver()->log_record()->LogRewriterHtmlStatus(
      RewriteOptions::FilterId(RewriteOptions::kLazyloadImages), should_apply);
}

void LazyloadImagesFilter::StartDocumentImpl() {
  Clear();
  native_mode_ = ShouldApplyNativeMode(driver());
}

void LazyloadImagesFilter::EndDocument() {
  // The critical-images beacon delegates its onload firing to the lazyload
  // loader whenever the js machinery is active on the request, so the loader
  // must be present even if no image was rewritten (e.g. every image was
  // critical). Without it the beacon would never fire and criticality data
  // would starve. Keyed on the beacon filter being in the filter chain, not
  // on ShouldBeacon(): that rate-limiter was already consumed by the beacon
  // filter this request and would answer false exactly when a delegated
  // beacon is waiting.
  if (!native_mode_ && !main_script_inserted_ && !abort_rewrite_ &&
      CspPermitsJsMode(driver()) &&
      driver()->is_critical_images_beacon_enabled()) {
    HtmlElement* script = driver()->NewElement(nullptr, HtmlName::kScript);
    InsertNodeAtBodyEnd(script);
    StaticAssetManager* static_asset_manager =
        driver()->server_context()->static_asset_manager();
    AddJsToElement(
        GetLazyloadJsSnippet(driver()->options(), static_asset_manager),
        script);
    driver()->AddAttribute(script, HtmlName::kDataPagespeedNoDefer,
                           StringPiece());
    main_script_inserted_ = true;
  }
  driver()->UpdatePropertyValueInDomCohort(
      driver()->fallback_property_page(), kIsLazyloadScriptInsertedPropertyName,
      main_script_inserted_ ? "1" : "0");
}

void LazyloadImagesFilter::Clear() {
  skip_rewrite_ = nullptr;
  native_mode_ = false;
  main_script_inserted_ = false;
  abort_rewrite_ = false;
  abort_script_inserted_ = false;
  csp_skip_counted_ = false;
  num_images_lazily_loaded_ = 0;
  num_eligible_images_seen_ = 0;
}

bool LazyloadImagesFilter::CspPermitsJsMode(RewriteDriver* driver) {
  // The js machinery consists of an inline loader <script> plus inline
  // onload/onerror attributes, so both inline scripts and inline script
  // attributes must be permitted.
  return !driver->options()->honor_csp() ||
         (driver->content_security_policy().PermitsInlineScript() &&
          driver->content_security_policy().PermitsInlineScriptAttribute());
}

bool LazyloadImagesFilter::ShouldApplyNativeMode(RewriteDriver* driver) {
  switch (driver->options()->lazyload_images_mode()) {
    case RewriteOptions::kLazyloadImagesModeNative:
      return true;
    case RewriteOptions::kLazyloadImagesModeJs:
      return false;
    case RewriteOptions::kLazyloadImagesModeAuto:
      break;
  }
  return driver->user_agent_matcher()->SupportsNativeLazyLoading(
      driver->user_agent());
}

RewriterHtmlApplication::Status LazyloadImagesFilter::ShouldApply(
    RewriteDriver* driver) {
  // Note: there's similar UA logic in
  // DedupInlinedImagedFilter::DetermineEnabled, so if this logic changes that
  // logic may well require alteration too.
  if (!driver->request_properties()->SupportsLazyloadImages()) {
    return RewriterHtmlApplication::USER_AGENT_NOT_SUPPORTED;
  }
  if (driver->request_headers() != nullptr &&
      driver->request_headers()->IsXmlHttpRequest()) {
    return RewriterHtmlApplication::DISABLED;
  }
  if (!ShouldApplyNativeMode(driver)) {
    // The js machinery injects an inline loader <script> and inline
    // onload/onerror handlers. Under a Content-Security-Policy that forbids
    // inline script the blanked-out images would never be restored, so the
    // filter must stay off. Native mode injects no JavaScript and remains
    // enabled under strict CSP. Header-borne CSP may be known here; meta-tag
    // CSP is discovered while parsing and re-checked per image.
    if (!CspPermitsJsMode(driver)) {
      return RewriterHtmlApplication::DISABLED;
    }
    CriticalImagesFinder* finder =
        driver->server_context()->critical_images_finder();
    if (finder->Available(driver) == CriticalImagesFinder::kNoDataYet) {
      // Don't lazyload images on a page that's waiting for critical image
      // data. However, this page should later be rewritten when data arrives.
      // Contrast this with the case where beaconing is explicitly disabled,
      // where images are lazy loaded (subject to LazyloadImagesSkipFirst).
      return RewriterHtmlApplication::DISABLED;
    }
  }
  return RewriterHtmlApplication::ACTIVE;
}

void LazyloadImagesFilter::StartElementImpl(HtmlElement* element) {
  if (noscript_element() != nullptr) {
    return;
  }
  if (skip_rewrite_ == nullptr) {
    if (element->keyword() == HtmlName::kNoembed ||
        element->keyword() == HtmlName::kMarquee ||
        (!native_mode_ && element->keyword() == HtmlName::kPicture)) {
      // In js mode an <img> inside <picture> must not be blanked out: the
      // sibling <source> elements are not rewritten, so the browser would
      // either load the original candidate eagerly or select the blank pixel.
      // The native loading="lazy" attribute is valid and effective on the
      // <img> inside <picture>, so native mode does rewrite it.
      skip_rewrite_ = element;
      return;
    }
    // Check if lazyloading is enabled for the given class name. If not,
    // skip rewriting all images till we reach the end of this element.
    HtmlElement::Attribute* class_attribute =
        element->FindAttribute(HtmlName::kClass);
    if (class_attribute != nullptr) {
      StringPiece class_value(class_attribute->DecodedValueOrNull());
      if (!class_value.empty()) {
        GoogleString class_string;
        class_value.CopyToString(&class_string);
        LowerString(&class_string);
        if (!driver()->options()->IsLazyloadEnabledForClassName(class_string)) {
          skip_rewrite_ = element;
          return;
        }
      }
    }
  }
  if (!native_mode_ && element->keyword() == HtmlName::kScript) {
    // This filter does not currently work with the jquery slider. We just
    // don't rewrite the page in this case.
    HtmlElement::Attribute* src = element->FindAttribute(HtmlName::kSrc);
    if (src != nullptr) {
      StringPiece url(src->DecodedValueOrNull());
      if (url.find(kJquerySlider) != StringPiece::npos) {
        abort_rewrite_ = true;
        return;
      }
    }
    InsertOverrideAttributesScript(element, true);
  }
}

void LazyloadImagesFilter::EndElementImpl(HtmlElement* element) {
  if (noscript_element() != nullptr || skip_rewrite_ != nullptr) {
    if (skip_rewrite_ == element) {
      skip_rewrite_ = nullptr;
    }
    return;
  }
  if (abort_rewrite_) {
    if (!abort_script_inserted_ && main_script_inserted_ &&
        num_images_lazily_loaded_ > 0) {
      // If we have already rewritten some elements on the page, insert a
      // script to load all previously rewritten images.
      HtmlElement* script = driver()->NewElement(element, HtmlName::kScript);
      driver()->AddAttribute(script, HtmlName::kType, "text/javascript");
      HtmlNode* script_code =
          driver()->NewCharactersNode(script, kLoadAllImages);
      driver()->InsertNodeAfterNode(element, script);
      driver()->AppendChild(script, script_code);
      abort_script_inserted_ = true;
    }
    return;
  }
  if (element->keyword() == HtmlName::kBody) {
    InsertOverrideAttributesScript(element, false);
    return;
  }
  // Only rewrite <img> tags. Don't rewrite <input> tags since the onload
  // event is not fired for them in some browsers.
  if (!driver()->IsRewritable(element) ||
      element->keyword() != HtmlName::kImg) {
    return;
  }

  HtmlElement::Attribute* src = element->FindAttribute(HtmlName::kSrc);
  if (src == nullptr) {
    return;
  }

  // Respect an author-supplied loading attribute in all modes: the author
  // already declared loading intent, and stacking a second deferral
  // mechanism on top of it (or overriding it) would fight that intent. In
  // particular, the js machinery would defer the fetch of its own blank
  // placeholder pixel on a loading="lazy" image.
  if (element->FindAttribute(HtmlName::kLoading) != nullptr) {
    return;
  }

  StringPiece url(src->DecodedValueOrNull());
  if (url.empty() || IsDataUrl(url) ||
      element->FindAttribute(HtmlName::kDataPagespeedNoDefer) != nullptr ||
      element->FindAttribute(HtmlName::kPagespeedNoDefer) != nullptr) {
    // TODO(rahulbansal): Log separately for pagespeed_no_defer.
    return;
  }
  AbstractLogRecord* log_record = driver()->log_record();
  if (element->FindAttribute(HtmlName::kDataPagespeedLazySrc) != nullptr ||
      element->FindAttribute(HtmlName::kDataSrc) != nullptr ||
      (!native_mode_ && !CanAddPagespeedOnloadToImage(*element))) {
    log_record->LogLazyloadFilter(
        RewriteOptions::FilterId(RewriteOptions::kLazyloadImages),
        RewriterApplication::NOT_APPLIED, false, false);
    return;
  }
  // Decode the url if it is rewritten.
  GoogleUrl gurl(base_url(), url);
  StringVector decoded_url_vector;
  if (driver()->DecodeUrl(gurl, &decoded_url_vector) &&
      decoded_url_vector.size() == 1) {
    // We only handle the case where the rewritten url corresponds to
    // a single original url which should be sufficient for all cases
    // other than image sprites.
    gurl.Reset(decoded_url_vector[0]);
  }
  if (!gurl.IsAnyValid()) {
    // Do not lazily load images with invalid urls.
    return;
  }
  StringPiece full_url = gurl.Spec();
  if (full_url.empty()) {
    return;
  }
  if (!driver()->options()->IsAllowed(full_url)) {
    // Do not lazily load images with blacklisted urls.
    log_record->LogLazyloadFilter(
        RewriteOptions::FilterId(RewriteOptions::kLazyloadImages),
        RewriterApplication::NOT_APPLIED, true, false);
    return;
  }

  CriticalImagesFinder* finder =
      driver()->server_context()->critical_images_finder();
  // Note that if the platform lacks a CriticalImageFinder implementation, we
  // consider all images to be non-critical and try to lazily load them.
  // Similarly, if we have disabled data gathering for lazy load, we again
  // lazy load all images (subject to the LazyloadImagesSkipFirst LCP
  // protection below). If, however, we simply haven't gathered enough data
  // yet, js mode considers all images critical and disables lazy loading (in
  // ShouldApply above) in order to provide better above-the-fold loading,
  // while native mode stays enabled with the skip-first protection.
  CriticalImagesFinder::Availability available = finder->Available(driver());
  if (available == CriticalImagesFinder::kAvailable) {
    // Decode the url since the critical images in the finder are not
    // rewritten.
    if (finder->IsHtmlCriticalImage(full_url, driver())) {
      log_record->LogLazyloadFilter(
          RewriteOptions::FilterId(RewriteOptions::kLazyloadImages),
          RewriterApplication::NOT_APPLIED, false, true);
      lazyload_images_skipped_critical_->Add(1);
      if (native_mode_) {
        // Critical images load eagerly; raise their fetch priority and keep
        // decoding off the critical path.
        if (element->FindAttribute(HtmlName::kFetchpriority) == nullptr) {
          driver()->AddAttribute(element, HtmlName::kFetchpriority, "high");
        }
        if (element->FindAttribute(HtmlName::kDecoding) == nullptr) {
          driver()->AddAttribute(element, HtmlName::kDecoding, "async");
        }
      }
      // Do not try to lazily load this image since it is critical.
      return;
    }
  }
  if (!native_mode_ && !CspPermitsJsMode(driver())) {
    // A meta-tag Content-Security-Policy forbidding inline script was seen
    // after ShouldApply ran. The js machinery would blank out images that
    // could never be restored, so leave the image untouched.
    if (!csp_skip_counted_) {
      lazyload_images_skipped_csp_->Add(1);
      csp_skip_counted_ = true;
    }
    log_record->LogLazyloadFilter(
        RewriteOptions::FilterId(RewriteOptions::kLazyloadImages),
        RewriterApplication::NOT_APPLIED, false, false);
    return;
  }
  if (available != CriticalImagesFinder::kAvailable) {
    // LCP protection: without critical-image data every image on the page
    // would be deferred, including the largest-contentful-paint image. Leave
    // the first N otherwise eligible images untouched.
    ++num_eligible_images_seen_;
    if (num_eligible_images_seen_ <=
        driver()->options()->lazyload_images_skip_first()) {
      log_record->LogLazyloadFilter(
          RewriteOptions::FilterId(RewriteOptions::kLazyloadImages),
          RewriterApplication::NOT_APPLIED, false, false);
      return;
    }
  }
  if (native_mode_) {
    driver()->AddAttribute(element, HtmlName::kLoading, "lazy");
    // decoding="async" keeps image decode off the main thread. Deliberately
    // no fetchpriority="low" on non-critical images: loading="lazy" already
    // deprioritizes the fetch, and a blanket low priority would further slow
    // just-below-the-fold images.
    if (element->FindAttribute(HtmlName::kDecoding) == nullptr) {
      driver()->AddAttribute(element, HtmlName::kDecoding, "async");
    }
    lazyload_images_native_applied_->Add(1);
    log_record->LogLazyloadFilter(
        RewriteOptions::FilterId(RewriteOptions::kLazyloadImages),
        RewriterApplication::APPLIED_OK, false, false);
    return;
  }
  if (!main_script_inserted_) {
    InsertLazyloadJsCode(element);
  }
  // Replace the src with data-pagespeed-lazy-src.
  driver()->SetAttributeName(src, HtmlName::kDataPagespeedLazySrc);
  // Rename srcset -> data-pagespeed-high-res-srcset
  HtmlElement::Attribute* srcset = element->FindAttribute(HtmlName::kSrcset);
  if (srcset != nullptr) {
    driver()->SetAttributeName(srcset, HtmlName::kDataPagespeedLazySrcset);
  }
  driver()->AddAttribute(element, HtmlName::kSrc, blank_image_url_);
  lazyload_images_applied_->Add(1);
  log_record->LogLazyloadFilter(
      RewriteOptions::FilterId(RewriteOptions::kLazyloadImages),
      RewriterApplication::APPLIED_OK, false, false);
  // Add an onload function to load the image if it is visible and then do
  // the criticality check. Since we check CanAddPagespeedOnloadToImage
  // before coming here, the only onload handler that we would delete would
  // be the one added by our very own beaconing code. We re-introduce this
  // beaconing onload logic via kImageOnloadCode.
  // TODO(jud): Add these with addEventListener rather than with the
  // attributes.
  element->DeleteAttribute(HtmlName::kOnload);
  driver()->AddAttribute(element, HtmlName::kOnload, kImageOnloadCode);
  // Add onerror handler just in case the temporary pixel doesn't load.
  element->DeleteAttribute(HtmlName::kOnerror);
  // Note: this.onerror=null to avoid infinitely repeating on failure:
  //   See: http://stackoverflow.com/questions/3984287
  driver()->AddAttribute(element, HtmlName::kOnerror,
                         StrCat("this.onerror=null;", kImageOnloadCode));
  ++num_images_lazily_loaded_;
}

void LazyloadImagesFilter::InsertLazyloadJsCode(HtmlElement* element) {
  // The loader script is inserted immediately before the first image the
  // filter rewrites, so pages without eligible images carry no script at
  // all.
  if (!driver()->is_lazyload_script_flushed() &&
      (!abort_rewrite_ || num_images_lazily_loaded_ > 0)) {
    HtmlElement* script = driver()->NewElement(element, HtmlName::kScript);
    driver()->InsertNodeBeforeNode(element, script);
    StaticAssetManager* static_asset_manager =
        driver()->server_context()->static_asset_manager();
    GoogleString lazyload_js =
        GetLazyloadJsSnippet(driver()->options(), static_asset_manager);
    AddJsToElement(lazyload_js, script);
    driver()->AddAttribute(script, HtmlName::kDataPagespeedNoDefer,
                           StringPiece());
  }
  main_script_inserted_ = true;
}

void LazyloadImagesFilter::InsertOverrideAttributesScript(
    HtmlElement* element, bool is_before_script) {
  if (num_images_lazily_loaded_ > 0) {
    HtmlElement* script = driver()->NewElement(element, HtmlName::kScript);
    driver()->AddAttribute(script, HtmlName::kType, "text/javascript");
    driver()->AddAttribute(script, HtmlName::kDataPagespeedNoDefer,
                           StringPiece());
    HtmlNode* script_code =
        driver()->NewCharactersNode(script, kOverrideAttributeFunctions);
    if (is_before_script) {
      driver()->InsertNodeBeforeNode(element, script);
    } else {
      driver()->AppendChild(element, script);
    }
    driver()->AppendChild(script, script_code);
    num_images_lazily_loaded_ = 0;
  }
}

GoogleString LazyloadImagesFilter::GetBlankImageSrc(
    const RewriteOptions* options,
    const StaticAssetManager* static_asset_manager) {
  const GoogleString& options_url = options->lazyload_images_blank_url();
  if (options_url.empty()) {
    return static_asset_manager->GetAssetUrl(StaticAssetEnum::BLANK_GIF,
                                             options);
  } else {
    return options_url;
  }
}

GoogleString LazyloadImagesFilter::GetLazyloadJsSnippet(
    const RewriteOptions* options, StaticAssetManager* static_asset_manager) {
  const GoogleString& load_onload =
      options->lazyload_images_after_onload() ? kTrue : kFalse;
  StringPiece lazyload_images_js = static_asset_manager->GetAsset(
      StaticAssetEnum::LAZYLOAD_IMAGES_JS, options);
  const GoogleString& blank_image_url =
      GetBlankImageSrc(options, static_asset_manager);
  // The blank image url is configurable (LazyloadImagesBlankUrl) and is
  // spliced into an inline script, so it must be escaped as a JS string
  // literal.
  GoogleString escaped_blank_image_url;
  EscapeToJsStringLiteral(blank_image_url, true /* add_quotes */,
                          &escaped_blank_image_url);
  GoogleString lazyload_js =
      StrCat(lazyload_images_js, "\npagespeed.lazyLoadInit(", load_onload, ", ",
             escaped_blank_image_url, ");\n");
  return lazyload_js;
}

}  // namespace net_instaweb
