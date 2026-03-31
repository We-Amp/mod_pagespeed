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

#ifndef PAGESPEED_KERNEL_HTML_HTML_EVENT_H_
#define PAGESPEED_KERNEL_HTML_HTML_EVENT_H_

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_filter.h"
#include "pagespeed/kernel/html/html_node.h"

namespace net_instaweb {

class HtmlEvent {
 public:
  explicit HtmlEvent(int line_number) : line_number_(line_number) {}
  virtual ~HtmlEvent();
  virtual void Run(HtmlFilter* filter) = 0;
  virtual GoogleString ToString() const = 0;

  // If this is a StartElement event, returns the HtmlElement that is being
  // started.  Otherwise returns NULL.
  virtual HtmlElement* GetElementIfStartEvent() { return NULL; }

  // If this is an EndElement event, returns the HtmlElement that is being
  // ended.  Otherwise returns NULL.
  virtual HtmlElement* GetElementIfEndEvent() { return NULL; }

  virtual HtmlLeafNode* GetLeafNode() { return NULL; }
  virtual HtmlNode* GetNode() { return NULL; }
  virtual HtmlCharactersNode* GetCharactersNode() { return NULL; }
  void DebugPrint();

  int line_number() const { return line_number_; }

 private:
  int line_number_;

  HtmlEvent(const HtmlEvent&) = delete;
  HtmlEvent& operator=(const HtmlEvent&) = delete;
};

class HtmlStartDocumentEvent : public HtmlEvent {
 public:
  explicit HtmlStartDocumentEvent(int line_number) : HtmlEvent(line_number) {}
  void Run(HtmlFilter* filter) override { filter->StartDocument(); }
  GoogleString ToString() const override { return "StartDocument"; }

 private:
  HtmlStartDocumentEvent(const HtmlStartDocumentEvent&) = delete;
  HtmlStartDocumentEvent& operator=(const HtmlStartDocumentEvent&) = delete;
};

class HtmlEndDocumentEvent : public HtmlEvent {
 public:
  explicit HtmlEndDocumentEvent(int line_number) : HtmlEvent(line_number) {}
  void Run(HtmlFilter* filter) override { filter->EndDocument(); }
  GoogleString ToString() const override { return "EndDocument"; }

 private:
  HtmlEndDocumentEvent(const HtmlEndDocumentEvent&) = delete;
  HtmlEndDocumentEvent& operator=(const HtmlEndDocumentEvent&) = delete;
};

class HtmlStartElementEvent : public HtmlEvent {
 public:
  HtmlStartElementEvent(HtmlElement* element, int line_number)
      : HtmlEvent(line_number), element_(element) {}
  void Run(HtmlFilter* filter) override { filter->StartElement(element_); }
  GoogleString ToString() const override {
    return StrCat("StartElement ", element_->ToString());
  }
  HtmlElement* GetElementIfStartEvent() override { return element_; }
  HtmlElement* GetNode() override { return element_; }

 private:
  HtmlElement* element_;

  HtmlStartElementEvent(const HtmlStartElementEvent&) = delete;
  HtmlStartElementEvent& operator=(const HtmlStartElementEvent&) = delete;
};

class HtmlEndElementEvent : public HtmlEvent {
 public:
  HtmlEndElementEvent(HtmlElement* element, int line_number)
      : HtmlEvent(line_number), element_(element) {}
  void Run(HtmlFilter* filter) override { filter->EndElement(element_); }
  GoogleString ToString() const override {
    return StrCat("EndElement ", element_->ToString());
  }
  HtmlElement* GetElementIfEndEvent() override { return element_; }
  HtmlElement* GetNode() override { return element_; }

 private:
  HtmlElement* element_;

  HtmlEndElementEvent(const HtmlEndElementEvent&) = delete;
  HtmlEndElementEvent& operator=(const HtmlEndElementEvent&) = delete;
};

class HtmlLeafNodeEvent : public HtmlEvent {
 public:
  explicit HtmlLeafNodeEvent(int line_number) : HtmlEvent(line_number) {}
  HtmlNode* GetNode() override { return GetLeafNode(); }

