// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_TOOLS_CLI_LINT_REPORT_H_
#define PAGESPEED_TOOLS_CLI_LINT_REPORT_H_

#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {
namespace lint {

// The output contract, schema "pagespeed-lint/1"
// (pagespeed/tools/cli/pagespeed-lint.schema.json).  Additive changes keep
// the "/1"; a rule id is never reused for a different check.
inline constexpr char kSchema[] = "pagespeed-lint/1";
inline constexpr char kToolVersion[] = "1.0.0";

// Exit codes.
inline constexpr int kExitClean = 0;     // nothing at or above --fail-on
inline constexpr int kExitFindings = 1;  // findings at or above --fail-on
inline constexpr int kExitUsage = 2;     // usage error or unreadable input
inline constexpr int kExitInternal = 3;  // internal error

enum class Severity { kInfo = 0, kWarning = 1, kError = 2 };
const char* SeverityName(Severity severity);
// Parses "error", "warning", "info"; false otherwise.
bool ParseSeverity(StringPiece text, Severity* severity);

// One finding.  Every string here is already safe to print: values that may
// hold a secret were replaced before the finding was made.
struct Finding {
  GoogleString rule;  // "PS101"
  Severity severity = Severity::kError;
  GoogleString file;  // root-relative, '/' separated
  int line = 0;       // 1-based; 0 when not about one line
  GoogleString scope;
  GoogleString directive;
  std::vector<GoogleString> args;
  GoogleString message;
};

struct Input {
  GoogleString path;  // root-relative, '/' separated
  GoogleString kind;  // "config", "include" or "htaccess"
  GoogleString sha256;
};

class Report {
 public:
  void AddInput(Input input) { inputs_.push_back(std::move(input)); }
  // At most kMaxFindings findings are kept; the next one is replaced by a
  // single PS907 "limit reached" finding and the rest are dropped.
  static constexpr size_t kMaxFindings = 20000;
  void Add(Finding finding);

  const std::vector<Input>& inputs() const { return inputs_; }
  const std::vector<Finding>& findings() const { return findings_; }
  int Count(Severity severity) const;

  // kExitFindings when a finding is at or above `fail_on`, else kExitClean.
  int ExitCode(Severity fail_on) const;

  // The JSON document.  Byte-for-byte the same for the same inputs on every
  // platform: findings in the order they were found, '\n' line ends, and
  // every byte outside printable ASCII escaped, so the output carries no
  // control character or terminal escape sequence.
  GoogleString ToJson(StringPiece module_version) const;
  // One line per finding, for a terminal; escaped the same way.
  GoogleString ToText() const;

 private:
  std::vector<Input> inputs_;
  std::vector<Finding> findings_;
};

// Appends `s` as a JSON string literal, escaping '"', '\\' and every byte
// outside 0x20..0x7e as \u00XX.
void AppendJsonString(StringPiece s, GoogleString* out);
// `s` with every byte outside 0x20..0x7e written as \xXX.
GoogleString PrintableText(StringPiece s);

}  // namespace lint
}  // namespace net_instaweb

#endif  // PAGESPEED_TOOLS_CLI_LINT_REPORT_H_
