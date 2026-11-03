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

#include "pagespeed/kernel/http/semantic_type.h"

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

class SemanticTypeTest : public testing::Test {};

TEST_F(SemanticTypeTest, TestParseCategory) {
  semantic_type::Category category;
  EXPECT_TRUE(semantic_type::ParseCategory("image", &category));
  EXPECT_EQ(semantic_type::kImage, category);
  // Check case-insensitivity.
  EXPECT_TRUE(semantic_type::ParseCategory("iMaGe", &category));
  EXPECT_EQ(semantic_type::kImage, category);
  EXPECT_TRUE(semantic_type::ParseCategory("script", &category));
  EXPECT_EQ(semantic_type::kScript, category);
  EXPECT_TRUE(semantic_type::ParseCategory("stylesheet", &category));
  EXPECT_EQ(semantic_type::kStylesheet, category);
  EXPECT_TRUE(semantic_type::ParseCategory("OtherResource", &category));
  EXPECT_EQ(semantic_type::kOtherResource, category);
  EXPECT_TRUE(semantic_type::ParseCategory("Hyperlink", &category));
  EXPECT_EQ(semantic_type::kHyperlink, category);
  EXPECT_FALSE(semantic_type::ParseCategory("", &category));
  EXPECT_FALSE(semantic_type::ParseCategory("Undefined", &category));
}

TEST_F(SemanticTypeTest, TestGetCategoryString) {
  EXPECT_STREQ(
      "Script",
      semantic_type::GetCategoryString(semantic_type::kScript).c_str());
  EXPECT_STREQ("Image",
               semantic_type::GetCategoryString(semantic_type::kImage).c_str());
  EXPECT_STREQ(
      "Stylesheet",
      semantic_type::GetCategoryString(semantic_type::kStylesheet).c_str());
  EXPECT_STREQ(
      "OtherResource",
      semantic_type::GetCategoryString(semantic_type::kOtherResource).c_str());
  EXPECT_STREQ(
      "Hyperlink",
      semantic_type::GetCategoryString(semantic_type::kHyperlink).c_str());
  EXPECT_STREQ(
      "Prefetch",
      semantic_type::GetCategoryString(semantic_type::kPrefetch).c_str());
  // kUndefined has no case of its own, so it falls through to the default.
  EXPECT_STREQ(
      "Unknown",
      semantic_type::GetCategoryString(semantic_type::kUndefined).c_str());
}

}  // namespace net_instaweb
