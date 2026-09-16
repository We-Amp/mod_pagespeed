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

// Filter to inject <link rel="preconnect"> / <link rel="dns-prefetch"> tags in
// the HEAD to let the browser warm up connections to domains the page will use.

#include "net/instaweb/rewriter/public/insert_dns_prefetch_filter.h"

#include <cstdlib>
#include <memory>
#include <set>
#include <utility>  // for pair
#include <vector>

#include "net/instaweb/rewriter/flush_early.pb.h"
#include "net/instaweb/rewriter/public/resource_tag_scanner.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/semantic_type.h"
#include "pagespeed/kernel/http/user_agent_matcher.h"
#include "pagespeed/opt/logging/enums.pb.h"
#include "pagespeed/opt/logging/log_record.h"

namespace {
// Maximum number of DNS prefetch tags inserted in an HTML page.
const int kMaxDnsPrefetchTags = 8;

// Maximum difference between the number of domains in two rewrites to consider
// the domains list stable.
const int kMaxDomainDiff = 2;

// Number of leading domains in the (stable) domain list that get a
// rel="preconnect" hint instead of rel="dns-prefetch". Preconnect opens a
// full TCP+TLS connection per domain, so it must stay small; the
// remaining domains get the nearly-free DNS-only hint.
const int kMaxPreconnectTags = 2;

// Below are the values of the "rel" attribute of LINK tag which are relevant
// to connection warm-up hints.
const char* kRelPrefetch = "prefetch";
const char* kRelDnsPrefetch = "dns-prefetch";
const char* kRelPreconnect = "preconnect";
}  // namespace

