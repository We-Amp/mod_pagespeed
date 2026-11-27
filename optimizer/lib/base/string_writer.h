// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Ported from mod_pagespeed for PageSpeed 2.0

#ifndef PAGESPEED_LIB_BASE_STRING_WRITER_H_
#define PAGESPEED_LIB_BASE_STRING_WRITER_H_

#include <string>
#include <string_view>

#include "lib/base/writer.h"

namespace net_instaweb {

class MessageHandler;

// Writer implementation for directing HTML output to a string.
class StringWriter : public Writer {
 public:
  explicit StringWriter(std::string* str) : string_(str) {}
  ~StringWriter() override;
  bool Write(std::string_view str, MessageHandler* message_handler) override;
  bool Flush(MessageHandler* message_handler) override;
  bool Dump(Writer* writer, MessageHandler* message_handler) override;

 private:
  std::string* string_;

  StringWriter(const StringWriter&) = delete;
  StringWriter& operator=(const StringWriter&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_BASE_STRING_WRITER_H_
