// SPDX-License-Identifier: Apache-2.0
// GENERATED — DO NOT EDIT BY HAND. Synced by tools/sync-html-kernel.sh.
// Canonical source: mod_pagespeed 1.15, pagespeed/kernel/html/html_node.cc (#1130).
// Synced from the commit pinned in lib/html/HTML_KERNEL_PIN.
// Drift guard: CI re-runs tools/sync-html-kernel.sh and byte-compares.
// The canonical file's original license header is retained in full below;
// Apache-2.0 headed files: see lib/html/LICENSE.apache-2.0 for the license text.

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

#include "lib/html/html_node.h"

#include "lib/html/html_element.h"
#include "lib/html/html_event.h"

namespace net_instaweb {

HtmlNode::~HtmlNode() {}

HtmlLeafNode::HtmlLeafNode(HtmlElement* parent,
                           const HtmlEventListIterator& iter,
                           const StringPiece& contents)
    : HtmlNode(parent), data_(std::make_unique<Data>(iter, contents)) {}

HtmlLeafNode::~HtmlLeafNode() {}

GoogleString HtmlLeafNode::ToString() const {
  HtmlEvent* event = *begin();
  return event->ToString();
}

void HtmlLeafNode::MarkAsDead(const HtmlEventListIterator& end) {
  if (data_.get() != nullptr) {
    set_iter(end);
    data_->is_live_ = false;
  }
}

HtmlCdataNode::~HtmlCdataNode() {}

void HtmlCdataNode::SynthesizeEvents(const HtmlEventListIterator& iter,
                                     HtmlEventList* queue) {
  // We use -1 as a bogus line number, since the event is synthetic.
  HtmlCdataEvent* event = new HtmlCdataEvent(this, -1);
  set_iter(queue->insert(iter, event));
}

HtmlCharactersNode::~HtmlCharactersNode() {}

void HtmlCharactersNode::SynthesizeEvents(const HtmlEventListIterator& iter,
                                          HtmlEventList* queue) {
  // We use -1 as a bogus line number, since the event is synthetic.
  HtmlCharactersEvent* event = new HtmlCharactersEvent(this, -1);
  set_iter(queue->insert(iter, event));
}

HtmlCommentNode::~HtmlCommentNode() {}

void HtmlCommentNode::SynthesizeEvents(const HtmlEventListIterator& iter,
                                       HtmlEventList* queue) {
  // We use -1 as a bogus line number, since the event is synthetic.
  HtmlCommentEvent* event = new HtmlCommentEvent(this, -1);
  set_iter(queue->insert(iter, event));
}

HtmlIEDirectiveNode::~HtmlIEDirectiveNode() {}

void HtmlIEDirectiveNode::SynthesizeEvents(const HtmlEventListIterator& iter,
                                           HtmlEventList* queue) {
  // We use -1 as a bogus line number, since the event is synthetic.
  HtmlIEDirectiveEvent* event = new HtmlIEDirectiveEvent(this, -1);
  set_iter(queue->insert(iter, event));
}

HtmlDirectiveNode::~HtmlDirectiveNode() {}

void HtmlDirectiveNode::SynthesizeEvents(const HtmlEventListIterator& iter,
                                         HtmlEventList* queue) {
  // We use -1 as a bogus line number, since the event is synthetic.
  HtmlDirectiveEvent* event = new HtmlDirectiveEvent(this, -1);
  set_iter(queue->insert(iter, event));
}

}  // namespace net_instaweb