namespace net_instaweb {

namespace {

// Extracts the host of the URL held by urlattr, resolved against base_url,
// into *domain, and its scheme-qualified origin (scheme://host[:port]) into
// *origin. Returns false if the attribute is missing or does not hold a valid
// web URL with a non-empty host.
bool ExtractDomain(const GoogleUrl& base_url, HtmlElement::Attribute* urlattr,
                   GoogleString* domain, GoogleString* origin) {
  domain->clear();
  origin->clear();
  if (urlattr == nullptr || urlattr->DecodedValueOrNull() == nullptr) {
    return false;
  }
  GoogleUrl url(base_url, urlattr->DecodedValueOrNull());
  if (!url.IsWebValid()) {
    return false;
  }
  url.Host().CopyToString(domain);
  if (domain->empty()) {
    return false;
  }
  // url was resolved against base_url, so its scheme is the resource's own
  // scheme for absolute URLs and the page scheme for scheme-relative ones.
  // HostAndPort keeps a non-default port (a preconnect warms an origin —
  // scheme, host, port — so dropping it would warm the wrong connection) and
  // omits the port when it is the scheme's default.
  *origin = StrCat(url.Scheme(), "://", url.HostAndPort());
  return true;
}

// Builds the href for a connection warm-up hint for a stored domain entry.
// Entries are stored as scheme-qualified origins (scheme://host[:port]);
// entries written by older revisions of this filter hold a bare host. A
// preconnect hint opens a scheme-specific connection (TCP, plus TLS for
// https), so it must carry the entry's scheme. A dns-prefetch hint only warms
// name resolution, so it keeps the scheme-relative, port-less form.
GoogleString WarmupHintHref(const GoogleString& domain, bool preconnect) {
  GoogleUrl url(domain);
  if (!url.IsWebValid()) {
    return StrCat("//", domain);  // Legacy bare-host entry.
  }
  return preconnect ? domain : StrCat("//", url.Host());
}

}  // namespace

InsertDnsPrefetchFilter::InsertDnsPrefetchFilter(RewriteDriver* driver)
    : CommonFilter(driver) {
  Clear();
}

InsertDnsPrefetchFilter::~InsertDnsPrefetchFilter() {}

void InsertDnsPrefetchFilter::DetermineEnabled(GoogleString* disabled_reason) {
  set_is_enabled(true);
  driver()->set_write_property_cache_dom_cohort(true);
}

void InsertDnsPrefetchFilter::Clear() {
  dns_prefetch_inserted_ = false;
  in_head_ = false;
  domains_to_ignore_.clear();
  domains_in_body_.clear();
  dns_prefetch_domains_.clear();
  user_agent_supports_dns_prefetch_ = false;
}

// Read the information related to DNS prefetch tags from the property cache
// info and populate it in the driver's flush_early_info.
void InsertDnsPrefetchFilter::StartDocumentImpl() {
  Clear();
  // Avoid inserting the domain name of this page by pre-inserting it into
  // domains_to_ignore_.
  GoogleString host = driver()->base_url().Host().as_string();
  domains_to_ignore_.insert(host);
  user_agent_supports_dns_prefetch_ =
      driver()->server_context()->user_agent_matcher()->SupportsDnsPrefetch(
          driver()->user_agent());
  RewriterHtmlApplication::Status status =
      user_agent_supports_dns_prefetch_
          ? RewriterHtmlApplication::ACTIVE
          : RewriterHtmlApplication::USER_AGENT_NOT_SUPPORTED;
  driver()->log_record()->LogRewriterHtmlStatus(
      RewriteOptions::FilterId(RewriteOptions::kInsertDnsPrefetch), status);
}

// Write the information about domains gathered in this rewrite into the
// driver's flush_early_info. This will be written to the property cache when
// the DOM cohort is written. We write a limited set of entries to avoid
// thrashing the browser's DNS cache.
void InsertDnsPrefetchFilter::EndDocument() {
  FlushEarlyInfo* flush_early_info = driver()->flush_early_info();
  flush_early_info->set_total_dns_prefetch_domains_previous(
      flush_early_info->total_dns_prefetch_domains());
  flush_early_info->set_total_dns_prefetch_domains(
      dns_prefetch_domains_.size());
  flush_early_info->clear_dns_prefetch_domains();
  for (const GoogleString& domain : dns_prefetch_domains_) {
    flush_early_info->add_dns_prefetch_domains(domain);
    if (flush_early_info->dns_prefetch_domains_size() >= kMaxDnsPrefetchTags) {
      break;
    }
  }
}

// When a resource url is encountered, try to add its domain to the list of
// domains for which DNS prefetch tags can be inserted. DNS prefetch tags added
// by the origin server will automatically be excluded since we process LINK
// tags.
// TODO(bharathbhushan): Make sure that this filter does not insert DNS prefetch
// tags for resources inserted by the flush early filter.
void InsertDnsPrefetchFilter::StartElementImpl(HtmlElement* element) {
  if (element->keyword() == HtmlName::kHead) {
    in_head_ = true;
    return;
  }
  // We don't need to add domains in NOSCRIPT elements since most browsers
  // support javascript and won't download resources inside NOSCRIPT elements.
  if (noscript_element() != nullptr) {
    return;
  }
  resource_tag_scanner::UrlCategoryVector attributes;
  resource_tag_scanner::ScanElement(element, driver()->options(), &attributes);
  for (int i = 0, n = attributes.size(); i < n; ++i) {
    switch (attributes[i].category) {
      // The categories below are downloaded by the browser to display the page.
      // So DNS prefetch hints are useful.
      case semantic_type::kImage:
      case semantic_type::kScript:
      case semantic_type::kStylesheet:
      case semantic_type::kOtherResource:
        MarkAlreadyInHead(attributes[i].url);
        break;

      case semantic_type::kPrefetch:
        if (element->keyword() == HtmlName::kLink) {
          // For LINK tags, many of the link types are detected as image or
          // stylesheet by the ResourceTagScanner. "prefetch", "dns-prefetch"
          // and "preconnect" are recognized here since they are relevant for
          // resource download. The scanner tokenizes the rel attribute on
          // spaces and matches the tokens case-insensitively; do the same
          // here, since comparing the whole attribute value would miss
          // multi-token hints like rel="preconnect dns-prefetch".
          HtmlElement::Attribute* rel_attr =
              element->FindAttribute(HtmlName::kRel);
          if (rel_attr != nullptr) {
            StringPieceVector rel_tokens;
            SplitStringPieceToVector(rel_attr->DecodedValueOrNull(), " ",
                                     &rel_tokens, true /* skip empty */);
            bool has_prefetch = false;
            bool has_conn_warmup = false;
            for (int j = 0, m = rel_tokens.size(); j < m; ++j) {
              if (StringCaseEqual(rel_tokens[j], kRelPrefetch)) {
                has_prefetch = true;
              } else if (StringCaseEqual(rel_tokens[j], kRelDnsPrefetch) ||
                         StringCaseEqual(rel_tokens[j], kRelPreconnect)) {
                has_conn_warmup = true;
              }
            }
            if (in_head_) {
              if (has_prefetch || has_conn_warmup) {
                // A connection warm-up hint written by the origin server:
                // record the domain so that resources using it later in the
                // page don't cause us to emit a duplicate hint. The domain is
                // only ever ignored here, never added to the list of domains
                // we emit hints for: the author's tag already does the job.
                GoogleString domain;
                GoogleString origin;
                if (ExtractDomain(driver()->base_url(), attributes[i].url,
                                  &domain, &origin)) {
                  domains_to_ignore_.insert(domain);
                }
              }
            } else if (has_prefetch) {
              // A "prefetch" href in BODY is a resource the page will
              // download, so record its domain. A dns-prefetch/preconnect
              // hint found in BODY is already doing its job, and recording
              // its domain would only make us emit a duplicate, so those
              // tokens never reach MarkAlreadyInHead from here.
              MarkAlreadyInHead(attributes[i].url);
            }
          }
        }
        break;

      case semantic_type::kHyperlink:
      case semantic_type::kUndefined:
        break;
    }
  }
}

// At the end of the first HEAD, insert the DNS prefetch tags if the list of
// domains is stable.
void InsertDnsPrefetchFilter::EndElementImpl(HtmlElement* element) {
  if (!user_agent_supports_dns_prefetch_) {
    return;
  }
  if (element->keyword() == HtmlName::kHead) {
    in_head_ = false;
    if (!dns_prefetch_inserted_) {
      // Don't add <link rel='dns-prefetch' ...> tags if we flushed them in
      // the flush early flow.
      dns_prefetch_inserted_ = true;
      const FlushEarlyInfo& flush_early_info = *(driver()->flush_early_info());
      if (IsDomainListStable(flush_early_info)) {
        // The domain list is ordered by first appearance in the BODY, which
        // approximates importance: the first few domains get the stronger
        // rel="preconnect" hint, the rest the nearly-free DNS-only hint.
        int tags_inserted = 0;
        for (const GoogleString& domain :
             flush_early_info.dns_prefetch_domains()) {
          const bool preconnect = tags_inserted < kMaxPreconnectTags;
          const char* tag_to_insert =
              preconnect ? kRelPreconnect : kRelDnsPrefetch;
          HtmlElement* link = driver()->NewElement(element, HtmlName::kLink);
          driver()->AddAttribute(link, HtmlName::kRel, tag_to_insert);
          driver()->AddAttribute(link, HtmlName::kHref,
                                 WarmupHintHref(domain, preconnect));
          driver()->AppendChild(element, link);
          driver()->log_record()->SetRewriterLoggingStatus(
              RewriteOptions::FilterId(RewriteOptions::kInsertDnsPrefetch),
              RewriterApplication::APPLIED_OK);
          ++tags_inserted;
        }
      } else {
        driver()->log_record()->SetRewriterLoggingStatus(
            RewriteOptions::FilterId(RewriteOptions::kInsertDnsPrefetch),
            RewriterApplication::NOT_APPLIED);
      }
    }
  }
}

void InsertDnsPrefetchFilter::MarkAlreadyInHead(
    HtmlElement::Attribute* urlattr) {
  GoogleString domain;
  GoogleString origin;
  if (!ExtractDomain(driver()->base_url(), urlattr, &domain, &origin)) {
    return;
  }
  if (in_head_) {
    domains_to_ignore_.insert(domain);
  } else {
    if (domains_to_ignore_.find(domain) == domains_to_ignore_.end()) {
      std::pair<StringSet::iterator, bool> result =
          domains_in_body_.insert(domain);
      if (result.second) {
        dns_prefetch_domains_.push_back(origin);
      }
    }
  }
}

// Say we are doing the 'n'th rewrite. If the number of domains eligible for DNS
// prefetch tags in 'n-1'th and 'n-2'th rewrite differs by at most
// kMaxDomainDiff, then the list is considered stable and this method returns
// true in that case.
//
// Deliberately count-only, not content-strict (decision recorded 2026-07):
// a page whose domain SET changes completely
// between rewrites but keeps a count within kMaxDomainDiff is judged stable,
// and hints are then emitted for the stored (stale) list. That trade-off is
// accepted because:
//   * the count tolerance is designed, not accidental — natural pages churn
//     slightly between rewrites (rotating ads, experiments), and the filter's
//     own FullFlowTest pins the intended 10->9->6-domains emission ramp;
//     content-strictness would suppress hints exactly when the design means
//     them to fire;
//   * the failure mode is benign: a hint for a domain the page no longer
//     uses wastes a few ms of connection setup — these are advisory hints,
//     never correctness-relevant, and the property-cache TTL bounds
//     staleness;
//   * a content-strict check requires persisting the previous domain LIST
//     (a flush_early.proto wire change with mixed-version-fleet rollout
//     handling), which is disproportionate to the defect.
// Revisit only if production evidence of harmful stale-set emission appears.
bool InsertDnsPrefetchFilter::IsDomainListStable(
    const FlushEarlyInfo& flush_early_info) const {
  return std::abs(flush_early_info.total_dns_prefetch_domains() -
                  flush_early_info.total_dns_prefetch_domains_previous()) <=
         kMaxDomainDiff;
}

}  // namespace net_instaweb
