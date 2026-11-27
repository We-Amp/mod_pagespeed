// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - SubresourceCollectorFilter Implementation

#include "lib/html/subresource_collector_filter.h"

#include <string>
#include <string_view>

#include "lib/base/string_util.h"
#include "lib/html/html_element.h"
#include "lib/html/html_name.h"

namespace net_instaweb {

SubresourceCollectorFilter::SubresourceCollectorFilter(NginxScanResult* result)
    : result_(result), template_depth_(0), limit_reached_(false) {}

SubresourceCollectorFilter::~SubresourceCollectorFilter() = default;

void SubresourceCollectorFilter::StartDocument() {
  template_depth_ = 0;
  limit_reached_ = false;
}

void SubresourceCollectorFilter::EndDocument() {}

void SubresourceCollectorFilter::StartElement(HtmlElement* element) {
  HtmlName::Keyword keyword = element->keyword();

  // Track <template> nesting depth.
  if (keyword == HtmlName::kTemplate) {
    ++template_depth_;
    return;
  }

  // Skip subresource collection inside <template> (inert content).
  if (template_depth_ > 0) {
    return;
  }

  // Check subresource limit.
  if (limit_reached_) {
    return;
  }

  if (keyword == HtmlName::kBase) {
    // <base href="..."> — set the base URL for resolving relative URLs.
    const char* href = element->AttributeValue(HtmlName::kHref);
    if (href != nullptr) {
      result_->base_url = href;
    }
    return;
  }

  if (keyword == HtmlName::kLink) {
    // <link rel="stylesheet" href="...">
    const char* rel = element->AttributeValue(HtmlName::kRel);
    if (rel != nullptr && RelContainsStylesheet(rel)) {
      const char* href = element->AttributeValue(HtmlName::kHref);
      if (href != nullptr) {
        NginxScanResult::SubresourceInfo info;
        info.url = ResolveUrl(href);
        info.type = NginxScanResult::SubresourceInfo::kStylesheet;

        const char* media = element->AttributeValue(HtmlName::kMedia);
        if (media != nullptr) {
          info.media = media;
        }

        const char* crossorigin =
            element->AttributeValue(HtmlName::kCrossorigin);
        if (crossorigin != nullptr) {
          info.crossorigin = crossorigin;
        }

        result_->subresources.push_back(std::move(info));
        if (static_cast<int>(result_->subresources.size()) >=
            kMaxSubresources) {
          limit_reached_ = true;
        }
      }
    }
    return;
  }

  if (keyword == HtmlName::kScript) {
    // <script src="...">
    const char* src = element->AttributeValue(HtmlName::kSrc);
    if (src != nullptr) {
      NginxScanResult::SubresourceInfo info;
      info.url = ResolveUrl(src);
      info.type = NginxScanResult::SubresourceInfo::kScript;

      const char* crossorigin = element->AttributeValue(HtmlName::kCrossorigin);
      if (crossorigin != nullptr) {
        info.crossorigin = crossorigin;
      }

      result_->subresources.push_back(std::move(info));
      if (static_cast<int>(result_->subresources.size()) >= kMaxSubresources) {
        limit_reached_ = true;
      }
    }
    return;
  }

  if (keyword == HtmlName::kMeta) {
    // <meta http-equiv="Content-Security-Policy" content="...">
    const char* http_equiv = element->AttributeValue(HtmlName::kHttpEquiv);
    if (http_equiv != nullptr) {
      std::string_view equiv_sv(http_equiv);
      if (net_instaweb::StringCaseEqual(equiv_sv, "Content-Security-Policy")) {
        const char* content = element->AttributeValue(HtmlName::kContent);
        if (content != nullptr) {
          ParseCspContent(content);
        }
      }
    }
  }
}

void SubresourceCollectorFilter::EndElement(HtmlElement* element) {
  if (element->keyword() == HtmlName::kTemplate && template_depth_ > 0) {
    --template_depth_;
  }
}

// static
bool SubresourceCollectorFilter::RelContainsStylesheet(const char* rel_value) {
  // Parse rel as space-separated tokens using HTML5 whitespace.
  // Matches "stylesheet" case-insensitively.
  std::string_view rel(rel_value);
  size_t i = 0;
  while (i < rel.size()) {
    // Skip whitespace.
    if (IsHtmlSpace(rel[i])) {
      ++i;
      continue;
    }
    // Find end of token.
    size_t start = i;
    while (i < rel.size() && !IsHtmlSpace(rel[i])) {
      ++i;
    }
    std::string_view token = rel.substr(start, i - start);
    if (StringCaseEqual(token, "stylesheet")) {
      return true;
    }
  }
  return false;
}

void SubresourceCollectorFilter::ParseCspContent(const char* content) {
  // CSP is semicolon-separated directives.
  // We look for style-src-elem, style-src, or default-src.
  std::string_view csp(content);
  std::string_view effective_value;
  bool found_style_src_elem = false;
  bool found_style_src = false;
  bool found_default_src = false;
  std::string_view style_src_elem_value;
  std::string_view style_src_value;
  std::string_view default_src_value;

  size_t pos = 0;
  while (pos < csp.size()) {
    size_t end = csp.find(';', pos);
    if (end == std::string_view::npos) {
      end = csp.size();
    }
    std::string_view directive = csp.substr(pos, end - pos);
    pos = end + 1;

    // Trim whitespace from directive.
    size_t ds = 0;
    while (ds < directive.size() && IsHtmlSpace(directive[ds])) ++ds;
    size_t de = directive.size();
    while (de > ds && IsHtmlSpace(directive[de - 1])) --de;
    directive = directive.substr(ds, de - ds);

    if (directive.empty()) continue;

    // Split directive name from value at first space.
    size_t space = 0;
    while (space < directive.size() && !IsHtmlSpace(directive[space])) {
      ++space;
    }
    std::string_view name = directive.substr(0, space);
    std::string_view value;
    if (space < directive.size()) {
      size_t vs = space;
      while (vs < directive.size() && IsHtmlSpace(directive[vs])) ++vs;
      value = directive.substr(vs);
    }

    if (StringCaseEqual(name, "style-src-elem")) {
      found_style_src_elem = true;
      style_src_elem_value = value;
    } else if (StringCaseEqual(name, "style-src")) {
      found_style_src = true;
      style_src_value = value;
    } else if (StringCaseEqual(name, "default-src")) {
      found_default_src = true;
      default_src_value = value;
    }
  }

  // CSP3 fallback chain: style-src-elem → style-src → default-src
  if (found_style_src_elem) {
    effective_value = style_src_elem_value;
  } else if (found_style_src) {
    effective_value = style_src_value;
  } else if (found_default_src) {
    effective_value = default_src_value;
  } else {
    // No relevant directive found — no restriction.
    return;
  }

  ParseCspDirectiveValue(effective_value);
}

void SubresourceCollectorFilter::ParseCspDirectiveValue(
    std::string_view value) {
  // Scan tokens for nonces, hashes, and unsafe-inline.
  bool has_nonce = false;
  bool has_hash = false;
  bool has_unsafe_inline = false;

  size_t i = 0;
  while (i < value.size()) {
    if (IsHtmlSpace(value[i])) {
      ++i;
      continue;
    }
    size_t start = i;
    while (i < value.size() && !IsHtmlSpace(value[i])) {
      ++i;
    }
    std::string_view token = value.substr(start, i - start);

    if (StringCaseStartsWith(token, "'nonce-")) {
      has_nonce = true;
    } else if (StringCaseStartsWith(token, "'sha256-") ||
               StringCaseStartsWith(token, "'sha384-") ||
               StringCaseStartsWith(token, "'sha512-")) {
      has_hash = true;
    } else if (StringCaseEqual(token, "'unsafe-inline'")) {
      has_unsafe_inline = true;
    }
  }

  if (has_nonce) {
    result_->has_nonce_csp = true;
  }
  if (has_hash) {
    result_->has_hash_csp = true;
  }

  // Restrictive if the directive exists without 'unsafe-inline' and
  // without nonces/hashes (nonces/hashes disable unsafe-inline per CSP3).
  if (!has_unsafe_inline && !has_nonce && !has_hash) {
    result_->has_restrictive_style_csp = true;
  }
}

std::string SubresourceCollectorFilter::ResolveUrl(const char* url) const {
  if (result_->base_url.empty()) {
    return std::string(url);
  }

  std::string_view url_sv(url);
  // Absolute URLs are not resolved against base.
  if (url_sv.starts_with("http://") || url_sv.starts_with("https://") ||
      url_sv.starts_with("//")) {
    return std::string(url);
  }

  // Simple resolution: base URL + relative URL.
  // For relative URLs starting with /, use the origin from base.
  std::string_view base = result_->base_url;

  if (url_sv.starts_with("/")) {
    // Find the origin part of the base URL (scheme://host).
    size_t scheme_end = base.find("://");
    if (scheme_end != std::string_view::npos) {
      size_t host_end = base.find('/', scheme_end + 3);
      if (host_end != std::string_view::npos) {
        return std::string(base.substr(0, host_end)) + std::string(url);
      }
    }
    // If base has no scheme, just return the URL as-is.
    return std::string(url);
  }

  // For other relative URLs, append to the base path directory.
  // Strip trailing filename from base.
  size_t last_slash = base.rfind('/');
  if (last_slash != std::string_view::npos) {
    return std::string(base.substr(0, last_slash + 1)) + std::string(url);
  }
  return std::string(url);
}

}  // namespace net_instaweb
