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

#include <cstdlib>
#include <sstream>
#include <string>

// Severity levels
namespace logging {
constexpr int LOG_INFO = 0;
constexpr int LOG_WARNING = 1;
constexpr int LOG_ERROR = 2;
constexpr int LOG_FATAL = 3;
// DFATAL = fatal in debug builds, error in release builds
#ifdef NDEBUG
constexpr int LOG_DFATAL = LOG_ERROR;
#else
constexpr int LOG_DFATAL = LOG_FATAL;
#endif
}  // namespace logging

namespace pagespeed_logging {

// Log sink interface for redirecting log output (e.g., to Apache error log)
class LogSink {
 public:
  virtual ~LogSink() = default;
  virtual void send(int severity, const char* full_filename,
                    const char* base_filename, int line, const char* message,
                    size_t message_len) = 0;
};

// Add/remove log sinks
void AddLogSink(LogSink* sink);
void RemoveLogSink(LogSink* sink);

// Signal that the process is shutting down. After this call, LOG() writes
// to stderr instead of spdlog, avoiding crashes from the static destruction
// order fiasco (spdlog's global logger may already be destroyed).
void ShutDownLogging();

// Internal: send to all registered sinks
void SendToSinks(int severity, const char* full_filename,
                 const char* base_filename, int line, const char* message,
                 size_t message_len);

// Log message stream that outputs on destruction
class LogMessage {
 public:
  LogMessage(int severity, const char* file, int line)
      : severity_(severity), file_(file), line_(line) {}

  ~LogMessage();

  std::ostream& stream() { return stream_; }

 private:
  int severity_;
  const char* file_;
  int line_;
  std::ostringstream stream_;
};

// LogMessage that checks a condition
class LogMessageVoidify {
 public:
  void operator&(std::ostream&) {}
};

// Extract base filename from full path
inline const char* GetBasename(const char* filepath) {
  const char* base = filepath;
  for (const char* p = filepath; *p; ++p) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  return base;
}

// Check failure message - logs and aborts
class CheckOpMessageBuilder {
 public:
  explicit CheckOpMessageBuilder(const char* exprtext)
      : stream_(new std::ostringstream) {
    *stream_ << "Check failed: " << exprtext << " ";
  }
  ~CheckOpMessageBuilder() { delete stream_; }
  std::ostream& stream() { return *stream_; }
  std::ostream* ForVar2() {
    *stream_ << " vs. ";
    return stream_;
  }
  std::string* NewString() {
    return new std::string(stream_->str());
  }

 private:
  std::ostringstream* stream_;
};

template <typename T1, typename T2>
std::string* MakeCheckOpString(const T1& v1, const T2& v2, const char* expr) {
  CheckOpMessageBuilder builder(expr);
  builder.stream() << v1;
  *builder.ForVar2() << v2;
  return builder.NewString();
}

// Check operation helpers
#define DEFINE_CHECK_OP(name, op)                                              \
  template <typename T1, typename T2>                                          \
  inline std::string* Check##name##Impl(const T1& v1, const T2& v2,            \
                                        const char* exprtext) {                \
    if (v1 op v2) return nullptr;                                              \
    return ::pagespeed_logging::MakeCheckOpString(v1, v2, exprtext);           \
  }

DEFINE_CHECK_OP(EQ, ==)
DEFINE_CHECK_OP(NE, !=)
DEFINE_CHECK_OP(LT, <)
DEFINE_CHECK_OP(LE, <=)
DEFINE_CHECK_OP(GT, >)
DEFINE_CHECK_OP(GE, >=)

#undef DEFINE_CHECK_OP

// Check failure class that logs on destruction and aborts for FATAL
class CheckOpResult {
 public:
  CheckOpResult(std::string* msg, const char* file, int line)
      : msg_(msg), file_(file), line_(line) {}
  ~CheckOpResult() {
    if (msg_) {
      ::pagespeed_logging::LogMessage(logging::LOG_FATAL, file_, line_)
          .stream()
          << *msg_ << extra_stream_.str();
      delete msg_;
    }
  }
  operator bool() const { return msg_ != nullptr; }
  std::ostream& stream() { return extra_stream_; }

 private:
  std::string* msg_;
  const char* file_;
  int line_;
  std::ostringstream extra_stream_;
};

}  // namespace pagespeed_logging

