// SPDX-License-Identifier: Apache-2.0

#include "pagespeed/tools/lint_spike/apache_scope.h"

#include <map>
#include <vector>

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_query.h"
#include "pagespeed/apache/apache_config.h"
#include "pagespeed/apache/apache_directive_table.h"

namespace net_instaweb {
namespace lint {

namespace {

using namespace apache_directive;  // NOLINT

enum TableKind { kScope, kDir1, kServer1, kDir2, kFetch2, kFetch23, kServer2,
                 kDir3 };

struct TableEntry {
  TableKind kind;
  const char* name;
};

#define PAGESPEED_LINT_ENTRY(kind, name, help) {kind, name},
const TableEntry kTable[] = {
    PAGESPEED_APACHE_DIRECTIVE_TABLE(PAGESPEED_LINT_ENTRY)};
#undef PAGESPEED_LINT_ENTRY

struct CaseLess {
  bool operator()(const GoogleString& a, const GoogleString& b) const {
    return StringCaseCompare(a, b) < 0;
  }
};

std::vector<ApacheDirective>* directives = nullptr;
std::map<GoogleString, int, CaseLess>* index = nullptr;

void Add(ApacheDirective d) {
  if (index->find(d.name) != index->end()) {
    return;  // httpd uses the first command with this name.
  }
  (*index)[d.name] = static_cast<int>(directives->size());
  directives->push_back(std::move(d));
}

ApacheDirective FromTable(const TableEntry& e) {
  ApacheDirective d;
  d.name = e.name;
  d.vhost = VhostHandling::kNone;
  d.from_option_table = false;
  d.deprecated_property = false;
  switch (e.kind) {
    case kScope:
      d.args = ApacheArgs::kScopeBlock;
      d.req_override = kRsrcConf;
      d.parser = Parser::kScope;
      break;
    case kDir1:
      d.args = ApacheArgs::kTake1;
      d.req_override = kOrAll;
      d.parser = Parser::kParse1;
      break;
    case kServer1:
      d.args = ApacheArgs::kTake1;
      d.req_override = kRsrcConf;
      d.parser = Parser::kParse1;
      break;
    case kDir2:
      d.args = ApacheArgs::kTake2;
      d.req_override = kOrAll;
      d.parser = Parser::kParse2;
      break;
    case kFetch2:
      d.args = ApacheArgs::kTake2;
      d.req_override = kRsrcConf | kAccessConf;
      d.parser = Parser::kParse2;
      break;
    case kFetch23:
      d.args = ApacheArgs::kTake23;
      d.req_override = kRsrcConf | kAccessConf;
      d.parser = Parser::kParse3;
      break;
    case kServer2:
      d.args = ApacheArgs::kTake2;
      d.req_override = kRsrcConf;
      d.parser = Parser::kParse2;
      break;
    case kDir3:
      d.args = ApacheArgs::kTake3;
      d.req_override = kOrAll;
      d.parser = Parser::kParse3;
      break;
  }
  return d;
}

// ApacheProcessContext::FillInApacheCommand.
ApacheDirective FromOption(StringPiece option_name,
                           RewriteOptions::OptionScope scope,
                           bool deprecated) {
  ApacheDirective d;
  d.name = StrCat("ModPagespeed", option_name);
  d.args = ApacheArgs::kTake1;
  d.parser = Parser::kParse1;
  d.from_option_table = true;
  d.deprecated_property = deprecated;
  d.vhost = VhostHandling::kNone;
  switch (scope) {
    case RewriteOptions::kDirectoryScope:
    case RewriteOptions::kQueryScope:
      d.req_override = kOrAll;
      break;
    case RewriteOptions::kServerScope:
      d.req_override = kRsrcConf;
      break;
    case RewriteOptions::kProcessScopeStrict:
      d.vhost = VhostHandling::kError;
      d.req_override = kRsrcConf;
      break;
    case RewriteOptions::kLegacyProcessScope:
      d.vhost = VhostHandling::kTolerate;
      d.req_override = kRsrcConf;
      break;
  }
  return d;
}

}  // namespace

void InitApacheDirectives() {
  if (directives != nullptr) return;
  directives = new std::vector<ApacheDirective>;
  index = new std::map<GoogleString, int, CaseLess>;
  for (const TableEntry& e : kTable) {
    Add(FromTable(e));
  }
  // Same as ApacheProcessContext::InstallCommands.
  ApacheConfig config_template("install_commands", nullptr);
  for (auto* option : config_template.all_options()) {
    if (option->help_text() != nullptr) {
      Add(FromOption(option->option_name(), option->scope(), false));
    }
  }
  const RewriteOptions::Properties* deprecated =
      RewriteOptions::deprecated_properties();
  for (int i = 0, n = deprecated->size(); i < n; ++i) {
    const RewriteOptions::PropertyBase* p = deprecated->property(i);
    Add(FromOption(p->option_name(), p->scope(), true));
  }
}

const ApacheDirective* LookupApacheDirective(StringPiece name) {
  auto it = index->find(name.as_string());
  return it == index->end() ? nullptr : &(*directives)[it->second];
}

int NumApacheDirectives() { return static_cast<int>(directives->size()); }
const ApacheDirective& ApacheDirectiveAt(int i) { return (*directives)[i]; }

bool CheckArity(const ApacheDirective& d, int n, GoogleString* message) {
  switch (d.args) {
    case ApacheArgs::kTake1:
      if (n == 1) return true;
      *message = StrCat(d.name, " takes one argument");
      return false;
    case ApacheArgs::kTake2:
      if (n == 2) return true;
      *message = StrCat(d.name, " takes two arguments");
      return false;
    case ApacheArgs::kTake3:
      if (n == 3) return true;
      *message = StrCat(d.name, " takes three arguments");
      return false;
    case ApacheArgs::kTake23:
      if (n == 2 || n == 3) return true;
      *message = StrCat(d.name, " takes two or three arguments");
      return false;
    case ApacheArgs::kScopeBlock:
      if (n == 1) return true;
      *message = StrCat(d.name, "> takes one argument");
      return false;
  }
  return false;
}

ScopeResult CheckApacheScope(const ApacheDirective& d, const Placement& p) {
  ScopeResult r;
  // 1. httpd's own check: req_override & the context's allowed overrides
  //    (server/vhost: RSRC_CONF|OR_ALL; directory: ACCESS_CONF|OR_ALL;
  //    .htaccess: OR_ALL with AllowOverride All assumed).
  uint32_t allowed = 0;
  switch (p.kind) {
    case ContextKind::kServer:
    case ContextKind::kVirtualHost:
      allowed = kRsrcConf | kOrAll;
      break;
    case ContextKind::kDirectory:
      allowed = kAccessConf | kOrAll;
      break;
    case ContextKind::kHtaccess:
      allowed = kOrAll;
      break;
  }
  if ((d.req_override & allowed) == 0) {
    r.verdict = Verdict::kError;
    if (p.kind == ContextKind::kHtaccess) {
      r.rule = "PS304";
      r.message = StrCat(d.name, " not allowed in .htaccess");
    } else {
      r.rule = "PS301";
      r.message = StrCat(d.name, " not allowed here (",
                         p.kind == ContextKind::kDirectory
                             ? "inside <Directory>/<Location>/<Files>"
                             : "at server level",
                         ")");
    }
    return r;
  }
  // 2. <ModPagespeedIf> itself: ParseScope's NOT_IN_DIR_LOC_FILE|NOT_IN_LIMIT
  //    and ProcessParsedScope's no-nesting rule.
  if (d.parser == Parser::kScope) {
    if (p.in_conditional) {
      r.verdict = Verdict::kError;
      r.rule = "PS305";
      r.message = "Can't nest <ModPagespeedIf> blocks";
    } else if (p.kind == ContextKind::kDirectory || p.in_limit) {
      r.verdict = Verdict::kError;
      r.rule = "PS301";
      r.message = "<ModPagespeedIf> not allowed inside a directory section or "
                  "<Limit>";
    }
    return r;
  }
  // 3. CheckProcessScope / CheckGlobalOption.
  if (d.vhost != VhostHandling::kNone) {
    if (p.in_vhost) {
      if (d.vhost == VhostHandling::kError) {
        r.verdict = Verdict::kError;
        r.rule = "PS302";
        r.message = StrCat("Directive ", d.name,
                           " used inside a <VirtualHost> but applies "
                           "globally.");
        return r;
      }
      r.verdict = Verdict::kWarning;
      r.rule = "PS303";
      r.message = StrCat("Directive ", d.name,
                         " used inside a <VirtualHost> but applies globally. "
                         "Accepting for backwards compatibility.");
    }
    if (p.in_conditional) {
      r.verdict = Verdict::kError;
      r.rule = "PS306";
      r.message =
          StrCat("Global directive ", d.name, " invalid inside conditional.");
      return r;
    }
  }
  return r;
}

}  // namespace lint
}  // namespace net_instaweb
