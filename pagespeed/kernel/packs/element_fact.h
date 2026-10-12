// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_ELEMENT_FACT_H_
#define PAGESPEED_KERNEL_PACKS_ELEMENT_FACT_H_

#include <cstdint>
#include <string>
#include <vector>

namespace net_instaweb {
class HtmlCharactersNode;
class HtmlElement;
}  // namespace net_instaweb

namespace pagespeed::packs {

// How the filter reads the text content of an element a kind observed.
enum class TextMode : std::uint8_t {
  kNone,     // the element has no text the kind cares about
  kDecoded,  // `value` = the entity-decoded text (title)
  kRaw,      // `value` = the text as written (JSON-LD), capped
};

// One element of a rule's kind found in the document (never inside svg, math,
// template, noscript, noembed or noframes). The planner treats the element
// pointers as opaque handles; only the filter dereferences them, and only
// while they are in the current flush window.
struct ElementFact {
  net_instaweb::HtmlElement* element = nullptr;
  bool in_head = false;
  // The element was first seen after the kind's decision point (for example a
  // canonical link inside <body> when the head was already decided).
  bool late = false;
  // canonical: the decoded href; title: the decoded text; description: the
  // decoded content. Not normalized.
  std::string value;
  // canonical: href present; description: content present.
  bool has_value_attr = false;
  // canonical only: the rel tokens other than "canonical", as written.
  std::vector<std::string> other_rel_tokens;
  // title, jsonld: the text nodes that make up its content.
  std::vector<net_instaweb::HtmlCharactersNode*> text_nodes;
  // Set by the kind when it observes the element: whether, and how, the
  // filter collects the element's text into `value`.
  TextMode text_mode = TextMode::kNone;
  // hreflang: the hreflang attribute, decoded and not normalized (`value`
  // holds the href).
  std::string code;
  // The text is too large to be read (kMaxJsonLdScanBytes); `value` is empty
  // then and the planner leaves the element alone.
  bool unreadable = false;
};

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_ELEMENT_FACT_H_
