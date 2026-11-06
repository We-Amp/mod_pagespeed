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

#include "pagespeed/kernel/http/data_url.h"

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/content_type.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

const GoogleString kAsciiData =
    "A_Rather=Long,But-conventional?looking_string#with;some:odd,characters.";
const GoogleString kAsciiDataBase64 =
    "QV9SYXRoZXI9TG9uZyxCdXQtY29udmVudGlvbmFsP2xvb2tpbmdfc3RyaW5nI3dpdGg7c29"
    "tZTpvZGQsY2hhcmFjdGVycy4=";

// A string with embedded NULs; we must construct it carefully to avoid
// truncation.
const char kMixedDataChars[] =
    "This string\ncontains\0lots of\tunusual\xe3~characters\xd7\xa5";
const char kMixedDataBase64[] =
    "VGhpcyBzdHJpbmcKY29udGFpbnMAbG90cyBvZgl1bnVzdWFs435jaGFyYWN0ZXJz16U=";

const char kPlainPrefix[] = "data:text/plain,";
const char kBase64Prefix[] = "data:text/plain;base64,";

const char kGifPlainPrefix[] = "data:image/gif,";
const char kGifBase64Prefix[] = "data:image/gif;base64,";

class DataUrlTest : public testing::Test {
 public:
  DataUrlTest()
      : mixed_data_(kMixedDataChars, STATIC_STRLEN(kMixedDataChars)) {}

 protected:
  // Make a ContentType yield readable failure output.  Needed this to fix bugs
  // in the tests!  (No actual program bugs found here...)
  const char* Mime(const ContentType* type) {
    if (type == nullptr || type->mime_type() == nullptr) {
      return "NULL";
    } else {
      return type->mime_type();
    }
  }

  void TestDecoding(const bool can_parse, const bool can_decode,
                    const GoogleString& prefix, const GoogleString& encoded,
                    const ContentType* type, Encoding encoding,
                    const GoogleString& decoded) {
    GoogleString url = prefix + encoded;
    const ContentType* parsed_type;
    Encoding parsed_encoding = UNKNOWN;
    StringPiece parsed_encoded;
    EXPECT_EQ(can_parse, ParseDataUrl(url, &parsed_type, &parsed_encoding,
                                      &parsed_encoded));
    EXPECT_EQ(encoding, parsed_encoding);
    EXPECT_EQ(type, parsed_type) << "type '" << Mime(type) << "' didn't match '"
                                 << Mime(parsed_type) << "'\n";
    EXPECT_EQ(encoded, parsed_encoded);
    GoogleString parsed_decoded;
    EXPECT_EQ(can_decode,
              DecodeDataUrlContent(encoding, parsed_encoded, &parsed_decoded));
    EXPECT_EQ(decoded, parsed_decoded);
  }

  GoogleString mixed_data_;

 private:
  DataUrlTest(const DataUrlTest&) = delete;
  DataUrlTest& operator=(const DataUrlTest&) = delete;
};

TEST_F(DataUrlTest, TestDataPlain) {
  GoogleString url;
  DataUrl(kContentTypeText, PLAIN, kAsciiData, &url);
  EXPECT_EQ(kPlainPrefix + kAsciiData, url);
}

TEST_F(DataUrlTest, TestDataBase64) {
  GoogleString url;
  DataUrl(kContentTypeText, BASE64, kAsciiData, &url);
  EXPECT_EQ(kBase64Prefix + kAsciiDataBase64, url);
}

TEST_F(DataUrlTest, TestData1Plain) {
  GoogleString url;
  DataUrl(kContentTypeGif, PLAIN, mixed_data_, &url);
  EXPECT_EQ(kGifPlainPrefix + mixed_data_, url);
}

TEST_F(DataUrlTest, TestData1Base64) {
  GoogleString url;
  DataUrl(kContentTypeGif, BASE64, mixed_data_, &url);
  EXPECT_EQ(StrCat(kGifBase64Prefix, kMixedDataBase64), url);
}

TEST_F(DataUrlTest, ParseDataPlain) {
  TestDecoding(true, true, kPlainPrefix, kAsciiData, &kContentTypeText, PLAIN,
               kAsciiData);
}

