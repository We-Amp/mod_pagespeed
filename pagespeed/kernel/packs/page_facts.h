// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_PAGE_FACTS_H_
#define PAGESPEED_KERNEL_PACKS_PAGE_FACTS_H_

#include <array>
#include <string>
#include <vector>

#include "pagespeed/kernel/packs/element_fact.h"
#include "pagespeed/kernel/packs/kind_registry.h"
#include "pagespeed/kernel/packs/pack.h"

namespace pagespeed::packs {

// What the traversal collected. Filled in StartElement/EndElement/Characters,
// read at the decision points.
struct PageFacts {
  net_instaweb::HtmlElement* head = nullptr;  // first explicit <head>
  // The first <html> and <body> elements: where a kind that decides at the
  // end of the document appends its element.
  net_instaweb::HtmlElement* html = nullptr;
  net_instaweb::HtmlElement* body = nullptr;
  // Where the logical <head> ends when the document does not close it with
  // </head>: the first element that cannot be head content (usually <body>).
  // Inserts go before it. Null when </head> (or the end of the document)
  // closes the head.
  net_instaweb::HtmlElement* insert_before = nullptr;
  // The head cannot be edited safely (an inert element such as <svg> is
  // still open where the head ends).
  bool malformed_head = false;
  std::string base_href;  // first <base href>, decoded

  // The elements of each kind, indexed by KindIndex(). Which elements belong
  // to a kind is that kind's business: canonical links, titles, descriptions,
  // alternate-language links, JSON-LD scripts.
  std::array<std::vector<ElementFact>, kKindCount> by_kind;

  const std::vector<ElementFact>& For(const RuleKind* kind) const {
    return by_kind[KindIndex(kind)];
  }
  std::vector<ElementFact>& For(const RuleKind* kind) {
    return by_kind[KindIndex(kind)];
  }
};

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_PAGE_FACTS_H_
