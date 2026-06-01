

#include "pagespeed/iis/log_message_handler.h"

#define _WINSOCKAPI_
#include <Windows.h>

#include <limits>
#include <string>

#include "base/logging.h"
#include "net/instaweb/public/version.h"
#include "pagespeed/kernel/base/string_util.h"

#include "pagespeed/iis/iis_message_handler.h"
#include "pagespeed/iis/iis_global_constants.h"
// Make sure we don't attempt to use LOG macros here, since doing so
// would cause us to go into an infinite log loop.
#undef LOG
#define LOG USING_LOG_HERE_WOULD_CAUSE_INFINITE_RECURSION

namespace {

static net_instaweb::IisMessageHandler* global_message_handler = NULL;

// Log sink that forwards pagespeed_logging messages to the IIS message handler.
class IisLogSink : public pagespeed_logging::LogSink {
 public:
  void send(int severity, const char* full_filename,
            const char* base_filename, int line, const char* message,
            size_t message_len) override {
    if (global_message_handler == NULL) {
      OutputDebugStringA(message);
      return;
    }

    net_instaweb::MessageType message_type;
    switch (severity) {
      case logging::LOG_INFO:
        message_type = net_instaweb::MessageType::kInfo;
        break;
      case logging::LOG_WARNING:
        message_type = net_instaweb::MessageType::kWarning;
        break;
      case logging::LOG_ERROR:
        message_type = net_instaweb::MessageType::kError;
        break;
      case logging::LOG_FATAL:
        message_type = net_instaweb::MessageType::kFatal;
        break;
      default:  // For VLOG(s)
        message_type = net_instaweb::MessageType::kInfo;
        break;
    }

    GoogleString msg(message, message_len);
    // Trim the newline off the end of the message string.
    if (!msg.empty() && msg.back() == '\n') {
      msg.pop_back();
    }
    msg.append("(vlog)");
    global_message_handler->Message(message_type, "%s", msg.c_str());
  }
};

static IisLogSink* g_log_sink = NULL;

}  // namespace


namespace net_instaweb {

namespace log_message_handler {


const int kDebugLogLevel = -2;

void Install(IisMessageHandler* handler) {
  global_message_handler = handler;
  OutputDebugStringA("Hooking up handling of chromium messages");
  if (g_log_sink == NULL) {
    g_log_sink = new IisLogSink();
    pagespeed_logging::AddLogSink(g_log_sink);
  }
}

void Deinstall() {
  global_message_handler = NULL;
  if (g_log_sink != NULL) {
    pagespeed_logging::RemoveLogSink(g_log_sink);
    delete g_log_sink;
    g_log_sink = NULL;
  }
}

}  // namespace log_message_handler

}  // namespace net_instaweb
