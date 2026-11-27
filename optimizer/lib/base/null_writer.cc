// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Ported from mod_pagespeed for PageSpeed 2.0

#include "lib/base/null_writer.h"

#include <string_view>

namespace net_instaweb {

class MessageHandler;

NullWriter::~NullWriter() = default;

bool NullWriter::Write(std::string_view /*str*/, MessageHandler* /*handler*/) {
  return true;
}

bool NullWriter::Flush(MessageHandler* /*handler*/) { return true; }

}  // namespace net_instaweb
