/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "ngx_rewrite_options.h"

extern "C" {
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
}

#include "net/instaweb/public/version.h"
#include "net/instaweb/rewriter/public/file_load_policy.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "ngx_pagespeed.h"
#include "ngx_rewrite_driver_factory.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/system/daemon_reader.h"
#include "pagespeed/system/system_caches.h"

namespace net_instaweb {

namespace {

const char kStatisticsPath[] = "StatisticsPath";
const char kGlobalStatisticsPath[] = "GlobalStatisticsPath";
const char kConsolePath[] = "ConsolePath";
const char kMessagesPath[] = "MessagesPath";
const char kAdminPath[] = "AdminPath";
const char kGlobalAdminPath[] = "GlobalAdminPath";
const char kDaemonApiSocketPath[] = "DaemonApiSocketPath";
const char kWebBotAuth[] = "WebBotAuth";
const char kWebBotAuthTelemetry[] = "WebBotAuthTelemetry";
const char kWebBotAuthBotDetection[] = "WebBotAuthBotDetection";
const char kWebBotAuthPublicCounter[] = "WebBotAuthPublicCounter";
const char kWebBotAuthDirectoryHost[] = "WebBotAuthDirectoryHost";
const char kWebBotAuthVerifiedBots[] = "WebBotAuthVerifiedBots";
const char kWebBotAuthKeyDirectoryFile[] = "WebBotAuthKeyDirectoryFile";
const char kWebBotAuthKeyDirectoryUrl[] = "WebBotAuthKeyDirectoryUrl";
const char kWebBotAuthKeyDirectoryAllowlist[] =
    "WebBotAuthKeyDirectoryAllowlist";
const char kWebBotAuthKeyDirectoryRefreshSec[] =
    "WebBotAuthKeyDirectoryRefreshSec";
const char kRslCapEnforcement[] = "RslCapEnforcement";
const char kRslCapKeyDirectoryFile[] = "RslCapKeyDirectoryFile";
const char kRslCapDirectoryHost[] = "RslCapDirectoryHost";
const char kRslCapRequestedLicense[] = "RslCapRequestedLicense";
const char kRslCapRequestedScope[] = "RslCapRequestedScope";
const char kRslCapIssuer[] = "RslCapIssuer";
const char kRslCapKeyDirectoryUrl[] = "RslCapKeyDirectoryUrl";
const char kRslCapKeyDirectoryAllowlist[] = "RslCapKeyDirectoryAllowlist";
const char kRslCapKeyDirectoryRefreshSec[] = "RslCapKeyDirectoryRefreshSec";

// These options are copied from mod_instaweb.cc, where APACHE_CONFIG_OPTIONX
// indicates that they can not be set at the directory/location level. They set
// options in the RewriteDriverFactory, so they're entirely global and do not
// appear in RewriteOptions.  They are not alphabetized on purpose, but rather
// left in the same order as in mod_instaweb.cc in case we end up needing to
// compare.
// TODO(oschaaf): this duplication is a short term solution.
const char* const server_only_options[] = {
    "FetcherTimeoutMs",
    "FetchProxy",
    "ForceCaching",
    "GeneratedFilePrefix",
    "ImgMaxRewritesAtOnce",
    "InheritVHostConfig",
    "InstallCrashHandler",
    "MessageBufferSize",
    "NumRewriteThreads",
    "NumExpensiveRewriteThreads",
    "StaticAssetPrefix",
    "TrackOriginalContentLength",
    "UsePerVHostStatistics",  // TODO(anupama): What to do about "No longer used"
    "BlockingRewriteRefererUrls",
    "CreateSharedMemoryMetadataCache",
    "LoadFromFile",
    "LoadFromFileMatch",
    "LoadFromFileRule",
    "LoadFromFileRuleMatch",
    "UseNativeFetcher",
    "NativeFetcherMaxKeepaliveRequests"};

// Options that can only be used in the main (http) option scope.
const char* const main_only_options[] = {"UseNativeFetcher",
                                         "NativeFetcherMaxKeepaliveRequests"};

}  // namespace

RewriteOptions::Properties* NgxRewriteOptions::ngx_properties_ = nullptr;

NgxRewriteOptions::NgxRewriteOptions(const StringPiece& description,
                                     ThreadSystem* thread_system)
    : SystemRewriteOptions(description, thread_system) {
  Init();
}

NgxRewriteOptions::NgxRewriteOptions(ThreadSystem* thread_system)
    : SystemRewriteOptions(thread_system) {
  Init();
}

void NgxRewriteOptions::Init() {
  DCHECK(ngx_properties_ != nullptr)
      << "Call NgxRewriteOptions::Initialize() before construction";
  clear_inherited_scripts_ = false;
  InitializeOptions(ngx_properties_);
}

void NgxRewriteOptions::AddProperties() {
  // Nginx-specific options.
  add_ngx_option("", &NgxRewriteOptions::statistics_path_, "nsp",
                 kStatisticsPath, kServerScope,
                 "Set the statistics path. Ex: /ngx_pagespeed_statistics",
                 false);
  add_ngx_option(
      "", &NgxRewriteOptions::global_statistics_path_, "ngsp",
      kGlobalStatisticsPath, kProcessScopeStrict,
      "Set the global statistics path. Ex: /ngx_pagespeed_global_statistics",
      false);
  add_ngx_option("", &NgxRewriteOptions::console_path_, "ncp", kConsolePath,
                 kServerScope, "Set the console path. Ex: /pagespeed_console",
                 false);
  add_ngx_option("", &NgxRewriteOptions::messages_path_, "nmp", kMessagesPath,
                 kServerScope,
                 "Set the messages path.  Ex: /ngx_pagespeed_message", false);
  add_ngx_option("", &NgxRewriteOptions::admin_path_, "nap", kAdminPath,
                 kServerScope, "Set the admin path.  Ex: /pagespeed_admin",
                 false);
  add_ngx_option("", &NgxRewriteOptions::global_admin_path_, "ngap",
                 kGlobalAdminPath, kProcessScopeStrict,
                 "Set the global admin path.  Ex: /pagespeed_global_admin",
                 false);
  add_ngx_option(
      kDefaultDaemonApiSocketPath, &NgxRewriteOptions::daemon_api_socket_path_,
      "dasp", kDaemonApiSocketPath, kServerScope,
      "Set the unix socket path of the optimizer daemon's management API, "
      "backing the /v1/daemon/* admin endpoints.  Empty disables them.",
      true);

  // the design record A1 Web-Bot-Auth (observe-only RFC 9421 verifier). All default off /
  // empty: zero behavior change unless the operator opts in.
  add_ngx_option(false, &NgxRewriteOptions::web_bot_auth_, "wba", kWebBotAuth,
                 kServerScope,
                 "Enable observe-only Web-Bot-Auth (RFC 9421) request "
                 "classification, surfaced as $x_verified_bot. Never blocks. "
                 "Default off.",
                 false);
  add_ngx_option(
      false, &NgxRewriteOptions::web_bot_auth_telemetry_, "wbat",
      kWebBotAuthTelemetry, kServerScope,
      "Count verified/signed-agent requests in the opt-in "
      "web_bot_auth_verified_signed_requests statistic (and non-web-bot-auth "
      "signature material in web_bot_auth_other_signature_requests). "
      "Default off.",
      false);
  // The one directive that lets the Web-Bot-Auth verdict change behaviour.
  // Kept separate from WebBotAuth, and default off, because WebBotAuth shipped
  // as observe-only: a deployment that enabled it for telemetry must not
  // silently acquire a change in which requests beacon.
  add_ngx_option(
      false, &NgxRewriteOptions::web_bot_auth_bot_detection_, "wbabd",
      kWebBotAuthBotDetection, kServerScope,
      "Let a verified Web-Bot-Auth signature classify the request as an "
      "automated client for PageSpeed's own bot detection, so a signed agent "
      "is recognised even when it presents a browser user-agent. Suppresses "
      "the measurement beacons and lazyload for that request; never blocks it. "
      "Requires WebBotAuth. Default off (verdict stays observe-only).",
      false);
  // the design record Bar-A (experimental): opt-in verified-crawl counter mode. One of
  // off (default) | private | public; gates the well-known counter endpoint
  // /.well-known/webbotauth-counter. NON-secret; the gating bearer token is a
  // SEPARATE secret carried via the PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN env
  // var, NEVER a directive (a directive would land in world-readable config).
  add_ngx_option(
      "", &NgxRewriteOptions::web_bot_auth_public_counter_, "wbapc",
      kWebBotAuthPublicCounter, kServerScope,
      "Opt-in verified-crawl counter mode (experimental): off (default) | "
      "private | public. Enabling a non-off mode publishes a discoverable "
      "marker at /.well-known/webbotauth-counter; the exact document is gated "
      "by the PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN env bearer token. Default "
      "off (endpoint invisible).",
      false);
  add_ngx_option(
      "", &NgxRewriteOptions::web_bot_auth_directory_host_, "wbadh",
      kWebBotAuthDirectoryHost, kServerScope,
      "The signer directory's host identity: the warm-fetch cache key, paired with WebBotAuthKeyDirectoryUrl. Ignored by the local-file "
      "static provider. Never request-derived. Default empty.",
      false);
  add_ngx_option("", &NgxRewriteOptions::web_bot_auth_verified_bots_, "wbavb",
                 kWebBotAuthVerifiedBots, kServerScope,
                 "Verified-bot registry: comma-separated keyid=name pairs.",
                 false);
  add_ngx_option("", &NgxRewriteOptions::web_bot_auth_key_directory_file_,
                 "wbakdf", kWebBotAuthKeyDirectoryFile, kServerScope,
                 "Path to a local JWKS file (the signer's published key "
                 "directory, copied locally by the operator) used to verify "
                 "signatures. A1 v1: no network fetch. Default empty.",
                 false);
  // the design record A2 network warm-fetch (default empty => off; A1 v1 behavior intact).
  add_ngx_option(
      "", &NgxRewriteOptions::web_bot_auth_key_directory_url_, "wbakdu",
      kWebBotAuthKeyDirectoryUrl, kServerScope,
      "HTTPS URL of the signer's JWKS directory, fetched off-request by a "
      "background thread and cached; the request path reads cache-only and "
      "never fetches. Requires WebBotAuthKeyDirectoryAllowlist and "
      "WebBotAuthDirectoryHost. Default empty (no network fetch).",
      false);
  add_ngx_option(
      "", &NgxRewriteOptions::web_bot_auth_key_directory_allowlist_, "wbakda",
      kWebBotAuthKeyDirectoryAllowlist, kServerScope,
      "SSRF allowlist: comma-separated https origins (scheme://host[:port]) "
      "the "
      "warm-fetch may contact. Empty => warm-fetch disabled (fail-closed). "
      "Default empty.",
      false);
  add_ngx_option(
      "", &NgxRewriteOptions::web_bot_auth_key_directory_refresh_sec_, "wbakdr",
      kWebBotAuthKeyDirectoryRefreshSec, kServerScope,
      "Warm-fetch refresh interval in seconds (clamped). Empty => default "
      "3600. Default empty.",
      false);

  // the design record A3 RSL-CAP enforcement (PAID; demand-gated). All default off /
  // empty: zero behavior change unless the operator explicitly opts in. When
  // enabled, the validator verdict maps to an inline 401/402; the engine never
  // settles/meters money.
  add_ngx_option(false, &NgxRewriteOptions::rsl_cap_enforcement_, "rce",
                 kRslCapEnforcement, kServerScope,
                 "Enable RSL-CAP capability-token enforcement: an "
                 "Authorization: License token is validated and the verdict is "
                 "mapped to an inline 401/402. Never settles/meters. Default "
                 "off.",
                 false);
  add_ngx_option("", &NgxRewriteOptions::rsl_cap_key_directory_file_, "rckdf",
                 kRslCapKeyDirectoryFile, kServerScope,
                 "Path to a local JWKS file (the issuer's published key "
                 "directory, copied locally by the operator) used to resolve "
                 "RSL-CAP signing keys. v1: synchronous, no network fetch. "
                 "Default empty.",
                 false);
  add_ngx_option(
      "", &NgxRewriteOptions::rsl_cap_directory_host_, "rcdh",
      kRslCapDirectoryHost, kServerScope,
      "Operator-mapped issuer directory host passed to the key "
      "provider (plane-split, never request-derived). Ignored by the "
      "v1 static-file provider; used by the follow-up network "
      "provider. Default empty.",
      false);
  add_ngx_option("", &NgxRewriteOptions::rsl_cap_requested_license_, "rcrl",
                 kRslCapRequestedLicense, kServerScope,
                 "License id this route requires; a token must grant it (and "
                 "the requested scope) to be authorized, else 402. Default "
                 "empty.",
                 false);
  add_ngx_option(
      "", &NgxRewriteOptions::rsl_cap_requested_scope_, "rcrs",
      kRslCapRequestedScope, kServerScope,
      "Scope this route requires; a token must grant it (and the "
      "requested license) to be authorized, else 402. Default empty.",
      false);
  add_ngx_option("", &NgxRewriteOptions::rsl_cap_issuer_, "rci", kRslCapIssuer,
                 kServerScope,
                 "Optional issuer pin: when set, an otherwise-authorized token "
                 "whose iss != this value is rejected (401). Use when a "
                 "directory host may serve multiple issuers. Default empty.",
                 false);
  // the design record A2 network warm-fetch for the RSL-CAP issuer directory (mirrors the
  // WebBotAuth* warm-fetch options; default empty => v1 static-file behavior).
  add_ngx_option(
      "", &NgxRewriteOptions::rsl_cap_key_directory_url_, "rckdu",
      kRslCapKeyDirectoryUrl, kServerScope,
      "HTTPS URL of the issuer's JWKS directory, fetched off-request by a "
      "background thread and cached; the request path reads cache-only. "
      "Requires RslCapKeyDirectoryAllowlist and RslCapDirectoryHost. Default "
      "empty (no network fetch).",
      false);
  add_ngx_option(
      "", &NgxRewriteOptions::rsl_cap_key_directory_allowlist_, "rckda",
      kRslCapKeyDirectoryAllowlist, kServerScope,
      "SSRF allowlist: comma-separated https origins the RSL-CAP warm-fetch "
      "may "
      "contact. Empty => warm-fetch disabled (fail-closed). Default empty.",
      false);
  add_ngx_option(
      "", &NgxRewriteOptions::rsl_cap_key_directory_refresh_sec_, "rckdr",
      kRslCapKeyDirectoryRefreshSec, kServerScope,
      "RSL-CAP warm-fetch refresh interval in seconds (clamped). Empty => "
      "default 3600. Default empty.",
      false);

  MergeSubclassProperties(ngx_properties_);

  // Default properties are global but to set them the current API requires
  // a RewriteOptions instance and we're in a static method.
  NgxRewriteOptions dummy_config(nullptr);
  dummy_config.set_default_x_header_value(kModPagespeedVersion);
  // Zero-copy serving is opt-in on every port, nginx included: the r18
  // default-on never actually engaged(the defect -- the port-neutral
  // CycloneZeroCopy default meant no value was ever mapped), so making it
  // genuinely-on now would jump from zero to full fleet engagement in one
  // release, alongside the r19 cache-format migration.  Enable with
  // "pagespeed CycloneZeroCopy on;" (the serve sink is default-on and
  // engages once values are mapped).  Revisit default-on only with the
  // protocol-matrix floor asserts in place and a revision of
  // opt-in field soak behind it.
}

void NgxRewriteOptions::Initialize() {
  if (Properties::Initialize(&ngx_properties_)) {
    SystemRewriteOptions::Initialize();
    AddProperties();
  }
}

void NgxRewriteOptions::Terminate() {
  if (Properties::Terminate(&ngx_properties_)) {
    SystemRewriteOptions::Terminate();
  }
}

bool NgxRewriteOptions::IsDirective(StringPiece config_directive,
                                    StringPiece compare_directive) {
  return StringCaseEqual(config_directive, compare_directive);
}

RewriteOptions::OptionScope NgxRewriteOptions::GetOptionScope(
    StringPiece option_name) {
  ngx_uint_t i;
  ngx_uint_t size = sizeof(main_only_options) / sizeof(char*);
  for (i = 0; i < size; i++) {
    if (StringCaseEqual(main_only_options[i], option_name)) {
      return kProcessScopeStrict;
    }
  }

  size = sizeof(server_only_options) / sizeof(char*);
  for (i = 0; i < size; i++) {
    if (StringCaseEqual(server_only_options[i], option_name)) {
      return kServerScope;
    }
  }

  // This could be made more efficient if RewriteOptions provided a map allowing
  // access of options by their name. It's not too much of a worry at present
  // since this is just during initialization.
  for (OptionBaseVector::const_iterator it = all_options().begin();
       it != all_options().end(); ++it) {
    RewriteOptions::OptionBase* option = *it;
    if (StringCaseEqual(option->option_name(), option_name)) {
      // We treat kLegacyProcessScope as kProcessScopeStrict, failing to start
      // if an option is out of place.
      return option->scope() == kLegacyProcessScope ? kProcessScopeStrict
                                                    : option->scope();
    }
  }
  return kDirectoryScope;
}

RewriteOptions::OptionSettingResult NgxRewriteOptions::ParseAndSetOptions0(
    StringPiece directive, GoogleString* msg, MessageHandler* handler) {
  EnabledEnum enabled;
  if (!ParseFromString(directive, &enabled)) {
    return RewriteOptions::kOptionNameUnknown;
  }
  if (enabled == RewriteOptions::kEnabledOff) {
    // In ngx_pagespeed, for historical reasons, we treat "off" as "unplugged".
    // Also, "off" is deprecated and people should be using "standby" or
    // "unplugged" now depending on which sense they want.  See comment on
    // RewriteOptions::EnabledEnum.
    enabled = RewriteOptions::kEnabledUnplugged;
  }
  set_enabled(enabled);
  return RewriteOptions::kOptionOk;
}

RewriteOptions::OptionSettingResult
NgxRewriteOptions::ParseAndSetOptionFromName1(StringPiece name, StringPiece arg,
                                              GoogleString* msg,
                                              MessageHandler* handler) {
  // FileCachePath needs error checking.
  if (StringCaseEqual(name, kFileCachePath)) {
    if (!StringCaseStartsWith(arg, "/")) {
      *msg = "must start with a slash";
      return RewriteOptions::kOptionValueInvalid;
    }
  }

  return SystemRewriteOptions::ParseAndSetOptionFromName1(name, arg, msg,
                                                          handler);
}

template <class DriverFactoryT>
RewriteOptions::OptionSettingResult ParseAndSetOptionHelper(
    StringPiece option_value, DriverFactoryT* driver_factory,
    void (DriverFactoryT::*set_option_method)(bool)) {
  bool parsed_value;
  if (StringCaseEqual(option_value, "on") ||
      StringCaseEqual(option_value, "true")) {
    parsed_value = true;
  } else if (StringCaseEqual(option_value, "off") ||
             StringCaseEqual(option_value, "false")) {
    parsed_value = false;
  } else {
    return RewriteOptions::kOptionValueInvalid;
  }

  (driver_factory->*set_option_method)(parsed_value);
  return RewriteOptions::kOptionOk;
}

namespace {

const char* ps_error_string_for_option(ngx_pool_t* pool, StringPiece directive,
                                       StringPiece warning) {
  GoogleString msg = StrCat("\"", directive, "\" ", warning);
  char* s = string_piece_to_pool_string(pool, msg);
  if (s == nullptr) {
    return "failed to allocate memory";
  }
  return s;
}

}  // namespace

// Very similar to apache/mod_instaweb::ParseDirective.
const char* NgxRewriteOptions::ParseAndSetOptions(
    StringPiece* args, int n_args, ngx_pool_t* pool, MessageHandler* handler,
    NgxRewriteDriverFactory* driver_factory, RewriteOptions::OptionScope scope,
    ngx_conf_t* cf, ProcessScriptVariablesMode script_mode) {
  CHECK_GE(n_args, 1);

  StringPiece directive = args[0];

  // Remove initial "ModPagespeed" if there is one.
  StringPiece mod_pagespeed("ModPagespeed");
  if (StringCaseStartsWith(directive, mod_pagespeed)) {
    directive.remove_prefix(mod_pagespeed.size());
  }

  if (GetOptionScope(directive) > scope) {
    return ps_error_string_for_option(pool, directive,
                                      "cannot be set at this scope.");
  }

  bool compile_scripts = false;

  if (script_mode != ProcessScriptVariablesMode::kOff) {
    // In the old mode we only allowed a few, so restrict to those.
    compile_scripts =
        StringCaseStartsWith(directive, "LoadFromFile") ||
        StringCaseEqual(directive, "EnableFilters") ||
        StringCaseEqual(directive, "DisableFilters") ||
        StringCaseEqual(directive, "DownstreamCachePurgeLocationPrefix") ||
        StringCaseEqual(directive, "DownstreamCachePurgeMethod") ||
        StringCaseEqual(directive,
                        "DownstreamCacheRewrittenPercentageThreshold") ||
        StringCaseEqual(directive, "ShardDomain");
    // In the new behaviour we also allow scripting of query- and directory-
    // scoped options.
    compile_scripts |=
        script_mode == ProcessScriptVariablesMode::kAll &&
        (GetOptionScope(directive) <= RewriteOptions::kDirectoryScope ||
         (StringCaseEqual(directive, "Allow") ||
          StringCaseEqual(directive, "BlockingRewriteRefererUrls") ||
          StringCaseEqual(directive, "Disallow") ||
          StringCaseEqual(directive, "DistributableFilters") ||
          StringCaseEqual(directive, "Domain") ||
          StringCaseEqual(directive, "ExperimentVariable") ||
          StringCaseEqual(directive, "ExperimentSpec") ||
          StringCaseEqual(directive, "ForbidFilters") ||
          StringCaseEqual(directive, "RetainComment") ||
          StringCaseEqual(directive, "CustomFetchHeader") ||
          StringCaseEqual(directive, "MapOriginDomain") ||
          StringCaseEqual(directive, "MapProxyDomain") ||
          StringCaseEqual(directive, "MapRewriteDomain") ||
          StringCaseEqual(directive, "UrlValuedAttribute") ||
          StringCaseEqual(directive, "Library")));
  }

  ScriptLine* script_line;
  script_line = nullptr;

  if (n_args == 1 && StringCaseEqual(directive, "ClearInheritedScripts")) {
    clear_inherited_scripts_ = true;
    return NGX_CONF_OK;
  }

  if (compile_scripts) {
    CHECK(cf != nullptr);
    int i;
    // Skip the first arg which is always 'pagespeed'
    for (i = 1; i < n_args; i++) {
      // Pool-allocate the source ngx_str_t (and its data) so it outlives this
      // loop iteration: sc is pool-allocated and stashed in script_line_ for
      // deferred execution, and sc->source must not dangle into the loop-local
      // std::string.
      std::string tmp = args[i].as_string();
      ngx_str_t* script_source =
          reinterpret_cast<ngx_str_t*>(ngx_palloc(cf->pool, sizeof(ngx_str_t)));
      script_source->len = tmp.length();
      script_source->data =
          reinterpret_cast<u_char*>(string_piece_to_pool_string(cf->pool, tmp));

      if (ngx_http_script_variables_count(script_source) > 0) {
        ngx_http_script_compile_t* sc =
            reinterpret_cast<ngx_http_script_compile_t*>(
                ngx_pcalloc(cf->pool, sizeof(ngx_http_script_compile_t)));
        sc->cf = cf;
        sc->source = script_source;
        sc->lengths = reinterpret_cast<ngx_array_t**>(
            ngx_pcalloc(cf->pool, sizeof(ngx_array_t*)));
        sc->values = reinterpret_cast<ngx_array_t**>(
            ngx_pcalloc(cf->pool, sizeof(ngx_array_t*)));
        sc->variables = 1;
        sc->complete_lengths = 1;
        sc->complete_values = 1;
        if (ngx_http_script_compile(sc) != NGX_OK) {
          return ps_error_string_for_option(
              pool, directive, "Failed to compile script variables");
        } else {
          if (script_line == nullptr) {
            script_line = new ScriptLine(args, n_args, scope);
          }
          script_line->AddScriptAndArgIndex(sc, i);
        }
      }
    }

    if (script_line != nullptr) {
      script_lines_.push_back(RefCountedPtr<ScriptLine>(script_line));
      // We have found script variables in the current configuration line, and
      // prepared the associated rewriteoptions for that.
      // We will defer parsing, validation and processing of this line to
      // request time. That means we are done handling this configuration line.
      return NGX_CONF_OK;
    }
  }

  GoogleString msg;
  OptionSettingResult result;
  if (n_args == 1) {
    result = ParseAndSetOptions0(directive, &msg, handler);
  } else if (n_args == 2) {
    StringPiece arg = args[1];
    if (IsDirective(directive, "UseNativeFetcher")) {
      result = ParseAndSetOptionHelper<NgxRewriteDriverFactory>(
          arg, driver_factory,
          &NgxRewriteDriverFactory::set_use_native_fetcher);
    } else if (IsDirective(directive, "NativeFetcherMaxKeepaliveRequests")) {
      int max_keepalive_requests;
      if (StringToInt(arg, &max_keepalive_requests) &&
          max_keepalive_requests > 0) {
        driver_factory->set_native_fetcher_max_keepalive_requests(
            max_keepalive_requests);
        result = RewriteOptions::kOptionOk;
      } else {
        result = RewriteOptions::kOptionValueInvalid;
      }
    } else if (StringCaseEqual("ProcessScriptVariables", directive)) {
      if (scope == RewriteOptions::kProcessScopeStrict) {
        ProcessScriptVariablesMode mode;
        if (StringCaseEqual(arg, "all")) {
          mode = ProcessScriptVariablesMode::kAll;
        } else if (StringCaseEqual(arg, "on")) {
          mode = ProcessScriptVariablesMode::kLegacyRestricted;
        } else if (StringCaseEqual(arg, "off")) {
          mode = ProcessScriptVariablesMode::kOff;
        } else {
          return const_cast<char*>(
              "pagespeed ProcessScriptVariables: invalid value");
        }
        if (driver_factory->SetProcessScriptVariables(mode)) {
          result = RewriteOptions::kOptionOk;
        } else {
          return const_cast<char*>(
              "pagespeed ProcessScriptVariables: can only be set once");
        }
      } else {
        return const_cast<char*>(
            "ProcessScriptVariables is only allowed at the top level");
      }
    } else {
      result = ParseAndSetOptionFromName1(directive, arg, &msg, handler);
      if (result == RewriteOptions::kOptionNameUnknown) {
        result = driver_factory->ParseAndSetOption1(
            directive, arg, scope >= RewriteOptions::kLegacyProcessScope, &msg,
            handler);
      }
    }
  } else if (n_args == 3) {
    result =
        ParseAndSetOptionFromName2(directive, args[1], args[2], &msg, handler);
    if (result == RewriteOptions::kOptionNameUnknown) {
      result = driver_factory->ParseAndSetOption2(
          directive, args[1], args[2],
          scope >= RewriteOptions::kLegacyProcessScope, &msg, handler);
    }
  } else if (n_args == 4) {
    result = ParseAndSetOptionFromName3(directive, args[1], args[2], args[3],
                                        &msg, handler);
  } else {
    result = RewriteOptions::kOptionNameUnknown;
  }

  switch (result) {
    case RewriteOptions::kOptionOk:
      return NGX_CONF_OK;
    case RewriteOptions::kOptionNameUnknown:
      return ps_error_string_for_option(pool, directive,
                                        "not recognized or too many arguments");
    case RewriteOptions::kOptionValueInvalid: {
      GoogleString full_directive;
      for (int i = 0; i < n_args; i++) {
        StrAppend(&full_directive, i == 0 ? "" : " ", args[i]);
      }
      return ps_error_string_for_option(pool, full_directive, msg);
    }
  }

  CHECK(false);
  return nullptr;
}

// Execute all entries in the script_lines vector, and hand the result off to
// ParseAndSetOptions to obtain the final option values.
bool NgxRewriteOptions::ExecuteScriptVariables(
    ngx_http_request_t* r, MessageHandler* handler,
    NgxRewriteDriverFactory* driver_factory) {
  bool script_error = false;

  if (script_lines_.size() > 0) {
    std::vector<RefCountedPtr<ScriptLine> >::iterator it;
    for (it = script_lines_.begin(); it != script_lines_.end(); ++it) {
      ScriptLine* script_line = it->get();
      StringPiece args[NGX_PAGESPEED_MAX_ARGS];
      std::vector<ScriptArgIndex*>::iterator cs_it;
      int i;

      for (i = 0; i < script_line->n_args(); i++) {
        args[i] = script_line->args()[i];
      }

      for (cs_it = script_line->data().begin();
           cs_it != script_line->data().end(); cs_it++) {
        ngx_http_script_compile_t* script;
        ngx_array_t* values;
        ngx_array_t* lengths;
        ngx_str_t value;

        script = (*cs_it)->script();
        lengths = *script->lengths;
        values = *script->values;

        if (ngx_http_script_run(r, &value, lengths->elts, 0, values->elts) ==
            nullptr) {
          handler->Message(kError, "ngx_http_script_run error");
          script_error = true;
          break;
        } else {
          args[(*cs_it)->index()] = str_to_string_piece(value);
        }
      }

      const char* status =
          ParseAndSetOptions(args, script_line->n_args(), r->pool, handler,
                             driver_factory, script_line->scope(),
                             nullptr /*cf*/, ProcessScriptVariablesMode::kOff);

      if (status != nullptr) {
        script_error = true;
        handler->Message(
            kWarning, "Error setting option value from script: '%s'", status);
        break;
      }
    }
  }

  if (script_error) {
    handler->Message(
        kWarning, "Script error(s) in configuration, disabling optimization");
    set_enabled(RewriteOptions::kEnabledOff);
    return false;
  }

  return true;
}

void NgxRewriteOptions::CopyScriptLinesTo(
    NgxRewriteOptions* destination) const {
  destination->script_lines_ = script_lines_;
}

void NgxRewriteOptions::AppendScriptLinesTo(
    NgxRewriteOptions* destination) const {
  destination->script_lines_.insert(destination->script_lines_.end(),
                                    script_lines_.begin(), script_lines_.end());
}

NgxRewriteOptions* NgxRewriteOptions::Clone() const {
  NgxRewriteOptions* options = new NgxRewriteOptions(
      StrCat("cloned from ", description()), thread_system());
  this->CopyScriptLinesTo(options);
  options->Merge(*this);
  return options;
}

const NgxRewriteOptions* NgxRewriteOptions::DynamicCast(
    const RewriteOptions* instance) {
  return dynamic_cast<const NgxRewriteOptions*>(instance);
}

NgxRewriteOptions* NgxRewriteOptions::DynamicCast(RewriteOptions* instance) {
  return dynamic_cast<NgxRewriteOptions*>(instance);
}

}  // namespace net_instaweb
