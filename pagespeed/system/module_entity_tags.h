// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// The entity tags this module mints itself, and the request rule that keeps
// them away from every upstream.
//
// The module answers some requests with a weak validator of its own: W/"psh-
// ..." for an HTML variant and W/"psm-..." for the Markdown copy of a page.
// No origin issued those tags, so a client conditional that carries one must
// never be evaluated by another handler or forwarded upstream: a chained
// server running this module could confirm a tag the outer one minted, and
// any other origin would answer for a validator it never sent.  Every port
// therefore runs StashClientConditionals() on every request, before any
// handler that evaluates or forwards conditionals, and when it returns true
// removes If-None-Match and If-Modified-Since from the request.  The stash
// keeps the fields as they arrived, so this module can still answer a
// conditional for its own copy.
//
// The rule is the module's own and needs no optimizer library.  Free of any
// server's headers, so it is unit-tested on its own.

#ifndef PAGESPEED_SYSTEM_MODULE_ENTITY_TAGS_H_
#define PAGESPEED_SYSTEM_MODULE_ENTITY_TAGS_H_

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// The opaque-tag prefixes (inside the quotes) of the tags this module mints.
extern const char kModuleHtmlEntityTagPrefix[];      // "psh-"
extern const char kModuleMarkdownEntityTagPrefix[];  // "psm-"

// True when any If-None-Match field line names an entity tag this module
// mints: a list member, weak (W/) or strong, whose opaque-tag starts with
// "psh-" or "psm-".  Empty list members and OWS are skipped (RFC 9110 5.6.1);
// `*` is the client's own condition and names no tag.  A line that is not a
// well-formed list counts as carrying a module tag when "psh-" or "psm-"
// appears anywhere in it, so a malformed spelling can never carry one past
// the rule.  The prefixes are matched exactly, as the tags are compared
// (RFC 9110 8.8.3.2).
bool ConditionalsCarryModuleTag(const StringPieceVector& if_none_match_lines);

// The client's conditionals as they arrived, kept per request by the port.
// Field names match DaemonHtmlRequestFacts, which reads them.
struct ClientConditionalStash {
  // Every If-None-Match line, joined with ", ".
  GoogleString if_none_match;
  bool has_if_none_match = false;
  // Every If-Modified-Since line, joined with ", ".
  GoogleString if_modified_since;
  bool has_if_modified_since = false;
  // The port removed both fields from the request.
  bool stripped = false;
};

// Fills `stash` from the request's field lines (an absent field has no
// lines) and returns true when the port must remove every If-None-Match and
// If-Modified-Since line from the request: when If-None-Match carries a tag
// this module mints.  Otherwise the request is left exactly as it arrived.
bool StashClientConditionals(const StringPieceVector& if_none_match_lines,
                             const StringPieceVector& if_modified_since_lines,
                             ClientConditionalStash* stash);

// Copies the stash into a request-facts struct with the same field names
// (DaemonHtmlRequestFacts).
template <typename Facts>
void CopyClientConditionals(const ClientConditionalStash& stash, Facts* facts) {
  facts->if_none_match = stash.if_none_match;
  facts->has_if_none_match = stash.has_if_none_match;
  facts->if_modified_since = stash.if_modified_since;
  facts->has_if_modified_since = stash.has_if_modified_since;
}

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_MODULE_ENTITY_TAGS_H_
