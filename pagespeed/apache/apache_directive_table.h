// SPDX-License-Identifier: Apache-2.0
//
// The Apache directive table of mod_pagespeed, without any httpd types, so
// that the module (which wraps each entry in AP_INIT_TAKEn) and the offline
// config linter read one list and cannot drift.  Spike: see
// pagespeed/tools/lint_spike/.

#ifndef PAGESPEED_APACHE_APACHE_DIRECTIVE_TABLE_H_
#define PAGESPEED_APACHE_APACHE_DIRECTIVE_TABLE_H_

#include "net/instaweb/rewriter/public/rewrite_query.h"

namespace net_instaweb {
namespace apache_directive {

inline constexpr char kModPagespeedIf[] = "<ModPagespeedIf";

inline constexpr char kModPagespeedAdminDomains[] = "ModPagespeedAdminDomains";
inline constexpr char kModPagespeedAllow[] = "ModPagespeedAllow";
inline constexpr char kModPagespeedBlockingRewriteRefererUrls[] =
    "ModPagespeedBlockingRewriteRefererUrls";
inline constexpr char kModPagespeedConsoleDomains[] = "ModPagespeedConsoleDomains";
inline constexpr char kModPagespeedCreateSharedMemoryMetadataCache[] =
    "ModPagespeedCreateSharedMemoryMetadataCache";
inline constexpr char kModPagespeedAddResourceHeader[] = "ModPagespeedAddResourceHeader";
inline constexpr char kModPagespeedCustomFetchHeader[] = "ModPagespeedCustomFetchHeader";
inline constexpr char kModPagespeedDisableFilters[] = "ModPagespeedDisableFilters";
inline constexpr char kModPagespeedDisallow[] = "ModPagespeedDisallow";
inline constexpr char kModPagespeedDomain[] = "ModPagespeedDomain";
inline constexpr char kModPagespeedDownstreamCachePurgeLocationPrefix[] =
    "ModPagespeedDownstreamCachePurgeLocationPrefix";
inline constexpr char kModPagespeedEnableFilters[] = "ModPagespeedEnableFilters";
inline constexpr char kModPagespeedFetcherTimeoutMs[] = "ModPagespeedFetcherTimeOutMs";
inline constexpr char kModPagespeedFileCachePath[] = "ModPagespeedFileCachePath";
inline constexpr char kModPagespeedForbidFilters[] = "ModPagespeedForbidFilters";
inline constexpr char kModPagespeedForceCaching[] = "ModPagespeedForceCaching";
inline constexpr char kModPagespeedExperimentVariable[] = "ModPagespeedExperimentVariable";
inline constexpr char kModPagespeedExperimentSpec[] = "ModPagespeedExperimentSpec";
inline constexpr char kModPagespeedGlobalAdminDomains[] = "ModPagespeedGlobalAdminDomains";
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
inline constexpr char kModPagespeedLoadFromFileMatch[] = "ModPagespeedLoadFromFileMatch";
inline constexpr char kModPagespeedLoadFromFileRule[] = "ModPagespeedLoadFromFileRule";
inline constexpr char kModPagespeedLoadFromFileRuleMatch[] =
    "ModPagespeedLoadFromFileRuleMatch";
inline constexpr char kModPagespeedLogDir[] = "ModPagespeedLogDir";
inline constexpr char kModPagespeedMapOriginDomain[] = "ModPagespeedMapOriginDomain";
inline constexpr char kModPagespeedMapProxyDomain[] = "ModPagespeedMapProxyDomain";
inline constexpr char kModPagespeedMapRewriteDomain[] = "ModPagespeedMapRewriteDomain";
inline constexpr char kModPagespeedMessageBufferSize[] = "ModPagespeedMessageBufferSize";
inline constexpr char kModPagespeedMessagesDomains[] = "ModPagespeedMessagesDomains";
inline constexpr char kModPagespeedNumExpensiveRewriteThreads[] =
    "ModPagespeedNumExpensiveRewriteThreads";
inline constexpr char kModPagespeedNumRewriteThreads[] = "ModPagespeedNumRewriteThreads";
inline constexpr char kModPagespeedPermitIdsForCssCombining[] =
    "ModPagespeedPermitIdsForCssCombining";
inline constexpr char kModPagespeedProxySuffix[] = "ModPagespeedProxySuffix";
inline constexpr char kModPagespeedRetainComment[] = "ModPagespeedRetainComment";
inline constexpr char kModPagespeedRulePackTrustKey[] = "ModPagespeedRulePackTrustKey";
inline constexpr char kModPagespeedRunExperiment[] = "ModPagespeedRunExperiment";
inline constexpr char kModPagespeedShardDomain[] = "ModPagespeedShardDomain";
inline constexpr char kModPagespeedStaticAssetPrefix[] = "ModPagespeedStaticAssetPrefix";
inline constexpr char kModPagespeedStatisticsDomains[] = "ModPagespeedStatisticsDomains";
inline constexpr char kModPagespeedTrackOriginalContentLength[] =
    "ModPagespeedTrackOriginalContentLength";
inline constexpr char kModPagespeedUrlValuedAttribute[] = "ModPagespeedUrlValuedAttribute";
inline constexpr char kModPagespeedUsePerVHostStatistics[] =
    "ModPagespeedUsePerVHostStatistics";

// The following are deprecated due to spelling
inline constexpr char kModPagespeedImgInlineMaxBytes[] = "ModPagespeedImgInlineMaxBytes";
inline constexpr char kModPagespeedImgMaxRewritesAtOnce[] =
    "ModPagespeedImgMaxRewritesAtOnce";
inline constexpr char kModPagespeedImageWebpRecompressionQuality[] =
    "ModPagespeedImageWebpRecompressionQuality";
inline constexpr char kModPagespeedImageWebpRecompressionQualityForSmallScreens[] =
    "ModPagespeedImageWebpRecompressionQualityForSmallScreens";

}  // namespace apache_directive
}  // namespace net_instaweb

