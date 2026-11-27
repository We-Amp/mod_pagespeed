// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Status reporting for scanline-based image operations.

#ifndef PAGESPEED_LIB_IMAGE_SCANLINE_STATUS_H_
#define PAGESPEED_LIB_IMAGE_SCANLINE_STATUS_H_

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

#include "lib/base/printf_format.h"

namespace net_instaweb {

#if defined(PAGESPEED_SCANLINE_STATUS) ||           \
    defined(PAGESPEED_SCANLINE_STATUS_SOURCE) ||    \
    defined(PAGESPEED_SCANLINE_STATUS_ENUM_NAME) || \
    defined(PAGESPEED_SCANLINE_STATUS_ENUM_STRING)
#error "Preprocessor macro collision."
#endif

#define PAGESPEED_SCANLINE_STATUS(_X)                                        \
  _X(SCANLINE_STATUS_UNINITIALIZED), _X(SCANLINE_STATUS_SUCCESS),            \
      _X(SCANLINE_STATUS_UNSUPPORTED_FORMAT),                                \
      _X(SCANLINE_STATUS_UNSUPPORTED_FEATURE),                               \
      _X(SCANLINE_STATUS_PARSE_ERROR), _X(SCANLINE_STATUS_MEMORY_ERROR),     \
      _X(SCANLINE_STATUS_INTERNAL_ERROR), _X(SCANLINE_STATUS_TIMEOUT_ERROR), \
      _X(SCANLINE_STATUS_INVOCATION_ERROR),                                  \
                                                                             \
      _X(NUM_SCANLINE_STATUS)

// Note the source of the error message by means of an enum rather
// than a string.
#define PAGESPEED_SCANLINE_STATUS_SOURCE(_X)                                  \
  _X(SCANLINE_UNKNOWN), _X(SCANLINE_PNGREADER), _X(SCANLINE_PNGREADERRAW),    \
      _X(SCANLINE_GIFREADER), _X(SCANLINE_GIFREADERRAW),                      \
      _X(SCANLINE_JPEGREADER), _X(SCANLINE_WEBPREADER), _X(SCANLINE_RESIZER), \
      _X(SCANLINE_PNGWRITER), _X(SCANLINE_JPEGWRITER),                        \
      _X(SCANLINE_WEBPWRITER), _X(SCANLINE_UTIL),                             \
      _X(SCANLINE_PIXEL_FORMAT_OPTIMIZER),                                    \
      _X(FRAME_TO_SCANLINE_READER_ADAPTER),                                   \
      _X(FRAME_TO_SCANLINE_WRITER_ADAPTER),                                   \
      _X(SCANLINE_TO_FRAME_READER_ADAPTER),                                   \
      _X(SCANLINE_TO_FRAME_WRITER_ADAPTER), _X(FRAME_GIFREADER),              \
      _X(FRAME_WEBPWRITER), _X(FRAME_PADDING_READER),                         \
                                                                              \
      _X(NUM_SCANLINE_SOURCE)

#define PAGESPEED_SCANLINE_STATUS_ENUM_NAME(_Y) _Y
#define PAGESPEED_SCANLINE_STATUS_ENUM_STRING(_Y) #_Y

enum ScanlineStatusType : std::uint8_t {
  PAGESPEED_SCANLINE_STATUS(PAGESPEED_SCANLINE_STATUS_ENUM_NAME)
};

enum ScanlineStatusSource : std::uint8_t {
  PAGESPEED_SCANLINE_STATUS_SOURCE(PAGESPEED_SCANLINE_STATUS_ENUM_NAME)
};

// A class to report the success or error of ScanlineInterface
// operations. Scanline*Interface should return the
// ScanlineStatus corresponding to the earliest error
// encountered. ScanlineStatus.details_ should be of the form
// "FunctionThatFailed()" or "failure message".
class ScanlineStatus {
 public:
  ScanlineStatus()
      : type_(SCANLINE_STATUS_SUCCESS), source_(SCANLINE_UNKNOWN), details_() {}

