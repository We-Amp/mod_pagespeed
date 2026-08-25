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

// The classic in-place serve path's 304 `Vary` composition.
//
// The behavioural half drives the seam fetch directly over a plain base
// fetch: the composition reads two request facts and the response status
// and nothing else, so every branch of it executes without a live httpd.
// The wiring half -- that InstawebHandler actually routes the in-place
// fetch through the wrapper -- is a call site that needs a live server, so
// it is pinned by reading the source, the same idiom daemon_seam_test.cc
// uses for its own call sites.

#include "pagespeed/apache/in_place_not_modified_vary_fetch.h"

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/request_context.h"
#include "pagespeed/apache/streaming_pagespeed_resource_fetch.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/http_options.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

GoogleString ReadSource(const GoogleString& relative_path) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path = StrCat(GTestSrcDir(), "/", relative_path);
  EXPECT_TRUE(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  return source;
}

class InPlaceNotModifiedVaryFetchTest : public testing::Test {
 protected:
  InPlaceNotModifiedVaryFetchTest()
      : request_context_(new RequestContext(kDefaultHttpOptionsForTests,
                                            new NullMutex, nullptr)) {}

  // Runs one response leg through the wrapper and returns the `Vary`
  // values on the response afterwards, copied out.  The wrapper shares
  // its headers with the base fetch and both die with this function, so
  // returning `Lookup`'s own pointers would return dangling ones -- the
  // assertions then read freed memory, which the sanitizers catch as a
  // heap-use-after-free.
  //
  // `seed_vary` pre-seeds one `Vary` value, standing in for an axis the
  // origin itself negotiated on; the composition must append the encoding
  // axis to that set rather than replace it.
  StringVector VaryAfterHeadersComplete(HttpStatus::Code status,
                                        const char* content_type, bool is_head,
                                        StringPiece seed_vary) {
    StringAsyncFetch base(request_context_);
    InPlaceNotModifiedVaryFetch fetch(content_type, is_head, &base);
    if (!seed_vary.empty()) {
      fetch.response_headers()->Add(HttpAttributes::kVary, seed_vary);
    }
    fetch.response_headers()->SetStatusAndReason(status);
    fetch.HeadersComplete();
    fetch.Done(true);
    ConstStringStarVector values;
    fetch.response_headers()->Lookup(HttpAttributes::kVary, &values);
    StringVector copied;
    copied.reserve(values.size());
    for (const GoogleString* value : values) {
      copied.push_back(*value);
    }
    return copied;
  }

  RequestContextPtr request_context_;
};

TEST_F(InPlaceNotModifiedVaryFetchTest,
       TheNotModifiedLegStatesTheEncodingAxisForCompressibleTypes) {
  // THE DEFECT, STATED AS THE PROPERTY IT BROKE: a 200 for a compressible
  // in-place resource carries `Vary: Accept-Encoding` (the compressor the
  // serving chain hands the body to stamps it), and the 304 that
  // revalidated the same resource carried none -- so a cache updating its
  // stored header fields from the 304 (RFC 9111 4.3.4) was told the
  // response varies on a narrower set than the one it had keyed on.
  const StringVector values =
      VaryAfterHeadersComplete(HttpStatus::kNotModified, "text/css", false, "");
  ASSERT_EQ(1, values.size());
  EXPECT_EQ(GoogleString(HttpAttributes::kAcceptEncoding), values[0]);
}

TEST_F(InPlaceNotModifiedVaryFetchTest,
       TheTwoHundredLegLeavesTheFieldToTheCompressor) {
  // On the 200 leg the compressor is the emitter.  A wrapper that stamped
  // the token there too would put two emitters on one field, which is the
  // shape a duplicated token is the symptom of.
  const StringVector values =
      VaryAfterHeadersComplete(HttpStatus::kOK, "text/css", false, "");
  EXPECT_TRUE(values.empty());
}

TEST_F(InPlaceNotModifiedVaryFetchTest,
       AnIncompressibleTypeStatesNoEncodingAxis) {
  // The token is not stamped unconditionally: a media type the module never
  // hands to the compressor -- every image -- has no encoding axis on
  // either leg, and a 304 that stated one would disagree with its own 200
  // with the sign flipped.
  EXPECT_TRUE(
      VaryAfterHeadersComplete(HttpStatus::kNotModified, "image/png", false, "")
          .empty());
  EXPECT_TRUE(
      VaryAfterHeadersComplete(HttpStatus::kNotModified, nullptr, false, "")
          .empty());
}

TEST_F(InPlaceNotModifiedVaryFetchTest, AHeadRequestStatesNoEncodingAxis) {
  // A HEAD's 200-equivalent carries no body past the compressor -- the same
  // small-response shortcut that silences a 304 fires on it -- so the 200
  // states no encoding axis and the 304 must not either.
  EXPECT_TRUE(
      VaryAfterHeadersComplete(HttpStatus::kNotModified, "text/css", true, "")
          .empty());
}

TEST_F(InPlaceNotModifiedVaryFetchTest,
       AnExistingVaryAxisIsComposedOntoNotClobbered) {
  // A response may already vary on `Accept` (origin negotiation) when the
  // encoding axis is added; the composition appends to the set.
  const StringVector values = VaryAfterHeadersComplete(
      HttpStatus::kNotModified, "text/css", false, "Accept");
  ASSERT_EQ(2, values.size());
  EXPECT_EQ(GoogleString(HttpAttributes::kAccept), values[0]);
  EXPECT_EQ(GoogleString(HttpAttributes::kAcceptEncoding), values[1]);
}

TEST_F(InPlaceNotModifiedVaryFetchTest,
       TheInPlaceFetchIsRoutedThroughTheVaryComposition) {
  // The composition only runs if the classic in-place path actually hands
  // the driver the wrapper rather than the bare ApacheFetch.  This call
  // site needs a live httpd to execute, so it is pinned in the source, the
  // daemon seam's idiom: the pin catches the realistic regression -- the
  // wrapper dropped from the wiring, or the driver handed the unwrapped
  // fetch -- and not a call site that runs and does nothing.
  const GoogleString source =
      ReadSource("pagespeed/apache/instaweb_handler.cc");
  const size_t in_place = source.find("bool InstawebHandler::HandleAsInPlace(");
  ASSERT_NE(GoogleString::npos, in_place);
  const size_t construct =
      source.find("InPlaceNotModifiedVaryFetch vary_fetch(", in_place);
  ASSERT_NE(GoogleString::npos, construct)
      << "the classic in-place path no longer constructs the 304 `Vary` "
         "composition wrapper";
  const size_t hand_off = source.find("FetchInPlaceResource(", in_place);
  ASSERT_NE(GoogleString::npos, hand_off);
  EXPECT_LT(construct, hand_off)
      << "the wrapper is constructed after the driver already has its fetch";
  EXPECT_NE(GoogleString::npos,
            source.find("&vary_fetch);\n  WaitForFetch();", in_place))
      << "the driver is handed the bare ApacheFetch, so no 304 passes "
         "through the `Vary` composition";
}

}  // namespace

}  // namespace net_instaweb
