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

#include "pagespeed/kernel/html/elide_attributes_filter.h"

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/html/html_parse_test_base.h"

namespace net_instaweb {

class ElideAttributesFilterTest : public HtmlParseTestBase {
 protected:
  ElideAttributesFilterTest() : elide_attributes_filter_(&html_parse_) {
    html_parse_.AddFilter(&elide_attributes_filter_);
  }

  bool AddBody() const override { return false; }

 private:
  ElideAttributesFilter elide_attributes_filter_;

  ElideAttributesFilterTest(const ElideAttributesFilterTest&) = delete;
  ElideAttributesFilterTest& operator=(const ElideAttributesFilterTest&) = delete;
};

TEST_F(ElideAttributesFilterTest, NoChanges) {
  ValidateNoChanges("no_changes",
                    "<head><script src=\"foo.js\"></script></head>"
                    "<body><form method=\"post\">"
                    "<input type=\"checkbox\" checked>"
                    "</form></body>");
}

TEST_F(ElideAttributesFilterTest, RemoveAttrWithDefaultValue) {
  ValidateExpected("remove_attr_with_default_value",
                   "<head></head><body><form method=get></form></body>",
                   "<head></head><body><form></form></body>");
}

TEST_F(ElideAttributesFilterTest, RemoveValueFromAttr) {
  SetDoctype(
      "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\" "
      "\"http://www.w3.org/TR/html4/strict.dtd\">");
  ValidateExpected("remove_value_from_attr",
                   "<head></head><body><form>"
                   "<input type=checkbox checked=checked></form></body>",
                   "<head></head><body><form>"
                   "<input type=checkbox checked></form></body>");
}

TEST_F(ElideAttributesFilterTest, DoNotRemoveValueFromAttrInXhtml) {
  SetDoctype(kXhtmlDtd);
  ValidateNoChanges("do_not_remove_value_from_attr_in_xhtml",
                    "<head></head><body><form>"
                    "<input type=checkbox checked=checked></form></body>");
}

TEST_F(ElideAttributesFilterTest, DoNotBreakVBScript) {
  SetDoctype("<!doctype html>");
  ValidateExpected("do_not_break_vbscript",
                   "<head><script language=\"JavaScript\">var x=1;</script>"
                   "<script language=\"VBScript\">"
                   "Sub foo(ByVal bar)\n  call baz(bar)\nend sub"
                   "</script></head><body></body>",
                   // Remove language="JavaScript", but not the VBScript one:
                   "<head><script>var x=1;</script>"
                   "<script language=\"VBScript\">"
                   "Sub foo(ByVal bar)\n  call baz(bar)\nend sub"
                   "</script></head><body></body>");
}

TEST_F(ElideAttributesFilterTest, RemoveScriptTypeInHtml5) {
  SetDoctype("<!doctype html>");
  ValidateExpected("remove_script_type_in_html_5",
                   "<head><script src=\"foo.js\" type=\"text/javascript\">"
                   "</script></head><body></body>",
                   "<head><script src=\"foo.js\">"
                   "</script></head><body></body>");
}

// See http://github.com/apache/incubator-pagespeed-mod/issues/59
TEST_F(ElideAttributesFilterTest, DoNotRemoveScriptTypeInHtml4) {
  SetDoctype(
      "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\" "
      "\"http://www.w3.org/TR/html4/strict.dtd\">");
  ValidateNoChanges("do_not_remove_script_type_in_html_4",
                    "<head><script src=\"foo.js\" type=\"text/javascript\">"
                    "</script></head><body></body>");
}

// Wordpress uses CSS selectors on type=text attributes in inputs, so don't
// remove it.
TEST_F(ElideAttributesFilterTest, DoNotRemoveTypeAttribute) {
  SetDoctype(
      "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\" "
      "\"http://www.w3.org/TR/html4/strict.dtd\">");
  ValidateNoChanges("do_not_remove_type_attribute_from_input",
                    "<head></head><body><form>"
                    "<input type=text></form></body>");
}

// loading=eager, decoding=auto, and fetchpriority=auto are the HTML5
// defaults for <img>, so they can be elided.
TEST_F(ElideAttributesFilterTest, RemoveModernImgDefaults) {
  SetDoctype("<!doctype html>");
  ValidateExpected("remove_modern_img_defaults",
                   "<head></head><body>"
                   "<img src=\"a.png\" loading=\"eager\" decoding=\"auto\" "
                   "fetchpriority=\"auto\"></body>",
                   "<head></head><body><img src=\"a.png\"></body>");
}

// fetchpriority=auto is the HTML5 default for <script> and <link>, so it
// can be elided.
TEST_F(ElideAttributesFilterTest, RemoveModernFetchpriorityDefaults) {
  SetDoctype("<!doctype html>");
  ValidateExpected("remove_modern_fetchpriority_defaults",
                   "<head><link rel=\"stylesheet\" href=\"a.css\" "
                   "fetchpriority=\"auto\">"
                   "<script src=\"a.js\" fetchpriority=\"auto\"></script>"
                   "</head><body></body>",
                   "<head><link rel=\"stylesheet\" href=\"a.css\">"
                   "<script src=\"a.js\"></script>"
                   "</head><body></body>");
}

// media=all is the HTML5 default for <link>.
TEST_F(ElideAttributesFilterTest, RemoveLinkMediaAllInHtml5) {
  SetDoctype("<!doctype html>");
  ValidateExpected("remove_link_media_all_in_html5",
                   "<head><link rel=\"stylesheet\" media=\"all\" "
                   "href=\"a.css\"></head><body></body>",
                   "<head><link rel=\"stylesheet\" href=\"a.css\">"
                   "</head><body></body>");
}

// media's HTML 4 default is "screen", not "all" (see
// elide_attributes_filter.cc), so media=all must be kept there.
TEST_F(ElideAttributesFilterTest, DoNotRemoveLinkMediaInHtml4) {
  SetDoctype(
      "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\" "
      "\"http://www.w3.org/TR/html4/strict.dtd\">");
  ValidateNoChanges("do_not_remove_link_media_in_html4",
                    "<head><link rel=\"stylesheet\" media=\"all\" "
                    "href=\"a.css\"></head><body></body>");
}

// loading does not exist in HTML 4, so loading=eager must be kept
// there.  img decoding=auto and fetchpriority=auto (and the script/link
// fetchpriority entries) share the same requires_version_5 gate, so one
// representative suffices.
TEST_F(ElideAttributesFilterTest, DoNotRemoveImgLoadingInHtml4) {
  SetDoctype(
      "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\" "
      "\"http://www.w3.org/TR/html4/strict.dtd\">");
  ValidateNoChanges("do_not_remove_img_loading_in_html4",
                    "<head></head><body>"
                    "<img src=\"a.png\" loading=\"eager\"></body>");
}

// dialog open, fieldset disabled, iframe allowfullscreen, and video
// playsinline are boolean attributes per the HTML living standard.
TEST_F(ElideAttributesFilterTest, RemoveValueFromModernBooleanAttrs) {
  // Boolean-attribute value stripping is not gated on the doctype version
  // (only XHTML disables it); the doctype is set here for consistency with
  // the neighboring tests and does not affect the outcome.
  SetDoctype("<!doctype html>");
  ValidateExpected("remove_value_from_modern_boolean_attrs",
                   "<head></head><body>"
                   "<dialog open=\"open\"></dialog>"
                   "<fieldset disabled=\"disabled\"></fieldset>"
                   "<iframe allowfullscreen=\"allowfullscreen\"></iframe>"
                   "<video playsinline=\"playsinline\"></video>"
                   "</body>",
                   "<head></head><body>"
                   "<dialog open></dialog>"
                   "<fieldset disabled></fieldset>"
                   "<iframe allowfullscreen></iframe>"
                   "<video playsinline></video>"
                   "</body>");
}

// <command>, <keygen>, area nohref, and style scoped were dropped from
// the HTML standard, so the filter must no longer know anything about
// them.
TEST_F(ElideAttributesFilterTest, DoNotElideDeadMarkup) {
  SetDoctype("<!doctype html>");
  ValidateNoChanges("do_not_elide_dead_markup",
                    "<head></head><body>"
                    "<command type=\"command\">"
                    "<keygen keytype=\"rsa\">"
                    "<area nohref=\"nohref\">"
                    "<style scoped=\"scoped\"></style>"
                    "</body>");
}

// hidden is an enumerated attribute (hidden=until-found), not a boolean
// one, so its value must never be stripped.
TEST_F(ElideAttributesFilterTest, DoNotElideHidden) {
  SetDoctype("<!doctype html>");
  ValidateNoChanges("do_not_elide_hidden",
                    "<head></head><body>"
                    "<div hidden=\"hidden\"></div>"
                    "<div hidden=\"until-found\"></div>"
                    "</body>");
}

// Non-default values of modern attributes must be kept.
TEST_F(ElideAttributesFilterTest, KeepNonDefaultModernValues) {
  SetDoctype("<!doctype html>");
  ValidateNoChanges("keep_non_default_modern_values",
                    "<head></head><body>"
                    "<img src=\"a.png\" loading=\"lazy\" decoding=\"async\" "
                    "fetchpriority=\"high\"></body>");
}

}  // namespace net_instaweb
