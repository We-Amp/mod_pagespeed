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

// The names an Apache virtual host is configured with, read from its own
// server record and the main server's, and the host a serve on it is
// recorded under.  Built on hand-made server_recs in the states httpd
// leaves them in after configuration: a virtual host without ServerName on
// a default address shares the main server's server_hostname POINTER; one
// without a usable address gets a placeholder name; one on its own address
// gets the reverse name of that address; a configured ServerName is always
// its own allocation.  Whether the configuration STATES a ServerName for a
// record is read from hand-made directive trees in the shape httpd's
// configuration reader leaves them: a <VirtualHost> node with its body as
// children, included and conditional directives already spliced in.

#include <cstring>
#include <initializer_list>
#include <set>

#include "apr_pools.h"   // NOLINT
#include "apr_tables.h"  // NOLINT
#include "pagespeed/apache/apache_httpd_includes.h"
#include "http_config.h"  // NOLINT
#include "pagespeed/apache/apache_server_context.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/system/serve_host_names.h"
#include "test/pagespeed/apache/mock_apache.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

class ApacheHostNamesTest : public testing::Test {
 protected:
  void SetUp() override {
    MockApache::Initialize();  // apr_initialize underneath.
    apr_pool_create(&pool_, nullptr);
    memset(&main_, 0, sizeof(main_));
    main_.server_hostname = main_name_;
    main_.is_virtual = 0;
    memset(&server_, 0, sizeof(server_));
    server_.server_hostname = own_name_;
    server_.is_virtual = 1;
    server_.names = Names({"Static.Example.TEST", "example.test"});
    server_.wild_names = Names({"*.example.test"});
    server_.defn_name = "/etc/apache2/sites/example.conf";
    server_.defn_line_number = 1;
    // Both records state their ServerName unless a test says otherwise.
    stated_ = {&main_, &server_};
  }

  void TearDown() override {
    apr_pool_destroy(pool_);
    MockApache::Terminate();
  }

  apr_array_header_t* Names(std::initializer_list<const char*> names) {
    apr_array_header_t* array =
        apr_array_make(pool_, 4, sizeof(const char*));
    for (const char* name : names) {
      APR_ARRAY_PUSH(array, const char*) = name;
    }
    return array;
  }

  ConfiguredHostNames Configured(const server_rec* server,
                                 const server_rec* main_server) {
    return ApacheConfiguredHostNames(server, main_server, stated_);
  }

  ConfiguredHostNames OwnNames() { return Configured(&server_, &main_); }

  // One directive node, as httpd's configuration reader makes it.
  ap_directive_t* Node(const char* directive, const char* filename,
                       int line_num) {
    ap_directive_t* node = static_cast<ap_directive_t*>(
        apr_pcalloc(pool_, sizeof(ap_directive_t)));
    node->directive = directive;
    node->args = "";
    node->filename = filename;
    node->line_num = line_num;
    return node;
  }

  // Links `nodes` as siblings and, when `parent` is given, as its children.
  // Returns the first node.
  ap_directive_t* List(ap_directive_t* parent,
                       std::initializer_list<ap_directive_t*> nodes) {
    ap_directive_t* first = nullptr;
    ap_directive_t* last = nullptr;
    for (ap_directive_t* node : nodes) {
      node->parent = parent;
      if (last == nullptr) {
        first = node;
      } else {
        last->next = node;
      }
      last = node;
    }
    if (parent != nullptr) {
      parent->first_child = first;
    }
    return first;
  }

  // A <VirtualHost> node at `filename`:`line_num` whose body is `children`.
  ap_directive_t* VirtualHost(const char* filename, int line_num,
                              std::initializer_list<const char*> children) {
    ap_directive_t* vhost = Node("<VirtualHost", filename, line_num);
    ap_directive_t* first = nullptr;
    ap_directive_t* last = nullptr;
    int line = line_num;
    for (const char* child : children) {
      ap_directive_t* node = Node(child, filename, ++line);
      node->parent = vhost;
      if (last == nullptr) {
        first = node;
      } else {
        last->next = node;
      }
      last = node;
    }
    vhost->first_child = first;
    return vhost;
  }

