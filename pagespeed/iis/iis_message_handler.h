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

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/system/system_message_handler.h"

namespace net_instaweb {

class AbstractMutex;
class Timer;

// Message handler for the IIS PageSpeed module.
// Extends SystemMessageHandler to get SharedCircularBuffer integration
// (for the Message History admin page). Also logs to IIS tracing (ETW),
// Windows Event Log (for errors), and OutputDebugString.
class IisMessageHandler : public SystemMessageHandler {
 public:
  explicit IisMessageHandler(Timer* timer, AbstractMutex* mutex);
  ~IisMessageHandler() override;

 protected:
  // Override the S-impl methods (called by SystemMessageHandler after
  // va_list formatting). We log to Windows-specific sinks and then
  // feed the SharedCircularBuffer via AddMessageToBuffer().
  void MessageSImpl(MessageType type, const GoogleString& message) override;
  void FileMessageSImpl(MessageType type, const char* file, int line,
                        const GoogleString& message) override;

 private:
  // Write to Windows Event Log (errors/fatals only).
  void WriteToEventLog(MessageType type, const GoogleString& message);

  // Write to IIS tracing (ETW).
  void WriteToTrace(MessageType type, const GoogleString& message);

  Timer* timer_;

#ifdef _WIN32
  void* event_log_handle_;  // Cached event log handle (HANDLE type)
#endif

  DISALLOW_COPY_AND_ASSIGN(IisMessageHandler);
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_MESSAGE_HANDLER_H_
