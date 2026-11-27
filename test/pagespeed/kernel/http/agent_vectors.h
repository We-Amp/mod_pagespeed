// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// The shared known-agent vectors (test/system/daemon_html/agent_vectors.txt),
// read the way its header describes, for every port's unit tests.

#ifndef TEST_PAGESPEED_KERNEL_HTTP_AGENT_VECTORS_H_
#define TEST_PAGESPEED_KERNEL_HTTP_AGENT_VECTORS_H_

#include <vector>

#include "pagespeed/kernel/base/string.h"

namespace net_instaweb {

struct AgentVector {
  int line = 0;
  int expected_class = 0;
  GoogleString user_agent;  // "(empty)" read as ""
};

struct AgentVectors {
  std::vector<GoogleString> tokens;  // the "# token:" header lines
  std::vector<AgentVector> vectors;
};

// CHECK-fails when the file is missing or a vector line has no TAB.
AgentVectors LoadAgentVectors();

}  // namespace net_instaweb

#endif  // TEST_PAGESPEED_KERNEL_HTTP_AGENT_VECTORS_H_
