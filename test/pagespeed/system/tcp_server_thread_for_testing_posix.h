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

#ifndef PAGESPEED_SYSTEM_TCP_SERVER_THREAD_FOR_TESTING_POSIX_H_
#define PAGESPEED_SYSTEM_TCP_SERVER_THREAD_FOR_TESTING_POSIX_H_

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <memory>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/condvar.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread.h"
#include "pagespeed/kernel/base/thread_system.h"

namespace net_instaweb {

// POSIX-based TCP server thread for testing. Unlike the APR version,
// this uses raw POSIX sockets and has no APR dependency.
class PosixTcpServerThread : public ThreadSystem::Thread {
 public:
  PosixTcpServerThread(int listen_port, StringPiece thread_name,
                       ThreadSystem* thread_system);
  ~PosixTcpServerThread() override;

  void ShutDown();

  // Blocks until the server is listening and returns the port.
  int GetListeningPort();

  // Pick a port for listening. port must be a pointer to a static int.
  static void PickListenPortOnce(int* port);

 protected:
  // Subclasses implement this to handle a client connection.
  // The fd will be closed after this returns.
  virtual void HandleClientConnection(int client_fd) = 0;

  // Wait for hangup or timeout (in microseconds) on the given fd.
  void WaitForHangupOrTimeout(int fd, int timeout_us);

 private:
  void Run() override;

  std::unique_ptr<ThreadSystem::CondvarCapableMutex> mutex_;
  std::unique_ptr<ThreadSystem::Condvar> ready_notify_;
  int requested_listen_port_;
  int actual_listening_port_;
  int listen_fd_;
  std::atomic<bool> terminating_;
  bool is_shut_down_;

  PosixTcpServerThread(const PosixTcpServerThread&) = delete;
  PosixTcpServerThread& operator=(const PosixTcpServerThread&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_TCP_SERVER_THREAD_FOR_TESTING_POSIX_H_
