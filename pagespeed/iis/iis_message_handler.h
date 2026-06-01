/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef PAGESPEED_IIS_IIS_MESSAGE_HANDLER_H_
#define PAGESPEED_IIS_IIS_MESSAGE_HANDLER_H_

#include <deque>
#include <mutex>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"

namespace net_instaweb {

class AbstractMutex;
class Timer;

// Message handler for the IIS PageSpeed module.
// Logs messages to IIS tracing (ETW), Windows Event Log (for errors),
// and maintains a buffer of recent messages for the admin UI.
class IisMessageHandler : public MessageHandler {
 public:
  // Create a message handler with the given buffer size for recent messages.
  explicit IisMessageHandler(Timer* timer,
                             AbstractMutex* mutex,
                             int buffer_size = 100);
  ~IisMessageHandler() override;

  // MessageHandler interface
  void MessageVImpl(MessageType type, const char* msg, va_list args) override;
  void FileMessageVImpl(MessageType type, const char* file, int line,
                        const char* msg, va_list args) override;

  // Get recent messages for the admin UI.
  // Returns messages in reverse chronological order (newest first).
  void GetRecentMessages(std::vector<GoogleString>* messages) const;

  // Clear the message buffer.
  void ClearMessages();

  // Get message count by type.
  int GetMessageCount(MessageType type) const;

  // Set minimum log level (messages below this level are not logged).
  void set_min_log_level(MessageType level) { min_log_level_ = level; }
  MessageType min_log_level() const { return min_log_level_; }

  // Enable/disable Windows Event Log for errors.
  void set_event_log_enabled(bool enabled) { event_log_enabled_ = enabled; }
  bool event_log_enabled() const { return event_log_enabled_; }

  // Enable/disable IIS tracing (ETW).
  void set_tracing_enabled(bool enabled) { tracing_enabled_ = enabled; }
  bool tracing_enabled() const { return tracing_enabled_; }

 private:
  // Format and store a message.
  void AddMessage(MessageType type, const GoogleString& message);

  // Write to Windows Event Log.
  void WriteToEventLog(MessageType type, const GoogleString& message);

  // Write to IIS tracing (ETW).
  void WriteToTrace(MessageType type, const GoogleString& message);

  // Get timestamp string.
  GoogleString GetTimestamp() const;

  // Get message type prefix.
  static const char* GetTypePrefix(MessageType type);

  Timer* timer_;
  AbstractMutex* mutex_;
  int max_buffer_size_;
  std::deque<GoogleString> message_buffer_;
  MessageType min_log_level_;
  bool event_log_enabled_;
  bool tracing_enabled_;

  // Message counts by type
  mutable std::mutex count_mutex_;
  int info_count_;
  int warning_count_;
  int error_count_;
  int fatal_count_;

#ifdef _WIN32
  void* event_log_handle_;  // Cached event log handle (HANDLE type)
#endif

  DISALLOW_COPY_AND_ASSIGN(IisMessageHandler);
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_MESSAGE_HANDLER_H_
