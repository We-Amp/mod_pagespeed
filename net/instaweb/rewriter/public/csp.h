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

//
// This provides basic parsing and evaluation of a (subset of)
// Content-Security-Policy that's relevant for PageSpeed Automatic.
// CspContext is the main class.
//
// Limitations versus the full spec:
// 1) We don't fully parse some kinds of source expressions, like nonce and
//    hash ones.
// 2) Only some of the directives are parsed.
// 3) URL matching mostly doesn't support WebSocket (ws: and wss:)
//    schemes, since mod_pagespeed doesn't rewrite them; the lone-*
//    source does cover them per spec, however.

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_CSP_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_CSP_H_

#include <memory>
#include <string>
#include <vector>

#include "net/instaweb/rewriter/public/csp_directive.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/google_url.h"

namespace net_instaweb {

class CspSourceExpression {
 public:
  enum Kind {
    kSelf,
    kSchemeSource,
    kHostSource,
    kUnsafeInline,
    kUnsafeEval,
    kStrictDynamic,
    kUnsafeHashedAttributes,
    kHashOrNonce,
    kUnknown
  };

  struct UrlData {
    UrlData() : path_exact_match(false) {}
    // Constructor for tests, assumes already normalized.
    UrlData(StringPiece in_scheme, StringPiece in_host, StringPiece in_port,
            StringPiece in_path, bool exact_match = false)
        : scheme_part(in_scheme.as_string()),
          host_part(in_host.as_string()),
          port_part(in_port.as_string()),
          path_exact_match(exact_match) {
      StringPieceVector portions;
      SplitStringPieceToVector(in_path, "/", &portions, true);
      for (StringPiece p : portions) {
        path_part.push_back(p.as_string());
      }
    }

    // All the components here are stored in a manner that matches the way
    // GoogleUrl stores their corresponding portions, to make it easy to
    // compare against incoming URLs:
    // 1) The case-insensitive scheme and host portions are lowercased.
    // 2) The case-sensitive path doesn't have its case changed, but the
    //    % escaping is normalized. We also pre-split it since we have
    //    to check per-component.
    GoogleString scheme_part;  // doesn't include :
    GoogleString host_part;
    GoogleString port_part;
    // separated by /
    std::vector<GoogleString> path_part;
    bool path_exact_match;

    GoogleString DebugString() const {
      return StrCat("scheme:", scheme_part, " host:", host_part,
                    " port:", port_part,
                    " path:", JoinCollection(path_part, "/"),
                    " path_exact_match:", BoolToString(path_exact_match));
    }

    // For convenience of unit testing.
    bool operator==(const UrlData& other) const {
      return scheme_part == other.scheme_part && host_part == other.host_part &&
             port_part == other.port_part && path_part == other.path_part &&
             path_exact_match == other.path_exact_match;
    }
  };

  CspSourceExpression() : kind_(kUnknown) {}
  explicit CspSourceExpression(Kind kind) : kind_(kind) {}
  CspSourceExpression(Kind kind, const UrlData& url_data) : kind_(kind) {
    *mutable_url_data() = url_data;
  }

  static CspSourceExpression Parse(StringPiece input);

  bool Matches(const GoogleUrl& origin_url, const GoogleUrl& url) const;

  GoogleString DebugString() const {
    return StrCat("kind:", IntegerToString(kind_), " url_data:{",
                  url_data().DebugString(), "}");
  }

  bool operator==(const CspSourceExpression& other) const {
    return (kind_ == other.kind_) && (url_data() == other.url_data());
  }

  Kind kind() const { return kind_; }

  const UrlData& url_data() const {
    if (url_data_.get() == nullptr) {
      url_data_ = std::make_unique<UrlData>();
    }
    return *url_data_.get();
  }

 private:
  // input here is without the quotes, and non-empty.
  static CspSourceExpression ParseQuoted(StringPiece input);

  // Returns true if input matches the base64-value production in CSP spec.
  static bool ParseBase64(StringPiece input);

  // Tries to see if the input is either an entire scheme-source, or the
  // scheme-part portion of a host-source, filling in url_data->scheme_part
  // appropriately. Returns true only if this is a scheme-source, however.
  bool TryParseScheme(StringPiece* input);

  static bool HasDefaultPortForScheme(const GoogleUrl& url);

  UrlData* mutable_url_data() {
    if (url_data_.get() == nullptr) {
      url_data_ = std::make_unique<UrlData>();
    }
    return url_data_.get();
  }

