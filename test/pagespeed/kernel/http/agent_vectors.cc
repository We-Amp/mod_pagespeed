// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "test/pagespeed/kernel/http/agent_vectors.h"

#include <cstdlib>

#include "base/logging.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

AgentVectors LoadAgentVectors() {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString text;
  const GoogleString path =
      StrCat(GTestSrcDir(), "/test/system/daemon_html/agent_vectors.txt");
  CHECK(file_system.ReadFile(path.c_str(), &text, &handler)) << path;
  AgentVectors out;
  const StringPiece kToken = "# token: ";
  int line_number = 0;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == GoogleString::npos) end = text.size();
    const StringPiece line(text.data() + at, end - at);
    at = end + 1;
    ++line_number;
    if (line.starts_with(kToken)) {
      out.tokens.push_back(line.substr(kToken.size()).as_string());
      continue;
    }
    if (line.empty() || line[0] == '#') continue;
    const size_t tab = line.find('\t');
    CHECK(tab != StringPiece::npos) << path << ":" << line_number << ": no TAB";
    AgentVector vector;
    vector.line = line_number;
    vector.expected_class =
        std::atoi(line.substr(0, tab).as_string().c_str());
    const StringPiece ua = line.substr(tab + 1);
    vector.user_agent = ua == "(empty)" ? GoogleString() : ua.as_string();
    out.vectors.push_back(vector);
  }
  return out;
}

}  // namespace net_instaweb
