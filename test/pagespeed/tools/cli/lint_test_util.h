// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Shared set-up for the `pagespeed lint` tests: the process-wide
// initialisation the binary does, and a scratch configuration root.

#ifndef TEST_PAGESPEED_TOOLS_CLI_LINT_TEST_UTIL_H_
#define TEST_PAGESPEED_TOOLS_CLI_LINT_TEST_UTIL_H_

#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "pagespeed/apache/apache_config.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/http/domain_registry.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/tools/cli/apache_lint.h"
#include "pagespeed/tools/cli/lint_report.h"
#include "pagespeed/tools/cli/log_capture.h"
#include "pagespeed/tools/cli/value_mask.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace lint {

class LintTestBase : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    SilenceModuleLogging();
    ApacheConfig::Initialize();
    domain_registry::Init();
    HtmlKeywords::Init();
  }
  static void TearDownTestSuite() { ApacheConfig::Terminate(); }

  void SetUp() override {
    thread_system_.reset(Platform::CreateThreadSystem());
    table_ = std::make_unique<ApacheCommandTable>();
    root_ = std::filesystem::path(GTestTempDir()) /
            testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }

  // Writes `content` (raw bytes) to `relative` under the scratch root.
  void WriteFile(const GoogleString& relative, const GoogleString& content) {
    const std::filesystem::path p = root_ / relative;
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << content;
  }

  // Lints and returns the JSON with a fixed module version, so goldens do not
  // change with each release.
  GoogleString Lint(const GoogleString& config,
                    const std::vector<GoogleString>& htaccess = {},
                    bool* readable = nullptr) {
    Report report;
    const bool ok = LintApacheFiles(root_, config, htaccess, *table_, mask_,
                                    thread_system_.get(), &report);
    if (readable != nullptr) {
      *readable = ok;
    }
    last_report_ = report;
    return report.ToJson("<module_version>");
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  std::unique_ptr<ApacheCommandTable> table_;
  ValueMask mask_;
  std::filesystem::path root_;
  Report last_report_;
};

}  // namespace lint
}  // namespace net_instaweb

#endif  // TEST_PAGESPEED_TOOLS_CLI_LINT_TEST_UTIL_H_