  Kind kind_;
  mutable std::unique_ptr<UrlData> url_data_;
};

class CspSourceList {
 public:
  CspSourceList()
      : saw_unsafe_inline_(false),
        saw_unsafe_eval_(false),
        saw_strict_dynamic_(false),
        saw_unsafe_hashed_attributes_(false),
        saw_hash_or_nonce_(false) {}

  static std::unique_ptr<CspSourceList> Parse(StringPiece input);
  const std::vector<CspSourceExpression>& expressions() const {
    return expressions_;
  }

  bool saw_unsafe_inline() const { return saw_unsafe_inline_; }
  bool saw_unsafe_eval() const { return saw_unsafe_eval_; }
  bool saw_strict_dynamic() const { return saw_strict_dynamic_; }
  bool saw_unsafe_hashed_attributes() const {
    return saw_unsafe_hashed_attributes_;
  }

  bool saw_hash_or_nonce() const { return saw_hash_or_nonce_; }

  bool Matches(const GoogleUrl& origin_url, const GoogleUrl& url) const;

  // Whether the list contains a scheme-source for 'scheme' (lowercase,
  // without the colon), e.g. "data". Host sources and '*' do not match
  // data: URLs, so this is what decides whether data: content may be
  // introduced under this list.
  bool HasSchemeSource(StringPiece scheme) const;

  // Whether this source list can match no URL whatsoever: it holds no
  // source expressions, so Matches() returns false for every URL. This is
  // the representation of 'none' and of an empty source list (keyword-only
  // sources such as 'unsafe-inline' or nonces/hashes contribute no matchable
  // expression either). For the base-uri directive this is the provably-safe
  // signal that the browser will ignore every <base> element, since no
  // <base href> value can satisfy the policy.
  //
  // Caveat: "matches no URL" does not imply "blocks everything" for every
  // directive. Where non-URL sources can authorize loads (e.g. nonces,
  // hashes, or 'unsafe-inline' under script-src), a list with an empty
  // expression set may still permit content; only draw the blocks-all
  // conclusion for directives matched purely by URL, such as base-uri.
  bool MatchesNothing() const { return expressions_.empty(); }

 private:
  std::vector<CspSourceExpression> expressions_;
  bool saw_unsafe_inline_;
  bool saw_unsafe_eval_;
  bool saw_strict_dynamic_;
  bool saw_unsafe_hashed_attributes_;
  bool saw_hash_or_nonce_;
};

// An individual policy. Note that a page is constrained by an intersection
// of some number of these.
class CspPolicy {
 public:
  CspPolicy();

  // May return null.
  static std::unique_ptr<CspPolicy> Parse(StringPiece input);

  // May return null.
  const CspSourceList* SourceListFor(CspDirective directive) const {
    return policies_[static_cast<int>(directive)].get();
  }

  bool PermitsEval() const;
  bool PermitsInlineScript() const;
  bool PermitsInlineScriptAttribute() const;
  bool PermitsInlineStyle() const;
  bool PermitsInlineStyleAttribute() const;

  // Whether an image with a data: URL is permitted: img-src (falling
  // back to default-src) must either be absent or contain an explicit
  // data: scheme-source.
  bool PermitsDataImage() const;

  // Tests whether 'url' can be loaded within 'origin_url' as 'role', where
  // 'role' should be kStyleSrc, kScriptSrc or kImgSrc.
  bool CanLoadUrl(CspDirective role, const GoogleUrl& origin_url,
                  const GoogleUrl& url) const;

  bool IsBasePermitted(const GoogleUrl& previous_origin,
                       const GoogleUrl& base_candidate) const;

  // Whether this policy's base-uri directive provably neutralizes any <base>
  // element on the page: the directive is present and its source list matches
  // no URL (e.g. base-uri 'none' or an empty base-uri list), so the browser
  // ignores every <base>, leaving relative-URL resolution anchored at the
  // document URL. When true a <base> tag cannot change resolution and may be
  // treated as inert. Returns false when base-uri is absent (no restriction on
  // <base>) or names any source that some <base href> could match --- in
  // particular 'self' or a host/scheme list, which still permit a same-origin
  // <base> to change the path and thus resolution.
  bool BaseUriDisablesAllBases() const;

 private:
  // Returns the source list that effectively governs 'specific',
  // following the CSP3 fallback chain: 'specific' if present, else
  // 'base', else default-src. May return null if none are present.
  const CspSourceList* EffectiveSourceList(CspDirective specific,
                                           CspDirective base) const;

