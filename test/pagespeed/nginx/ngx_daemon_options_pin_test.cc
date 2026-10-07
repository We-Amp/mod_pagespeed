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

// The two daemon path options, across the three ports.
//
// The options are parsed and stored but not acted on, so there is no
// behaviour to execute -- and there can be no linkable unit test for the
// parsing either: in this build graph nginx is a headers-only library, so
// anything linking //pagespeed/nginx:nginx_pagespeed picks up undefined
// ngx_* symbols (see the comment at the top of test/pagespeed/nginx/BUILD),
// and the IIS sources build on Windows only. What can and must be pinned
// from here is the CONTRACT between the ports: all three register the same
// option names under the same option ids with the same accessor spellings,
// because a divergence would silently split the daemon's shared cache
// between deployments that believe they share it. Pinned by reading the
// sources, the same idiom as test/pagespeed/apache/daemon_seam_test.cc.
//
// A source pin is weaker than an execution pin and is written knowing that:
// it catches a rename, an id change, a scope or default change, which are
// the realistic regressions, and it does not catch registration code that
// runs and does the wrong thing.

#include <cctype>

#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

const char kNgxOptionsCc[] = "pagespeed/nginx/ngx_rewrite_options.cc";
const char kNgxOptionsH[] = "pagespeed/nginx/ngx_rewrite_options.h";
const char kApacheConfigCc[] = "pagespeed/apache/apache_config.cc";
const char kIisOptionsCc[] = "pagespeed/iis/iis_rewrite_options.cpp";
const char kIisOptionsH[] = "pagespeed/iis/iis_rewrite_options.h";

const char kReservedSentence[] =
    "Reserved: the nginx port records through the daemon but does not serve "
    "through it yet.";
const char kIisReservedSentence[] =
    "Reserved: the IIS port records through the daemon but does not serve "
    "through it yet.";

GoogleString ReadSource(const GoogleString& relative_path) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path = StrCat(GTestSrcDir(), "/", relative_path);
  EXPECT_TRUE(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  return source;
}

