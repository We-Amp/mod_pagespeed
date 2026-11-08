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

// Source pins for how response headers are copied into nginx. The port
// cannot be linked into a unit test (see test/pagespeed/nginx/BUILD), so
// these read the sources. StripWhitespace strips whitespace, not comments,
// so a pinned token may not appear in a comment either.

#include <cctype>

#include "base/logging.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

GoogleString ReadStrippedSource(const GoogleString& relative_path) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path = StrCat(GTestSrcDir(), "/", relative_path);
  CHECK(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  CHECK(!source.empty()) << path;
  GoogleString out;
  for (const char c : source) {
    if (!isspace(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

// Returns the position of the brace closing the block opened at `open_pos`,
// or npos. Counts braces only.
size_t MatchingBrace(const GoogleString& text, size_t open_pos) {
  int depth = 0;
  for (size_t i = open_pos; i < text.size(); ++i) {
    if (text[i] == '{') {
      ++depth;
    } else if (text[i] == '}') {
      if (--depth == 0) {
        return i;
      }
    }
  }
  return GoogleString::npos;
}

GoogleString CopyResponseHeadersBody() {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  const size_t fn = cc.find("ngx_int_tcopy_response_headers_to_ngx(");
  CHECK_NE(GoogleString::npos, fn)
      << "copy_response_headers_to_ngx was renamed";
  const size_t open = cc.find("){", fn);
  CHECK_NE(GoogleString::npos, open);
  const size_t close = MatchingBrace(cc, open + 1);
  CHECK_NE(GoogleString::npos, close);
  return cc.substr(open, close - open);
}

TEST(NgxResponseHeaderCopyPinTest, UnreadableContentLengthIsLeftOut) {
  // A length that cannot be read is left out of the response, and is never
  // asserted on.
  const GoogleString body = CopyResponseHeadersBody();
  EXPECT_EQ(GoogleString::npos,
            body.find("CHECK(pagespeed_headers.FindContentLength"));
  const size_t skip =
      body.find("if(StringCaseEqual(name_gs,\"Content-Length\")){");
  ASSERT_NE(GoogleString::npos, skip);
  const size_t parse = body.find("FindContentLength(&content_length)", skip);
  ASSERT_NE(GoogleString::npos, parse);
  // The decision is made before the header is pushed into the output list.
  const size_t push = body.find("ngx_list_push(");
  ASSERT_NE(GoogleString::npos, push);
  EXPECT_LT(parse, push);
  EXPECT_NE(GoogleString::npos,
            body.find("value_gs.find_first_not_of(\"0123456789\")", skip));
}

TEST(NgxResponseHeaderCopyPinTest, HeaderBytesAreCopiedAtTheirFullLength) {
  // Names and values are copied at the full length nginx sends, and a header
  // that is not valid on the wire is left out.
  const GoogleString body = CopyResponseHeadersBody();
  EXPECT_EQ(GoogleString::npos, body.find("name_gs.c_str()"));
  EXPECT_EQ(GoogleString::npos, body.find("value_gs.c_str()"));
  EXPECT_NE(GoogleString::npos,
            body.find("string_piece_to_pool_string(r->pool,value_gs)"));
  EXPECT_NE(GoogleString::npos,
            body.find("string_piece_to_pool_string(r->pool,name_gs)"));
  const size_t nul = body.find("name_gs.find('\\0')");
  ASSERT_NE(GoogleString::npos, nul);
  EXPECT_NE(GoogleString::npos, body.find("value_gs.find('\\0')", nul));
  const size_t copy = body.find("string_piece_to_pool_string(");
  ASSERT_NE(GoogleString::npos, copy);
  EXPECT_LT(nul, copy);
}

TEST(NgxResponseHeaderCopyPinTest,
     ConnectionFramingFieldsAreMatchedWithoutCase) {
  // The connection and framing fields are nginx's to state; they are left out
  // of the copy whatever the spelling of their names.
  const GoogleString body = CopyResponseHeadersBody();
  for (const char* field : {"Connection", "Keep-Alive", "Transfer-Encoding"}) {
    EXPECT_NE(GoogleString::npos,
              body.find(StrCat("StringCaseEqual(name_gs,\"", field, "\")")))
        << field;
    EXPECT_EQ(GoogleString::npos,
              body.find(StrCat("STR_EQ_LITERAL(name,\"", field, "\")")))
        << field;
  }
}

}  // namespace
}  // namespace net_instaweb
