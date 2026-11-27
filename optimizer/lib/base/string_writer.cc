// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Ported from mod_pagespeed for PageSpeed 2.0

#include "lib/base/string_writer.h"

#include <string>
#include <string_view>

namespace net_instaweb {

class MessageHandler;

StringWriter::~StringWriter() = default;

bool StringWriter::Write(std::string_view str, MessageHandler* /*handler*/) {
  string_->append(str.data(), str.size());
  return true;
}

bool StringWriter::Flush(MessageHandler* /*message_handler*/) { return true; }

bool StringWriter::Dump(Writer* writer, MessageHandler* message_handler) {
  return writer->Write(*string_, message_handler);
}

}  // namespace net_instaweb
