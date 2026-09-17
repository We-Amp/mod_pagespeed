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

// Unit tests for HtmlElement: attribute handling (with focus on valueless
// attributes such as <input disabled> whose escaped value is nullptr) and
// ToString serialization.  The attribute-manipulation and ToString cases
// are ported from pagespeed-optimizer's test/lib/html/html_element_test.cc;
// the ToString partial-line-number cases were dropped in the
// vendoring migration because canonical granted no test peer access to the
// line-number setters (now provided via HtmlTestingPeer).

#include "pagespeed/kernel/html/html_element.h"

#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/html/html_testing_peer.h"

namespace net_instaweb {

class HtmlElementTest : public testing::Test {
 protected:
  HtmlElementTest()
      : message_handler_(new NullMutex), html_parse_(&message_handler_) {}

  void SetUp() override { HtmlKeywords::Init(); }

  // Create an element with a known keyword tag.
  HtmlElement* NewElement(HtmlName::Keyword keyword) {
    return html_parse_.NewElement(nullptr, keyword);
  }

  // Create an element with a custom tag name.
  HtmlElement* NewElement(const StringPiece& name) {
    return html_parse_.NewElement(nullptr, name);
  }

  // Add an attribute via the HtmlParse convenience method (DOUBLE_QUOTE).
  void AddAttribute(HtmlElement* element, HtmlName::Keyword keyword,
                    const StringPiece& value) {
    html_parse_.AddAttribute(element, keyword, value);
  }

  // Adds a valueless attribute (null escaped value, no quotes) and returns it.
  HtmlElement::Attribute* AddValuelessAttribute(HtmlElement* element,
                                                HtmlName::Keyword keyword) {
    element->AddEscapedAttribute(html_parse_.MakeName(keyword), StringPiece(),
                                 HtmlElement::NO_QUOTE);
    return element->FindAttribute(keyword);
  }

