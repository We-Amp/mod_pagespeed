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

// The port's own header tables against the record arm's reproducibility
// rule.  The rule is fail-closed over the names it is handed, and this
// port mints `__x_` bookmarks and spells the entity tag its own way, so
// the ONLY way a recording's headers survive the rule is that every name
// the port can put in front of it is one the rule knows.  That is a
// property of two tables that evolve separately, which is exactly why it
// needs a test rather than a hope.

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "pagespeed/iis/iis_daemon_record.h"
#include "pagespeed/iis/iis_global_constants.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_record_arm.h"

namespace net_instaweb {

namespace {

// (a) Every bookmark the known-header table mints passes the rule.
TEST(IisDaemonRecordHeadersTest, EveryKnownHeaderBookmarkPassesTheRule) {
  for (size_t i = 0; i < sizeof(HTTP_HEADER_ID_STRINGS_RESPONSE_CACHE) /
                             sizeof(HTTP_HEADER_ID_STRINGS_RESPONSE_CACHE[0]);
       ++i) {
    const char* bookmark = HTTP_HEADER_ID_STRINGS_RESPONSE_CACHE[i];
    if (bookmark == NULL) {
      continue;
    }
    std::vector<DaemonOriginHeader> headers;
    headers.push_back(DaemonOriginHeader{"Content-Type", "text/css"});
    headers.push_back(DaemonOriginHeader{bookmark, "some-value"});
    EXPECT_TRUE(OriginHeadersAreReproducible(headers))
        << "the bookmark " << bookmark << " is marked not reproducible";
  }
}

// (b) The REAL name behind every bookmarked known header passes the rule
// AS THE PORT SPELLS IT, with nothing rewritten anywhere on the port:
// this is the leg that caught the entity tag, whose port spelling is
// "E-Tag" while the rule's canonical spelling is "ETag".  Only the
// bookmarked indices: a known header with no bookmark (Set-Cookie,
// Location, ...) is origin content the rule rightly fails.
TEST(IisDaemonRecordHeadersTest, EveryBookmarkedRealHeaderNamePassesTheRule) {
  for (size_t i = 0; i < sizeof(HTTP_HEADER_ID_STRINGS_RESPONSE_CACHE) /
                             sizeof(HTTP_HEADER_ID_STRINGS_RESPONSE_CACHE[0]);
       ++i) {
    if (HTTP_HEADER_ID_STRINGS_RESPONSE_CACHE[i] == NULL) {
      continue;
    }
    const char* name = HTTP_HEADER_ID_STRINGS_RESPONSE[i];
    if (name == NULL) {
      continue;
    }
    std::vector<DaemonOriginHeader> headers;
    headers.push_back(DaemonOriginHeader{"Content-Type", "text/css"});
    headers.push_back(DaemonOriginHeader{name, "some-value"});
    EXPECT_TRUE(OriginHeadersAreReproducible(headers))
        << "the real header name " << name << " is marked not reproducible";
  }
}

// (c) Every bookmark the unknown-header leg can mint passes the rule.
TEST(IisDaemonRecordHeadersTest, EveryUnknownHeaderBookmarkPassesTheRule) {
  for (size_t i = 0; i < sizeof(UNKNOWN_HEADER_BOOKMARK_NAMES) /
                             sizeof(UNKNOWN_HEADER_BOOKMARK_NAMES[0]);
       ++i) {
    const std::string bookmark =
        std::string("__x_") + UNKNOWN_HEADER_BOOKMARK_NAMES[i];
    std::vector<DaemonOriginHeader> headers;
    headers.push_back(DaemonOriginHeader{"Content-Type", "text/css"});
    headers.push_back(DaemonOriginHeader{bookmark, "some-value"});
    EXPECT_TRUE(OriginHeadersAreReproducible(headers))
        << "the bookmark " << bookmark << " is marked not reproducible";
  }
}

// (d) A default static-file response, built from the port's own tables
// with NOTHING rewritten: the rule accepts it as the port hands it over.
TEST(IisDaemonRecordHeadersTest, ADefaultStaticResponseIsReproducible) {
  std::vector<DaemonOriginHeader> headers;
  headers.push_back(DaemonOriginHeader{"Content-Type", "text/css"});
  headers.push_back(
      DaemonOriginHeader{"Last-Modified", "Mon, 10 Aug 2026 00:00:00 GMT"});
  headers.push_back(DaemonOriginHeader{"E-Tag", "\"abc\""});
  headers.push_back(DaemonOriginHeader{"Accept-Ranges", "bytes"});
  headers.push_back(DaemonOriginHeader{"Cache-Control", "max-age=600"});
  // The bookmarks the port mints for the cache-related ones.
  headers.push_back(
      DaemonOriginHeader{"__x_Last-Modified", "Mon, 10 Aug 2026 00:00:00 GMT"});
  headers.push_back(DaemonOriginHeader{"__x_E-Tag", "\"abc\""});
  headers.push_back(DaemonOriginHeader{"__x_Accept-Ranges", "bytes"});
  headers.push_back(DaemonOriginHeader{"__x_Cache-Control", "max-age=600"});
  EXPECT_TRUE(OriginHeadersAreReproducible(headers));
}

}  // namespace

}  // namespace net_instaweb
