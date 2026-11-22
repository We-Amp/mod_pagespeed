// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_TOOLS_CLI_APACHE_LINT_H_
#define PAGESPEED_TOOLS_CLI_APACHE_LINT_H_

#include <filesystem>
#include <map>
#include <memory>
#include <vector>

#include "pagespeed/apache/apache_directive_table.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/tools/cli/apache_reader.h"
#include "pagespeed/tools/cli/lint_report.h"
#include "pagespeed/tools/cli/value_mask.h"

namespace net_instaweb {
namespace lint {

// One command httpd would accept for mod_pagespeed, as the module registers
// it: the hand-written table first, then one per option with help text, then
// one per deprecated property (httpd uses the first command with a name).
struct ApacheCommand {
  GoogleString name;  // "ModPagespeedEnableFilters"
  apache_directive::Args args;
  int req_override;
  apache_directive::Handler handler;
  apache_directive::VHostHandling vhost;
  bool deprecated_property;  // accepted, does nothing
};

// The module's Apache command set, built the way mod_instaweb.cc builds it.
// Requires ApacheConfig::Initialize().
class ApacheCommandTable {
 public:
  ApacheCommandTable();
  // Case-insensitive, like httpd; nullptr when httpd would refuse the name.
  const ApacheCommand* Find(StringPiece name) const;
  const std::vector<ApacheCommand>& commands() const { return commands_; }
  // The known command whose name is closest to `name`, for a suggestion;
  // nullptr when none is close.
  const ApacheCommand* Nearest(StringPiece name) const;

 private:
  void Add(ApacheCommand command);
  struct CaseLess {
    bool operator()(const GoogleString& a, const GoogleString& b) const {
      return StringCaseCompare(a, b) < 0;
    }
  };
  std::vector<ApacheCommand> commands_;
  std::map<GoogleString, size_t, CaseLess> index_;
};

// Checks each mod_pagespeed line with the module's own rules and option
// parser:
//   PS1xx validity: unknown directive, invalid value, wrong arity, a value the
//         parser logged an internal error for;
//   PS2xx deprecated and removed directives, spellings and filters;
//   PS3xx placement: a directive httpd refuses in this context (server,
//         <VirtualHost>, directory section, .htaccess), the process-scope and
//         <ModPagespeedIf> rules of mod_instaweb.cc.
// Values are masked by `mask`.
class ApacheLinter : public ApacheLineVisitor {
 public:
  ApacheLinter(const ApacheCommandTable* table, const ValueMask* mask,
               ThreadSystem* thread_system, Report* report);

  void Directive(const GoogleString& file, int line, const Placement& placement,
                 const GoogleString& name,
                 const std::vector<GoogleString>& args) override;
  void Conditional(const GoogleString& file, int line,
                   const Placement& placement,
                   const std::vector<GoogleString>& args) override;

 private:
  struct Line;
  void Add(const Line& at, const char* rule, Severity severity,
           const GoogleString& message);
  bool CheckPlacement(const Line& at, const ApacheCommand& command);
  void CheckValue(const Line& at, const ApacheCommand& command);
  void CheckFilterList(const Line& at, StringPiece list);
  void ReportUnknown(const Line& at);

  const ApacheCommandTable* table_;
  const ValueMask* mask_;
  ThreadSystem* thread_system_;
  Report* report_;
};

// Lints the Apache configuration `config` (none when empty) and the
// .htaccess files `htaccess`, all named relative to `root`, into `report`.
// False when one of those files cannot be read; the report then holds what
// was found before it.  Requires ApacheConfig::Initialize().
bool LintApacheFiles(const std::filesystem::path& root, StringPiece config,
                     const std::vector<GoogleString>& htaccess,
                     const ApacheCommandTable& table, const ValueMask& mask,
                     ThreadSystem* thread_system, Report* report);

}  // namespace lint
}  // namespace net_instaweb

#endif  // PAGESPEED_TOOLS_CLI_APACHE_LINT_H_
