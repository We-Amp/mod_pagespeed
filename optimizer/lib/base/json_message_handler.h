// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_BASE_JSON_MESSAGE_HANDLER_H_
#define PAGESPEED_LIB_BASE_JSON_MESSAGE_HANDLER_H_

#include <cstdio>
#include <string>

#include "lib/base/message_handler.h"

namespace pagespeed {

// JSON-structured message handler.
// Outputs one JSON object per line to stderr:
//   {"timestamp":"2024-01-01T00:00:00Z","level":"INFO","message":"..."}
class JsonMessageHandler : public MessageHandler {
 public:
  void Message(MessageType type, const char* format, ...) override;

 protected:
  void MessageV(MessageType type, const char* format, va_list args) override;

 private:
  static const char* LevelString(MessageType type);
  static std::string JsonEscape(std::string_view input);
  static std::string GetTimestamp();
};

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_BASE_JSON_MESSAGE_HANDLER_H_
