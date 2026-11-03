// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "net/instaweb/rewriter/public/hydration_signals.h"

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

namespace {

// Path segments that only the build output of these frameworks uses.
bool IsFrameworkAssetUrl(const char* url) {
  if (url == nullptr) {
    return false;
  }
  StringPiece path(url);
  const size_t end = path.find_first_of("?#");
  if (end != StringPiece::npos) {
    path = path.substr(0, end);
  }
  return path.find("/_next/") != StringPiece::npos ||
         path.find("/_nuxt/") != StringPiece::npos ||
         path.find("/_app/immutable/") != StringPiece::npos;
}

bool IsMarkerAttribute(const HtmlElement::Attribute& attribute) {
  const StringPiece name(attribute.name_str());
  return StringCaseEqual(name, "data-precedence") ||
         StringCaseEqual(name, "data-reactroot") ||
         StringCaseEqual(name, "data-server-rendered") ||
         StringCaseEqual(name, "ng-app-id") ||
         StringCaseEqual(name, "ng-version") ||
         StringCaseEqual(name, "ng-server-context") ||
         StringCaseStartsWith(name, "data-sveltekit-");
}

bool IsMarkerId(const HtmlElement& element, StringPiece id) {
  if (element.keyword() == HtmlName::kScript) {
    return id == "__NEXT_DATA__" || id == "__NUXT_DATA__" || id == "ng-state";
  }
  return id == "__next" || id == "__nuxt";
}

}  // namespace

bool HydrationSignalInHeaders(const ResponseHeaders& headers) {
  ConstStringStarVector values;
  if (headers.Lookup(HttpAttributes::kVary, &values)) {
    for (const GoogleString* value : values) {
      if (value != nullptr && StringCaseEqual(*value, "RSC")) {
        return true;
      }
    }
  }
  return false;
}

bool HydrationSignalInElement(const HtmlElement& element) {
  const HtmlName::Keyword keyword = element.keyword();
  if ((keyword == HtmlName::kScript &&
       IsFrameworkAssetUrl(element.AttributeValue(HtmlName::kSrc))) ||
      (keyword == HtmlName::kLink &&
       IsFrameworkAssetUrl(element.AttributeValue(HtmlName::kHref)))) {
    return true;
  }
  if (StringCaseEqual(element.name_str(), "astro-island")) {
    return true;
  }
  for (const HtmlElement::Attribute& attribute : element.attributes()) {
    if (IsMarkerAttribute(attribute)) {
      return true;
    }
    if (attribute.keyword() == HtmlName::kId &&
        attribute.DecodedValueOrNull() != nullptr &&
        IsMarkerId(element, attribute.DecodedValueOrNull())) {
      return true;
    }
  }
  return false;
}

bool HydrationSignalInComment(StringPiece contents) {
  // React marks Suspense boundaries with $, /$, $? and $!. Vue and Svelte mark
  // fragments with [ and ].
  return contents == "$" || contents == "/$" || contents == "$?" ||
         contents == "$!" || contents == "[" || contents == "]";
}

bool HydrationSignalInInlineScript(StringPiece contents) {
  return contents.find("window.__reactRouterContext") != StringPiece::npos ||
         contents.find("window.__remixContext") != StringPiece::npos;
}

}  // namespace net_instaweb