  // The expectation is that some of these may be null.
  std::vector<std::unique_ptr<CspSourceList>> policies_;
};

// A set of all policies (maybe none!) on the page. Note that we do not track
// those with report disposition, only those that actually enforce --- reporting
// seems like it would keep the page author informed about our effects as it is.
//
// Thread-safety: RewriteDriver publishes CspContext objects copy-on-write
// (see RewriteDriver::AddCspPolicy) --- a published context is never
// mutated again, so it may be read from rewrite threads without locking.
// Copying a context is cheap: the policies themselves are shared, not
// duplicated.
class CspContext {
 public:
  bool PermitsEval() const { return AllPermit(&CspPolicy::PermitsEval); }

  bool PermitsInlineScript() const {
    return AllPermit(&CspPolicy::PermitsInlineScript);
  }

  bool PermitsInlineScriptAttribute() const {
    return AllPermit(&CspPolicy::PermitsInlineScriptAttribute);
  }

  bool PermitsInlineStyle() const {
    return AllPermit(&CspPolicy::PermitsInlineStyle);
  }

  bool PermitsInlineStyleAttribute() const {
    return AllPermit(&CspPolicy::PermitsInlineStyleAttribute);
  }

  bool PermitsDataImage() const {
    return AllPermit(&CspPolicy::PermitsDataImage);
  }

  bool CanLoadUrl(CspDirective role, const GoogleUrl& origin_url,
                  const GoogleUrl& url) const {
    // All policies must OK, with base case being 'true'.
    for (const auto& policy : policies_) {
      if (!policy->CanLoadUrl(role, origin_url, url)) {
        return false;
      }
    }
    return true;
  }

  bool IsBasePermitted(const GoogleUrl& previous_origin,
                       const GoogleUrl& base_candidate) const {
    for (const auto& policy : policies_) {
      if (!policy->IsBasePermitted(previous_origin, base_candidate)) {
        return false;
      }
    }
    return true;
  }

  // Whether the combined page policy provably neutralizes every <base>
  // element: some enforced policy's base-uri matches nothing. CSP is a
  // conjunction, so a single policy that blocks all <base> URLs is enough to
  // make the browser ignore <base> entirely, regardless of what the other
  // policies allow. When true, rewriting can treat a <base> tag as inert
  // instead of bailing. Conservative: false unless at least one policy
  // provably blocks all bases.
  bool IsBaseNeutralizedByCsp() const {
    for (const auto& policy : policies_) {
      if (policy->BaseUriDisablesAllBases()) {
        return true;
      }
    }
    return false;
  }

  bool HasDirective(CspDirective directive) const {
    for (const auto& policy : policies_) {
      if (policy->SourceListFor(directive) != nullptr) {
        return true;
      }
    }
    return false;
  }

  bool HasDirectiveOrDefaultSrc(CspDirective directive) const {
    for (const auto& policy : policies_) {
      if (policy->SourceListFor(directive) != nullptr ||
          policy->SourceListFor(CspDirective::kDefaultSrc) != nullptr) {
        return true;
      }
      // The CSP3 -elem/-attr variants govern the same content classes,
      // so their presence matters just as much to callers asking
      // whether any applicable policy exists.
      if (directive == CspDirective::kScriptSrc &&
          (policy->SourceListFor(CspDirective::kScriptSrcElem) != nullptr ||
           policy->SourceListFor(CspDirective::kScriptSrcAttr) != nullptr)) {
        return true;
      }
      if (directive == CspDirective::kStyleSrc &&
          (policy->SourceListFor(CspDirective::kStyleSrcElem) != nullptr ||
           policy->SourceListFor(CspDirective::kStyleSrcAttr) != nullptr)) {
        return true;
      }
    }
    return false;
  }

  void AddPolicy(std::unique_ptr<CspPolicy> policy);
  void Clear() { policies_.clear(); }
  size_t policies_size() const { return policies_.size(); }
  bool empty() const { return policies_.empty(); }

 private:
  typedef bool (CspPolicy::*SimplePredicateFn)() const;

  bool AllPermit(SimplePredicateFn predicate) const {
    // Note that empty policies_ means "true" --- there is no policy whatsoever,
    // so everything is permitted. If there is more than that, all policies
    // must agree, too.
    for (const auto& policy : policies_) {
      if (!(policy.get()->*predicate)()) {
        return false;
      }
    }
    return true;
  }

  std::vector<std::shared_ptr<const CspPolicy>> policies_;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_CSP_H_
