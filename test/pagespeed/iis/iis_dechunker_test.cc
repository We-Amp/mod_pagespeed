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

#include "pagespeed/iis/iis_dechunker.h"

#include "gtest/gtest.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class DechunkerTest : public testing::Test {
 protected:
  Dechunker dechunker_;
};

TEST_F(DechunkerTest, EmptyInput) {
  // No data added, not done yet
  EXPECT_FALSE(dechunker_.IsDone());
  EXPECT_FALSE(dechunker_.HasError());
  EXPECT_TRUE(dechunker_.GetData().empty());
}

TEST_F(DechunkerTest, EmptyChunk) {
  // A valid empty chunked response is just "0\r\n\r\n"
  EXPECT_TRUE(dechunker_.AddData("0\r\n\r\n"));
  EXPECT_TRUE(dechunker_.IsDone());
  EXPECT_FALSE(dechunker_.HasError());
  EXPECT_TRUE(dechunker_.GetData().empty());
}

TEST_F(DechunkerTest, SingleChunk) {
  // Single chunk with "Hello"
  EXPECT_TRUE(dechunker_.AddData("5\r\nHello\r\n0\r\n\r\n"));
  EXPECT_TRUE(dechunker_.IsDone());
  EXPECT_FALSE(dechunker_.HasError());
  EXPECT_EQ("Hello", dechunker_.GetData());
}

TEST_F(DechunkerTest, MultipleChunks) {
  // Multiple chunks: "Hello" + " " + "World"
  EXPECT_TRUE(dechunker_.AddData("5\r\nHello\r\n1\r\n \r\n5\r\nWorld\r\n0\r\n\r\n"));
  EXPECT_TRUE(dechunker_.IsDone());
  EXPECT_FALSE(dechunker_.HasError());
  EXPECT_EQ("Hello World", dechunker_.GetData());
}

TEST_F(DechunkerTest, HexSizes) {
  // Chunk size in hex (a = 10, 10 characters)
  EXPECT_TRUE(dechunker_.AddData("a\r\n0123456789\r\n0\r\n\r\n"));
  EXPECT_TRUE(dechunker_.IsDone());
  EXPECT_FALSE(dechunker_.HasError());
  EXPECT_EQ("0123456789", dechunker_.GetData());
}

TEST_F(DechunkerTest, UppercaseHex) {
  // Uppercase hex
  EXPECT_TRUE(dechunker_.AddData("A\r\n0123456789\r\n0\r\n\r\n"));
  EXPECT_TRUE(dechunker_.IsDone());
  EXPECT_FALSE(dechunker_.HasError());
  EXPECT_EQ("0123456789", dechunker_.GetData());
}

TEST_F(DechunkerTest, LargerHexSize) {
  // Size 0x10 = 16 bytes
  GoogleString data(16, 'x');
  GoogleString chunked = "10\r\n" + data + "\r\n0\r\n\r\n";
  EXPECT_TRUE(dechunker_.AddData(chunked));
  EXPECT_TRUE(dechunker_.IsDone());
  EXPECT_FALSE(dechunker_.HasError());
  EXPECT_EQ(data, dechunker_.GetData());
}

TEST_F(DechunkerTest, PartialInput) {
  // Feed data in multiple calls
  EXPECT_TRUE(dechunker_.AddData("5\r\n"));
  EXPECT_FALSE(dechunker_.IsDone());
  EXPECT_FALSE(dechunker_.HasError());

  EXPECT_TRUE(dechunker_.AddData("Hel"));
  EXPECT_FALSE(dechunker_.IsDone());
  EXPECT_FALSE(dechunker_.HasError());

  EXPECT_TRUE(dechunker_.AddData("lo\r\n"));
  EXPECT_FALSE(dechunker_.IsDone());
  EXPECT_FALSE(dechunker_.HasError());

  EXPECT_TRUE(dechunker_.AddData("0\r\n\r\n"));
  EXPECT_TRUE(dechunker_.IsDone());
  EXPECT_FALSE(dechunker_.HasError());
  EXPECT_EQ("Hello", dechunker_.GetData());
}

TEST_F(DechunkerTest, ByteByByte) {
  // Feed byte by byte
  const char* input = "5\r\nHello\r\n0\r\n\r\n";
  for (size_t i = 0; input[i] != '\0'; ++i) {
    EXPECT_TRUE(dechunker_.AddData(input + i, 1));
    EXPECT_FALSE(dechunker_.HasError());
  }
  EXPECT_TRUE(dechunker_.IsDone());
  EXPECT_EQ("Hello", dechunker_.GetData());
}

TEST_F(DechunkerTest, InvalidHexCharacter) {
  EXPECT_FALSE(dechunker_.AddData("g\r\n"));  // 'g' is not valid hex
  EXPECT_TRUE(dechunker_.HasError());
}

TEST_F(DechunkerTest, MissingCRLF) {
  EXPECT_FALSE(dechunker_.AddData("5\nHello\n0\n\n"));  // Missing CR
  EXPECT_TRUE(dechunker_.HasError());
}

TEST_F(DechunkerTest, EmptyChunkSize) {
  EXPECT_FALSE(dechunker_.AddData("\r\nHello\r\n"));  // No size before CRLF
  EXPECT_TRUE(dechunker_.HasError());
}

TEST_F(DechunkerTest, Reset) {
  // Process some data
  EXPECT_TRUE(dechunker_.AddData("5\r\nHello\r\n0\r\n\r\n"));
  EXPECT_TRUE(dechunker_.IsDone());
  EXPECT_EQ("Hello", dechunker_.GetData());

  // Reset and process new data
  dechunker_.Reset();
  EXPECT_FALSE(dechunker_.IsDone());
  EXPECT_FALSE(dechunker_.HasError());
  EXPECT_TRUE(dechunker_.GetData().empty());

  EXPECT_TRUE(dechunker_.AddData("5\r\nWorld\r\n0\r\n\r\n"));
  EXPECT_TRUE(dechunker_.IsDone());
  EXPECT_EQ("World", dechunker_.GetData());
}

TEST_F(DechunkerTest, DataAfterEnd) {
  // Extra data after final chunk should be ignored
  EXPECT_TRUE(dechunker_.AddData("5\r\nHello\r\n0\r\n\r\nextra garbage"));
  EXPECT_TRUE(dechunker_.IsDone());
  EXPECT_EQ("Hello", dechunker_.GetData());
}

TEST_F(DechunkerTest, BinaryData) {
  // Binary data with null bytes
  GoogleString binary_data;
  binary_data.push_back('\x00');
  binary_data.push_back('\x01');
  binary_data.push_back('\xff');

  GoogleString chunked = "3\r\n" + binary_data + "\r\n0\r\n\r\n";
  EXPECT_TRUE(dechunker_.AddData(chunked));
  EXPECT_TRUE(dechunker_.IsDone());
  EXPECT_EQ(binary_data, dechunker_.GetData());
}

TEST_F(DechunkerTest, RFCExample) {
  // Example from RFC 7230
  const char* rfc_example =
      "7\r\n"
      "Mozilla\r\n"
      "9\r\n"
      "Developer\r\n"
      "7\r\n"
      "Network\r\n"
      "0\r\n"
      "\r\n";
  EXPECT_TRUE(dechunker_.AddData(rfc_example));
  EXPECT_TRUE(dechunker_.IsDone());
  EXPECT_EQ("MozillaDeveloperNetwork", dechunker_.GetData());
}

}  // namespace net_instaweb
