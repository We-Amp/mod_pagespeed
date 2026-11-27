// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// TeeMessageHandler - Delegates to an existing handler and also calls a
// callback with each formatted message.  Used to tee log output to the
// WebSocket /v1/ws/logs endpoint.

#ifndef PAGESPEED_LIB_BASE_TEE_MESSAGE_HANDLER_H_
#define PAGESPEED_LIB_BASE_TEE_MESSAGE_HANDLER_H_

#include <functional>
#include <string>

#include "lib/base/message_handler.h"

namespace pagespeed {

class TeeMessageHandler : public MessageHandler {
 public:
  // `delegate` receives every message (stderr / JSON file / etc.).
  // `callback` is invoked after the delegate with the formatted string.
  // Both must outlive this object.
  TeeMessageHandler(MessageHandler* delegate,
                    std::function<void(MessageType, std::string)> callback);

  void Message(MessageType type, const char* format, ...) override;

 protected:
  void MessageV(MessageType type, const char* format, va_list args) override;

 private:
  MessageHandler* delegate_;
  std::function<void(MessageType, std::string)> callback_;
};

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_BASE_TEE_MESSAGE_HANDLER_H_