  MockMessageHandler message_handler_;
  HtmlParse html_parse_;
};

TEST_F(HtmlElementTest, ValuelessAttributeDecodedValue) {
  // Computing the decoded value of a valueless attribute must not
  // construct a StringPiece from a null const char* (undefined behavior).
  HtmlElement* input = NewElement(HtmlName::kInput);
  const HtmlElement::Attribute* attr =
      AddValuelessAttribute(input, HtmlName::kDisabled);
  ASSERT_NE(nullptr, attr);
  EXPECT_EQ(nullptr, attr->escaped_value());
  EXPECT_EQ(nullptr, attr->DecodedValueOrNull());
  EXPECT_FALSE(attr->decoding_error());
}

TEST_F(HtmlElementTest, AddAttributeCopiesValuelessAttribute) {
  // Copying a valueless attribute with AddAttribute(const Attribute&) used
  // to construct a StringPiece from the null escaped value (undefined
  // behavior).
  HtmlElement* src = NewElement(HtmlName::kInput);
  const HtmlElement::Attribute* src_attr =
      AddValuelessAttribute(src, HtmlName::kDisabled);
  ASSERT_NE(nullptr, src_attr);

  HtmlElement* dst = NewElement(HtmlName::kInput);
  dst->AddAttribute(*src_attr);
  const HtmlElement::Attribute* dst_attr =
      dst->FindAttribute(HtmlName::kDisabled);
  ASSERT_NE(nullptr, dst_attr);
  EXPECT_EQ(nullptr, dst_attr->escaped_value());
  EXPECT_EQ(nullptr, dst_attr->DecodedValueOrNull());
  EXPECT_FALSE(dst_attr->decoding_error());
}

TEST_F(HtmlElementTest, SetValueOnValuelessAttribute) {
  // SetValue on a valueless attribute used to strlen(nullptr) in its
  // pointer-overlap DCHECK.
  HtmlElement* input = NewElement(HtmlName::kInput);
  HtmlElement::Attribute* attr =
      AddValuelessAttribute(input, HtmlName::kDisabled);
  ASSERT_NE(nullptr, attr);
  attr->SetValue("on");
  EXPECT_STREQ("on", attr->escaped_value());
  EXPECT_STREQ("on", attr->DecodedValueOrNull());
  EXPECT_FALSE(attr->decoding_error());
}

TEST_F(HtmlElementTest, SetValueKeepsDecodedStateConsistent) {
  // After SetValue, the attribute's decoded state must reflect the value
  // just set (not a lazy recomputation), and copying the attribute must
  // carry the decoded value across.
  HtmlElement* img = NewElement(HtmlName::kImg);
  html_parse_.AddAttribute(img, HtmlName::kSrc, "a&b");
  HtmlElement::Attribute* attr = img->FindAttribute(HtmlName::kSrc);
  ASSERT_NE(nullptr, attr);
  attr->SetValue("c&d");
  EXPECT_STREQ("c&amp;d", attr->escaped_value());
  EXPECT_STREQ("c&d", attr->DecodedValueOrNull());
  EXPECT_FALSE(attr->decoding_error());

  HtmlElement* img2 = NewElement(HtmlName::kImg);
  img2->AddAttribute(*attr);
  const HtmlElement::Attribute* copy = img2->FindAttribute(HtmlName::kSrc);
  ASSERT_NE(nullptr, copy);
  EXPECT_STREQ("c&amp;d", copy->escaped_value());
  EXPECT_STREQ("c&d", copy->DecodedValueOrNull());
  EXPECT_FALSE(copy->decoding_error());
}

// --- DeleteAttribute tests (ported from the optimizer's suite) ---

TEST_F(HtmlElementTest, DeleteAttributeByKeyword) {
  HtmlElement* div = NewElement(HtmlName::kDiv);
  AddAttribute(div, HtmlName::kId, "main");
  ASSERT_NE(nullptr, div->FindAttribute(HtmlName::kId));
  EXPECT_TRUE(div->DeleteAttribute(HtmlName::kId));
  EXPECT_EQ(nullptr, div->FindAttribute(HtmlName::kId));
}

TEST_F(HtmlElementTest, DeleteAttributeReturnsFalseWhenMissing) {
  HtmlElement* div = NewElement(HtmlName::kDiv);
  EXPECT_FALSE(div->DeleteAttribute(HtmlName::kId));
}

TEST_F(HtmlElementTest, DeleteAttributeByString) {
  HtmlElement* div = NewElement(HtmlName::kDiv);
  AddAttribute(div, HtmlName::kClass, "foo");
  EXPECT_TRUE(div->DeleteAttribute("class"));
  EXPECT_EQ(nullptr, div->FindAttribute(HtmlName::kClass));
}

TEST_F(HtmlElementTest, DeleteAttributeByStringMissing) {
  HtmlElement* div = NewElement(HtmlName::kDiv);
  EXPECT_FALSE(div->DeleteAttribute("nonexistent"));
}

TEST_F(HtmlElementTest, DeleteOneOfMultipleAttributes) {
  HtmlElement* div = NewElement(HtmlName::kDiv);
  AddAttribute(div, HtmlName::kId, "x");
  AddAttribute(div, HtmlName::kClass, "y");
  AddAttribute(div, HtmlName::kStyle, "z");

  EXPECT_TRUE(div->DeleteAttribute(HtmlName::kClass));
  EXPECT_EQ(nullptr, div->FindAttribute(HtmlName::kClass));
  EXPECT_NE(nullptr, div->FindAttribute(HtmlName::kId));
  EXPECT_NE(nullptr, div->FindAttribute(HtmlName::kStyle));
}

// --- AddAttribute tests ---

TEST_F(HtmlElementTest, AddAttributeWithDecodedValue) {
  HtmlElement* a = NewElement(HtmlName::kA);
  HtmlName href = html_parse_.MakeName(HtmlName::kHref);
  a->AddAttribute(href, "http://example.com/?a=1&b=2",
                  HtmlElement::DOUBLE_QUOTE);
  const HtmlElement::Attribute* attr = a->FindAttribute(HtmlName::kHref);
  ASSERT_NE(nullptr, attr);
  // Decoded value should match the original.
  EXPECT_STREQ("http://example.com/?a=1&b=2", attr->DecodedValueOrNull());
}

TEST_F(HtmlElementTest, AddEscapedAttribute) {
  HtmlElement* span = NewElement(HtmlName::kSpan);
  HtmlName data_attr = html_parse_.MakeName("data-val");
  span->AddEscapedAttribute(data_attr, "a&amp;b", HtmlElement::SINGLE_QUOTE);
  const HtmlElement::Attribute* attr = span->FindAttribute("data-val");
  ASSERT_NE(nullptr, attr);
  EXPECT_STREQ("a&amp;b", attr->escaped_value());
}

TEST_F(HtmlElementTest, AddAttributeCopiesFromAnother) {
  HtmlElement* src = NewElement(HtmlName::kDiv);
  AddAttribute(src, HtmlName::kId, "original");
  const HtmlElement::Attribute* src_attr = src->FindAttribute(HtmlName::kId);
  ASSERT_NE(nullptr, src_attr);

  HtmlElement* dst = NewElement(HtmlName::kSpan);
  dst->AddAttribute(*src_attr);
  const HtmlElement::Attribute* dst_attr = dst->FindAttribute(HtmlName::kId);
  ASSERT_NE(nullptr, dst_attr);
  EXPECT_STREQ("original", dst_attr->DecodedValueOrNull());
}

// --- FindAttribute tests ---

TEST_F(HtmlElementTest, FindAttributeByStringReturnsNullWhenMissing) {
  HtmlElement* div = NewElement(HtmlName::kDiv);
  EXPECT_EQ(nullptr, div->FindAttribute("nope"));
}

TEST_F(HtmlElementTest, HasAttribute) {
  HtmlElement* img = NewElement(HtmlName::kImg);
  AddAttribute(img, HtmlName::kSrc, "test.png");
  EXPECT_TRUE(img->HasAttribute(HtmlName::kSrc));
  EXPECT_FALSE(img->HasAttribute(HtmlName::kAlt));
}

TEST_F(HtmlElementTest, AttributeValue) {
  HtmlElement* img = NewElement(HtmlName::kImg);
  AddAttribute(img, HtmlName::kSrc, "test.png");
  EXPECT_STREQ("test.png", img->AttributeValue(HtmlName::kSrc));
  EXPECT_EQ(nullptr, img->AttributeValue(HtmlName::kAlt));
}

// --- ToString tests ---

TEST_F(HtmlElementTest, ToStringAutoClose) {
  HtmlElement* div = NewElement(HtmlName::kDiv);
  // Default style is AUTO_CLOSE.
  GoogleString s = div->ToString();
  EXPECT_NE(GoogleString::npos, s.find("<div"));
  EXPECT_NE(GoogleString::npos, s.find("not yet closed"));
}

TEST_F(HtmlElementTest, ToStringExplicitClose) {
  HtmlElement* div = NewElement(HtmlName::kDiv);
  div->set_style(HtmlElement::EXPLICIT_CLOSE);
  GoogleString s = div->ToString();
  EXPECT_NE(GoogleString::npos, s.find("</div>"));
}

TEST_F(HtmlElementTest, ToStringBriefClose) {
  HtmlElement* br = NewElement(HtmlName::kBr);
  br->set_style(HtmlElement::BRIEF_CLOSE);
  EXPECT_NE(GoogleString::npos, br->ToString().find("/>"));
}

TEST_F(HtmlElementTest, ToStringImplicitClose) {
  HtmlElement* li = NewElement(HtmlName::kLi);
  li->set_style(HtmlElement::IMPLICIT_CLOSE);
  GoogleString s = li->ToString();
  EXPECT_NE(GoogleString::npos, s.find("<li>"));
  // Should NOT contain closing tag or markers.
  EXPECT_EQ(GoogleString::npos, s.find("</li>"));
  EXPECT_EQ(GoogleString::npos, s.find("unclosed"));
  EXPECT_EQ(GoogleString::npos, s.find("not yet closed"));
}

TEST_F(HtmlElementTest, ToStringUnclosed) {
  HtmlElement* div = NewElement(HtmlName::kDiv);
  div->set_style(HtmlElement::UNCLOSED);
  EXPECT_NE(GoogleString::npos, div->ToString().find("unclosed"));
}

TEST_F(HtmlElementTest, ToStringInvisible) {
  HtmlElement* div = NewElement(HtmlName::kDiv);
  div->set_style(HtmlElement::INVISIBLE);
  EXPECT_NE(GoogleString::npos, div->ToString().find("invisible"));
}

TEST_F(HtmlElementTest, ToStringWithAttributes) {
  HtmlElement* a = NewElement(HtmlName::kA);
  AddAttribute(a, HtmlName::kHref, "http://test.com");
  a->set_style(HtmlElement::EXPLICIT_CLOSE);
  GoogleString s = a->ToString();
  EXPECT_NE(GoogleString::npos, s.find("href"));
  EXPECT_NE(GoogleString::npos, s.find("http://test.com"));
}

TEST_F(HtmlElementTest, ToStringNoLineNumbers) {
  // NewElement creates elements with kMaxLineNumber (no line info).
  // Verify ToString still works without line number output.
  HtmlElement* div = NewElement(HtmlName::kDiv);
  div->set_style(HtmlElement::EXPLICIT_CLOSE);
  GoogleString s = div->ToString();
  EXPECT_NE(GoogleString::npos, s.find("<div"));
  EXPECT_NE(GoogleString::npos, s.find("</div>"));
}

TEST_F(HtmlElementTest, ToStringCustomTagName) {
  HtmlElement* custom = NewElement("my-component");
  custom->set_style(HtmlElement::EXPLICIT_CLOSE);
  GoogleString s = custom->ToString();
  EXPECT_NE(GoogleString::npos, s.find("my-component"));
  EXPECT_NE(GoogleString::npos, s.find("</my-component>"));
}

// --- Attribute quote style ---

TEST_F(HtmlElementTest, QuoteStyleString) {
  HtmlElement* div = NewElement(HtmlName::kDiv);
  HtmlName id = html_parse_.MakeName(HtmlName::kId);

  div->AddAttribute(id, "test", HtmlElement::SINGLE_QUOTE);
  const HtmlElement::Attribute* attr = div->FindAttribute(HtmlName::kId);
  ASSERT_NE(nullptr, attr);
  EXPECT_EQ(HtmlElement::SINGLE_QUOTE, attr->quote_style());
  EXPECT_STREQ("'", attr->quote_str());
}

TEST_F(HtmlElementTest, QuoteStyleNone) {
  HtmlElement* div = NewElement(HtmlName::kDiv);
  HtmlName id = html_parse_.MakeName(HtmlName::kId);
  div->AddAttribute(id, "test", HtmlElement::NO_QUOTE);
  const HtmlElement::Attribute* attr = div->FindAttribute(HtmlName::kId);
  ASSERT_NE(nullptr, attr);
  EXPECT_STREQ("", attr->quote_str());
}

// --- Attribute mutation ---

TEST_F(HtmlElementTest, SetValueOnAttribute) {
  HtmlElement* img = NewElement(HtmlName::kImg);
  AddAttribute(img, HtmlName::kSrc, "old.png");
  HtmlElement::Attribute* attr = img->FindAttribute(HtmlName::kSrc);
  ASSERT_NE(nullptr, attr);
  attr->SetValue("new.png");
  EXPECT_STREQ("new.png", attr->DecodedValueOrNull());
}

// --- ToString partial line numbers (via HtmlTestingPeer) ---

TEST_F(HtmlElementTest, ToStringBeginLineOnly) {
  // Covers the ToString() branch where begin_line_number is set but
  // end_line_number is kMaxLineNumber (no end line info).
  HtmlElement* div = NewElement(HtmlName::kDiv);
  div->set_style(HtmlElement::EXPLICIT_CLOSE);
  HtmlTestingPeer::SetBeginLineNumber(div, 42);
  HtmlTestingPeer::SetEndLineNumber(div, HtmlTestingPeer::MaxLineNumber());
  GoogleString s = div->ToString();
  // Should contain the begin line number followed by "..." and no end
  // number: the format is " 42...".
  EXPECT_NE(GoogleString::npos, s.find("42"));
  EXPECT_NE(GoogleString::npos, s.find("..."));
  EXPECT_NE(GoogleString::npos, s.find(" 42..."));
}

TEST_F(HtmlElementTest, ToStringEndLineOnly) {
  // Covers the ToString() branch where end_line_number is set but
  // begin_line_number is kMaxLineNumber (no begin line info).
  HtmlElement* div = NewElement(HtmlName::kDiv);
  div->set_style(HtmlElement::EXPLICIT_CLOSE);
  HtmlTestingPeer::SetBeginLineNumber(div, HtmlTestingPeer::MaxLineNumber());
  HtmlTestingPeer::SetEndLineNumber(div, 99);
  GoogleString s = div->ToString();
  // Should contain " ...99" (no begin number, just end).
  EXPECT_NE(GoogleString::npos, s.find("...99"));
  EXPECT_EQ(GoogleString::npos, s.find("42"));
}

// --- Valueless attribute in ToString ---

TEST_F(HtmlElementTest, ToStringValuelessAttribute) {
  // Covers the ToString() else branch for an attribute with a name but no
  // value.
  HtmlElement* input = NewElement(HtmlName::kInput);
  input->set_style(HtmlElement::BRIEF_CLOSE);
  HtmlName disabled = html_parse_.MakeName(HtmlName::kDisabled);
  // Add attribute with no value (NO_QUOTE, empty value).
  input->AddEscapedAttribute(disabled, StringPiece(), HtmlElement::NO_QUOTE);
  GoogleString s = input->ToString();
  // Should contain " disabled" without "=" or a value.
  EXPECT_NE(GoogleString::npos, s.find(" disabled"));
  EXPECT_EQ(GoogleString::npos, s.find("disabled="));
}

}  // namespace net_instaweb
