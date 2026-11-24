// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "net/instaweb/rewriter/public/served_savings.h"

namespace net_instaweb {

ServedSavings::Outcome ServedSavings::Record(const Response& response,
                                             RewriteStats* stats) {
  return kNotCounted;
}

}  // namespace net_instaweb
