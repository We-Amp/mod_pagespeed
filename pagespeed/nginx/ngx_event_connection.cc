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

extern "C" {

#include <ngx_channel.h>
}

#include <unistd.h>

#include "ngx_event_connection.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/message_handler.h"

namespace net_instaweb {

namespace {

// Events read per read() syscall while draining the pipe. Writes are atomic
// multiples of sizeof(ps_event_data), so a read never returns a partial
// event.
const int kReadBatchSize = 32;

// Bound on reader-side overflow flush passes per event-loop wakeup, so
// sustained writers cannot starve the event loop; see FlushOverflow().
const int kMaxOverflowFlushCycles = 4;

// Internal wake-marker event type (sender == nullptr, never dispatched).
const char kWakeMarkerType = 'W';

}  // namespace

NgxEventConnection::NgxEventConnection(callbackPtr callback)
    : event_handler_(callback), overflow_mode_(false) {
  pthread_mutex_init(&write_mutex_, nullptr);
}

NgxEventConnection::~NgxEventConnection() {
  pthread_mutex_destroy(&write_mutex_);
}

bool NgxEventConnection::Init(ngx_cycle_t* cycle) {
  int file_descriptors[2];

  if (pipe(file_descriptors) != 0) {
    ngx_log_error(NGX_LOG_EMERG, cycle->log, 0, "pagespeed: pipe() failed");
    return false;
  }
  if (ngx_nonblocking(file_descriptors[0]) == -1) {
    ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                  ngx_nonblocking_n "pagespeed:  pipe[0] failed");
  } else if (ngx_nonblocking(file_descriptors[1]) == -1) {
    ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                  ngx_nonblocking_n "pagespeed:  pipe[1] failed");
  } else if (!CreateNgxConnection(cycle, file_descriptors[0])) {
    ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                  "pagespeed: failed to create connection.");
  } else {
    pipe_read_fd_ = file_descriptors[0];
    pipe_write_fd_ = file_descriptors[1];
    // Attempt to bump the pipe capacity, because running out of buffer space
    // can potentially lead up to writes spinning on EAGAIN.
    // See https://github.com/apache/incubator-pagespeed-ngx/issues/1380
    // TODO(oschaaf): Consider implementing a queueing mechanism for retrying
    // failed writes.
#ifdef F_SETPIPE_SZ
    fcntl(pipe_write_fd_, F_SETPIPE_SZ,
          200 * 1024 /* minimal amount of bytes */);
#endif
    return true;
  }
  close(file_descriptors[0]);
  close(file_descriptors[1]);
  return false;
}

bool NgxEventConnection::CreateNgxConnection(ngx_cycle_t* cycle,
                                             ngx_fd_t pipe_fd) {
  // pipe_fd (the read side of the pipe will end up as c->fd on the
  // underlying ngx_connection_t that gets created here)
  ngx_int_t rc = ngx_add_channel_event(cycle, pipe_fd, NGX_READ_EVENT,
                                       &NgxEventConnection::ReadEventHandler);
  return rc == NGX_OK;
}

void NgxEventConnection::ReadEventHandler(ngx_event_t* ev) {
  ngx_connection_t* c = static_cast<ngx_connection_t*>(ev->data);
  ngx_int_t result = ngx_handle_read_event(ev, 0);
  if (result != NGX_OK) {
    CHECK(false) << "pagespeed: ngx_handle_read_event error: " << result;
  }

  if (ev->timedout) {
    ev->timedout = 0;
    return;
  }

  if (!NgxEventConnection::ReadAndNotify(c->fd)) {
    // This was copied from ngx_channel_handler(): for epoll, we need to call
    // ngx_del_conn(). Sadly, no documentation as to why.
    if (ngx_event_flags & NGX_USE_EPOLL_EVENT) {
      ngx_del_conn(c, 0);
    }
    ngx_close_connection(c);
    ngx_del_event(ev, NGX_READ_EVENT, 0);
  }
}

