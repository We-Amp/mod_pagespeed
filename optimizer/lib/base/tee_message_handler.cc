// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// TeeMessageHandler implementation.

#include "lib/base/tee_message_handler.h"

#include <cstdarg>
#include <utility>

namespace pagespeed {

TeeMessageHandler::TeeMessageHandler(
    MessageHandler* delegate,
    std::function<void(MessageType, std::string)> callback)
    : delegate_(delegate), callback_(std::move(callback)) {}

void TeeMessageHandler::Message(MessageType type, const char* format, ...) {
  va_list args;
  va_start(args, format);
  MessageV(type, format, args);
  va_end(args);
}

void TeeMessageHandler::MessageV(MessageType type, const char* format,
                                 va_list args) {
  // Format once, share with both sinks.
  std::string formatted = FormatMessage(format, args);

  // Delegate first (may abort on Fatal).
  delegate_->Message(type, "%s", formatted.c_str());

  // Callback second (skipped if Fatal aborted above).
  if (callback_) {
    callback_(type, std::move(formatted));
  }
}

}  // namespace pagespeed
