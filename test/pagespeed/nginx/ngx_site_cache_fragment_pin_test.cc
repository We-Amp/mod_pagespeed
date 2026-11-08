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

// A server block's cache keyspace is settled while the configuration is read.
// nginx cannot be linked into a unit test, so what the block does at merge
// time is pinned from the source; the derivation itself is tested in
// test/pagespeed/system/site_cache_fragment_test.cc.

#include <cctype>

#include "base/logging.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

GoogleString ReadStrippedSource() {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path =
      StrCat(GTestSrcDir(), "/pagespeed/nginx/ngx_pagespeed.cc");
  CHECK(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  GoogleString out;
  for (const char c : source) {
    if (!isspace(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

TEST(NgxSiteCacheFragmentPinTest, ABlockWithoutAFragmentGetsItsOwn) {
  const GoogleString cc = ReadStrippedSource();
  const size_t merge = cc.find("char*ps_merge_srv_conf(");
  ASSERT_NE(GoogleString::npos, merge);
  const size_t merged =
      cc.find("global_options()->Merge(*cfg_s->options)", merge);
  ASSERT_NE(GoogleString::npos, merged);
  const size_t empty = cc.find("ngx_options->cache_fragment().empty()", merged);
  ASSERT_NE(GoogleString::npos, empty) << "an explicit CacheFragment wins";
  const size_t set = cc.find(
      "ngx_options->set_cache_fragment(DeriveSiteCacheFragment("
      "NgxCollectSiteIdentity(cf,cscf)))",
      empty);
  EXPECT_NE(GoogleString::npos, set);
  EXPECT_NE(GoogleString::npos, cc.find("usescachefragment%s", set))
      << "the value is withheld from the options payload, so it is logged";
  EXPECT_NE(GoogleString::npos, cc.find("inheritsCacheFragment", set));
}

TEST(NgxSiteCacheFragmentPinTest, IdentityIsReadFromTheBlockNotTheRequest) {
  const GoogleString cc = ReadStrippedSource();
  const size_t fn = cc.find("SiteIdentityNgxCollectSiteIdentity(");
  ASSERT_NE(GoogleString::npos, fn);
  const size_t end = cc.find("returnidentity;}", cc.find("ngx_sock_ntop", fn));
  ASSERT_NE(GoogleString::npos, end);
  const GoogleString body = cc.substr(fn, end - fn);
  EXPECT_NE(GoogleString::npos, body.find("cmcf->ports"));
  EXPECT_NE(GoogleString::npos, body.find("servers[s]==cscf"));
  // Patterns count, and the primary name (which can be the machine's) does not.
  EXPECT_NE(GoogleString::npos, body.find("name.insert(0,\"~\")"));
  EXPECT_EQ(GoogleString::npos, body.find("cscf->server_name."));
}

}  // namespace
}  // namespace net_instaweb
