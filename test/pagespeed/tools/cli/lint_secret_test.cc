// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Rule x secret-option matrix: every directive whose values are masked is
// written with unique marker values in every context (server, <VirtualHost>,
// a directory section, <ModPagespeedIf>, .htaccess), with the right number
// of arguments, one too many and one too few, and with values that make the
// option parser fail (a URL, a number with junk).  Then the malformed forms:
// the value glued onto the name ("Name=value", "Name:value", "Namevalue"),
// a <ModPagespeedIf> argument, section names, other modules' directives
// inside <ModPagespeedIf>, and filter names in a filter list.  No marker may
// appear in the JSON or the text output.

#include "test/pagespeed/tools/cli/lint_test_util.h"

namespace net_instaweb {
namespace lint {
namespace {

constexpr char kMarker[] = "SEKRIT";

class LintSecretTest : public LintTestBase {
 protected:
  GoogleString Mark() { return StrCat(kMarker, next_++, "Q"); }

  void ExpectNoMarker() {
    const GoogleString json = last_report_.ToJson("<module_version>");
    const GoogleString text = last_report_.ToText();
    EXPECT_EQ(GoogleString::npos, json.find(kMarker))
        << json.substr(json.find(kMarker) > 200 ? json.find(kMarker) - 200 : 0,
                       400);
    EXPECT_EQ(GoogleString::npos, text.find(kMarker));
  }

  int next_ = 0;
};

TEST_F(LintSecretTest, NoMaskedValueIsEverPrinted) {
  std::vector<std::pair<GoogleString, int>> masked;  // name, arguments
  for (const ApacheCommand& c : table_->commands()) {
    if (c.handler == apache_directive::Handler::kParseScope ||
        mask_.ValuesAreSafe(c.name)) {
      continue;
    }
    int n = 1;
    if (c.args == apache_directive::Args::kTake2 ||
        c.args == apache_directive::Args::kTake23) {
      n = 2;
    } else if (c.args == apache_directive::Args::kTake3) {
      n = 3;
    }
    masked.push_back({c.name, n});
  }
  // The secret-valued options named in the design must be among them.
  for (const char* secret :
       {"ModPagespeedUrlSigningKey", "ModPagespeedRequestOptionOverride",
        "ModPagespeedCustomFetchHeader", "ModPagespeedRedisServer",
        "ModPagespeedMemcachedServers", "ModPagespeedProxyAuth"}) {
    bool found = false;
    for (const auto& m : masked) {
      found = found || StringCaseEqual(m.first, secret);
    }
    EXPECT_TRUE(found) << secret << " is not masked";
  }
  ASSERT_GT(masked.size(), 50u);

  GoogleString server;
  GoogleString htaccess;
  int lines = 0;
  for (const auto& [name, n] : masked) {
    for (int shape = 0; shape < 3; ++shape) {
      for (int nargs : {n, n + 1, n - 1}) {
        GoogleString line = name;
        for (int j = 0; j < nargs; ++j) {
          GoogleString v = Mark();
          if (shape == 1) {
            v = StrCat("http://user:", v, "@host/", Mark());
          } else if (shape == 2) {
            v = StrCat("9", v);
          }
          StrAppend(&line, " \"", v, "\"");
        }
        StrAppend(&server, line, "\n");
        StrAppend(&htaccess, line, "\n");
        ++lines;
      }
    }
    // The malformed forms.
    StrAppend(&server, name, "=", Mark(), "\n", name, ":", Mark(), " x\n",
              name, Mark(), " ", Mark(), "\n");
    StrAppend(&htaccess, name, "=", Mark(), "\n");
  }
  const GoogleString config = StrCat(
      "ModPagespeed on\n", server, "<VirtualHost *:80>\n", server,
      "  <Directory /var/www>\n", server, "  </Directory>\n</VirtualHost>\n",
      "<ModPagespeedIf spdy>\n", server, "  ", Mark(), " value\n  <", Mark(),
      " x>\n  </", Mark(), ">\n</ModPagespeedIf>\n<ModPagespeedIf ", Mark(),
      ">\n</ModPagespeedIf>\n</", Mark(), ">\nModPagespeedEnableFilters ",
      "rewrite_css,", Mark(), ",insert_ga\nModPagespeedDisableFilters ", Mark(),
      "\nModPagespeedForbidFilters x", Mark(), "\nModPagespeed", Mark(), "\n",
      "ModPagespeed ", Mark(), "\n");
  WriteFile("secrets.conf", config);
  WriteFile("secrets.htaccess", htaccess);

  Lint("secrets.conf", {"secrets.htaccess"});
  EXPECT_GT(last_report_.findings().size(), static_cast<size_t>(lines));
  ExpectNoMarker();
}

TEST_F(LintSecretTest, ShowValuesShowsThem) {
  WriteFile("one.conf", StrCat("ModPagespeedUrlSigningKey ", Mark(), " x\n"));
  mask_.ShowAll();
  Lint("one.conf");
  EXPECT_NE(GoogleString::npos,
            last_report_.ToJson("<module_version>").find(kMarker));
}

TEST_F(LintSecretTest, SafeValuesAreShown) {
  WriteFile("one.conf", "ModPagespeedRewriteLevel Bogus\n");
  Lint("one.conf");
  ASSERT_EQ(1u, last_report_.findings().size());
  EXPECT_EQ(std::vector<GoogleString>({"Bogus"}),
            last_report_.findings()[0].args);
}

}  // namespace
}  // namespace lint
}  // namespace net_instaweb
