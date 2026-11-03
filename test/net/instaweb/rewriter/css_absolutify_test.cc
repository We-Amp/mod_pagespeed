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

// Unit-test CssAbsolutify, focusing on conditional group rules
// (@supports/@layer/@container): url()s inside their opaque preludes must be
// absolutified in proxy/domain-mapping mode, just like url()s in their
// parsed bodies.

#include "net/instaweb/rewriter/public/css_absolutify.h"

#include <memory>

#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/http/google_url.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "third_party/css_parser/src/util/utf8/public/unicodetext.h"
#include "third_party/css_parser/src/webutil/css/parser.h"
#include "third_party/css_parser/src/webutil/css/property.h"
#include "third_party/css_parser/src/webutil/css/value.h"

namespace net_instaweb {

namespace {

class CssAbsolutifyTest : public RewriteTestBase {
 protected:
  void SetUp() override {
    RewriteTestBase::SetUp();
    // Map test.com onto the sharded cdn.com origin so absolutification is
    // observable. This mirrors the domain setup of the CssFilter absolutify
    // tests, so url(a.png) shards to cdn2.com and url(sub/c.png) to
    // cdn1.com.
    DomainLawyer* domain_lawyer = options()->WriteableDomainLawyer();
    MessageHandler* handler = message_handler();
    ASSERT_TRUE(domain_lawyer->AddDomain("http://cdn.com/", handler));
    ASSERT_TRUE(domain_lawyer->AddDomain("http://test.com/", handler));
    ASSERT_TRUE(
        domain_lawyer->AddShard("cdn.com", "cdn1.com,cdn2.com", handler));
    ASSERT_TRUE(domain_lawyer->AddRewriteDomainMapping("http://cdn.com",
                                                       "http://test.com",
                                                       handler));
  }

  // Parses css in preservation mode (no parse errors expected) and runs
  // CssAbsolutify::AbsolutifyUrls over it like the CssFilter
  // proxy/domain-mapping path does, with the stylesheet served from
  // http://test.com/foo.css.
  std::unique_ptr<Css::Stylesheet> Absolutify(
      const char* css, bool handle_parseable_sections,
      bool handle_unparseable_sections) {
    Css::Parser parser(css);
    parser.set_preservation_mode(true);
    parser.set_quirks_mode(false);
    std::unique_ptr<Css::Stylesheet> stylesheet(parser.ParseRawStylesheet());
    EXPECT_TRUE(parser.errors_seen_mask() == Css::Parser::kNoError);
    GoogleUrl base("http://test.com/foo.css");
    CssAbsolutify::AbsolutifyUrls(stylesheet.get(), base,
                                  handle_parseable_sections,
                                  handle_unparseable_sections,
                                  rewrite_driver(), message_handler());
    return stylesheet;
  }

