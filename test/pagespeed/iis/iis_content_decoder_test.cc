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

#include "pagespeed/iis/iis_content_decoder.h"

#include <zlib.h>

#include "gtest/gtest.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class ContentDecoderTest : public testing::Test {
 protected:
  // Helper to gzip compress a string
  static GoogleString GzipCompress(const GoogleString& input) {
    z_stream stream = {};
    // 15 + 16 for gzip encoding
    deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                 Z_DEFAULT_STRATEGY);

    GoogleString output;
    output.resize(deflateBound(&stream, input.size()));

    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = input.size();
    stream.next_out = reinterpret_cast<Bytef*>(&output[0]);
    stream.avail_out = output.size();

    deflate(&stream, Z_FINISH);
    output.resize(stream.total_out);
    deflateEnd(&stream);
    return output;
  }

  // Helper to raw deflate compress a string
  static GoogleString DeflateCompress(const GoogleString& input) {
    z_stream stream = {};
    // -15 for raw deflate (no header)
    deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                 Z_DEFAULT_STRATEGY);

    GoogleString output;
    output.resize(deflateBound(&stream, input.size()));

    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = input.size();
    stream.next_out = reinterpret_cast<Bytef*>(&output[0]);
    stream.avail_out = output.size();

    deflate(&stream, Z_FINISH);
    output.resize(stream.total_out);
    deflateEnd(&stream);
    return output;
  }
};

TEST_F(ContentDecoderTest, IdentityEncodingPassthrough) {
  GoogleString input = "Hello World";
  GoogleString output;

  EXPECT_TRUE(ContentDecoder::Decode("identity", input, &output));
  EXPECT_EQ(input, output);
}

TEST_F(ContentDecoderTest, EmptyEncodingPassthrough) {
  GoogleString input = "Hello World";
  GoogleString output;

  EXPECT_TRUE(ContentDecoder::Decode("", input, &output));
  EXPECT_EQ(input, output);
}

TEST_F(ContentDecoderTest, GzipDecoding) {
  GoogleString original = "Hello, this is some text to compress!";
  GoogleString compressed = GzipCompress(original);
  GoogleString decompressed;

  EXPECT_TRUE(ContentDecoder::DecodeGzip(compressed, &decompressed));
  EXPECT_EQ(original, decompressed);
}

TEST_F(ContentDecoderTest, GzipDecodingViaGeneric) {
  GoogleString original = "Hello, this is some text to compress!";
  GoogleString compressed = GzipCompress(original);
  GoogleString decompressed;

  EXPECT_TRUE(ContentDecoder::Decode("gzip", compressed, &decompressed));
  EXPECT_EQ(original, decompressed);
}

TEST_F(ContentDecoderTest, XGzipDecoding) {
  GoogleString original = "Hello, x-gzip variant";
  GoogleString compressed = GzipCompress(original);
  GoogleString decompressed;

  EXPECT_TRUE(ContentDecoder::Decode("x-gzip", compressed, &decompressed));
  EXPECT_EQ(original, decompressed);
}

TEST_F(ContentDecoderTest, DeflateDecoding) {
  GoogleString original = "Hello, this is deflate compressed text!";
  GoogleString compressed = DeflateCompress(original);
  GoogleString decompressed;

  EXPECT_TRUE(ContentDecoder::DecodeDeflate(compressed, &decompressed));
  EXPECT_EQ(original, decompressed);
}

TEST_F(ContentDecoderTest, DeflateDecodingViaGeneric) {
  GoogleString original = "Hello, this is deflate compressed text!";
  GoogleString compressed = DeflateCompress(original);
  GoogleString decompressed;

  EXPECT_TRUE(ContentDecoder::Decode("deflate", compressed, &decompressed));
  EXPECT_EQ(original, decompressed);
}

TEST_F(ContentDecoderTest, EmptyGzipInput) {
  GoogleString empty;
  GoogleString output;

  EXPECT_TRUE(ContentDecoder::DecodeGzip(empty, &output));
  EXPECT_TRUE(output.empty());
}

TEST_F(ContentDecoderTest, InvalidGzipData) {
  GoogleString garbage = "this is not gzip data";
  GoogleString output;

  EXPECT_FALSE(ContentDecoder::DecodeGzip(garbage, &output));
}

TEST_F(ContentDecoderTest, UnsupportedEncoding) {
  GoogleString input = "some data";
  GoogleString output;

  EXPECT_FALSE(ContentDecoder::Decode("br", input, &output));  // brotli not supported here
}

TEST_F(ContentDecoderTest, IsEncodingSupported) {
  EXPECT_TRUE(ContentDecoder::IsEncodingSupported(""));
  EXPECT_TRUE(ContentDecoder::IsEncodingSupported("identity"));
  EXPECT_TRUE(ContentDecoder::IsEncodingSupported("gzip"));
  EXPECT_TRUE(ContentDecoder::IsEncodingSupported("GZIP"));
  EXPECT_TRUE(ContentDecoder::IsEncodingSupported("x-gzip"));
  EXPECT_TRUE(ContentDecoder::IsEncodingSupported("deflate"));
  EXPECT_TRUE(ContentDecoder::IsEncodingSupported("DEFLATE"));

  EXPECT_FALSE(ContentDecoder::IsEncodingSupported("br"));
  EXPECT_FALSE(ContentDecoder::IsEncodingSupported("compress"));
  EXPECT_FALSE(ContentDecoder::IsEncodingSupported("unknown"));
}

TEST_F(ContentDecoderTest, CaseInsensitive) {
  GoogleString original = "Test case insensitivity";
  GoogleString compressed = GzipCompress(original);
  GoogleString decompressed;

  EXPECT_TRUE(ContentDecoder::Decode("GZIP", compressed, &decompressed));
  EXPECT_EQ(original, decompressed);

  EXPECT_TRUE(ContentDecoder::Decode("Gzip", compressed, &decompressed));
  EXPECT_EQ(original, decompressed);
}

TEST_F(ContentDecoderTest, LargerData) {
  // Create a larger input to test buffer handling
  GoogleString original;
  for (int i = 0; i < 1000; ++i) {
    original += "This is line " + std::to_string(i) + " of the test data.\n";
  }

  GoogleString compressed = GzipCompress(original);
  GoogleString decompressed;

  EXPECT_TRUE(ContentDecoder::DecodeGzip(compressed, &decompressed));
  EXPECT_EQ(original, decompressed);
}

TEST_F(ContentDecoderTest, BinaryData) {
  // Test with binary data including null bytes
  GoogleString original;
  for (int i = 0; i < 256; ++i) {
    original.push_back(static_cast<char>(i));
  }

  GoogleString compressed = GzipCompress(original);
  GoogleString decompressed;

  EXPECT_TRUE(ContentDecoder::DecodeGzip(compressed, &decompressed));
  EXPECT_EQ(original, decompressed);
}

}  // namespace net_instaweb
