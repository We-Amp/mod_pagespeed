// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/tools/cli/apache_lint.h"

#include <algorithm>
#include <utility>

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/apache/apache_config.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/system/system_process_options.h"
#include "pagespeed/tools/cli/log_capture.h"

namespace net_instaweb {
namespace lint {

using apache_directive::Args;
using apache_directive::Handler;
using apache_directive::VHostHandling;

namespace {

constexpr char kPrefix[] = "ModPagespeed";
constexpr size_t kPrefixSize = sizeof(kPrefix) - 1;

// httpd's own override bits that matter for placement (http_config.h).
constexpr int kOrLimit = 1;
constexpr int kOrAuthCfg = 8;

// Messages from the option parser are captured, never printed directly: they
// may quote a value.
class CaptureHandler : public MessageHandler {
 public:
  std::vector<GoogleString> warnings;

 protected:
  void MessageSImpl(MessageType type, const GoogleString& message) override {
    if (type >= kWarning) {
      warnings.push_back(message);
    }
  }
  void FileMessageSImpl(MessageType type, const char* filename, int line,
                        const GoogleString& message) override {
    MessageSImpl(type, message);
  }
};

// Names longer than this get no "did you mean" suggestion: a directive or
// filter name is short, and the distance costs the product of the lengths.
constexpr size_t kMaxSuggestLength = 128;

// Levenshtein distance on ASCII-lowered strings, or `bound` when it is at
// least `bound` (computed only that far).
int Distance(StringPiece a, StringPiece b, int bound) {
  const size_t diff =
      a.size() > b.size() ? a.size() - b.size() : b.size() - a.size();
  if (a.size() > kMaxSuggestLength || b.size() > kMaxSuggestLength ||
      diff >= static_cast<size_t>(bound)) {
    return bound;
  }
  std::vector<int> prev(b.size() + 1);
  std::vector<int> cur(b.size() + 1);
  for (size_t j = 0; j <= b.size(); ++j) {
    prev[j] = static_cast<int>(j);
  }
  for (size_t i = 1; i <= a.size(); ++i) {
    cur[0] = static_cast<int>(i);
    int row_min = cur[0];
    for (size_t j = 1; j <= b.size(); ++j) {
      const bool same = LowerChar(a[i - 1]) == LowerChar(b[j - 1]);
      cur[j] =
          std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (same ? 0 : 1)});
      row_min = std::min(row_min, cur[j]);
    }
    if (row_min >= bound) {
      return bound;
    }
    std::swap(prev, cur);
  }
  return std::min(prev[b.size()], bound);
}

const char* ArgsText(Args args) {
  switch (args) {
    case Args::kTake1:
      return "one argument";
    case Args::kTake2:
      return "two arguments";
    case Args::kTake3:
      return "three arguments";
    case Args::kTake23:
      return "two or three arguments";
  }
  return "";
}

bool ArityOk(Args args, size_t n) {
  switch (args) {
    case Args::kTake1:
      return n == 1;
    case Args::kTake2:
      return n == 2;
    case Args::kTake3:
      return n == 3;
    case Args::kTake23:
      return n == 2 || n == 3;
  }
  return false;
}

bool IsFilterList(StringPiece option) {
  return StringCaseEqual(option, RewriteOptions::kEnableFilters) ||
         StringCaseEqual(option, RewriteOptions::kDisableFilters) ||
         StringCaseEqual(option, RewriteOptions::kForbidFilters);
}

// The nearest configurable filter name to `name`, or empty.
GoogleString NearestFilter(StringPiece name) {
  GoogleString best;
  int best_distance = 4;
  for (int i = RewriteOptions::kFirstFilter; i < RewriteOptions::kEndOfFilters;
       ++i) {
    StringPiece candidate = RewriteOptions::LookupFilterName(
        static_cast<RewriteOptions::Filter>(i));
    if (candidate.empty()) {
      continue;
    }
    const int d = Distance(name, candidate, best_distance);
    if (d < best_distance) {
      best_distance = d;
      best = candidate.as_string();
    }
  }
  return best;
}

}  // namespace

// ---------------------------------------------------------------------------

