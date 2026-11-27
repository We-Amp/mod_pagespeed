// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Ported from mod_pagespeed for PageSpeed 2.0

#ifndef PAGESPEED_LIB_BASE_NULL_WRITER_H_
#define PAGESPEED_LIB_BASE_NULL_WRITER_H_

#include <string_view>

#include "lib/base/writer.h"

namespace net_instaweb {

class MessageHandler;

// A writer that silently eats the bytes. This can be used, for
// example, with writers designed to cascade to another one, such
// as CountingWriter. If you just want to count the bytes and don't
// want to store them, you can pass a NullWriter to a CountingWriter's
// constructor.
class NullWriter : public Writer {
 public:
  NullWriter() {}
  ~NullWriter() override;
  bool Write(std::string_view str, MessageHandler* handler) override;
  bool Flush(MessageHandler* handler) override;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_BASE_NULL_WRITER_H_
