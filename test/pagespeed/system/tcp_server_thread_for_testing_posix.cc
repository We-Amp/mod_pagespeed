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

#include "test/pagespeed/system/tcp_server_thread_for_testing_posix.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "base/logging.h"
#include "pagespeed/kernel/base/abstract_mutex.h"

namespace net_instaweb {

PosixTcpServerThread::PosixTcpServerThread(int listen_port,
                                           StringPiece thread_name,
                                           ThreadSystem* thread_system)
    : Thread(thread_system, thread_name, ThreadSystem::kJoinable),
      mutex_(thread_system->NewMutex()),
      ready_notify_(mutex_->NewCondvar()),
      requested_listen_port_(listen_port),
      actual_listening_port_(0),
      listen_fd_(-1),
      terminating_(false),
      is_shut_down_(false) {}

PosixTcpServerThread::~PosixTcpServerThread() {
  CHECK(is_shut_down_) << "Must call ShutDown() before destroying";
}

void PosixTcpServerThread::ShutDown() {
  if (is_shut_down_) {
    return;
  }
  terminating_ = true;
  // Close the listen socket to unblock accept()
  {
    ScopedMutex lock(mutex_.get());
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      close(listen_fd_);
      listen_fd_ = -1;
    }
  }
  Join();
  is_shut_down_ = true;
}

int PosixTcpServerThread::GetListeningPort() {
  ScopedMutex lock(mutex_.get());
  while (actual_listening_port_ == 0 && !terminating_) {
    ready_notify_->Wait();
  }
  return actual_listening_port_;
}

void PosixTcpServerThread::PickListenPortOnce(int* port) {
  if (*port != 0) {
    return;
  }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK_GE(fd, 0);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // Let OS pick
  CHECK_EQ(0, bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)));
  socklen_t len = sizeof(addr);
  CHECK_EQ(0, getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len));
  *port = ntohs(addr.sin_port);
  close(fd);
}

void PosixTcpServerThread::Run() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK_GE(fd, 0);

  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(requested_listen_port_);

  CHECK_EQ(0, bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)));
  CHECK_EQ(0, listen(fd, 5));

  socklen_t len = sizeof(addr);
  CHECK_EQ(0, getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len));

  {
    ScopedMutex lock(mutex_.get());
    listen_fd_ = fd;
    actual_listening_port_ = ntohs(addr.sin_port);
    ready_notify_->Signal();
  }

  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  int client_fd = accept(fd, reinterpret_cast<struct sockaddr*>(&client_addr),
                         &client_len);

  if (client_fd >= 0 && !terminating_) {
    HandleClientConnection(client_fd);
    close(client_fd);
  }

  {
    ScopedMutex lock(mutex_.get());
    if (listen_fd_ >= 0) {
      close(listen_fd_);
      listen_fd_ = -1;
    }
  }
}

void PosixTcpServerThread::WaitForHangupOrTimeout(int fd, int timeout_us) {
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLIN | POLLHUP | POLLERR;
  pfd.revents = 0;
  int timeout_ms = timeout_us / 1000;
  if (timeout_ms <= 0) timeout_ms = 1;
  int ret;
  do {
    ret = poll(&pfd, 1, timeout_ms);
  } while (ret < 0 && errno == EINTR);
}

}  // namespace net_instaweb