ApacheCommandTable::ApacheCommandTable() {
  using namespace apache_directive;  // NOLINT(build/namespaces)
#define PAGESPEED_LINT_COMMAND(kind, name, help)                         \
  Add({name, InfoFor(Kind::kind).args, InfoFor(Kind::kind).req_override, \
       InfoFor(Kind::kind).handler, VHostHandling::kNotProcessScope, false});
  PAGESPEED_APACHE_DIRECTIVE_TABLE(PAGESPEED_LINT_COMMAND)
#undef PAGESPEED_LINT_COMMAND
  ApacheConfig config_template("lint_commands", nullptr);
  ForEachOptionCommand(
      config_template,
      [this](StringPiece option_name, RewriteOptions::OptionScope scope,
             const char* help_text, bool deprecated) {
        const OptionCommand command = CommandForOptionScope(scope);
        Add({StrCat(kPrefix, option_name), Args::kTake1, command.req_override,
             Handler::kParseDirective, command.vhost, deprecated});
      });
}

void ApacheCommandTable::Add(ApacheCommand command) {
  if (index_.find(command.name) != index_.end()) {
    return;  // httpd uses the first command with a name.
  }
  index_[command.name] = commands_.size();
  commands_.push_back(std::move(command));
}

const ApacheCommand* ApacheCommandTable::Find(StringPiece name) const {
  auto it = index_.find(name.as_string());
  return it == index_.end() ? nullptr : &commands_[it->second];
}

const ApacheCommand* ApacheCommandTable::Nearest(StringPiece name) const {
  const ApacheCommand* best = nullptr;
  int best_distance = 4;
  for (const ApacheCommand& c : commands_) {
    if (c.handler == Handler::kParseScope) {
      continue;
    }
    const int d = Distance(name, c.name, best_distance);
    if (d < best_distance) {
      best_distance = d;
      best = &c;
    }
  }
  return best;
}

// ---------------------------------------------------------------------------

struct ApacheLinter::Line {
  const GoogleString& file;
  int line;
  const Placement& placement;
  const GoogleString& name;  // as written
  const std::vector<GoogleString>& args;
  GoogleString shown_name;
  std::vector<GoogleString> shown_args;
  bool values_safe;
};

ApacheLinter::ApacheLinter(const ApacheCommandTable* table,
                           const ValueMask* mask, ThreadSystem* thread_system,
                           Report* report)
    : table_(table),
      mask_(mask),
      thread_system_(thread_system),
      report_(report) {}

void ApacheLinter::Add(const Line& at, const char* rule, Severity severity,
                       const GoogleString& message) {
  Finding f;
  f.rule = rule;
  f.severity = severity;
  f.file = at.file;
  f.line = at.line;
  f.scope = ScopeLabel(at.placement);
  f.directive = at.shown_name;
  f.args = at.shown_args;
  f.message = message;
  report_->Add(std::move(f));
}

void ApacheLinter::ReportUnknown(const Line& at) {
  // A name that was removed on purpose is known, so it may be shown.
  if (StringCaseStartsWith(at.name, kPrefix)) {
    for (const RewriteOptions::RemovedOption& removed :
         RewriteOptions::RemovedOptions()) {
      if (StringCaseEqual(StringPiece(at.name).substr(kPrefixSize),
                          removed.name)) {
        Line known = at;
        known.shown_name = StrCat(kPrefix, removed.name);
        Add(known, "PS203", Severity::kError,
            StrCat(known.shown_name, " was removed and no longer loads; ",
                   removed.instead));
        return;
      }
    }
  }
  // A secret can be glued onto a mistyped name ("ModPagespeedUrlSigningKey=
  // s3cret", "ModPagespeedUrlSigningKeys3cret"), so the name is shown only
  // when the mask is lifted.  What is shown instead is a known name.
  for (const char separator : {'=', ':'}) {
    const size_t at_sep = at.name.find(separator);
    if (at_sep == GoogleString::npos) {
      continue;
    }
    const ApacheCommand* known = table_->Find(at.name.substr(0, at_sep));
    if (known != nullptr) {
      Line fixed = at;
      fixed.shown_name = known->name;
      fixed.shown_args.assign(at.args.size() + 1, kMasked);
      if (mask_->showing_all()) {
        fixed.shown_args = at.args;
        fixed.shown_args.insert(fixed.shown_args.begin(),
                                at.name.substr(at_sep + 1));
      }
      Add(fixed, "PS106", Severity::kError,
          StrCat(known->name, " is followed by '", GoogleString(1, separator),
                 "': Apache directives take their arguments after a space"));
      return;
    }
  }
  for (const ApacheCommand& c : table_->commands()) {
    if (at.name.size() > c.name.size() &&
        StringCaseStartsWith(at.name, c.name) &&
        c.handler != Handler::kParseScope) {
      // Prefer the longest known name that is a prefix.
      const ApacheCommand* longest = &c;
      for (const ApacheCommand& d : table_->commands()) {
        if (d.name.size() > longest->name.size() &&
            at.name.size() > d.name.size() &&
            StringCaseStartsWith(at.name, d.name)) {
          longest = &d;
        }
      }
      if (longest->name.size() > kPrefixSize) {
        Add(at, "PS101", Severity::kError,
            StrCat("Unknown directive (name not shown): text follows ",
                   longest->name, " without a space"));
        return;
      }
      break;
    }
  }
  const ApacheCommand* nearest = table_->Nearest(at.name);
  Add(at, "PS101", Severity::kError,
      nearest != nullptr
          ? StrCat("Unknown directive (name not shown); did you mean ",
                   nearest->name, "?")
          : GoogleString("Unknown directive (name not shown)"));
}