  // Asserts that the single ruleset inside group's body carries a single
  // background-image declaration whose url() equals expected_url.
  static void ExpectBodyBackgroundImage(const Css::Ruleset* group,
                                        const char* expected_url) {
    const Css::Stylesheet& body = group->group_body();
    ASSERT_EQ(1, body.rulesets().size());
    const Css::Ruleset* body_ruleset = body.rulesets()[0];
    ASSERT_EQ(Css::Ruleset::RULESET, body_ruleset->type());
    ASSERT_EQ(1, body_ruleset->declarations().size());
    const Css::Declaration& decl = body_ruleset->declaration(0);
    ASSERT_EQ(Css::Property::BACKGROUND_IMAGE, decl.prop());
    const Css::Values* values = decl.values();
    ASSERT_EQ(1, values->size());
    const Css::Value* value = values->at(0);
    ASSERT_EQ(Css::Value::URI, value->GetLexicalUnitType());
    EXPECT_EQ(expected_url, UnicodeTextToUTF8(value->GetStringValue()));
  }
};

// Regression test: a url() inside a group-rule prelude (opaque, structure-
// blind bytes that the parsed-declaration walk never sees) must be
// absolutified by the textual unparseable-section path, exactly as it was
// when a @supports block was still preserved verbatim as an UnparsedRegion.
TEST_F(CssAbsolutifyTest, GroupRulePreludeUrlAbsolutifiedWithDomainMapping) {
  std::unique_ptr<Css::Stylesheet> stylesheet = Absolutify(
      "@supports (background: image-set(url(a.png) 1x)) {"
      " body { background-image: url(sub/c.png) } }",
      true /* handle_parseable_sections */,
      true /* handle_unparseable_sections */);
  ASSERT_EQ(1, stylesheet->rulesets().size());
  const Css::Ruleset* group = stylesheet->rulesets()[0];
  ASSERT_EQ(Css::Ruleset::GROUP_RULE, group->type());
  EXPECT_EQ("@supports (background: image-set(url(http://cdn2.com/a.png) 1x))",
            group->group_prelude());
}

// Control: url()s in the group body are parsed declarations and must keep
// being absolutified by the structural path.
TEST_F(CssAbsolutifyTest, GroupRuleBodyUrlAbsolutifiedWithDomainMapping) {
  std::unique_ptr<Css::Stylesheet> stylesheet = Absolutify(
      "@supports (background: image-set(url(a.png) 1x)) {"
      " body { background-image: url(sub/c.png) } }",
      true /* handle_parseable_sections */,
      true /* handle_unparseable_sections */);
  ASSERT_EQ(1, stylesheet->rulesets().size());
  ExpectBodyBackgroundImage(stylesheet->rulesets()[0],
                            "http://cdn1.com/sub/c.png");
}

// The walk recurses into group bodies, so preludes of nested group rules
// must be absolutified too.
TEST_F(CssAbsolutifyTest, NestedGroupRulePreludeUrlAbsolutified) {
  std::unique_ptr<Css::Stylesheet> stylesheet = Absolutify(
      "@layer outer {"
      " @supports (mask-image: url(a.png)) {"
      " body { background-image: url(sub/c.png) } } }",
      true /* handle_parseable_sections */,
      true /* handle_unparseable_sections */);
  ASSERT_EQ(1, stylesheet->rulesets().size());
  const Css::Ruleset* layer = stylesheet->rulesets()[0];
  ASSERT_EQ(Css::Ruleset::GROUP_RULE, layer->type());
  // No url() in this prelude: it must be left byte-for-byte intact.
  EXPECT_EQ("@layer outer", layer->group_prelude());
  const Css::Stylesheet& layer_body = layer->group_body();
  ASSERT_EQ(1, layer_body.rulesets().size());
  const Css::Ruleset* supports = layer_body.rulesets()[0];
  ASSERT_EQ(Css::Ruleset::GROUP_RULE, supports->type());
  EXPECT_EQ("@supports (mask-image: url(http://cdn2.com/a.png))",
            supports->group_prelude());
  ExpectBodyBackgroundImage(supports, "http://cdn1.com/sub/c.png");
}

// The prelude transform is gated on handle_unparseable_sections (as the
// UNPARSED_REGION path is): with it off, the prelude bytes stay verbatim
// while parsed body declarations are still absolutified.
TEST_F(CssAbsolutifyTest, PreludeUntouchedWhenUnparseableSectionsDisabled) {
  std::unique_ptr<Css::Stylesheet> stylesheet = Absolutify(
      "@supports (background: image-set(url(a.png) 1x)) {"
      " body { background-image: url(sub/c.png) } }",
      true /* handle_parseable_sections */,
      false /* handle_unparseable_sections */);
  ASSERT_EQ(1, stylesheet->rulesets().size());
  const Css::Ruleset* group = stylesheet->rulesets()[0];
  ASSERT_EQ(Css::Ruleset::GROUP_RULE, group->type());
  EXPECT_EQ("@supports (background: image-set(url(a.png) 1x))",
            group->group_prelude());
  ExpectBodyBackgroundImage(group, "http://cdn1.com/sub/c.png");
}

// A url() passed to a CSS function in a parsed declaration is resolved like
// one that stands on its own.
TEST_F(CssAbsolutifyTest, UrlInsideFunctionAbsolutified) {
  std::unique_ptr<Css::Stylesheet> stylesheet =
      Absolutify("body { background-image: image-set(url(a.png) 1x) }",
                 true /* handle_parseable_sections */,
                 true /* handle_unparseable_sections */);
  ASSERT_EQ(1, stylesheet->rulesets().size());
  const Css::Ruleset* ruleset = stylesheet->rulesets()[0];
  ASSERT_EQ(Css::Ruleset::RULESET, ruleset->type());
  ASSERT_EQ(1, ruleset->declarations().size());
  const Css::Values* values = ruleset->declaration(0).values();
  ASSERT_EQ(1, values->size());
  const Css::Value* function = values->at(0);
  ASSERT_EQ(Css::Value::FUNCTION, function->GetLexicalUnitType());
  EXPECT_EQ("image-set", UnicodeTextToUTF8(function->GetFunctionName()));
  const Css::FunctionParameters* params =
      function->GetParametersWithSeparators();
  ASSERT_EQ(2, params->size());
  ASSERT_EQ(Css::Value::URI, params->value(0)->GetLexicalUnitType());
  EXPECT_EQ("http://cdn2.com/a.png",
            UnicodeTextToUTF8(params->value(0)->GetStringValue()));
  // The other parameter and the separator before it are kept.
  EXPECT_EQ(Css::FunctionParameters::SPACE_SEPARATED, params->separator(1));
  EXPECT_EQ(Css::Value::NUMBER, params->value(1)->GetLexicalUnitType());
}

// The same holds for a url() in a function nested in another function.
TEST_F(CssAbsolutifyTest, UrlInsideNestedFunctionAbsolutified) {
  std::unique_ptr<Css::Stylesheet> stylesheet =
      Absolutify("body { background-image: image-set(image(url(a.png)) 1x) }",
                 true /* handle_parseable_sections */,
                 true /* handle_unparseable_sections */);
  ASSERT_EQ(1, stylesheet->rulesets().size());
  const Css::Values* values =
      stylesheet->rulesets()[0]->declaration(0).values();
  ASSERT_EQ(1, values->size());
  const Css::Value* outer = values->at(0);
  ASSERT_EQ(Css::Value::FUNCTION, outer->GetLexicalUnitType());
  const Css::Value* inner = outer->GetParametersWithSeparators()->value(0);
  ASSERT_EQ(Css::Value::FUNCTION, inner->GetLexicalUnitType());
  const Css::Value* uri = inner->GetParametersWithSeparators()->value(0);
  ASSERT_EQ(Css::Value::URI, uri->GetLexicalUnitType());
  EXPECT_EQ("http://cdn2.com/a.png", UnicodeTextToUTF8(uri->GetStringValue()));
}

// A fragment-only reference names an element of the document that uses the
// stylesheet, so it stays as written, in parsed declarations and in
// sections kept as text alike.
TEST_F(CssAbsolutifyTest, FragmentReferenceUnchanged) {
  std::unique_ptr<Css::Stylesheet> stylesheet = Absolutify(
      "@supports (mask: url(#m)) {"
      " body { background-image: url(#g) } }"
      " div { background-image: image-set(url(#h) 1x) }",
      true /* handle_parseable_sections */,
      true /* handle_unparseable_sections */);
  ASSERT_EQ(2, stylesheet->rulesets().size());
  const Css::Ruleset* group = stylesheet->rulesets()[0];
  ASSERT_EQ(Css::Ruleset::GROUP_RULE, group->type());
  EXPECT_EQ("@supports (mask: url(#m))", group->group_prelude());
  ExpectBodyBackgroundImage(group, "#g");
  const Css::Values* values =
      stylesheet->rulesets()[1]->declaration(0).values();
  ASSERT_EQ(1, values->size());
  ASSERT_EQ(Css::Value::FUNCTION, values->at(0)->GetLexicalUnitType());
  const Css::Value* uri =
      values->at(0)->GetParametersWithSeparators()->value(0);
  ASSERT_EQ(Css::Value::URI, uri->GetLexicalUnitType());
  EXPECT_EQ("#h", UnicodeTextToUTF8(uri->GetStringValue()));
}

}  // namespace

}  // namespace net_instaweb