// X(kind, name, help).  kind is one of
//   kScope    <ModPagespeedIf>, RSRC_CONF | EXEC_ON_READ
//   kDir1     TAKE1, OR_ALL
//   kServer1  TAKE1, RSRC_CONF
//   kDir2     TAKE2, OR_ALL
//   kFetch2   TAKE2, RSRC_CONF | ACCESS_CONF
//   kFetch23  TAKE23, RSRC_CONF | ACCESS_CONF
//   kServer2  TAKE2, RSRC_CONF
//   kDir3     TAKE3, OR_ALL
// Names are the constants above (in net_instaweb::apache_directive) or
// RewriteQuery::kModPagespeed; the user of the macro brings them into scope.
#define PAGESPEED_APACHE_DIRECTIVE_TABLE(X) \
    X(kScope, kModPagespeedIf, \
                        "Conditionally apply some mod_pagespeed options. " \
                        "Possible arguments: spdy, !spdy") \
    X(kDir1, RewriteQuery::kModPagespeed, "Enable instaweb") \
    X(kDir1, kModPagespeedAllow, "wildcard_spec for urls") \
    X(kDir1, kModPagespeedDisableFilters, \
                             "Comma-separated list of disabled filters") \
    X(kDir1, kModPagespeedDisallow, "wildcard_spec for urls") \
    X(kDir1,  \
        kModPagespeedDomain, \
        "Authorize mod_pagespeed to rewrite resources in a domain.") \
    X(kDir1,  \
        kModPagespeedDownstreamCachePurgeLocationPrefix, \
        "The host:port/path prefix to be used for purging requests from " \
        "the downstream cache.") \
    X(kDir1, kModPagespeedEnableFilters, \
                             "Comma-separated list of enabled filters") \
    X(kDir1, kModPagespeedForbidFilters, \
                             "Comma-separated list of forbidden filters") \
    X(kDir1, kModPagespeedExperimentVariable, \
                             "Deprecated and ignored (configured the " \
                             "insert_ga filter).") \
    X(kDir1,  \
        kModPagespeedExperimentSpec, \
        "Configuration for one side of an experiment in the form: " \
        "'id= ;enabled= ;disabled= ;ga= ;percent= ...' The 'ga=' " \
        "component is accepted for compatibility and ignored.") \
    X(kDir1,  \
        kModPagespeedListOutstandingUrlsOnError, \
        "Adds an error message into the log for every URL fetch in " \
        "flight when the HTTP stack encounters a system error, e.g. " \
        "Connection Refused") \
    X(kDir1,  \
        kModPagespeedPermitIdsForCssCombining, \
        "Allow combining CSS files with IDs matching wildcard") \
    X(kDir1,  \
        kModPagespeedProxySuffix, \
        "Sets up a proxy suffix to be used when slurping.") \
    X(kDir1,  \
        kModPagespeedRetainComment, \
        "Retain HTML comments matching wildcard, even with remove_comments " \
        "enabled") \
    X(kDir1,  \
        kModPagespeedRunExperiment, \
        "Run an experiment to test the effectiveness of rewriters.") \
    X(kDir1,  \
        kModPagespeedImgInlineMaxBytes, \
        "DEPRECATED, use ModPagespeedImageInlineMaxBytes.") \
    X(kDir1,  \
        kModPagespeedImageWebpRecompressionQuality, \
        "Deprecated.  Use ModPagespeedWebpRecompressionQuality") \
    X(kDir1,  \
        kModPagespeedImageWebpRecompressionQualityForSmallScreens, \
        "Deprecated.  Use ModPagespeedWebpRecompressionQualityForSmallScreens") \
    X(kServer1, kModPagespeedFetcherTimeoutMs, \
                         "Set internal fetcher timeout in milliseconds") \
    X(kServer1, kModPagespeedForceCaching, \
                         "Ignore HTTP cache headers and TTLs") \
    X(kServer1, kModPagespeedImgMaxRewritesAtOnce, \
                         "DEPRECATED, use ModPagespeedImageMaxRewritesAtOnce.") \
    X(kServer1, kModPagespeedInstallCrashHandler, \
                         "Try to dump backtrace on crashes. For developer use") \
    X(kServer1,  \
        kModPagespeedMessageBufferSize, \
        "Set the size of buffer used for /mod_pagespeed_message.") \
    X(kServer1, kModPagespeedNumRewriteThreads, \
                         "Number of threads to use for inexpensive portions of " \
                         "resource-rewriting. <= 0 to auto-detect") \
    X(kServer1,  \
        kModPagespeedNumExpensiveRewriteThreads, \
        "Number of threads to use for computation-intensive portions of " \
        "resource-rewriting. <= 0 to auto-detect") \
    X(kServer1,  \
        kModPagespeedStaticAssetPrefix, \
        "Where to serve static support files for pagespeed filters from.") \
    X(kServer1,  \
        kModPagespeedTrackOriginalContentLength, \
        "Add X-Original-Content-Length headers to rewritten resources") \
    X(kServer1,  \
        kModPagespeedUsePerVHostStatistics, \
        "If true, keep track of statistics per VHost and not just globally") \
    X(kServer1,  \
        kModPagespeedBlockingRewriteRefererUrls, \
        "wildcard_spec for referer urls which trigger blocking " \
        "rewrites") \
    X(kDir2,  \
        kModPagespeedAddResourceHeader, \
        "add_resource_header_name add_resource_header_value") \
    X(kFetch2, kModPagespeedCustomFetchHeader, \
                                "custom_header_name custom_header_value") \
    X(kFetch23,  \
        kModPagespeedMapOriginDomain, \
        "to_domain from_domain[,from_domain]* [host_header]") \
    X(kFetch23, kModPagespeedMapProxyDomain, \
                                 "proxy_domain origin_domain [to_domain]") \
    X(kDir2, kModPagespeedMapRewriteDomain, \
                              "to_domain from_domain[,from_domain]*") \
    X(kDir2, kModPagespeedShardDomain, \
                              "from_domain shard_domain1[,shard_domain2]*") \
    X(kServer2, kModPagespeedCreateSharedMemoryMetadataCache, \
                          "name size_kb") \
    X(kServer2, kModPagespeedRulePackTrustKey, \
                          "Trust rule packs signed with this key: <key id> " \
                          "<base64 Ed25519 public key>") \
    X(kServer2, kModPagespeedLoadFromFile, \
                          "url_prefix filename_prefix") \
    X(kServer2, kModPagespeedLoadFromFileMatch, \
                          "url_regexp filename_prefix") \
    X(kServer2, kModPagespeedLoadFromFileRule, \
                          "<Allow|Disallow> filename_prefix") \
    X(kServer2, kModPagespeedLoadFromFileRuleMatch, \
                          "<Allow|Disallow> filename_regexp") \
    X(kServer2, kModPagespeedStatisticsDomains, \
                          "<Allow|Disallow> domain_wildcard") \
    X(kServer2, kModPagespeedGlobalStatisticsDomains, \
                          "<Allow|Disallow> domain_wildcard") \
    X(kServer2, kModPagespeedMessagesDomains, \
                          "<Allow|Disallow> domain_wildcard") \
    X(kServer2, kModPagespeedConsoleDomains, \
                          "<Allow|Disallow> domain_wildcard") \
    X(kServer2, kModPagespeedAdminDomains, \
                          "<Allow|Disallow> domain_wildcard") \
    X(kServer2, kModPagespeedGlobalAdminDomains, \
                          "<Allow|Disallow> domain_wildcard") \
    X(kServer2, "ModPagespeedExperimentalMeasurementProxy", \
                          "https://root.domain password (Experimental)") \
    X(kDir3, kModPagespeedUrlValuedAttribute, \
                              "Specify an additional url-valued attribute.") \
    X(kDir3,  \
        kModPagespeedLibrary, \
        "Specify size, md5, and canonical url for JavaScript library, " \
        "separated by spaces.\n" \
        "These values may be obtained by running:\n" \
        "  js_minify --print_size_and_hash library.js\n" \
        "Yielding an entry like:\n" \
        "  ModPagespeedLibrary 105527 ltVVzzYxo0 " \
        "//ajax.googleapis.com/ajax/libs/1.6.1.0/prototype.js") \
  /* end */

#endif  // PAGESPEED_APACHE_APACHE_DIRECTIVE_TABLE_H_