  ScanlineStatus(ScanlineStatusType type, ScanlineStatusSource source,
                 const std::string& details)
      : type_(type), source_(source), details_(details) {}

  explicit ScanlineStatus(ScanlineStatusType type)
      : type_(type), source_(SCANLINE_UNKNOWN), details_() {}

  // This function takes variadic arguments so that we can use the
  // same sets of arguments here and for logging via the
  // PS_LOGGED_STATUS macro below.
  static ScanlineStatus New(ScanlineStatusType type,
                            ScanlineStatusSource source, const char* details,
                            ...) INSTAWEB_PRINTF_FORMAT(3, 4) {
    va_list args;
    va_start(args, details);
    std::string detail_list = FormatV(details, args);
    va_end(args);
    return ScanlineStatus(type, source, detail_list);
  }

  bool Success() const { return (type_ == SCANLINE_STATUS_SUCCESS); }
  ScanlineStatusType type() const { return type_; }
  ScanlineStatusSource source() const { return source_; }
  const std::string& details() const { return details_; }

  const char* TypeStr() const {
    static const char* const kScanlineStatusTypeNames[] = {
        PAGESPEED_SCANLINE_STATUS(PAGESPEED_SCANLINE_STATUS_ENUM_STRING)};
    return kScanlineStatusTypeNames[type_];
  }

  const char* SourceStr() const {
    static const char* const kScanlineStatusSourceNames[] = {
        PAGESPEED_SCANLINE_STATUS_SOURCE(
            PAGESPEED_SCANLINE_STATUS_ENUM_STRING)};
    return kScanlineStatusSourceNames[source_];
  }

  std::string ToString() const {
    return std::string(SourceStr()) + "/" + TypeStr() + " " + details();
  }

  // Determines whether the source of this status is a reader of some
  // sort.
  bool ComesFromReader() const {
    switch (source_) {
      case SCANLINE_PNGREADER:
      case SCANLINE_PNGREADERRAW:
      case SCANLINE_GIFREADER:
      case SCANLINE_GIFREADERRAW:
      case SCANLINE_JPEGREADER:
      case SCANLINE_WEBPREADER:
      case FRAME_TO_SCANLINE_READER_ADAPTER:
      case SCANLINE_TO_FRAME_READER_ADAPTER:
      case FRAME_GIFREADER:
      case FRAME_PADDING_READER:
        return true;
      default:
        return false;
    }
  }

 private:
  // Helper function to format strings with va_list
  static std::string FormatV(const char* format, va_list args) {
    // First, determine the required size
    va_list args_copy;
    va_copy(args_copy, args);
    int size = vsnprintf(nullptr, 0, format, args_copy);
    va_end(args_copy);

    if (size < 0) {
      return std::string();
    }

    // Allocate buffer and format the string
    std::string result(static_cast<size_t>(size) + 1, '\0');
    vsnprintf(result.data(), result.size(), format, args);
    result.resize(static_cast<size_t>(size));
    return result;
  }

  ScanlineStatusType type_;
  ScanlineStatusSource source_;
  std::string details_;

  // Note: we are allowing the implicit copy constructor and
  // assignment operators.
};

#undef PAGESPEED_SCANLINE_STATUS_ENUM_STRING
#undef PAGESPEED_SCANLINE_STATUS_ENUM_NAME
#undef PAGESPEED_SCANLINE_STATUS_SOURCE
#undef PAGESPEED_SCANLINE_STATUS

// Convenience macro for simultaneously logging error descriptions and
// creating a ScanlineStatus with that error description. _LOGGER is
// meant to be one of the PS_LOG* macros defined in message_handler.h.
#define PS_LOGGED_STATUS(_LOGGER, _HANDLER, _TYPE, _SOURCE, ...) \
  (_LOGGER(_HANDLER, #_SOURCE "/" #_TYPE " " __VA_ARGS__),       \
   ScanlineStatus::New(_TYPE, _SOURCE, __VA_ARGS__))

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_IMAGE_SCANLINE_STATUS_H_