bool ApacheLinter::CheckPlacement(const Line& at,
                                  const ApacheCommand& command) {
  using apache_directive::kAccessConf;
  using apache_directive::kOrAll;
  using apache_directive::kRsrcConf;
  // httpd's own check: the command's req_override against what the context
  // allows.  A .htaccess file is read with AllowOverride All assumed.
  int allowed = 0;
  switch (at.placement.kind) {
    case ContextKind::kServer:
    case ContextKind::kVirtualHost:
      allowed = (kRsrcConf | kOrAll) & ~(kOrAuthCfg | kOrLimit);
      break;
    case ContextKind::kDirectory:
      allowed = kAccessConf | kOrAll;
      break;
    case ContextKind::kHtaccess:
      allowed = kOrAll;
      break;
  }
  if ((command.req_override & allowed) == 0) {
    switch (at.placement.kind) {
      case ContextKind::kHtaccess:
        Add(at, "PS304", Severity::kError,
            StrCat(command.name, " is not allowed in .htaccess"));
        break;
      case ContextKind::kDirectory:
        Add(at, "PS301", Severity::kError,
            StrCat(command.name,
                   " is not allowed inside <Directory>, <Location>, <Files>, "
                   "<If> or <Proxy>"));
        break;
      default:
        Add(at, "PS301", Severity::kError,
            StrCat(command.name, " is not allowed at server level"));
        break;
    }
    return false;
  }
  if (at.placement.in_limit && (command.req_override & kOrLimit) == 0) {
    Add(at, "PS301", Severity::kError,
        StrCat(command.name, " is not allowed inside <Limit>"));
    return false;
  }
  // mod_instaweb.cc's process-scope rule (ParseDirective only).
  if (command.handler == Handler::kParseDirective) {
    const apache_directive::ProcessScopeUse use =
        apache_directive::CheckProcessScopeUse(command.name, command.vhost,
                                               at.placement.in_virtual_host,
                                               at.placement.in_conditional);
    if (!use.warning.empty()) {
      Add(at, "PS303", Severity::kWarning, use.warning);
    }
    if (!use.error.empty()) {
      const bool vhost_error = at.placement.in_virtual_host &&
                               command.vhost == VHostHandling::kErrorInVHost;
      Add(at, vhost_error ? "PS302" : "PS306", Severity::kError, use.error);
      return false;
    }
  }
  return true;
}

void ApacheLinter::Directive(const GoogleString& file, int line,
                             const Placement& placement,
                             const GoogleString& name,
                             const std::vector<GoogleString>& args) {
  const ApacheCommand* command = table_->Find(name);
  Line at{file, line, placement, name, args, "", {}, false};
  if (command == nullptr) {
    at.shown_name = mask_->Text(name);
    at.shown_args.assign(args.size(), kMasked);
    if (mask_->showing_all()) {
      at.shown_args = args;
    }
    ReportUnknown(at);
    return;
  }
  // The name as written matched a known command, so it holds no value.
  at.shown_name = name;
  at.values_safe = mask_->ValuesAreSafe(name);
  at.shown_args = mask_->Args(name, args);
  if (command->handler == Handler::kParseScope) {
    return;  // A "<ModPagespeedIf" written without '<' cannot reach here.
  }
  if (!ArityOk(command->args, args.size())) {
    Add(at, "PS103", Severity::kError,
        StrCat(command->name, " takes ", ArgsText(command->args)));
    return;
  }
  if (!CheckPlacement(at, *command)) {
    return;
  }
  CheckValue(at, *command);
}

