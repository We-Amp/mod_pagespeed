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
    : event_handler_(callback),
      connection_(nullptr),
      overflow_mode_(false),
      closed_(false) {
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
  // A copy of nginx's ngx_add_channel_event(), which does not hand the
  // ngx_connection_t it creates back to its caller: Shutdown() needs that
  // connection to close the read side through nginx. It matches the vendored
  // nginx 1.30.6 (see bazel/nginx.bzl) apart from the rc != NGX_OK test,
  // which is stricter than upstream's rc == NGX_ERROR. c->recv stays unset
  // exactly as upstream leaves it: only the read event's handler is ever
  // called for a channel connection, and that handler is ours.
  // pipe_fd (the read side of the pipe will end up as c->fd on the
  // underlying ngx_connection_t that gets created here)
  ngx_connection_t* c = ngx_get_connection(pipe_fd, cycle->log);
  if (c == nullptr) {
    return false;
  }
  c->pool = cycle->pool;
  ngx_event_t* rev = c->read;
  ngx_event_t* wev = c->write;
  rev->log = cycle->log;
  wev->log = cycle->log;
  rev->channel = 1;
  wev->channel = 1;
  rev->handler = &NgxEventConnection::ReadEventHandler;

  ngx_int_t rc;
  if (ngx_add_conn && (ngx_event_flags & NGX_USE_EPOLL_EVENT) == 0) {
    rc = ngx_add_conn(c);
  } else {
    rc = ngx_add_event(rev, NGX_READ_EVENT, 0);
  }
  if (rc != NGX_OK) {
    ngx_free_connection(c);
    return false;
  }
  connection_ = c;
  return true;
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

  // On a read error (EOF or a real error) leave the close to Shutdown():
  // this static handler cannot reach the owning object to null connection_,
  // so closing here would let Shutdown() close the same connection object a
  // second time, after nginx recycled it into an unrelated connection. The
  // write end is only ever closed by Shutdown(), on this same event-loop
  // thread, which closes the read side immediately after, so in practice
  // nothing reaches this path before Shutdown() owns the teardown.
  (void)NgxEventConnection::ReadAndNotify(c->fd);
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
  if (closed_) {
    // Shutdown() has closed the pipe: the worker is exiting, nobody would
    // collect the output, and the fd number may already have been reused.
    // Drop the event.
    result = false;
  } else if (overflow_mode_) {
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
    for (const auto& it : batch) {
      DispatchEvent(it);
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
  // Mark the connection closed and close the write side under write_mutex_,
  // so a writer either finishes its write before the close or sees closed_
  // and drops the event. Without this, a rewrite thread that outlived the
  // shutdown drain could write to the closed fd number after worker-exit
  // teardown (factory shutdown, record pool shutdown, cache cleanup) reused
  // it for a new file or socket.
  pthread_mutex_lock(&write_mutex_);
  closed_ = true;
  close(pipe_write_fd_);
  pthread_mutex_unlock(&write_mutex_);
  // Close the read side through nginx: it is a channel connection registered
  // with the event loop, and a raw close() would leave the loop's event
  // bookkeeping pointing at a dead fd.
  if (connection_ != nullptr) {
    ngx_close_connection(connection_);
    connection_ = nullptr;
  } else {
    close(pipe_read_fd_);
  }
}

}  // namespace net_instaweb
