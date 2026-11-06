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

#include "pagespeed/kernel/html/collapse_whitespace_filter.h"

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/html/html_parse_test_base.h"

namespace net_instaweb {

class CollapseWhitespaceFilterTest : public HtmlParseTestBase {
 protected:
  CollapseWhitespaceFilterTest() : filter_(&html_parse_) {
    html_parse_.AddFilter(&filter_);
  }

  bool AddBody() const override { return true; }

 private:
  CollapseWhitespaceFilter filter_;
  CollapseWhitespaceFilterTest(const CollapseWhitespaceFilterTest&) = delete;
  CollapseWhitespaceFilterTest& operator=(const CollapseWhitespaceFilterTest&) = delete;
};

TEST_F(CollapseWhitespaceFilterTest, NoChange) {
  ValidateNoChanges("no_change",
                    "<head><title>Hello</title></head>"
                    "<body>Why, hello there!</body>");
}

TEST_F(CollapseWhitespaceFilterTest, CollapseWhitespace) {
  ValidateExpected("collapse_whitespace",
                   "<body>hello   world,   it\n"
                   "    is good  to     see you   </body>",
                   "<body>hello world, it\n"
                   "is good to see you </body>");
}

TEST_F(CollapseWhitespaceFilterTest, NewlineTakesPrecedence) {
  ValidateExpected("newline_takes_precedence",
                   "<body>hello world, it      \n"
                   "    is good to see you</body>",
                   "<body>hello world, it\n"
                   "is good to see you</body>");
}

TEST_F(CollapseWhitespaceFilterTest, DoNotCollapseWithinCode) {
  ValidateNoChanges("do_not_collapse_within_code",
                    "<body><code>hello   world,   it\n"
                    "    is good  to     see you   </code></body>");
}

TEST_F(CollapseWhitespaceFilterTest, DoNotCollapseWithinPre) {
  ValidateNoChanges("do_not_collapse_within_pre",
                    "<body><pre>hello   world,   it\n"
                    "    is good  to     see you   </pre></body>");
}

TEST_F(CollapseWhitespaceFilterTest, CollapseAfterNestedPre) {
  ValidateExpected("collapse_after_nested_pre",
                   "<body><pre>hello   <pre>world,   it</pre>\n"
                   "    is good</pre>  to     see you   </body>",
                   "<body><pre>hello   <pre>world,   it</pre>\n"
                   "    is good</pre> to see you </body>");
}

TEST_F(CollapseWhitespaceFilterTest, DoNotCollapseWithinScript) {
  ValidateExpected("do_not_collapse_within_script",
                   "<head><script>x = \"don't    collapse\"</script></head>"
                   "<body>do       collapse</body>",
                   "<head><script>x = \"don't    collapse\"</script></head>"
                   "<body>do collapse</body>");
}

TEST_F(CollapseWhitespaceFilterTest, DoNotCollapseWithinStyle) {
  ValidateNoChanges("do_not_collapse_within_style",
                    "<head><style>P{font-family:\"don't   collapse\";}</style>"
                    "</head><body></body>");
}

TEST_F(CollapseWhitespaceFilterTest, DoNotCollapseWithinTextarea) {
  ValidateNoChanges("do_not_collapse_within_textarea",
                    "<body><textarea>hello   world,   it\n"
                    "    is good  to     see you   </textarea></body>");
}

// In inline SVG and MathML, textarea is an ordinary element, so its
// whitespace is collapsed like any other text.  (pre and code end SVG and
// MathML content, so they are always HTML.)
TEST_F(CollapseWhitespaceFilterTest, CollapseWithinForeignTextarea) {
  ValidateExpected("collapse_within_foreign_textarea",
                   "<body><svg><textarea>a   b</textarea></svg>"
                   "<math><textarea>c   d</textarea></math>"
                   "<svg><g><pre>e   f</pre></body>",
                   "<body><svg><textarea>a b</textarea></svg>"
                   "<math><textarea>c d</textarea></math>"
                   "<svg><g><pre>e   f</pre></body>");
}

// HTML elements inside an integration point are HTML again.
TEST_F(CollapseWhitespaceFilterTest, DoNotCollapseWithinPreInIntegrationPoint) {
  ValidateNoChanges("pre_in_foreign_object",
                    "<body><svg><foreignObject><pre>a   b</pre>"
                    "</foreignObject></svg><math><mi><textarea>c   d"
                    "</textarea></mi></math></body>");
}

// SVG script and style hold script and CSS text, so their whitespace,
// including that of their child elements, is kept.
TEST_F(CollapseWhitespaceFilterTest, DoNotCollapseWithinForeignScriptOrStyle) {
  ValidateExpected("foreign_script_and_style",
                   "<body><svg><script>x = \"a   b\"<g>c   d</g></script>"
                   "<style>e   f</style><text>g   h</text></svg></body>",
                   "<body><svg><script>x = \"a   b\"<g>c   d</g></script>"
                   "<style>e   f</style><text>g h</text></svg></body>");
}

}  // namespace net_instaweb