// Deserialize ps_event_data's from the pipe as they become available and
// dispatch them strictly in write order.
//
// Historically this read (and dispatched) a single event per event-loop
// wakeup, out of fear that re-entrant calls would process events out of
// order. That re-entrancy does not exist: Drain() is only called from
// NgxBaseFetch::Terminate() at worker shutdown, and the receiving callbacks
// (which end in ngx_http_finalize_request / ngx_http_run_posted_requests)
// never dispatch new event-loop events. Draining in batches means one epoll
// wakeup handles everything the pipe holds instead of one wakeup (plus an
// ngx_handle_read_event call) per event.
//
// Events whose base fetch got released while they sat in this batch are
// skipped by the receiving callback via its existing refcount/request-context
// checks, exactly as when they sat unread in the pipe.
bool NgxEventConnection::ReadAndNotify(ngx_fd_t fd) {
  NgxEventConnection* connection = nullptr;
  while (true) {
    ps_event_data events[kReadBatchSize];
    ngx_int_t size = read(fd, static_cast<void*>(events), sizeof(events));

    if (size == -1) {
      if (errno == EINTR) {
        continue;
      }
      if (ngx_errno == EAGAIN || ngx_errno == EWOULDBLOCK) {
        break;  // Pipe fully drained.
      }
      return false;
    }
    if (size == 0) {
      return false;  // Write end closed.
    }
    CHECK(size % static_cast<ngx_int_t>(sizeof(ps_event_data)) == 0)
        << "pagespeed: partial event read from pipe: " << size;
    ngx_int_t count = size / sizeof(ps_event_data);
    for (ngx_int_t i = 0; i < count; i++) {
      DCHECK(connection == nullptr || connection == events[i].connection)
          << "pagespeed: one pipe must carry one connection's events";
      connection = events[i].connection;
      DispatchEvent(events[i]);
    }
  }
  // With the pipe drained, deliver anything that overflowed while it was
  // full. If we read no events at all (spurious wakeup) any queued overflow
  // is picked up via its guaranteed wake marker instead.
  if (connection != nullptr) {
    connection->FlushOverflow();
  }
  return true;
}

void NgxEventConnection::DispatchEvent(const ps_event_data& data) {
  if (data.sender == nullptr) {
    return;  // Internal wake marker, see FlushOverflow().
  }
  data.connection->event_handler_(data);
}

bool NgxEventConnection::WriteEvent(void* sender) {
  return WriteEvent('X' /* Anything char is fine */, sender);
}

bool NgxEventConnection::WriteEvent(char type, void* sender) {
  ps_event_data data;

  ngx_memzero(&data, sizeof(data));
  data.type = type;
  data.sender = sender;
  data.connection = this;

  // When the pipe buffer is full, queue the event instead of blocking this
  // (PSOL) thread in a usleep backoff that could stall it for seconds and
  // ultimately drop the event. The event loop is guaranteed to
  // wake up -- the pipe is full -- and FlushOverflow() delivers the queue
  // after the pipe's contents. Ordering invariant: while the queue is
  // non-empty no new event may enter the pipe, otherwise it would be
  // delivered ahead of older queued events.
  pthread_mutex_lock(&write_mutex_);
  bool result;
  if (overflow_mode_) {
    overflow_.push_back(data);
    result = true;
  } else {
    result = TryWriteToPipe(data);
    if (!result && (ngx_errno == EAGAIN || ngx_errno == EWOULDBLOCK)) {
      overflow_mode_ = true;
      overflow_.push_back(data);
      result = true;
    }
  }
  pthread_mutex_unlock(&write_mutex_);
  return result;
}

bool NgxEventConnection::TryWriteToPipe(const ps_event_data& data) {
  while (true) {
    ssize_t size = write(pipe_write_fd_, &data, sizeof(data));
    if (size == static_cast<ssize_t>(sizeof(data))) {
      return true;
    }
    if (size == -1) {
      if (ngx_errno == EINTR) {
        continue;
      }
      return false;  // ngx_errno tells the caller why.
    }
    // A pipe write of less than PIPE_BUF bytes is atomic; a short write
    // cannot happen.
    CHECK(false) << "pagespeed: unexpected return value from write(): " << size;
  }
}

void NgxEventConnection::FlushOverflow() {
  // Deliver queued events in bounded passes: writers may keep appending
  // while we dispatch, and an unbounded loop here would starve the event
  // loop. If the queue is still non-empty after the last pass, write an
  // internal wake marker (the pipe has room again -- writers in overflow
  // mode do not touch it) so another wakeup finishes the job.
  for (int cycle = 0; cycle < kMaxOverflowFlushCycles; cycle++) {
    std::deque<ps_event_data> batch;
    pthread_mutex_lock(&write_mutex_);
    if (overflow_.empty()) {
      overflow_mode_ = false;
      pthread_mutex_unlock(&write_mutex_);
      return;
    }
    batch.swap(overflow_);
    pthread_mutex_unlock(&write_mutex_);
    for (std::deque<ps_event_data>::const_iterator it = batch.begin();
         it != batch.end(); ++it) {
      DispatchEvent(*it);
    }
  }
  ps_event_data marker;
  ngx_memzero(&marker, sizeof(marker));
  marker.type = kWakeMarkerType;
  marker.sender = nullptr;
  marker.connection = this;
  pthread_mutex_lock(&write_mutex_);
  // If even this write fails the queue is still delivered on the next
  // natural wakeup; nothing is lost.
  (void)TryWriteToPipe(marker);
  pthread_mutex_unlock(&write_mutex_);
}

// Reads and processes what is available in the pipe.
void NgxEventConnection::Drain() {
  NgxEventConnection::ReadAndNotify(pipe_read_fd_);
  FlushOverflow();
}

void NgxEventConnection::Shutdown() {
  close(pipe_write_fd_);
  close(pipe_read_fd_);
}

}  // namespace net_instaweb
