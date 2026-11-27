// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for HtmlElement: attribute manipulation and ToString serialization.

#include "lib/html/html_element.h"

#include <string>

#include "gtest/gtest.h"
#include "lib/html/compat/message_handler.h"
#include "lib/html/html_keywords.h"
#include "lib/html/html_name.h"
#include "lib/html/html_parse.h"

namespace net_instaweb {

class HtmlElementTest : public testing::Test {
 protected:
  void SetUp() override {
    HtmlKeywords::Init();
    html_parse_ = std::make_unique<HtmlParse>(&message_handler_);
  }

  NullMessageHandler message_handler_;

  // Create an element with a known keyword tag.
  HtmlElement* NewElement(HtmlName::Keyword keyword) {
    return html_parse_->NewElement(nullptr, keyword);
  }

  // Create an element with a custom tag name.
  HtmlElement* NewElement(std::string_view name) {
    return html_parse_->NewElement(nullptr, name);
  }

  // Add an attribute via the HtmlParse convenience method (DOUBLE_QUOTE).
  void AddAttribute(HtmlElement* elem, HtmlName::Keyword keyword,
                    std::string_view value) {
    html_parse_->AddAttribute(elem, keyword, value);
  }

  std::unique_ptr<HtmlParse> html_parse_;
};

// --- DeleteAttribute tests ---

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
  HtmlName href = html_parse_->MakeName(HtmlName::kHref);
  a->AddAttribute(href, "http://example.com/?a=1&b=2",
                  HtmlElement::DOUBLE_QUOTE);
  const HtmlElement::Attribute* attr = a->FindAttribute(HtmlName::kHref);
  ASSERT_NE(nullptr, attr);
  // Decoded value should match the original.
  EXPECT_STREQ("http://example.com/?a=1&b=2", attr->DecodedValueOrNull());
}

TEST_F(HtmlElementTest, AddEscapedAttribute) {
  HtmlElement* span = NewElement(HtmlName::kSpan);
  HtmlName data_attr = html_parse_->MakeName("data-val");
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
  std::string s = div->ToString();
  EXPECT_NE(std::string::npos, s.find("<div"));
  EXPECT_NE(std::string::npos, s.find("not yet closed"));
}

TEST_F(HtmlElementTest, ToStringExplicitClose) {
  HtmlElement* div = NewElement(HtmlName::kDiv);
  div->set_style(HtmlElement::EXPLICIT_CLOSE);
  std::string s = div->ToString();
  EXPECT_NE(std::string::npos, s.find("</div>"));
}

TEST_F(HtmlElementTest, ToStringBriefClose) {
  HtmlElement* br = NewElement(HtmlName::kBr);
  br->set_style(HtmlElement::BRIEF_CLOSE);
  EXPECT_NE(std::string::npos, br->ToString().find("/>"));
}

TEST_F(HtmlElementTest, ToStringImplicitClose) {
  HtmlElement* li = NewElement(HtmlName::kLi);
  li->set_style(HtmlElement::IMPLICIT_CLOSE);
  std::string s = li->ToString();
  EXPECT_NE(std::string::npos, s.find("<li>"));
  // Should NOT contain closing tag or markers.
  EXPECT_EQ(std::string::npos, s.find("</li>"));
  EXPECT_EQ(std::string::npos, s.find("unclosed"));
  EXPECT_EQ(std::string::npos, s.find("not yet closed"));
}

TEST_F(HtmlElementTest, ToStringUnclosed) {
  HtmlElement* div = NewElement(HtmlName::kDiv);
  div->set_style(HtmlElement::UNCLOSED);
  EXPECT_NE(std::string::npos, div->ToString().find("unclosed"));
}

TEST_F(HtmlElementTest, ToStringInvisible) {
  HtmlElement* div = NewElement(HtmlName::kDiv);
  div->set_style(HtmlElement::INVISIBLE);
  EXPECT_NE(std::string::npos, div->ToString().find("invisible"));
}

TEST_F(HtmlElementTest, ToStringWithAttributes) {
  HtmlElement* a = NewElement(HtmlName::kA);
  AddAttribute(a, HtmlName::kHref, "http://test.com");
  a->set_style(HtmlElement::EXPLICIT_CLOSE);
  std::string s = a->ToString();
  EXPECT_NE(std::string::npos, s.find("href"));
  EXPECT_NE(std::string::npos, s.find("http://test.com"));
}

TEST_F(HtmlElementTest, ToStringNoLineNumbers) {
  // NewElement creates elements with kMaxLineNumber (no line info).
  // Verify ToString still works without line number output.
  HtmlElement* div = NewElement(HtmlName::kDiv);
  div->set_style(HtmlElement::EXPLICIT_CLOSE);
  std::string s = div->ToString();
  EXPECT_NE(std::string::npos, s.find("<div"));
  EXPECT_NE(std::string::npos, s.find("</div>"));
}

TEST_F(HtmlElementTest, ToStringCustomTagName) {
  HtmlElement* custom = NewElement("my-component");
  custom->set_style(HtmlElement::EXPLICIT_CLOSE);
  std::string s = custom->ToString();
  EXPECT_NE(std::string::npos, s.find("my-component"));
  EXPECT_NE(std::string::npos, s.find("</my-component>"));
}

// --- Attribute quote style ---

TEST_F(HtmlElementTest, QuoteStyleString) {
  HtmlElement* div = NewElement(HtmlName::kDiv);
  HtmlName id = html_parse_->MakeName(HtmlName::kId);

  div->AddAttribute(id, "test", HtmlElement::SINGLE_QUOTE);
  const HtmlElement::Attribute* attr = div->FindAttribute(HtmlName::kId);
  ASSERT_NE(nullptr, attr);
  EXPECT_EQ(HtmlElement::SINGLE_QUOTE, attr->quote_style());
  EXPECT_STREQ("'", attr->quote_str());
}

TEST_F(HtmlElementTest, QuoteStyleNone) {
  HtmlElement* div = NewElement(HtmlName::kDiv);
  HtmlName id = html_parse_->MakeName(HtmlName::kId);
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

// NOTE (#1130 vendoring): the ToString partial-line-number tests that
// lived here were dropped — they set line numbers via a test-only
// HtmlTestingPeer friended in the optimizer's amputated html_element.h, and
// canonical html_element.h grants no such friend (set_begin/end_line_number
// and Data are private). The partial-line branches in ToString() are
// canonical code and are exercised through real parses elsewhere.

// --- Valueless attribute in ToString (html_element.cc line 182) ---

TEST_F(HtmlElementTest, ToStringValuelessAttribute) {
  // Covers html_element.cc line 182: attribute with name but no value.
  // This hits the else branch of the value-check in ToString().
  HtmlElement* input = NewElement(HtmlName::kInput);
  input->set_style(HtmlElement::BRIEF_CLOSE);
  HtmlName disabled = html_parse_->MakeName(HtmlName::kDisabled);
  // Add attribute with no value (NO_QUOTE, nullptr value)
  input->AddEscapedAttribute(disabled, std::string_view(),
                             HtmlElement::NO_QUOTE);
  std::string s = input->ToString();
  // Should contain " disabled" without "=" or a value
  EXPECT_NE(std::string::npos, s.find(" disabled"));
  EXPECT_EQ(std::string::npos, s.find("disabled="));
}

}  // namespace net_instaweb
