// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/tools/cli/log_capture.h"

#include "spdlog/spdlog.h"

namespace net_instaweb {
namespace lint {

void SilenceModuleLogging() {
  spdlog::default_logger()->set_level(spdlog::level::off);
}

LogCapture::LogCapture() { pagespeed_logging::AddLogSink(this); }

LogCapture::~LogCapture() { pagespeed_logging::RemoveLogSink(this); }

void LogCapture::send(int severity, const char* /*full_filename*/,
                      const char* /*base_filename*/, int /*line*/,
                      const char* /*message*/, size_t /*message_len*/) {
  if (severity >= logging::LOG_ERROR) {
    ++errors_;
  }
}

}  // namespace lint
}  // namespace net_instaweb
