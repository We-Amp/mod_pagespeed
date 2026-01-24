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

#pragma once

#include "glog/logging.h"

namespace logging {
constexpr int LOG_INFO = google::GLOG_INFO;
constexpr int LOG_ERROR = google::GLOG_ERROR;
constexpr int LOG_WARNING = google::GLOG_WARNING;
constexpr int LOG_FATAL = google::GLOG_FATAL;
}  // namespace logging

namespace net_instaweb {

class PageSpeedGLogSink : public google::LogSink {
 public:
  PageSpeedGLogSink();

  // Override both send signatures to avoid hidden overload warning
  void send(google::LogSeverity severity, const char* full_filename,
            const char* base_filename, int line, const struct tm* tm_time,
            const char* message, size_t message_len) override {
    send(severity, full_filename, base_filename, line, tm_time, message,
         message_len, 0);
  }

  void send(google::LogSeverity severity, const char* full_filename,
            const char* base_filename, int line, const struct tm* tm_time,
            const char* message, size_t message_len,
            google::int32 usecs) override;

  void setMinLogLevel(int) {
    // XXX(oschaaf): check callees and make this take effect.
  }
};

}  // namespace net_instaweb