// Removes every whitespace byte -- including line breaks and indentation
// -- so the result is identical no matter where clang-format puts a line
// break: the same call wrapped across several lines or written on one
// normalizes to the same string. Used by every pin below except the one
// that needs the reconstructed, human-readable help text (see
// JoinWrappedLiteral), so an expected fragment here must itself contain no
// internal whitespace.
GoogleString StripWhitespace(const GoogleString& text) {
  GoogleString out;
  for (const char c : text) {
    if (!isspace(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

// Collapses each whitespace run to one space, then joins adjacent
// string-literal fragments back together (the pattern clang-format uses to
// wrap a long help string across lines), so the result reads as the
// compiled, concatenated text. Used only by the Reserved-sentence check,
// which needs the real word-separating spaces that StripWhitespace throws
// away. Limitation: this can mis-join a fragment that ends in an escaped
// quote followed by a space -- not present in either source today.
GoogleString JoinWrappedLiteral(const GoogleString& text) {
  GoogleString out;
  bool pending_space = false;
  for (const char c : text) {
    if (isspace(static_cast<unsigned char>(c))) {
      pending_space = true;
    } else {
      if (pending_space && !out.empty()) {
        out.push_back(' ');
      }
      out.push_back(c);
      pending_space = false;
    }
  }
  GlobalReplaceSubstring("\" \"", "", &out);
  return out;
}

class NgxDaemonOptionsPinTest : public testing::Test {};

// The important one: a different option name or id on any side silently
// splits the shared cache between the ports. Pins, on all three sides and
// in the whitespace-free form: the name constant's definition, and the
// full (accessor, id, name) registration triple. Pinning the triple (not
// just each literal separately) catches transposing the two ids, which
// would otherwise leave all four literals present in every file.
TEST_F(NgxDaemonOptionsPinTest, NamesAndIdsMatchAcrossPorts) {
  const GoogleString nginx = StripWhitespace(ReadSource(kNgxOptionsCc));
  const GoogleString apache = StripWhitespace(ReadSource(kApacheConfigCc));
  const GoogleString iis = StripWhitespace(ReadSource(kIisOptionsCc));

  const char* const kNameDefinitions[] = {
      "kDaemonSocketPath[]=\"DaemonSocketPath\"",
      "kDaemonVolumePath[]=\"DaemonVolumePath\"",
  };
  for (const char* literal : kNameDefinitions) {
    EXPECT_NE(GoogleString::npos, nginx.find(literal))
        << literal << " is not defined in ngx_rewrite_options.cc";
    EXPECT_NE(GoogleString::npos, apache.find(literal))
        << literal
        << " is no longer defined in apache_config.cc; this pin "
           "compares against the Apache registration, so check both sides "
           "before changing it";
    EXPECT_NE(GoogleString::npos, iis.find(literal))
        << literal
        << " is not defined in iis_rewrite_options.cpp; the three ports "
           "must define the same names";
  }

  const char* const kNgxTriples[] = {
      "&NgxRewriteOptions::daemon_socket_path_,\"dmsp\",kDaemonSocketPath",
      "&NgxRewriteOptions::daemon_volume_path_,\"dmvp\",kDaemonVolumePath",
  };
  for (const char* literal : kNgxTriples) {
    EXPECT_NE(GoogleString::npos, nginx.find(literal))
        << literal
        << " is not registered in ngx_rewrite_options.cc; the "
           "ports must bind the same name to the same id or the shared "
           "cache splits";
  }

  const char* const kApacheTriples[] = {
      "&ApacheConfig::daemon_socket_path_,\"dmsp\",kDaemonSocketPath",
      "&ApacheConfig::daemon_volume_path_,\"dmvp\",kDaemonVolumePath",
  };
  for (const char* literal : kApacheTriples) {
    EXPECT_NE(GoogleString::npos, apache.find(literal))
        << literal
        << " is no longer registered in apache_config.cc; this "
           "pin compares against the Apache registration, so check both "
           "sides before changing it";
  }

  const char* const kIisTriples[] = {
      "&IisRewriteOptions::daemon_socket_path_,\"dmsp\",kDaemonSocketPath",
      "&IisRewriteOptions::daemon_volume_path_,\"dmvp\",kDaemonVolumePath",
  };
  for (const char* literal : kIisTriples) {
    EXPECT_NE(GoogleString::npos, iis.find(literal))
        << literal
        << " is not registered in iis_rewrite_options.cpp; the three "
           "ports must bind the same name to the same id or the shared "
           "cache splits";
  }
}

// Finds `head` -- the structural start of an add_ngx_option call, in the
// whitespace-free form -- and asserts that the call it opens ends `,true)`
// before the next `);`. Deliberately does not pin the help text in
// between: only default "", kServerScope and safe_to_print are the
// contract this case checks; the help text has its own case below.
void ExpectRegistrationEndsSafeToPrint(const GoogleString& text,
                                       const GoogleString& head) {
  const size_t pos = text.find(head);
  ASSERT_NE(GoogleString::npos, pos)
      << head
      << " registration changed; default, id and scope are part "
         "of the pinned contract";
  const size_t end = text.find(");", pos);
  ASSERT_NE(GoogleString::npos, end) << "no closing `);` found after " << head;
  const GoogleString call = text.substr(pos, end - pos);
  EXPECT_TRUE(strings::EndsWith(call, ",true"))
      << head
      << " must end `, true)`; safe_to_print is part of the "
         "pinned contract, got: "
      << call;
}

TEST_F(NgxDaemonOptionsPinTest, RegistrationsKeepDefaultScopeAndPrintability) {
  const GoogleString nginx = StripWhitespace(ReadSource(kNgxOptionsCc));
  ExpectRegistrationEndsSafeToPrint(
      nginx,
      "add_ngx_option(\"\",&NgxRewriteOptions::daemon_socket_path_,"
      "\"dmsp\",kDaemonSocketPath,kServerScope,");
  ExpectRegistrationEndsSafeToPrint(
      nginx,
      "add_ngx_option(\"\",&NgxRewriteOptions::daemon_volume_path_,"
      "\"dmvp\",kDaemonVolumePath,kServerScope,");

  // IIS registers the same options at its own process scope (the parser
  // refuses them inside a match block, tested on Windows in
  // test/pagespeed/iis/iis_configuration_test.cc).
  const GoogleString iis = StripWhitespace(ReadSource(kIisOptionsCc));
  ExpectRegistrationEndsSafeToPrint(
      iis,
      "add_iis_option(\"\",&IisRewriteOptions::daemon_socket_path_,"
      "\"dmsp\",kDaemonSocketPath,kProcessScopeStrict,");
  ExpectRegistrationEndsSafeToPrint(
      iis,
      "add_iis_option(\"\",&IisRewriteOptions::daemon_volume_path_,"
      "\"dmvp\",kDaemonVolumePath,kProcessScopeStrict,");
}

TEST_F(NgxDaemonOptionsPinTest, HeadersDeclareTheSameAccessorsAcrossPorts) {
  const char* const kAccessorSpellings[] = {
      "constGoogleString&daemon_socket_path()const",
      "constGoogleString&daemon_volume_path()const",
  };
  const GoogleString nginx_header = StripWhitespace(ReadSource(kNgxOptionsH));
  const GoogleString iis_header = StripWhitespace(ReadSource(kIisOptionsH));
  for (const char* literal : kAccessorSpellings) {
    EXPECT_NE(GoogleString::npos, nginx_header.find(literal))
        << literal
        << " was renamed; it must keep the Apache port's accessor name -- "
           "compare by hand against pagespeed/apache/apache_config.h";
    EXPECT_NE(GoogleString::npos, iis_header.find(literal))
        << literal
        << " was renamed in the IIS port; the three ports must spell the "
           "accessor identically";
  }
}

// The nginx daemon path records and serves through the daemon, and ships
// documented: its help strings point at the nginx guide, and the old
// Reserved sentence must not come back.
TEST_F(NgxDaemonOptionsPinTest, NgxHelpStringsPointAtTheGuideNotReserved) {
  const GoogleString nginx = JoinWrappedLiteral(ReadSource(kNgxOptionsCc));
  EXPECT_EQ(0, CountSubstring(nginx, kReservedSentence))
      << "the nginx daemon options are documented and working; the Reserved "
         "sentence says otherwise";
  EXPECT_EQ(2, CountSubstring(nginx, "See docs/install-nginx.md."))
      << "both nginx daemon option help strings must point at the nginx "
         "guide";
}

// The IIS daemon path records and serves, and ships documented: its help
// strings point at the IIS guide, and the old Reserved sentence must not
// come back.
TEST_F(NgxDaemonOptionsPinTest, IisHelpStringsPointAtTheGuideNotReserved) {
  const GoogleString iis = JoinWrappedLiteral(ReadSource(kIisOptionsCc));
  EXPECT_EQ(0, CountSubstring(iis, kIisReservedSentence))
      << "the IIS daemon options are documented and working; the Reserved "
         "sentence says otherwise";
  EXPECT_EQ(2, CountSubstring(iis, "See docs/daemon-adapter-iis.md."))
      << "both IIS daemon option help strings must point at the IIS guide";
}

// The stored-copies switch: the same name, id, default and scope on the two
// ports that serve them, kept out of the configuration's signature on both,
// and absent on IIS, which serves the uncompressed copy in this release.
TEST_F(NgxDaemonOptionsPinTest, TheStoredEncodingsSwitchMatchesAcrossPorts) {
  const GoogleString nginx = StripWhitespace(ReadSource(kNgxOptionsCc));
  const GoogleString apache = StripWhitespace(ReadSource(kApacheConfigCc));
  const GoogleString iis = StripWhitespace(ReadSource(kIisOptionsCc));

  const char kName[] =
      "kDaemonServeStoredEncodings[]=\"DaemonServeStoredEncodings\"";
  EXPECT_NE(GoogleString::npos, nginx.find(kName));
  EXPECT_NE(GoogleString::npos, apache.find(kName));

  const char kNgxRegistration[] =
      "add_ngx_option(false,&NgxRewriteOptions::daemon_serve_stored_"
      "encodings_,\"dsse\",kDaemonServeStoredEncodings,kServerScope,";
  const char kApacheRegistration[] =
      "AddApacheProperty(false,&ApacheConfig::daemon_serve_stored_encodings_,"
      "\"dsse\",kDaemonServeStoredEncodings,";
  const size_t ngx_at = nginx.find(kNgxRegistration);
  const size_t apache_at = apache.find(kApacheRegistration);
  ASSERT_NE(GoogleString::npos, ngx_at)
      << "the nginx registration changed: default, id, name and scope are "
         "the contract";
  ASSERT_NE(GoogleString::npos, apache_at)
      << "the Apache registration changed: default, id and name are the "
         "contract";

  // Excluded from signature computation, by the call that directly follows
  // each registration (before the next one).
  const size_t ngx_next = nginx.find("add_ngx_option(", ngx_at + 1);
  const GoogleString ngx_tail = nginx.substr(ngx_at, ngx_next - ngx_at);
  EXPECT_NE(GoogleString::npos,
            ngx_tail.find("ngx_properties_->property(ngx_properties_->size()-"
                          "1)->set_do_not_use_for_signature_computation("
                          "true);"))
      << "the nginx switch participates in the configuration's signature";
  const size_t apache_next = apache.find("AddDeprecatedProperty(", apache_at);
  const GoogleString apache_tail =
      apache.substr(apache_at, apache_next - apache_at);
  EXPECT_NE(GoogleString::npos,
            apache_tail.find("apache_properties_->property(apache_properties_"
                             "->size()-1)->set_do_not_use_for_signature_"
                             "computation(true);"))
      << "the Apache switch participates in the configuration's signature";

  EXPECT_EQ(GoogleString::npos, iis.find("DaemonServeStoredEncodings"))
      << "the IIS port registers the switch; it serves the uncompressed copy "
         "in this release and its docs say so";
}

}  // namespace
}  // namespace net_instaweb
