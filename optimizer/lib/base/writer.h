// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Ported from mod_pagespeed for PageSpeed 2.0

#ifndef PAGESPEED_LIB_BASE_WRITER_H_
#define PAGESPEED_LIB_BASE_WRITER_H_

#include <string_view>

namespace net_instaweb {

class MessageHandler;

// Interface for writing bytes to an output stream.
class Writer {
 public:
  Writer() {}
  virtual ~Writer();

  virtual bool Write(std::string_view str, MessageHandler* handler) = 0;
  virtual bool Flush(MessageHandler* message_handler) = 0;

  // Dumps the contents of what's been written to the Writer. Many
  // Writer implementations will not be able to do this, and the default
  // implementation will return false. But StringWriter can dump its
  // contents, and overrides this with an implementation that returns true.
  virtual bool Dump(Writer* writer, MessageHandler* message_handler);

 private:
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_BASE_WRITER_H_
