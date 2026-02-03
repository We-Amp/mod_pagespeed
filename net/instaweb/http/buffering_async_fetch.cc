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

#include "net/instaweb/http/public/buffering_async_fetch.h"

#include "pagespeed/kernel/base/message_handler.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {
void BufferingDebugLog(const char* msg) {
  HANDLE hFile = CreateFileA(
      "C:\\inetpub\\pagespeed\\buffering_debug.log",
      FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      NULL,
      OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      NULL);
  if (hFile != INVALID_HANDLE_VALUE) {
    DWORD written;
    WriteFile(hFile, msg, strlen(msg), &written, NULL);
    WriteFile(hFile, "\r\n", 2, &written, NULL);
    CloseHandle(hFile);
  }
}
}  // namespace

namespace net_instaweb {

BufferingAsyncFetch::BufferingAsyncFetch(const RequestContextPtr& request_ctx)
    : AsyncFetch(request_ctx) {}

BufferingAsyncFetch::~BufferingAsyncFetch() = default;

bool BufferingAsyncFetch::HandleWrite(const StringPiece& content,
                                       MessageHandler* handler) {
  char buf[512];
  snprintf(buf, sizeof(buf), "HandleWrite: content_size=%zu body_size_before=%zu",
           content.size(), body_.size());
  BufferingDebugLog(buf);

  if (content.empty()) {
    BufferingDebugLog("HandleWrite: empty content, returning");
    return true;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  content.AppendToString(&body_);
  snprintf(buf, sizeof(buf), "HandleWrite: body_size_after=%zu", body_.size());
  BufferingDebugLog(buf);
  return true;
}

bool BufferingAsyncFetch::HandleFlush(MessageHandler* handler) {
  // Buffering fetch - flush is a no-op since we accumulate everything.
  return true;
}

void BufferingAsyncFetch::HandleDone(bool success) {
  std::lock_guard<std::mutex> lock(mutex_);
  success_ = success;
  done_ = true;
  OnDone(success);
}

}  // namespace net_instaweb
