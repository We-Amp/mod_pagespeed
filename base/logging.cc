#include "base/logging.h"

#include "fmt/core.h"
#include "spdlog/spdlog.h"

namespace net_instaweb {

PageSpeedGLogSink::PageSpeedGLogSink() { AddLogSink(this); }

void PageSpeedGLogSink::send(google::LogSeverity severity,
                             const char* /* full_filename */,
                             const char* base_filename, int line,
                             const struct tm* /*tm_time*/, const char* message,
                             size_t message_len, google::int32 /*usecs*/) {
  constexpr char fmtstring[] = "[pagespeed] [{}:{} {}";
  const std::string message_view = std::string(message, message_len);
  switch (severity) {
    case google::INFO:
      spdlog::info(fmt::runtime(fmtstring), base_filename, line, message_view);
      return;
    case google::WARNING:
      spdlog::warn(fmt::runtime(fmtstring), base_filename, line, message_view);
      return;
    case google::ERROR:
      spdlog::error(fmt::runtime(fmtstring), base_filename, line, message_view);
      return;
    case google::FATAL:
      spdlog::critical(fmt::runtime(fmtstring), base_filename, line, message_view);
      spdlog::dump_backtrace();
      // TODO(oschaaf): break the debugger / log stacktrace?
      return;
  }
  CHECK(false) << "Can't be reached";
}

}  // namespace net_instaweb