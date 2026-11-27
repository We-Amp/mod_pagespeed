// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Ported from mod_pagespeed for PageSpeed 2.0

#include "lib/base/writer.h"

namespace net_instaweb {

Writer::~Writer() = default;

bool Writer::Dump(Writer* /*writer*/, MessageHandler* /*message_handler*/) {
  return false;
}

}  // namespace net_instaweb
