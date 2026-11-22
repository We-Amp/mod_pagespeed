// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The Apache directive table of mod_pagespeed and the rules for where each
// directive may be used, without any httpd type.  mod_instaweb.cc builds its
// command_rec table and applies its placement checks from this file, and the
// offline config linter (//pagespeed/tools/cli) reads the same table and calls
// the same checks, so the two cannot drift.
//
// If you add a directive here, exercise it in a system test under
// test/system/ and give it a linter fixture under
// test/pagespeed/tools/cli/testdata/.

#ifndef PAGESPEED_APACHE_APACHE_DIRECTIVE_TABLE_H_
#define PAGESPEED_APACHE_APACHE_DIRECTIVE_TABLE_H_

#include <cstdint>
#include <functional>

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_query.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {
namespace apache_directive {

// Instaweb directive names -- these must match
// install/common/pagespeed.conf.template.
inline constexpr char kModPagespeedIf[] = "<ModPagespeedIf";

inline constexpr char kModPagespeedAdminDomains[] = "ModPagespeedAdminDomains";
inline constexpr char kModPagespeedAllow[] = "ModPagespeedAllow";
inline constexpr char kModPagespeedBlockingRewriteRefererUrls[] =
    "ModPagespeedBlockingRewriteRefererUrls";
inline constexpr char kModPagespeedConsoleDomains[] =
    "ModPagespeedConsoleDomains";
inline constexpr char kModPagespeedCreateSharedMemoryMetadataCache[] =
    "ModPagespeedCreateSharedMemoryMetadataCache";
inline constexpr char kModPagespeedAddResourceHeader[] =
    "ModPagespeedAddResourceHeader";
inline constexpr char kModPagespeedCustomFetchHeader[] =
    "ModPagespeedCustomFetchHeader";
inline constexpr char kModPagespeedDisableFilters[] =
    "ModPagespeedDisableFilters";
inline constexpr char kModPagespeedDisallow[] = "ModPagespeedDisallow";
inline constexpr char kModPagespeedDomain[] = "ModPagespeedDomain";
inline constexpr char kModPagespeedDownstreamCachePurgeLocationPrefix[] =
    "ModPagespeedDownstreamCachePurgeLocationPrefix";
inline constexpr char kModPagespeedEnableFilters[] =
    "ModPagespeedEnableFilters";
inline constexpr char kModPagespeedFetcherTimeoutMs[] =
    "ModPagespeedFetcherTimeOutMs";
inline constexpr char kModPagespeedFileCachePath[] =
    "ModPagespeedFileCachePath";
inline constexpr char kModPagespeedForbidFilters[] =
    "ModPagespeedForbidFilters";
inline constexpr char kModPagespeedForceCaching[] = "ModPagespeedForceCaching";
inline constexpr char kModPagespeedExperimentVariable[] =
    "ModPagespeedExperimentVariable";
inline constexpr char kModPagespeedExperimentSpec[] =
    "ModPagespeedExperimentSpec";
inline constexpr char kModPagespeedGlobalAdminDomains[] =
    "ModPagespeedGlobalAdminDomains";
inline constexpr char kModPagespeedGlobalStatisticsDomains[] =
    "ModPagespeedGlobalStatisticsDomains";
inline constexpr char kModPagespeedImageInlineMaxBytes[] =
    "ModPagespeedImageInlineMaxBytes";
inline constexpr char kModPagespeedImageMaxRewritesAtOnce[] =
    "ModPagespeedImageMaxRewritesAtOnce";
inline constexpr char kModPagespeedInstallCrashHandler[] =
    "ModPagespeedInstallCrashHandler";
inline constexpr char kModPagespeedLibrary[] = "ModPagespeedLibrary";
inline constexpr char kModPagespeedListOutstandingUrlsOnError[] =
    "ModPagespeedListOutstandingUrlsOnError";
inline constexpr char kModPagespeedLoadFromFile[] = "ModPagespeedLoadFromFile";
inline constexpr char kModPagespeedLoadFromFileMatch[] =
    "ModPagespeedLoadFromFileMatch";
inline constexpr char kModPagespeedLoadFromFileRule[] =
    "ModPagespeedLoadFromFileRule";
inline constexpr char kModPagespeedLoadFromFileRuleMatch[] =
    "ModPagespeedLoadFromFileRuleMatch";
inline constexpr char kModPagespeedLogDir[] = "ModPagespeedLogDir";
inline constexpr char kModPagespeedMapOriginDomain[] =
    "ModPagespeedMapOriginDomain";
inline constexpr char kModPagespeedMapProxyDomain[] =
    "ModPagespeedMapProxyDomain";
inline constexpr char kModPagespeedMapRewriteDomain[] =
    "ModPagespeedMapRewriteDomain";
inline constexpr char kModPagespeedMessageBufferSize[] =
    "ModPagespeedMessageBufferSize";
inline constexpr char kModPagespeedMessagesDomains[] =
    "ModPagespeedMessagesDomains";
inline constexpr char kModPagespeedNumExpensiveRewriteThreads[] =
    "ModPagespeedNumExpensiveRewriteThreads";
inline constexpr char kModPagespeedNumRewriteThreads[] =
    "ModPagespeedNumRewriteThreads";
inline constexpr char kModPagespeedPermitIdsForCssCombining[] =
    "ModPagespeedPermitIdsForCssCombining";
inline constexpr char kModPagespeedProxySuffix[] = "ModPagespeedProxySuffix";
inline constexpr char kModPagespeedRetainComment[] =
    "ModPagespeedRetainComment";
inline constexpr char kModPagespeedRulePackTrustKey[] =
    "ModPagespeedRulePackTrustKey";
inline constexpr char kModPagespeedRunExperiment[] =
    "ModPagespeedRunExperiment";
inline constexpr char kModPagespeedShardDomain[] = "ModPagespeedShardDomain";
inline constexpr char kModPagespeedStaticAssetPrefix[] =
    "ModPagespeedStaticAssetPrefix";
inline constexpr char kModPagespeedStatisticsDomains[] =
    "ModPagespeedStatisticsDomains";
inline constexpr char kModPagespeedTrackOriginalContentLength[] =
    "ModPagespeedTrackOriginalContentLength";
inline constexpr char kModPagespeedUrlValuedAttribute[] =
    "ModPagespeedUrlValuedAttribute";
inline constexpr char kModPagespeedUsePerVHostStatistics[] =
    "ModPagespeedUsePerVHostStatistics";

// The following are deprecated due to spelling
inline constexpr char kModPagespeedImgInlineMaxBytes[] =
    "ModPagespeedImgInlineMaxBytes";
inline constexpr char kModPagespeedImgMaxRewritesAtOnce[] =
    "ModPagespeedImgMaxRewritesAtOnce";
inline constexpr char kModPagespeedImageWebpRecompressionQuality[] =
    "ModPagespeedImageWebpRecompressionQuality";
inline constexpr char
    kModPagespeedImageWebpRecompressionQualityForSmallScreens[] =
        "ModPagespeedImageWebpRecompressionQualityForSmallScreens";

// httpd's req_override bits (http_config.h), with the same values;
// mod_instaweb.cc static_asserts that they match.
inline constexpr int kOrAll = 1 | 2 | 4 | 8 | 16;  // OR_ALL
inline constexpr int kAccessConf = 64;             // ACCESS_CONF
inline constexpr int kRsrcConf = 128;              // RSRC_CONF
inline constexpr int kExecOnRead = 256;            // EXEC_ON_READ

// How many arguments a directive takes (httpd's TAKE1, TAKE2, ...).
enum class Args : std::uint8_t { kTake1, kTake2, kTake3, kTake23 };

// Which of mod_instaweb.cc's callbacks parses a directive.
enum class Handler : std::uint8_t {
  kParseScope,       // <ModPagespeedIf>
  kParseDirective,   // one argument
  kParseDirective2,  // two arguments
  kParseDirective3,  // two or three arguments
};

// The kinds of hand-written directive in the table below.
enum class Kind : std::uint8_t {
  kScope,    // <ModPagespeedIf>; server config and <VirtualHost> only
  kDir1,     // one argument, anywhere including .htaccess
  kServer1,  // one argument, server config and <VirtualHost> only
  kDir2,     // two arguments, anywhere including .htaccess
  kFetch2,   // two arguments, anywhere except .htaccess
  kFetch23,  // two or three arguments, anywhere except .htaccess
  kServer2,  // two arguments, server config and <VirtualHost> only
  kDir3,     // three arguments, anywhere including .htaccess
};

struct KindInfo {
  Args args;
  int req_override;
  Handler handler;
};

constexpr KindInfo InfoFor(Kind kind) {
  switch (kind) {
    case Kind::kScope:
      return {Args::kTake1, kRsrcConf | kExecOnRead, Handler::kParseScope};
    case Kind::kDir1:
      return {Args::kTake1, kOrAll, Handler::kParseDirective};
    case Kind::kServer1:
      return {Args::kTake1, kRsrcConf, Handler::kParseDirective};
    case Kind::kDir2:
      return {Args::kTake2, kOrAll, Handler::kParseDirective2};
    // Directives that steer the server's own fetches: where a resource is
    // fetched from and with which request headers.  They are accepted in the
    // configuration the operator writes (server, <VirtualHost>, <Directory>,
    // <Location>) but not in .htaccess, as a fetch made for one site fills a
    // cache that other sites on the same server read.
    case Kind::kFetch2:
      return {Args::kTake2, kRsrcConf | kAccessConf, Handler::kParseDirective2};
    case Kind::kFetch23:
      return {Args::kTake23, kRsrcConf | kAccessConf,
              Handler::kParseDirective3};
    case Kind::kServer2:
      return {Args::kTake2, kRsrcConf, Handler::kParseDirective2};
    case Kind::kDir3:
      return {Args::kTake3, kOrAll, Handler::kParseDirective3};
  }
  return {Args::kTake1, 0, Handler::kParseDirective};
}

// How a process-scope option is treated inside a <VirtualHost>.
enum class VHostHandling : std::uint8_t {
  kNotProcessScope,  // not a process-scope option
  kTolerateInVHost,  // warn and accept (legacy process scope)
  kErrorInVHost,     // refuse (strict process scope)
};

// The command an option from the RewriteOptions table becomes: always one
// argument, parsed by ParseDirective, placed by its option scope.
struct OptionCommand {
  int req_override;
  VHostHandling vhost;
};

constexpr OptionCommand CommandForOptionScope(
    RewriteOptions::OptionScope scope) {
  switch (scope) {
    case RewriteOptions::kDirectoryScope:
    case RewriteOptions::kQueryScope:
      return {kOrAll, VHostHandling::kNotProcessScope};
    case RewriteOptions::kServerScope:
      return {kRsrcConf, VHostHandling::kNotProcessScope};
    case RewriteOptions::kProcessScopeStrict:
      return {kRsrcConf, VHostHandling::kErrorInVHost};
    case RewriteOptions::kLegacyProcessScope:
      return {kRsrcConf, VHostHandling::kTolerateInVHost};
  }
  return {kRsrcConf, VHostHandling::kNotProcessScope};
}

// Calls `fn` for every option-table command, in the order the module installs
// them after the hand-written table: every option of `config_template` with
// help text (an option without help text is not available in the module),
// then every deprecated property.  httpd resolves a name to the first command
// that has it.
void ForEachOptionCommand(
    const RewriteOptions& config_template,
    const std::function<void(StringPiece option_name,
                             RewriteOptions::OptionScope scope,
                             const char* help_text, bool deprecated)>& fn);

// Old spellings the module accepts under a new name.  Returns the name to
// parse `directive` as (`directive` itself when it is not an old spelling).
StringPiece CanonicalDirectiveName(StringPiece directive);

// mod_pagespeed's own rule for a process-scope option used below the main
// server config: inside a <VirtualHost> it is an error (strict) or a warning
// (legacy), and inside <ModPagespeedIf> it is always an error.  Either
// message may be empty.  `directive` is the name as written.
struct ProcessScopeUse {
  GoogleString warning;
  GoogleString error;
};
ProcessScopeUse CheckProcessScopeUse(StringPiece directive, VHostHandling mode,
                                     bool in_virtual_host, bool in_conditional);

// <ModPagespeedIf> rules.  `arg` is the argument as httpd passes it to the
// section handler, with the closing '>' (for example "spdy>").
bool ParseConditionalArgument(StringPiece arg, bool* for_spdy);
inline constexpr char kConditionalArgumentError[] =
    "Conditional must be spdy or !spdy.";
inline constexpr char kConditionalInDirectoryError[] =
    "Can't use <ModPagespeedIf except at top-level or VirtualHost context";
inline constexpr char kConditionalOnlyOurDirectivesError[] =
    "Only mod_pagespeed directives should be inside <ModPagespeedIf blocks";
inline constexpr char kConditionalNestedError[] =
    "Can't nest <ModPagespeedIf> blocks";

}  // namespace apache_directive
}  // namespace net_instaweb

