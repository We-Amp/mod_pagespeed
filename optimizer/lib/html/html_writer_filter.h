// SPDX-License-Identifier: Apache-2.0
// GENERATED — DO NOT EDIT BY HAND. Synced by tools/sync-html-kernel.sh.
// Canonical source: mod_pagespeed 1.15, pagespeed/kernel/html/html_writer_filter.h (#1130).
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

#ifndef PAGESPEED_KERNEL_HTML_HTML_WRITER_FILTER_H_
#define PAGESPEED_KERNEL_HTML_HTML_WRITER_FILTER_H_

#include "lib/base/basictypes.h"
#include "lib/html/compat/string.h"
#include "lib/html/compat/string_util.h"
#include "lib/html/html_element.h"
#include "lib/html/html_filter.h"
#include "lib/html/html_name.h"
#include "lib/html/html_node.h"

namespace net_instaweb {

class HtmlParse;
class Writer;

// Filter that serializes HTML to a Writer stream.
class HtmlWriterFilter : public HtmlFilter {
 public:
  explicit HtmlWriterFilter(HtmlParse* html_parse);

  void set_writer(Writer* writer) { writer_ = writer; }
  ~HtmlWriterFilter() override;

  void StartDocument() override;
  void EndDocument() override;
  void StartElement(HtmlElement* element) override;
  void EndElement(HtmlElement* element) override;
  void Cdata(HtmlCdataNode* cdata) override;
  void Comment(HtmlCommentNode* comment) override;
  void IEDirective(HtmlIEDirectiveNode* directive) override;
  void Characters(HtmlCharactersNode* characters) override;
  void Directive(HtmlDirectiveNode* directive) override;
  void Flush() override;
  void DetermineEnabled(GoogleString* disabled_reason) override;
  // This filter will not change urls.
  bool CanModifyUrls() override { return false; }
  ScriptUsage GetScriptUsage() const override { return kNeverInjectsScripts; }

  void set_max_column(int max_column) { max_column_ = max_column; }
  void set_case_fold(bool case_fold) { case_fold_ = case_fold; }

  const char* Name() const override { return "HtmlWriter"; }

 protected:
  // Clear various variables for rewriting a new html file.
  virtual void Clear();

  Writer* writer() { return writer_; }

  // Terminates the current lazy close element if it is not already terminated.
  void TerminateLazyCloseElement();

 private:
  void EmitBytes(const StringPiece& str);

  // Emits an HTML name, possibly case-folded depending on the
  // caller-specified option.
  void EmitName(const HtmlName& name);

  HtmlElement::Style GetElementStyle(HtmlElement* element);

  // Escapes arbitrary text as HTML, e.g. turning & into &amp;.  If quoteChar
  // is non-zero, e.g. '"', then it would escape " as well.
  void EncodeBytes(const GoogleString& val, int quoteChar);

  HtmlParse* html_parse_;
  Writer* writer_;

  // Helps writer exploit shortcuts like <img .../> rather than writing
  // <img ...></img>.  At the end of StartElement, we defer writing the ">"
  // until we see what's coming next.  If it's the matching end_tag, then
  // we can emit />.  If something else comes first, then we have to
  // first emit the delayed ">" before continuing.
  HtmlElement* lazy_close_element_;

  int column_;
  int max_column_;
  int write_errors_;
  bool case_fold_;
  GoogleString case_fold_buffer_;

  HtmlWriterFilter(const HtmlWriterFilter&) = delete;
  HtmlWriterFilter& operator=(const HtmlWriterFilter&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_HTML_HTML_WRITER_FILTER_H_
