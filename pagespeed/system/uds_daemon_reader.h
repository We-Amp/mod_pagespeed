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

#ifndef PAGESPEED_SYSTEM_UDS_DAEMON_READER_H_
#define PAGESPEED_SYSTEM_UDS_DAEMON_READER_H_

#include <cstddef>
#include <memory>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_reader.h"

namespace net_instaweb {

class AbstractMutex;
class CurlUrlAsyncFetcher;
class MessageHandler;
class ThreadSystem;
class Timer;

// DaemonReader transport speaking HTTP/1.1 over the daemon's unix-domain
// management socket via curl's CURLOPT_UNIX_SOCKET_PATH.
class UdsDaemonReader : public DaemonReader {
 public:
  // Bounds enforced on every read, independent of any fetcher configuration:
  // the admin proxy contract puts a 5s ceiling on upstream latency and a
  // 1 MiB ceiling on the response body.
  static constexpr int64 kUpstreamTimeoutMs = 5000;
  static constexpr size_t kMaxResponseBodyBytes = 1024 * 1024;

  // An empty `socket_path` disables the reader: every Get() then fails its
  // fetch immediately (callers map this to 502 "daemon unreachable").
  UdsDaemonReader(ThreadSystem* thread_system, Timer* timer,
                  const GoogleString& socket_path,
                  MessageHandler* message_handler);
  ~UdsDaemonReader() override;

  void Get(StringPiece daemon_path, AsyncFetch* fetch) override;

 private:
  // Returns the curl fetcher, creating it on first use.  Lazy so a quiescent
  // server (or a pre-fork parent process) never pays for its poll thread.
  // Returns nullptr when the reader is disabled (empty socket path).
  CurlUrlAsyncFetcher* GetOrCreateFetcher();

  ThreadSystem* thread_system_;
  Timer* timer_;
  GoogleString socket_path_;
  MessageHandler* message_handler_;
  std::unique_ptr<AbstractMutex> mutex_;
  std::unique_ptr<CurlUrlAsyncFetcher> fetcher_;  // guarded by mutex_

  UdsDaemonReader(const UdsDaemonReader&) = delete;
  UdsDaemonReader& operator=(const UdsDaemonReader&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_UDS_DAEMON_READER_H_
