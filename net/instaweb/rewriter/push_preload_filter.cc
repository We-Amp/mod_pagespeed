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
// This implements a filter which generate HTTP2 push or preload fetch hints.
// (e.g. Link: <foo>; rel=preload HTTP headers). Over HTTP2 with mod_http2
// or h2o this will result in a push (if the server is authoritative for the
// resource host); some clients (Chrome 50 as of writing) will also interpret
// it as a hint to preload the resource regardless of protocol.
// http://w3c.github.io/preload is the spec that provides for both behaviors.

#include "net/instaweb/rewriter/public/push_preload_filter.h"

#include <algorithm>
#include <unordered_set>
#include <utility>  // for pair

#include "base/logging.h"
#include "net/instaweb/rewriter/dependencies.pb.h"
#include "net/instaweb/rewriter/public/collect_dependencies_filter.h"
#include "net/instaweb/rewriter/public/dependency_tracker.h"
#include "net/instaweb/rewriter/public/input_info_utils.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

namespace {

// Cap on font preload hints emitted per page, counted in emission order.
// Fonts are byte-heavy and a wrong hint is pure waste, so we bound the
// exposure; CSS and JS hints remain uncapped as before.
const int kMaxFontPreloads = 4;

}  // namespace

PushPreloadFilter::PushPreloadFilter(RewriteDriver* rewrite_driver)
    : CommonFilter(rewrite_driver) {}

PushPreloadFilter::~PushPreloadFilter() {}

void PushPreloadFilter::StartDocumentImpl() {
  ResponseHeaders* headers = driver()->mutable_response_headers();

  // This is something of a workaround, see comments in
  // PushPreloadFilterTest.WeirdTiming.
  if (headers == nullptr) {
    return;
  }

  const Dependencies* deps = driver()->dependency_tracker()->read_in_info();
  CHECK(deps != nullptr) << "DetermineEnabled should have prevented this";

  // Sort dependencies by order_key. Fonts and modules are stored in separate
  // fields (see dependencies.proto), so merge them in before sorting; their
  // order keys were assigned in the same key space as everything else, so the
  // sort restores document order --- fonts right after their parent
  // stylesheet, modules wherever their script element sat.
  std::vector<Dependency> ordered_deps;
  for (int i = 0, n = deps->dependency_size(); i < n; ++i) {
    ordered_deps.push_back(deps->dependency(i));
  }
  for (int i = 0, n = deps->font_dependency_size(); i < n; ++i) {
    ordered_deps.push_back(deps->font_dependency(i));
  }
  for (int i = 0, n = deps->module_dependency_size(); i < n; ++i) {
    ordered_deps.push_back(deps->module_dependency(i));
  }
  DependencyOrderCompator dep_order;
  std::sort(ordered_deps.begin(), ordered_deps.end(), dep_order);

  std::unordered_set<GoogleString> already_seen;
  int font_preloads = 0;

  for (const Dependency& dep : ordered_deps) {
    GoogleUrl dep_url(dep.url());

    if (!dep_url.IsWebValid()) {
      continue;
    }

    // Enforce the font cap before the dedupe bookkeeping below, so
    // already_seen only ever contains URLs actually considered for emission.
    // The counter itself is bumped at emission, in the type switch.
    if (dep.content_type() == DEP_FONT && font_preloads >= kMaxFontPreloads) {
      continue;
    }

    if (!already_seen.insert(dep.url()).second) {
      // Skip dupes.
      continue;
    }

    // See if all the inputs are valid.
    int64 now_ms = driver()->timer()->NowMs();
    for (int i = 0; i < dep.validity_info_size(); ++i) {
      const InputInfo& input = dep.validity_info(i);
      bool purged_ignored, stale_rewrite_ignored;
      if (!input_info_utils::IsInputValid(server_context(), rewrite_options(),
                                          false /* not nested_rewriter*/, input,
                                          now_ms, &purged_ignored,
                                          &stale_rewrite_ignored)) {
        // Stop at first invalid entry, to avoid out-of-order hints.
        return;
      }
    }

    StringPiece rel_url =
        dep_url.Relativize(kAbsolutePath, driver()->google_url());

    // The link relation is per type: modules need rel=modulepreload, which is
    // the only relation the module map fetch consults.
    GoogleString link_val = StrCat("<", GoogleUrl::Sanitize(rel_url), ">");

    switch (dep.content_type()) {
      case DEP_JAVASCRIPT:
        StrAppend(&link_val, "; rel=preload; as=script");
        break;
      case DEP_CSS:
        StrAppend(&link_val, "; rel=preload; as=style");
        break;
      case DEP_FONT:
        ++font_preloads;  // The cap was already checked before dedupe.
        // Fonts are always fetched in anonymous CORS mode, even same-origin;
        // a preload without matching crossorigin would occupy a separate
        // cache entry and cause a double fetch --- worse than no hint.
        StrAppend(&link_val, "; rel=preload; as=font; crossorigin");
        break;
      case DEP_MODULE:
        // No as= (modulepreload's destination already defaults to script) and
        // no crossorigin (its default credentials mode already matches a
        // <script type=module> without the attribute; the collector skips the
        // use-credentials modules that would need one).
        StrAppend(&link_val, "; rel=modulepreload");
        break;
      default:
        LOG(DFATAL) << dep.content_type();
    }

    // We don't want pushes now, since we can't tell for sure when they're
    // a good idea.
    StrAppend(&link_val, "; nopush");

    headers->Add(HttpAttributes::kLink, link_val);
  }
}

void PushPreloadFilter::DetermineEnabled(GoogleString* disabled_reason) {
  if (driver()->dependency_tracker()->read_in_info() == nullptr) {
    set_is_enabled(false);
    *disabled_reason = "No push/preload candidates found in pcache";
  } else {
    set_is_enabled(true);
  }
}

}  // namespace net_instaweb