void ApacheLinter::Conditional(const GoogleString& file, int line,
                               const Placement& placement,
                               const std::vector<GoogleString>& args) {
  static const GoogleString kName = "<ModPagespeedIf>";
  // The argument is shown only when it is one of the two valid ones.
  std::vector<GoogleString> shown;
  shown.reserve(args.size());
  for (const GoogleString& a : args) {
    shown.push_back(StringCaseEqual(a, "spdy") || StringCaseEqual(a, "!spdy")
                        ? a
                        : mask_->Text(a));
  }
  Line at{file, line, placement, kName, args, kName, shown, true};
  if (placement.kind == ContextKind::kHtaccess) {
    Add(at, "PS304", Severity::kError,
        "<ModPagespeedIf> is not allowed in .htaccess");
    return;
  }
  if (placement.in_conditional) {
    Add(at, "PS305", Severity::kError,
        apache_directive::kConditionalNestedError);
    return;
  }
  if (placement.kind == ContextKind::kDirectory || placement.in_limit) {
    Add(at, "PS301", Severity::kError,
        "<ModPagespeedIf> is not allowed inside a directory section or "
        "<Limit>");
    return;
  }
  if (args.size() != 1) {
    Add(at, "PS103", Severity::kError, "<ModPagespeedIf takes one argument");
    return;
  }
  bool for_spdy = false;
  if (!apache_directive::ParseConditionalArgument(StrCat(args[0], ">"),
                                                  &for_spdy)) {
    Add(at, "PS102", Severity::kError,
        apache_directive::kConditionalArgumentError);
  }
}

void ApacheLinter::CheckFilterList(const Line& at, StringPiece list) {
  const bool enabling = StringCaseStartsWith(at.name, "ModPagespeedEnable");
  StringPieceVector names;
  SplitStringPieceToVector(list, ",", &names, true);
  for (StringPiece raw : names) {
    StringPiece name = raw;
    TrimWhitespace(&name);
    if (name.empty()) {
      continue;
    }
    RewriteOptions::FilterSet scratch;
    if (!RewriteOptions::AddByNameToFilterSet(name, &scratch, nullptr,
                                              enabling)) {
      // An unknown name is not shown: it may be anything.
      const GoogleString nearest = NearestFilter(name);
      Add(at, "PS105", Severity::kError,
          nearest.empty() ? GoogleString("Unknown filter name (not shown)")
                          : StrCat("Unknown filter name (not shown); did you "
                                   "mean ",
                                   nearest, "?"));
      continue;
    }
    const RewriteOptions::Filter filter = RewriteOptions::LookupFilter(name);
    if (enabling && filter != RewriteOptions::kEndOfFilters) {
      const char* note = RewriteOptions::DeprecatedFilterNote(filter);
      if (note != nullptr) {
        Add(at, "PS204", Severity::kWarning, note);
      }
    }
  }
}