TEST_F(DataUrlTest, ParseDataBase64) {
  TestDecoding(true, true, kBase64Prefix, kAsciiDataBase64, &kContentTypeText,
               BASE64, kAsciiData);
}

TEST_F(DataUrlTest, ParseData1Plain) {
  TestDecoding(true, true, kPlainPrefix, mixed_data_, &kContentTypeText, PLAIN,
               mixed_data_);
}

TEST_F(DataUrlTest, ParseData1Base64) {
  TestDecoding(true, true, kBase64Prefix, kMixedDataBase64, &kContentTypeText,
               BASE64, mixed_data_);
}

TEST_F(DataUrlTest, ParseBadProtocol) {
  TestDecoding(false, false, "http://www.google.com/", "", nullptr, UNKNOWN,
               "");
}

TEST_F(DataUrlTest, ParseNoComma) {
  TestDecoding(false, false,
               StrCat("data:text/plain;base64;", kMixedDataBase64), "", nullptr,
               UNKNOWN, "");
}

TEST_F(DataUrlTest, ParseNoMime) {
  TestDecoding(true, true, "data:;base64,", kMixedDataBase64, nullptr, BASE64,
               mixed_data_);
}

TEST_F(DataUrlTest, ParseCorruptMime) {
  TestDecoding(true, true, "data:#$!;base64,", kMixedDataBase64, nullptr,
               BASE64, mixed_data_);
}

TEST_F(DataUrlTest, ParseBadEncodingIsPlain) {
  TestDecoding(true, true, "data:text/plain;mumbledypeg,", mixed_data_,
               &kContentTypeText, PLAIN, mixed_data_);
}

TEST_F(DataUrlTest, ParseBadBase64) {
  TestDecoding(true, false, kBase64Prefix, "@%#$%@#$%^@%%^%*%^&*",
               &kContentTypeText, BASE64, "");
}

TEST_F(DataUrlTest, IsDataUrl) {
  EXPECT_TRUE(IsDataUrl("data:text/plain,hello"));
  EXPECT_TRUE(IsDataUrl("data:image/gif;base64,R0lGODlhAQABAAAAACw=="));
  // Pure prefix check: no comma or valid mime type is required.
  EXPECT_TRUE(IsDataUrl("data:"));

  EXPECT_FALSE(IsDataUrl(""));
  EXPECT_FALSE(IsDataUrl("http://www.example.com/image.png"));
  EXPECT_FALSE(IsDataUrl("data"));
  // The prefix must appear at the start of the URL.
  EXPECT_FALSE(IsDataUrl("xdata:text/plain,hello"));
  // Matching is case-sensitive.
  EXPECT_FALSE(IsDataUrl("DATA:text/plain,hello"));
}

TEST_F(DataUrlTest, IsDataImageUrl) {
  EXPECT_TRUE(IsDataImageUrl("data:image/gif;base64,R0lGODlhAQABAAAAACw=="));
  EXPECT_TRUE(IsDataImageUrl("data:image/png;base64,iVBORw0KGgo="));
  EXPECT_TRUE(IsDataImageUrl("data:image/gif,R0lGODlh"));

  // data: URLs that do not carry an image mime type do not match.
  EXPECT_FALSE(IsDataImageUrl("data:text/plain,hello"));
  EXPECT_FALSE(IsDataImageUrl("data:text/css;base64,aGVsbG8="));
  EXPECT_FALSE(IsDataImageUrl("data:;base64,AAAA"));
  EXPECT_FALSE(IsDataImageUrl("data:"));

  // Non-data URLs never match.
  EXPECT_FALSE(IsDataImageUrl("http://www.example.com/image.png"));
  // The prefix must appear at the start of the URL and matching is
  // case-sensitive.
  EXPECT_FALSE(IsDataImageUrl("xdata:image/png;base64,AAAA"));
  EXPECT_FALSE(IsDataImageUrl("DATA:image/png;base64,AAAA"));
}

}  // namespace
}  // namespace net_instaweb