 private:
  HtmlLeafNodeEvent(const HtmlLeafNodeEvent&) = delete;
  HtmlLeafNodeEvent& operator=(const HtmlLeafNodeEvent&) = delete;
};

class HtmlIEDirectiveEvent : public HtmlLeafNodeEvent {
 public:
  HtmlIEDirectiveEvent(HtmlIEDirectiveNode* directive, int line_number)
      : HtmlLeafNodeEvent(line_number), directive_(directive) {}
  void Run(HtmlFilter* filter) override { filter->IEDirective(directive_); }
  GoogleString ToString() const override {
    return StrCat("IEDirective ", directive_->contents());
  }
  HtmlLeafNode* GetLeafNode() override { return directive_; }

 private:
  HtmlIEDirectiveNode* directive_;

  HtmlIEDirectiveEvent(const HtmlIEDirectiveEvent&) = delete;
  HtmlIEDirectiveEvent& operator=(const HtmlIEDirectiveEvent&) = delete;
};

class HtmlCdataEvent : public HtmlLeafNodeEvent {
 public:
  HtmlCdataEvent(HtmlCdataNode* cdata, int line_number)
      : HtmlLeafNodeEvent(line_number), cdata_(cdata) {}
  void Run(HtmlFilter* filter) override { filter->Cdata(cdata_); }
  GoogleString ToString() const override {
    return StrCat("Cdata ", cdata_->contents());
  }
  HtmlLeafNode* GetLeafNode() override { return cdata_; }

 private:
  HtmlCdataNode* cdata_;

  HtmlCdataEvent(const HtmlCdataEvent&) = delete;
  HtmlCdataEvent& operator=(const HtmlCdataEvent&) = delete;
};

class HtmlCommentEvent : public HtmlLeafNodeEvent {
 public:
  HtmlCommentEvent(HtmlCommentNode* comment, int line_number)
      : HtmlLeafNodeEvent(line_number), comment_(comment) {}
  void Run(HtmlFilter* filter) override { filter->Comment(comment_); }
  GoogleString ToString() const override {
    return StrCat("Comment ", comment_->contents());
  }
  HtmlLeafNode* GetLeafNode() override { return comment_; }

 private:
  HtmlCommentNode* comment_;

  HtmlCommentEvent(const HtmlCommentEvent&) = delete;
  HtmlCommentEvent& operator=(const HtmlCommentEvent&) = delete;
};

class HtmlCharactersEvent : public HtmlLeafNodeEvent {
 public:
  HtmlCharactersEvent(HtmlCharactersNode* characters, int line_number)
      : HtmlLeafNodeEvent(line_number), characters_(characters) {}
  void Run(HtmlFilter* filter) override { filter->Characters(characters_); }
  GoogleString ToString() const override {
    return StrCat("Characters ", characters_->contents());
  }
  HtmlLeafNode* GetLeafNode() override { return characters_; }
  HtmlCharactersNode* GetCharactersNode() override { return characters_; }

 private:
  HtmlCharactersNode* characters_;

  HtmlCharactersEvent(const HtmlCharactersEvent&) = delete;
  HtmlCharactersEvent& operator=(const HtmlCharactersEvent&) = delete;
};

class HtmlDirectiveEvent : public HtmlLeafNodeEvent {
 public:
  HtmlDirectiveEvent(HtmlDirectiveNode* directive, int line_number)
      : HtmlLeafNodeEvent(line_number), directive_(directive) {}
  void Run(HtmlFilter* filter) override { filter->Directive(directive_); }
  GoogleString ToString() const override {
    return StrCat("Directive: ", directive_->contents());
  }
  HtmlLeafNode* GetLeafNode() override { return directive_; }

 private:
  HtmlDirectiveNode* directive_;

  HtmlDirectiveEvent(const HtmlDirectiveEvent&) = delete;
  HtmlDirectiveEvent& operator=(const HtmlDirectiveEvent&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_HTML_HTML_EVENT_H_