void ApacheLinter::CheckValue(const Line& at, const ApacheCommand& command) {
  RewriteOptions::ResetDeprecatedFilterWarnings();  // per line, deterministic
  ApacheConfig config("lint", thread_system_);
  CaptureHandler handler;
  LogCapture log_capture;
  GoogleString msg;
  RewriteOptions::OptionSettingResult result =
      RewriteOptions::kOptionNameUnknown;
  const bool process_scope = !at.placement.in_virtual_host;
  StringPiece directive = at.name;
  bool removed_property = false;
  bool ignored_here = false;
  bool filter_list = false;

  if (command.handler == Handler::kParseDirective) {
    const GoogleString& arg = at.args[0];
    if (StringCaseEqual(directive,
                        apache_directive::kModPagespeedFileCachePath) ||
        StringCaseEqual(directive, apache_directive::kModPagespeedLogDir)) {
      // The module creates the directory and sets the path; nothing else is
      // checked, and the linter does not touch the file system.
      return;
    }
    StringPiece canonical = apache_directive::CanonicalDirectiveName(directive);
    if (canonical != directive) {
      Add(at, "PS201", Severity::kWarning,
          StrCat(at.name, " is a deprecated spelling; use ", canonical));
      directive = canonical;
    }
    StringPiece option = directive.substr(kPrefixSize);
    if (!option.empty()) {
      filter_list = IsFilterList(option);
      result =
          (StringCaseEqual(option, RewriteOptions::kExperimentSpec) &&
           at.placement.kind == ContextKind::kHtaccess)
              ? config.ParseAndSetOptionFromNameWithScope(
                    option, arg, RewriteOptions::kDirectoryScope, &msg,
                    &handler)
              : config.ParseAndSetOptionFromName1(option, arg, &msg, &handler);
      if (result == RewriteOptions::kOptionNameUnknown) {
        SystemProcessOptions::Value value;
        result = SystemProcessOptions::Parse1(option, arg, process_scope,
                                              &value, &msg, &handler);
        ignored_here = result == RewriteOptions::kOptionOk && !value.apply;
        if (result == RewriteOptions::kOptionValueInvalid && !process_scope) {
          // Below process scope Parse1 refuses the option without looking at
          // its value: a placement error, not a value error.
          Add(at, "PS302", Severity::kError,
              StrCat(at.name,
                     " applies to the whole server and cannot be set inside "
                     "<VirtualHost>"));
          return;
        }
      }
      if (result == RewriteOptions::kOptionNameUnknown &&
          RewriteOptions::IsDeprecatedOptionName(option)) {
        removed_property = true;
        result = RewriteOptions::kOptionOk;
      }
    } else {
      RewriteOptions::EnabledEnum enabled;
      result = RewriteOptions::ParseFromString(arg, &enabled)
                   ? RewriteOptions::kOptionOk
                   : RewriteOptions::kOptionValueInvalid;
      msg = "ModPagespeed must be on, off, standby or unplugged";
    }
    if (filter_list) {
      CheckFilterList(at, arg);
    }
  } else {
    StringPiece option = directive.substr(kPrefixSize);
    if (command.handler == Handler::kParseDirective2) {
      result = config.ParseAndSetOptionFromName2(option, at.args[0], at.args[1],
                                                 &msg, &handler);
      if (result == RewriteOptions::kOptionNameUnknown) {
        bool apply = false;
        int64 size_kb = 0;
        result = SystemProcessOptions::Parse2(option, at.args[0], at.args[1],
                                              process_scope, &apply, &size_kb,
                                              &msg, &handler);
        ignored_here = result == RewriteOptions::kOptionOk && !apply;
      }
    } else {
      result = config.ParseAndSetOptionFromName3(
          option, at.args[0], at.args[1],
          at.args.size() > 2 ? StringPiece(at.args[2]) : StringPiece(), &msg,
          &handler);
    }
  }

  const int logged_errors = log_capture.errors();
  if (logged_errors > 0) {
    Add(at, "PS111", Severity::kWarning,
        StrCat("The option parser logged ", logged_errors,
               " internal error(s) for this line (text not shown)"));
  }
  if (result == RewriteOptions::kOptionNameUnknown) {
    Add(at, "PS101", Severity::kError,
        StrCat(command.name,
               " is registered with Apache but the module does "
               "not handle it"));
    return;
  }
  if (result == RewriteOptions::kOptionValueInvalid && !filter_list) {
    Add(at, "PS102", Severity::kError,
        at.values_safe
            ? (msg.empty() ? StrCat("Invalid value for ", at.name) : msg)
            : StrCat("Invalid value for ", at.name, " (value not shown)"));
  }
  if (removed_property) {
    Add(at, "PS202", Severity::kWarning,
        StrCat(at.name,
               " was removed and is ignored; remove it from the "
               "configuration"));
    return;
  }
  if (ignored_here) {
    Add(at, "PS303", Severity::kWarning,
        StrCat(at.name,
               " applies to the whole server and is ignored inside "
               "<VirtualHost>"));
  }
  GoogleString deprecated;
  const StringPiece option_name = directive.substr(kPrefixSize);
  const bool is_deprecated =
      !option_name.empty() &&
      (SystemRewriteOptions::DeprecatedOptionMessage(option_name,
                                                     &deprecated) ||
       (command.handler == Handler::kParseDirective2 && at.args.size() >= 2 &&
        RewriteOptions::DeprecatedOptionMessage(option_name, at.args[0],
                                                at.args[1], &deprecated)));
  if (is_deprecated) {
    Add(at, "PS202", Severity::kWarning,
        at.values_safe ? deprecated
                       : StrCat(at.name, " is deprecated (text not shown, the "
                                         "value may be secret)"));
  }
  if (filter_list || ignored_here) {
    return;  // Their parser warnings are reported above.
  }
  for (const GoogleString& w : handler.warnings) {
    if (is_deprecated && w == deprecated) {
      continue;
    }
    Add(at, "PS110", Severity::kWarning,
        at.values_safe ? w
                       : StrCat(at.name,
                                ": the option parser gave a warning (text not "
                                "shown, the value may be secret)"));
  }
}

bool LintApacheFiles(const std::filesystem::path& root, StringPiece config,
                     const std::vector<GoogleString>& htaccess,
                     const ApacheCommandTable& table, const ValueMask& mask,
                     ThreadSystem* thread_system, Report* report) {
  ApacheLinter linter(&table, &mask, thread_system, report);
  ApacheConfigReader reader(root, &mask, report, &linter);
  if (!config.empty() && !reader.ReadConfig(config)) {
    return false;
  }
  for (const GoogleString& file : htaccess) {
    if (!reader.ReadHtaccess(file)) {
      return false;
    }
  }
  return true;
}

}  // namespace lint
}  // namespace net_instaweb
