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

#ifndef PAGESPEED_KERNEL_HTML_HTML_TESTING_PEER_H_
#define PAGESPEED_KERNEL_HTML_HTML_TESTING_PEER_H_

#include <cstddef>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_lexer.h"
#include "pagespeed/kernel/html/html_node.h"
#include "pagespeed/kernel/html/html_parse.h"

namespace net_instaweb {

class HtmlEvent;

class HtmlTestingPeer {
 public:
  HtmlTestingPeer() {}

  static void SetNodeParent(HtmlNode* node, HtmlElement* parent) {
    node->set_parent(parent);
  }
  static void AddEvent(HtmlParse* parser, HtmlEvent* event) {
    parser->AddEvent(event);
  }
  static void SetCurrent(HtmlParse* parser, HtmlNode* node) {
    parser->SetCurrent(node);
  }
  static void set_coalesce_characters(HtmlParse* parser, bool x) {
    parser->set_coalesce_characters(x);
  }
  static size_t symbol_table_size(HtmlParse* parser) {
    return parser->symbol_table_size();
  }
  static void set_buffer_events(HtmlParse* parse, bool value) {
    parse->set_buffer_events(value);
  }

  // Access to the parse session's lexer (HtmlParse friended this peer).
  static HtmlLexer* GetLexer(HtmlParse* parse) { return parse->lexer_.get(); }

  // Forces the empty-literal invariant violation that HtmlLexer::Restart()'s
  // release-mode guard protects against: Parse() normally guarantees
  // literal_ is non-empty when Restart runs, but if that is ever violated a
  // release build (asserts compiled out) would otherwise compute
  // literal_.resize(literal_.size() - 1) as resize(SIZE_MAX).
  static void RestartWithEmptyLiteral(HtmlLexer* lexer, char c) {
    lexer->literal_.clear();
    lexer->Restart(c);
  }

  // Line-number setters for covering the partial-line-number branches of
  // HtmlElement::ToString().
  static void SetBeginLineNumber(HtmlElement* element, int line) {
    element->set_begin_line_number(line);
  }
  static void SetEndLineNumber(HtmlElement* element, int line) {
    element->set_end_line_number(line);
  }
  static int MaxLineNumber() { return HtmlElement::Data::kMaxLineNumber; }
  // Reports whether a leaf node is still holding its Data buffer, i.e.
  // FreeData() has not been called on it (nor has it been destroyed).
  static bool LeafNodeHasData(const HtmlLeafNode* node) {
    return node->data_.get() != nullptr;
  }

 private:
  HtmlTestingPeer(const HtmlTestingPeer&) = delete;
  HtmlTestingPeer& operator=(const HtmlTestingPeer&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_HTML_HTML_TESTING_PEER_H_
