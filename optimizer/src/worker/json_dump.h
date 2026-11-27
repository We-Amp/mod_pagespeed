// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Safe JSON Serialization
//
// DumpJson() is the one serialization point: every JSON response and stream
// message the worker emits goes through it instead of calling
// nlohmann::json::dump() directly (test/src/worker:json_output_test gates
// this with a source-tree scan).
//
// What it guarantees: the result is always well-formed JSON, whatever the
// values hold, and serializing never fails on a value's content. For
// well-formed input the output is byte-identical to calling dump() with the
// same arguments.

#ifndef PAGESPEED_SRC_WORKER_JSON_DUMP_H_
#define PAGESPEED_SRC_WORKER_JSON_DUMP_H_

#include <string>

#include "nlohmann/json.hpp"

namespace pagespeed {

inline std::string DumpJson(const nlohmann::json& j, int indent = -1) {
  return j.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_JSON_DUMP_H_