// Main LOG macro
#define LOG(severity)                                                          \
  ::pagespeed_logging::LogMessage(logging::LOG_##severity, __FILE__, __LINE__) \
      .stream()

// VLOG - verbose logging (treated as INFO if verbose level is met)
// For simplicity, always log VLOG messages as INFO
#define VLOG(verboselevel) LOG(INFO)

// VLOG_IS_ON - check if verbose logging is enabled at given level
#define VLOG_IS_ON(verboselevel) (true)

// LOG_IF - conditional logging
#define LOG_IF(severity, condition)                                            \
  !(condition)                                                                 \
      ? (void)0                                                                \
      : ::pagespeed_logging::LogMessageVoidify() &                             \
            ::pagespeed_logging::LogMessage(logging::LOG_##severity, __FILE__, \
                                            __LINE__)                          \
                .stream()

// DLOG - debug logging (compiles to nothing in release builds)
#ifdef NDEBUG
#define DLOG(severity)                                                         \
  true ? (void)0                                                               \
       : ::pagespeed_logging::LogMessageVoidify() &                            \
             ::pagespeed_logging::LogMessage(logging::LOG_##severity,          \
                                             __FILE__, __LINE__)               \
                 .stream()
#define DVLOG(verboselevel)                                                    \
  true ? (void)0                                                               \
       : ::pagespeed_logging::LogMessageVoidify() &                            \
             ::pagespeed_logging::LogMessage(logging::LOG_INFO, __FILE__,      \
                                             __LINE__)                         \
                 .stream()
#else
#define DLOG(severity) LOG(severity)
#define DVLOG(verboselevel) VLOG(verboselevel)
#endif

// Note: LOG(DFATAL) is handled via logging::LOG_DFATAL constexpr (see above).
// DFATAL = fatal in debug builds, error in release builds.

// CHECK macros - always active
#define CHECK(condition)                                                       \
  LOG_IF(FATAL, !(condition)) << "Check failed: " #condition " "

#define CHECK_OP(name, op, val1, val2)                                         \
  if (::pagespeed_logging::CheckOpResult _result{                              \
          ::pagespeed_logging::Check##name##Impl(                              \
              (val1), (val2), #val1 " " #op " " #val2),                         \
          __FILE__, __LINE__};                                                 \
      _result)                                                                 \
  _result.stream()

#define CHECK_EQ(val1, val2) CHECK_OP(EQ, ==, val1, val2)
#define CHECK_NE(val1, val2) CHECK_OP(NE, !=, val1, val2)
#define CHECK_LT(val1, val2) CHECK_OP(LT, <, val1, val2)
#define CHECK_LE(val1, val2) CHECK_OP(LE, <=, val1, val2)
#define CHECK_GT(val1, val2) CHECK_OP(GT, >, val1, val2)
#define CHECK_GE(val1, val2) CHECK_OP(GE, >=, val1, val2)

// DCHECK macros - only active in debug builds
#ifdef NDEBUG
#define DCHECK(condition)                                                      \
  while (false) CHECK(condition)
#define DCHECK_EQ(val1, val2)                                                  \
  while (false) CHECK_EQ(val1, val2)
#define DCHECK_NE(val1, val2)                                                  \
  while (false) CHECK_NE(val1, val2)
#define DCHECK_LT(val1, val2)                                                  \
  while (false) CHECK_LT(val1, val2)
#define DCHECK_LE(val1, val2)                                                  \
  while (false) CHECK_LE(val1, val2)
#define DCHECK_GT(val1, val2)                                                  \
  while (false) CHECK_GT(val1, val2)
#define DCHECK_GE(val1, val2)                                                  \
  while (false) CHECK_GE(val1, val2)
#else
#define DCHECK(condition) CHECK(condition)
#define DCHECK_EQ(val1, val2) CHECK_EQ(val1, val2)
#define DCHECK_NE(val1, val2) CHECK_NE(val1, val2)
#define DCHECK_LT(val1, val2) CHECK_LT(val1, val2)
#define DCHECK_LE(val1, val2) CHECK_LE(val1, val2)
#define DCHECK_GT(val1, val2) CHECK_GT(val1, val2)
#define DCHECK_GE(val1, val2) CHECK_GE(val1, val2)
#endif

// CHECK_NOTNULL - check that a pointer is not null and return it
template <typename T>
T* CheckNotNull(const char* file, int line, const char* expr, T* ptr) {
  if (ptr == nullptr) {
    ::pagespeed_logging::LogMessage(logging::LOG_FATAL, file, line).stream()
        << "Check failed: " << expr << " != nullptr";
  }
  return ptr;
}

#define CHECK_NOTNULL(val)                                                     \
  ::CheckNotNull(__FILE__, __LINE__, #val, (val))

// COMPILE_ASSERT - static assertion (use static_assert in C++11+)
// Only define if not already defined (basictypes.h may define it first)
#ifndef COMPILE_ASSERT
#define COMPILE_ASSERT(expr, msg) static_assert(expr, #msg)
#endif

namespace net_instaweb {

// Abstract log sink that can be subclassed by Apache/Envoy/etc.
// This provides the same interface as the old glog-based PageSpeedGLogSink
class PageSpeedLogSink : public ::pagespeed_logging::LogSink {
 public:
  PageSpeedLogSink();
  virtual ~PageSpeedLogSink();

  void send(int severity, const char* full_filename, const char* base_filename,
            int line, const char* message, size_t message_len) override;

  void setMinLogLevel(int level) { min_log_level_ = level; }

 protected:
  int min_log_level_ = logging::LOG_INFO;
};

// Alias for backward compatibility
using PageSpeedGLogSink = PageSpeedLogSink;

}  // namespace net_instaweb
