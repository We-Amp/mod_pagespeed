// SPDX-License-Identifier: Apache-2.0
//
// Spike: the Apache placement rules of mod_pagespeed's directives as a pure
// function of (directive, context), without cmd_parms or any httpd type.
//
// The directive set and each directive's Apache class come from the same two
// sources mod_instaweb.cc builds its command table from:
//   1. the hand-written table PAGESPEED_APACHE_DIRECTIVE_TABLE
//      (pagespeed/apache/apache_directive_table.h), and
//   2. every ApacheConfig option with help text, plus the deprecated
//      properties, with the Apache class derived from the option scope the
//      way ApacheProcessContext::FillInApacheCommand derives it.
// Apache resolves a name to the first matching command, so (1) wins over (2).

#ifndef PAGESPEED_TOOLS_LINT_SPIKE_APACHE_SCOPE_H_
#define PAGESPEED_TOOLS_LINT_SPIKE_APACHE_SCOPE_H_

#include <cstdint>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {
namespace lint {

// Mirrors httpd's req_override bits that matter here.
enum ApacheOverride : uint32_t {
  kRsrcConf = 1,    // server config and <VirtualHost>
  kAccessConf = 2,  // <Directory>, <Location>, <Files>, <If>
  kOrAll = 4,       // anywhere, including .htaccess
};

enum class ApacheArgs { kTake1, kTake2, kTake3, kTake23, kScopeBlock };

// How a process-scope option is treated inside <VirtualHost>
// (mod_instaweb.cc VHostHandling).
enum class VhostHandling { kNone, kTolerate, kError };

// Which ParseDirective* the module would call.
enum class Parser { kParse1, kParse2, kParse3, kScope };

struct ApacheDirective {
  GoogleString name;  // as registered, e.g. "ModPagespeedEnableFilters"
  ApacheArgs args;
  uint32_t req_override;
  VhostHandling vhost;
  Parser parser;
  bool from_option_table;  // false: hand-written table
  bool deprecated_property;
};

enum class ContextKind {
  kServer,       // main server config
  kVirtualHost,  // inside <VirtualHost>
  kDirectory,    // inside <Directory>/<Location>/<Files>/<If> (any vhost)
  kHtaccess,     // a .htaccess file
};

struct Placement {
  ContextKind kind = ContextKind::kServer;
  bool in_vhost = false;        // cmd->server->is_virtual
  bool in_conditional = false;  // inside <ModPagespeedIf>
  bool in_limit = false;        // inside <Limit>/<LimitExcept>
};

enum class Verdict { kOk, kWarning, kError };

struct ScopeResult {
  Verdict verdict = Verdict::kOk;
  const char* rule = "";  // PS3xx
  GoogleString message;   // never contains an argument value
};

// Must be called after ApacheConfig::Initialize().
void InitApacheDirectives();

// Case-insensitive, like httpd.  nullptr when httpd would say
// "Invalid command".
const ApacheDirective* LookupApacheDirective(StringPiece name);

// The httpd req_override check plus mod_pagespeed's own CmdOptions /
// CheckProcessScope / ParseScope checks.
ScopeResult CheckApacheScope(const ApacheDirective& d, const Placement& p);

// Arity check as httpd's TAKEn does it.  Returns false with a message.
bool CheckArity(const ApacheDirective& d, int nargs, GoogleString* message);

int NumApacheDirectives();

}  // namespace lint
}  // namespace net_instaweb

#endif  // PAGESPEED_TOOLS_LINT_SPIKE_APACHE_SCOPE_H_