  void DefinedAt(server_rec* server, const char* filename, int line_num) {
    server->defn_name = filename;
    server->defn_line_number = line_num;
  }

  bool States(const ap_directive_t* tree, const server_rec* server) {
    return ApacheConfigStatesServerName(tree, server, &main_);
  }

  // Separate allocations, as httpd's ServerName parser makes them.
  char main_name_[32] = "www.main.test";
  char own_name_[32] = "www.example.test";
  char same_text_as_main_[32] = "www.main.test";
  apr_pool_t* pool_ = nullptr;
  server_rec main_;
  server_rec server_;
  std::set<const server_rec*> stated_;
};

TEST_F(ApacheHostNamesTest, TheServerNameAndExactAliasesAreTheNames) {
  const ConfiguredHostNames names = OwnNames();
  EXPECT_EQ("www.example.test", names.primary);
  ASSERT_EQ(2u, names.aliases.size());
  EXPECT_EQ("Static.Example.TEST", names.aliases[0]);
  EXPECT_EQ("example.test", names.aliases[1]);
}

TEST_F(ApacheHostNamesTest, AnExactAliasIsTheRow) {
  EXPECT_EQ("static.example.test",
            VouchedServeHost("STATIC.example.test:8443", OwnNames()));
  EXPECT_EQ("www.example.test", VouchedServeHost("www.example.test.", OwnNames()));
}

TEST_F(ApacheHostNamesTest, AWildcardAliasCountsUnderTheServerName) {
  EXPECT_EQ("www.example.test",
            VouchedServeHost("shop.example.test", OwnNames()));
}

TEST_F(ApacheHostNamesTest, AnUnknownHostCountsUnderTheVirtualHostsOwnName) {
  EXPECT_EQ("www.example.test",
            VouchedServeHost("visitor-chosen.test", OwnNames()));
  EXPECT_EQ("www.example.test", VouchedServeHost("", OwnNames()));
}

TEST_F(ApacheHostNamesTest, AnInheritedNameIsNoName) {
  // <VirtualHost *:80> without ServerName: httpd gives it the main server's
  // server_hostname pointer.  It has no primary name of its own -- not even
  // for a request that names the main site -- but its exact aliases count.
  server_.server_hostname = main_.server_hostname;
  const ConfiguredHostNames names = OwnNames();
  EXPECT_EQ("", names.primary);
  EXPECT_EQ("", VouchedServeHost("visitor-chosen.test", names));
  EXPECT_EQ("", VouchedServeHost("www.main.test", names));
  EXPECT_EQ("static.example.test",
            VouchedServeHost("static.example.test", names));
}

TEST_F(ApacheHostNamesTest, AnOwnServerNameSpeltLikeTheMainOneCounts) {
  // Configured, not inherited: its own allocation, the same text.
  server_.server_hostname = same_text_as_main_;
  EXPECT_EQ("www.main.test", OwnNames().primary);
}

TEST_F(ApacheHostNamesTest, TheMainServersOwnNameCounts) {
  // The main server's name counts when its configuration states it.
  const ConfiguredHostNames names = Configured(&main_, &main_);
  EXPECT_EQ("www.main.test", names.primary);
  EXPECT_EQ("www.main.test", VouchedServeHost("visitor-chosen.test", names));
}

