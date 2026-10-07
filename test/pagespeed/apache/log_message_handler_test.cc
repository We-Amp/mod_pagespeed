// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Tests for the Apache log routing of base/logging.h severities:
//  - VLOG(n) carries the negative severity -n (glog convention) and maps to
//    APLOG_DEBUG, so verbose diagnostics (e.g. the CSS parser's notes about
//    unsupported modern syntax) only surface when the server's LogLevel is
//    debug — not at the default warn/notice/info.
//  - LOG(INFO/WARNING/ERROR/FATAL) keep their established Apache levels.
//  - ~LogMessage does not mirror verbose messages to the process default
//    spdlog logger: that console output is not level-filtered by the hosting
//    server, so mirroring it flooded server logs at the default LogLevel.
//
// The routing tests exercise the pre-shutdown spdlog path, so nothing in
// this test binary may call ShutDownLogging() (the flag is sticky).

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/logging.h"
#include "pagespeed/apache/apache_httpd_includes.h"
#include "pagespeed/apache/apache_logging_includes.h"
#include "pagespeed/apache/log_message_handler.h"
#include "spdlog/sinks/base_sink.h"
#include "spdlog/spdlog.h"
#include "test/pagespeed/kernel/base/gtest.h"

// Referencing GetApacheLogLevel below pulls log_message_handler.o (an archive
// member of apache_core) into this binary; its AddServerConfig dereferences
// pagespeed_module via the static aplog_module_index that APLOG_USE_MODULE
// (apache_logging_includes.h) plants in every TU. The real definition lives
// in mod_instaweb.cc, which apache_core excludes to keep test binaries free
// of the module's static ApacheProcessContext. Define an inert, never
// registered module struct so the link resolves; nothing here runs Apache.
extern "C" {
module pagespeed_module = {};
}  // extern "C"

namespace {

using net_instaweb::log_message_handler::GetApacheLogLevel;

constexpr char kVlogMarker[] = "vlog-routing-marker";
constexpr char kInfoMarker[] = "info-routing-marker";

TEST(GetApacheLogLevelTest, LogSeveritiesKeepTheirApacheLevels) {
  EXPECT_EQ(APLOG_NOTICE, GetApacheLogLevel(logging::LOG_INFO));
  EXPECT_EQ(APLOG_WARNING, GetApacheLogLevel(logging::LOG_WARNING));
  EXPECT_EQ(APLOG_ERR, GetApacheLogLevel(logging::LOG_ERROR));
  EXPECT_EQ(APLOG_ALERT, GetApacheLogLevel(logging::LOG_FATAL));
}

TEST(GetApacheLogLevelTest, VlogSeveritiesMapToApacheDebug) {
  EXPECT_EQ(APLOG_DEBUG, GetApacheLogLevel(-1));  // VLOG(1)
  EXPECT_EQ(APLOG_DEBUG, GetApacheLogLevel(-2));  // VLOG(2)
}

// Records the severity of every message delivered through the LogSink
// interface, the seam the server integrations (Apache, nginx, IIS) use.
class SeverityCapturingSink : public pagespeed_logging::LogSink {
 public:
  void send(int severity, const char* /*full_filename*/,
            const char* /*base_filename*/, int /*line*/, const char* message,
            size_t message_len) override {
    severities_.push_back(severity);
    messages_.emplace_back(message, message_len);
  }

  const std::vector<int>& severities() const { return severities_; }
  const std::vector<std::string>& messages() const { return messages_; }

 private:
  std::vector<int> severities_;
  std::vector<std::string> messages_;
};

TEST(VlogRoutingTest, VlogCarriesNegativeSeverity) {
  SeverityCapturingSink sink;
  pagespeed_logging::AddLogSink(&sink);

  VLOG(1) << kVlogMarker;
  LOG(INFO) << kInfoMarker;

  pagespeed_logging::RemoveLogSink(&sink);

  int vlog_severity = 0;
  int info_severity = -100;  // LOG(INFO) never carries this.
  for (size_t i = 0; i < sink.severities().size(); ++i) {
    if (sink.messages()[i].find(kVlogMarker) != std::string::npos) {
      vlog_severity = sink.severities()[i];
    } else if (sink.messages()[i].find(kInfoMarker) != std::string::npos) {
      info_severity = sink.severities()[i];
    }
  }
  // VLOG(1) must arrive as verbose severity 1 (glog convention), so sinks
  // can route it to debug-level logging instead of reporting it as info.
  EXPECT_EQ(-1, vlog_severity);
  EXPECT_EQ(logging::LOG_INFO, info_severity);
}

// Counts marker messages reaching the process default spdlog logger, i.e.
// the console mirror ~LogMessage performs in addition to the sinks.
class CountingSpdlogSink : public spdlog::sinks::base_sink<std::mutex> {
 public:
  int vlog_count() const { return vlog_count_; }
  int info_count() const { return info_count_; }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    const std::string payload(msg.payload.data(), msg.payload.size());
    if (payload.find(kVlogMarker) != std::string::npos) {
      ++vlog_count_;
    }
    if (payload.find(kInfoMarker) != std::string::npos) {
      ++info_count_;
    }
  }

  void flush_() override {}

 private:
  int vlog_count_ = 0;
  int info_count_ = 0;
};

TEST(VlogRoutingTest, VlogIsNotMirroredToTheDefaultLogger) {
  auto counting_sink = std::make_shared<CountingSpdlogSink>();
  auto& sinks = spdlog::default_logger()->sinks();
  sinks.push_back(counting_sink);

  LOG(INFO) << kInfoMarker;
  VLOG(1) << kVlogMarker;

  auto it = std::find(sinks.begin(), sinks.end(), counting_sink);
  ASSERT_TRUE(it != sinks.end());
  sinks.erase(it);

  // The default logger's console output is not filtered by any server
  // LogLevel, so verbose diagnostics must not be mirrored to it.
  EXPECT_EQ(1, counting_sink->info_count());
  EXPECT_EQ(0, counting_sink->vlog_count());
}

}  // namespace
