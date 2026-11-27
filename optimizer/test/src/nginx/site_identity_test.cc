// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the cache keyspace (site) of an nginx server block
// (src/nginx/site_identity.h).

#include "src/nginx/site_identity.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "src/proto/worker_ipc.h"

using pagespeed::DeriveServerBlockSite;
using pagespeed::IsSiteId;

namespace {

TEST(ServerBlockSite, IsAValidSiteId) {
  EXPECT_TRUE(IsSiteId(DeriveServerBlockSite({"example.com"}, {"0.0.0.0:80"})));
  // A block with no name of its own still has a site.
  EXPECT_TRUE(IsSiteId(DeriveServerBlockSite({""}, {"0.0.0.0:80"})));
  EXPECT_TRUE(IsSiteId(DeriveServerBlockSite({}, {})));
}

TEST(ServerBlockSite, IsStableForTheSameBlock) {
  // Pinned: a change here gives every block a new, empty keyspace.
  EXPECT_EQ(DeriveServerBlockSite({"localhost"}, {"0.0.0.0:1984"}),
            DeriveServerBlockSite({"localhost"}, {"0.0.0.0:1984"}));
  EXPECT_EQ("03ff50e08dd18d0c63578be04db33b077508dd979e489fd432f5e4122b7f83cd",
            DeriveServerBlockSite({"localhost"}, {"0.0.0.0:1984"}));
}

TEST(ServerBlockSite, IgnoresOrderRepeatsAndNameCase) {
  const std::string site = DeriveServerBlockSite(
      {"example.com", "www.example.com"}, {"0.0.0.0:80", "[::]:80"});
  EXPECT_EQ(site, DeriveServerBlockSite(
                      {"WWW.Example.com", "example.com", "example.com"},
                      {"[::]:80", "0.0.0.0:80", "[::]:80"}));
}

TEST(ServerBlockSite, BlocksWithTheSameNamesOnOtherListenersDiffer) {
  EXPECT_NE(DeriveServerBlockSite({"example.com"}, {"0.0.0.0:80"}),
            DeriveServerBlockSite({"example.com"}, {"0.0.0.0:8080"}));
  EXPECT_NE(DeriveServerBlockSite({"example.com"}, {"0.0.0.0:80"}),
            DeriveServerBlockSite({"example.com"}, {"127.0.0.1:80"}));
  EXPECT_NE(
      DeriveServerBlockSite({"example.com"}, {"0.0.0.0:80"}),
      DeriveServerBlockSite({"example.com"}, {"0.0.0.0:80", "0.0.0.0:8080"}));
}

TEST(ServerBlockSite, BlocksOnTheSameListenerWithOtherNamesDiffer) {
  EXPECT_NE(DeriveServerBlockSite({"a.example"}, {"0.0.0.0:80"}),
            DeriveServerBlockSite({"b.example"}, {"0.0.0.0:80"}));
  EXPECT_NE(DeriveServerBlockSite({"a.example"}, {"0.0.0.0:80"}),
            DeriveServerBlockSite({"a.example", "b.example"}, {"0.0.0.0:80"}));
  EXPECT_NE(DeriveServerBlockSite({""}, {"0.0.0.0:80"}),
            DeriveServerBlockSite({"_"}, {"0.0.0.0:80"}));
}

TEST(ServerBlockSite, NamesAndAddressesDoNotRunTogether) {
  // The same strings, split differently between the two lists.
  EXPECT_NE(DeriveServerBlockSite({"a", "b"}, {}),
            DeriveServerBlockSite({"a"}, {"b"}));
  EXPECT_NE(DeriveServerBlockSite({"ab"}, {}),
            DeriveServerBlockSite({"a", "b"}, {}));
}

TEST(ServerBlockSite, RegularExpressionNamesKeepTheirCase) {
  EXPECT_NE(DeriveServerBlockSite({"~^A\\.example$"}, {"0.0.0.0:80"}),
            DeriveServerBlockSite({"~^a\\.example$"}, {"0.0.0.0:80"}));
  // A regular expression is not the plain name with the same text.
  EXPECT_NE(DeriveServerBlockSite({"~example"}, {"0.0.0.0:80"}),
            DeriveServerBlockSite({"example"}, {"0.0.0.0:80"}));
}

}  // namespace