TEST_F(ApacheHostNamesTest, PlaceholderNamesAreNoNames) {
  for (const char* placeholder :
       {"bogus_host_without_forward_dns", "bogus_host_without_reverse_dns"}) {
    char name[64];
    strncpy(name, placeholder, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    server_.server_hostname = name;
    EXPECT_EQ("", OwnNames().primary) << placeholder;
    EXPECT_EQ("", VouchedServeHost("visitor-chosen.test",
                                   Configured(&server_, nullptr)))
        << placeholder;
    main_.server_hostname = name;
    EXPECT_EQ("", Configured(&main_, &main_).primary)
        << placeholder;
    EXPECT_EQ("", VouchedServeHost("visitor-chosen.test",
                                   Configured(&main_, &main_)))
        << placeholder;
    main_.server_hostname = main_name_;
  }
}

TEST_F(ApacheHostNamesTest, AnUnknownMainRecordGivesAVirtualHostNoName) {
  // Fail safe: without a (non-virtual) main record, inheritance cannot be
  // told apart, so a virtual host has no primary name.
  EXPECT_EQ("", Configured(&server_, nullptr).primary);
  EXPECT_EQ("", Configured(&server_, &server_).primary);
  EXPECT_EQ("static.example.test",
            VouchedServeHost("static.example.test",
                             Configured(&server_, nullptr)));
}

TEST_F(ApacheHostNamesTest, NoRecordGivesNoNames) {
  const ConfiguredHostNames none = Configured(nullptr, &main_);
  EXPECT_EQ("", none.primary);
  EXPECT_TRUE(none.aliases.empty());
}

TEST_F(ApacheHostNamesTest, ADerivedNameIsNoName) {
  // A name httpd derived -- the reverse name of a virtual host's own
  // address, or the machine name for a main server without ServerName -- is
  // its own allocation, like a configured one; only the configuration tells
  // them apart.  Without a stated ServerName there is no primary name, but
  // the exact aliases still count.
  stated_.clear();
  const ConfiguredHostNames names = OwnNames();
  EXPECT_EQ("", names.primary);
  EXPECT_EQ("", VouchedServeHost("www.example.test", names));
  EXPECT_EQ("", VouchedServeHost("visitor-chosen.test", names));
  EXPECT_EQ("static.example.test",
            VouchedServeHost("static.example.test", names));
  EXPECT_EQ("", Configured(&main_, &main_).primary);
  EXPECT_EQ("", VouchedServeHost("www.main.test", Configured(&main_, &main_)));
}

TEST_F(ApacheHostNamesTest, AVirtualHostWithServerNameStatesIt) {
  const ap_directive_t* tree = List(
      nullptr, {Node("Listen", "/etc/apache2/apache2.conf", 1),
                VirtualHost("/etc/apache2/sites/example.conf", 1,
                            {"ServerName", "ServerAlias", "DocumentRoot"})});
  EXPECT_TRUE(States(tree, &server_));
}

TEST_F(ApacheHostNamesTest, AVirtualHostWithoutServerNameDoesNotStateIt) {
  for (std::initializer_list<const char*> body :
       {std::initializer_list<const char*>{"DocumentRoot"},
        std::initializer_list<const char*>{"ServerAlias", "DocumentRoot"},
        std::initializer_list<const char*>{}}) {
    const ap_directive_t* tree = List(
        nullptr,
        {VirtualHost("/etc/apache2/sites/example.conf", 1, body)});
    EXPECT_FALSE(States(tree, &server_));
  }
}

TEST_F(ApacheHostNamesTest, VirtualHostsInOneFileAreToldApartByLine) {
  const char* file = "/etc/apache2/sites/example.conf";
  const ap_directive_t* tree =
      List(nullptr, {VirtualHost(file, 10, {"ServerName", "DocumentRoot"}),
                     VirtualHost(file, 20, {"DocumentRoot"})});
  DefinedAt(&server_, file, 10);
  EXPECT_TRUE(States(tree, &server_));
  DefinedAt(&server_, file, 20);
  EXPECT_FALSE(States(tree, &server_));
  DefinedAt(&server_, file, 30);
  EXPECT_FALSE(States(tree, &server_));
}

TEST_F(ApacheHostNamesTest, VirtualHostsOnTheSameLineAreToldApartByFile) {
  const ap_directive_t* tree = List(
      nullptr, {VirtualHost("/etc/apache2/sites/a.conf", 5, {"ServerName"}),
                VirtualHost("/etc/apache2/sites/b.conf", 5, {"DocumentRoot"})});
  // The record's file name is its own string, equal in text only.
  char a[64] = "/etc/apache2/sites/a.conf";
  char b[64] = "/etc/apache2/sites/b.conf";
  DefinedAt(&server_, a, 5);
  EXPECT_TRUE(States(tree, &server_));
  DefinedAt(&server_, b, 5);
  EXPECT_FALSE(States(tree, &server_));
}

TEST_F(ApacheHostNamesTest, DirectiveNamesMatchInAnyCase) {
  ap_directive_t* vhost = Node("<virtualhost", server_.defn_name, 1);
  List(vhost, {Node("servername", server_.defn_name, 2)});
  EXPECT_TRUE(States(List(nullptr, {vhost}), &server_));
}

TEST_F(ApacheHostNamesTest, TheMainRecordStatesATopLevelServerName) {
  EXPECT_TRUE(States(
      List(nullptr, {Node("Listen", "/etc/apache2/apache2.conf", 1),
                     Node("ServerName", "/etc/apache2/apache2.conf", 2)}),
      &main_));
  EXPECT_FALSE(States(
      List(nullptr,
           {Node("Listen", "/etc/apache2/apache2.conf", 1),
            VirtualHost("/etc/apache2/sites/example.conf", 1,
                        {"ServerName"})}),
      &main_));
}

TEST_F(ApacheHostNamesTest, ATopLevelServerNameDoesNotStateAVirtualHosts) {
  const ap_directive_t* tree = List(
      nullptr, {Node("ServerName", "/etc/apache2/apache2.conf", 1),
                VirtualHost("/etc/apache2/sites/example.conf", 1,
                            {"DocumentRoot"})});
  EXPECT_FALSE(States(tree, &server_));
  EXPECT_TRUE(States(tree, &main_));
}

TEST_F(ApacheHostNamesTest, WithoutATreeOrARecordNothingIsStated) {
  EXPECT_FALSE(States(nullptr, &server_));
  EXPECT_FALSE(States(nullptr, &main_));
  const ap_directive_t* tree = List(
      nullptr, {Node("ServerName", "/etc/apache2/apache2.conf", 1),
                VirtualHost("/etc/apache2/sites/example.conf", 1,
                            {"ServerName"})});
  EXPECT_FALSE(States(tree, nullptr));
  server_.defn_name = nullptr;
  EXPECT_FALSE(States(tree, &server_));
  // Without a (non-virtual) main record, the main record is not known.
  EXPECT_FALSE(ApacheConfigStatesServerName(tree, &main_, nullptr));
  EXPECT_FALSE(ApacheConfigStatesServerName(tree, &main_, &server_));
}

TEST_F(ApacheHostNamesTest, EveryRecordIsLookedUpOnceForTheConfiguration) {
  server_rec plain;
  memset(&plain, 0, sizeof(plain));
  plain.is_virtual = 1;
  plain.server_hostname = own_name_;
  DefinedAt(&plain, "/etc/apache2/sites/example.conf", 20);
  main_.next = &server_;
  server_.next = &plain;
  const ap_directive_t* tree = List(
      nullptr,
      {Node("ServerName", "/etc/apache2/apache2.conf", 1),
       VirtualHost("/etc/apache2/sites/example.conf", 1, {"ServerName"}),
       VirtualHost("/etc/apache2/sites/example.conf", 20, {"DocumentRoot"})});
  EXPECT_EQ((std::set<const server_rec*>{&main_, &server_}),
            ApacheStatedServerNamesFromTree(tree, &main_));
  // A site's plain and TLS virtual hosts may share one ServerName: each
  // record that states it counts.
  const ap_directive_t* both = List(
      nullptr,
      {VirtualHost("/etc/apache2/sites/example.conf", 1, {"ServerName"}),
       VirtualHost("/etc/apache2/sites/example.conf", 20, {"ServerName"})});
  EXPECT_EQ((std::set<const server_rec*>{&server_, &plain}),
            ApacheStatedServerNamesFromTree(both, &main_));
  EXPECT_TRUE(ApacheStatedServerNamesFromTree(nullptr, &main_).empty());
  EXPECT_TRUE(ApacheStatedServerNamesFromTree(tree, nullptr).empty());
}

}  // namespace
}  // namespace net_instaweb
