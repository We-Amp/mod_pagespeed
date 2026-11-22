// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_TOOLS_CLI_LOG_CAPTURE_H_
#define PAGESPEED_TOOLS_CLI_LOG_CAPTURE_H_

#include "base/logging.h"

namespace net_instaweb {
namespace lint {

// The module's LOG() goes to spdlog's default logger, which writes to stdout,
// and some of its messages quote an option value.  SilenceModuleLogging()
// turns that logger off for the whole process; a LogCapture then counts the
// ERROR-or-worse messages logged while it exists, without keeping their
// text.
void SilenceModuleLogging();

class LogCapture : public pagespeed_logging::LogSink {
 public:
  LogCapture();
  ~LogCapture() override;
  LogCapture(const LogCapture&) = delete;
  LogCapture& operator=(const LogCapture&) = delete;

  int errors() const { return errors_; }

  void send(int severity, const char* full_filename, const char* base_filename,
            int line, const char* message, size_t message_len) override;

 private:
  int errors_ = 0;
};

}  // namespace lint
}  // namespace net_instaweb

#endif  // PAGESPEED_TOOLS_CLI_LOG_CAPTURE_H_
