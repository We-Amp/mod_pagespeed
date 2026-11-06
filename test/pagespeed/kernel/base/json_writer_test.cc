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

#include "pagespeed/kernel/base/json_writer.h"

#include "pagespeed/kernel/base/json.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

class JsonWriterTest : public testing::Test {
 protected:
  JsonWriterTest() : writer_(&string_writer_, &stack_) {
    // JsonWriter never dereferences the HtmlElement half of the pair,
    // so a null element is fine here.
    stack_.push_back(ElementJsonPair(nullptr, &json_));
  }

  // Declaration order matters: sink_, json_ and stack_ must all outlive
  // writer_, which keeps raw pointers to them.
  GoogleString sink_;
  StringWriter string_writer_{&sink_};
  Json::Value json_{Json::objectValue};
  std::vector<ElementJsonPair> stack_;
  JsonWriter writer_;
  NullMessageHandler message_handler_;
};

TEST_F(JsonWriterTest, WriteBuffersUntilUpdateDictionary) {
  EXPECT_FALSE(json_.isMember(kInstanceHtml));

  EXPECT_TRUE(writer_.Write("hello, ", &message_handler_));
  EXPECT_TRUE(writer_.Write("world", &message_handler_));

  // Write() only buffers: the delegate writer sees none of the bytes.
  EXPECT_TRUE(sink_.empty());

  writer_.UpdateDictionary();
  EXPECT_EQ("hello, world", json_[kInstanceHtml].asString());

  // The buffer is cleared, so a second update adds nothing.
  writer_.UpdateDictionary();
  EXPECT_EQ("hello, world", json_[kInstanceHtml].asString());

  // Writes after an update append to the existing value.
  EXPECT_TRUE(writer_.Write("!", &message_handler_));
  writer_.UpdateDictionary();
  EXPECT_EQ("hello, world!", json_[kInstanceHtml].asString());

  // The delegate writer never received anything.
  EXPECT_TRUE(sink_.empty());
}

TEST_F(JsonWriterTest, UpdateDictionaryAppendsToExistingValue) {
  json_[kInstanceHtml] = "prefix";
  json_["other"] = "kept";

  EXPECT_TRUE(writer_.Write("+suffix", &message_handler_));
  writer_.UpdateDictionary();

  EXPECT_EQ("prefix+suffix", json_[kInstanceHtml].asString());
  EXPECT_EQ("kept", json_["other"].asString());
}

TEST_F(JsonWriterTest, UpdateDictionaryWritesTopOfStack) {
  Json::Value bottom{Json::objectValue};
  stack_.insert(stack_.begin(), ElementJsonPair(nullptr, &bottom));

  EXPECT_TRUE(writer_.Write("top", &message_handler_));
  writer_.UpdateDictionary();

  EXPECT_EQ("top", json_[kInstanceHtml].asString());
  EXPECT_FALSE(bottom.isMember(kInstanceHtml));
}

}  // namespace net_instaweb