// X(kind, name, help).  kind is a Kind enumerator name (kScope, kDir1, ...).
// Names are the constants above (in net_instaweb::apache_directive) or
// RewriteQuery::kModPagespeed; the user of the macro brings them into scope.
// The order is the module's command order: httpd uses the first command with
// a given name.
#define PAGESPEED_APACHE_DIRECTIVE_TABLE(X)                                    \
  /* Special conditional op. */                                                \
  X(kScope, kModPagespeedIf,                                                   \
    "Conditionally apply some mod_pagespeed options. "                         \
    "Possible arguments: spdy, !spdy")                                         \
                                                                               \
  X(kDir1, RewriteQuery::kModPagespeed, "Enable instaweb")                     \
  X(kDir1, kModPagespeedAllow, "wildcard_spec for urls")                       \
  X(kDir1, kModPagespeedDisableFilters,                                        \
    "Comma-separated list of disabled filters")                                \
  X(kDir1, kModPagespeedDisallow, "wildcard_spec for urls")                    \
  X(kDir1, kModPagespeedDomain,                                                \
    "Authorize mod_pagespeed to rewrite resources in a domain.")               \
  X(kDir1, kModPagespeedDownstreamCachePurgeLocationPrefix,                    \
    "The host:port/path prefix to be used for purging requests from "          \
    "the downstream cache.")                                                   \
  X(kDir1, kModPagespeedEnableFilters,                                         \
    "Comma-separated list of enabled filters")                                 \
  X(kDir1, kModPagespeedForbidFilters,                                         \
    "Comma-separated list of forbidden filters")                               \
  X(kDir1, kModPagespeedExperimentVariable,                                    \
    "Deprecated and ignored (configured the "                                  \
    "insert_ga filter).")                                                      \
  X(kDir1, kModPagespeedExperimentSpec,                                        \
    "Configuration for one side of an experiment in the form: "                \
    "'id= ;enabled= ;disabled= ;ga= ;percent= ...' The 'ga=' "                 \
    "component is accepted for compatibility and ignored.")                    \
  X(kDir1, kModPagespeedListOutstandingUrlsOnError,                            \
    "Adds an error message into the log for every URL fetch in "               \
    "flight when the HTTP stack encounters a system error, e.g. "              \
    "Connection Refused")                                                      \
  X(kDir1, kModPagespeedPermitIdsForCssCombining,                              \
    "Allow combining CSS files with IDs matching wildcard")                    \
  X(kDir1, kModPagespeedProxySuffix,                                           \
    "Sets up a proxy suffix to be used when slurping.")                        \
  X(kDir1, kModPagespeedRetainComment,                                         \
    "Retain HTML comments matching wildcard, even with remove_comments "       \
    "enabled")                                                                 \
  X(kDir1, kModPagespeedRunExperiment,                                         \
    "Run an experiment to test the effectiveness of rewriters.")               \
                                                                               \
  /* All one parameter deprecated options. */                                  \
  X(kDir1, kModPagespeedImgInlineMaxBytes,                                     \
    "DEPRECATED, use ModPagespeedImageInlineMaxBytes.")                        \
  X(kDir1, kModPagespeedImageWebpRecompressionQuality,                         \
    "Deprecated.  Use ModPagespeedWebpRecompressionQuality")                   \
  X(kDir1, kModPagespeedImageWebpRecompressionQualityForSmallScreens,          \
    "Deprecated.  Use ModPagespeedWebpRecompressionQualityForSmallScreens")    \
                                                                               \
  /* All one parameter options that can only be specified at the server */     \
  /* level. (Not in <Directory> blocks.) */                                    \
  X(kServer1, kModPagespeedFetcherTimeoutMs,                                   \
    "Set internal fetcher timeout in milliseconds")                            \
  X(kServer1, kModPagespeedForceCaching, "Ignore HTTP cache headers and TTLs") \
  X(kServer1, kModPagespeedImgMaxRewritesAtOnce,                               \
    "DEPRECATED, use ModPagespeedImageMaxRewritesAtOnce.")                     \
  X(kServer1, kModPagespeedInstallCrashHandler,                                \
    "Try to dump backtrace on crashes. For developer use")                     \
  X(kServer1, kModPagespeedMessageBufferSize,                                  \
    "Set the size of buffer used for /mod_pagespeed_message.")                 \
  X(kServer1, kModPagespeedNumRewriteThreads,                                  \
    "Number of threads to use for inexpensive portions of "                    \
    "resource-rewriting. <= 0 to auto-detect")                                 \
  X(kServer1, kModPagespeedNumExpensiveRewriteThreads,                         \
    "Number of threads to use for computation-intensive portions of "          \
    "resource-rewriting. <= 0 to auto-detect")                                 \
  X(kServer1, kModPagespeedStaticAssetPrefix,                                  \
    "Where to serve static support files for pagespeed filters from.")         \
  X(kServer1, kModPagespeedTrackOriginalContentLength,                         \
    "Add X-Original-Content-Length headers to rewritten resources")            \
  X(kServer1, kModPagespeedUsePerVHostStatistics,                              \
    "If true, keep track of statistics per VHost and not just globally")       \
  X(kServer1, kModPagespeedBlockingRewriteRefererUrls,                         \
    "wildcard_spec for referer urls which trigger blocking "                   \
    "rewrites")                                                                \
                                                                               \
  /* All two parameter options that are allowed in <Directory> blocks. */      \
  X(kDir2, kModPagespeedAddResourceHeader,                                     \
    "add_resource_header_name add_resource_header_value")                      \
  X(kFetch2, kModPagespeedCustomFetchHeader,                                   \
    "custom_header_name custom_header_value")                                  \
  X(kFetch23, kModPagespeedMapOriginDomain,                                    \
    "to_domain from_domain[,from_domain]* [host_header]")                      \
  X(kFetch23, kModPagespeedMapProxyDomain,                                     \
    "proxy_domain origin_domain [to_domain]")                                  \
  X(kDir2, kModPagespeedMapRewriteDomain,                                      \
    "to_domain from_domain[,from_domain]*")                                    \
  X(kDir2, kModPagespeedShardDomain,                                           \
    "from_domain shard_domain1[,shard_domain2]*")                              \
                                                                               \
  /* All two parameter options that can only be specified at the server */     \
  /* level. (Not in <Directory> blocks.) */                                    \
  X(kServer2, kModPagespeedCreateSharedMemoryMetadataCache, "name size_kb")    \
  X(kServer2, kModPagespeedRulePackTrustKey,                                   \
    "Trust rule packs signed with this key: <key id> "                         \
    "<base64 Ed25519 public key>")                                             \
  X(kServer2, kModPagespeedLoadFromFile, "url_prefix filename_prefix")         \
  X(kServer2, kModPagespeedLoadFromFileMatch, "url_regexp filename_prefix")    \
  X(kServer2, kModPagespeedLoadFromFileRule,                                   \
    "<Allow|Disallow> filename_prefix")                                        \
  X(kServer2, kModPagespeedLoadFromFileRuleMatch,                              \
    "<Allow|Disallow> filename_regexp")                                        \
  X(kServer2, kModPagespeedStatisticsDomains,                                  \
    "<Allow|Disallow> domain_wildcard")                                        \
  X(kServer2, kModPagespeedGlobalStatisticsDomains,                            \
    "<Allow|Disallow> domain_wildcard")                                        \
  X(kServer2, kModPagespeedMessagesDomains,                                    \
    "<Allow|Disallow> domain_wildcard")                                        \
  X(kServer2, kModPagespeedConsoleDomains, "<Allow|Disallow> domain_wildcard") \
  X(kServer2, kModPagespeedAdminDomains, "<Allow|Disallow> domain_wildcard")   \
  X(kServer2, kModPagespeedGlobalAdminDomains,                                 \
    "<Allow|Disallow> domain_wildcard")                                        \
                                                                               \
  X(kServer2, "ModPagespeedExperimentalMeasurementProxy",                      \
    "https://root.domain password (Experimental)")                             \
                                                                               \
  /* All three parameter options that are allowed in <Directory> blocks. */    \
  X(kDir3, kModPagespeedUrlValuedAttribute,                                    \
    "Specify an additional url-valued attribute.")                             \
  X(kDir3, kModPagespeedLibrary,                                               \
    "Specify size, md5, and canonical url for JavaScript library, "            \
    "separated by spaces.\n"                                                   \
    "These values may be obtained by running:\n"                               \
    "  js_minify --print_size_and_hash library.js\n"                           \
    "Yielding an entry like:\n"                                                \
    "  ModPagespeedLibrary 105527 ltVVzzYxo0 "                                 \
    "//ajax.googleapis.com/ajax/libs/1.6.1.0/prototype.js")

#endif  // PAGESPEED_APACHE_APACHE_DIRECTIVE_TABLE_H_
