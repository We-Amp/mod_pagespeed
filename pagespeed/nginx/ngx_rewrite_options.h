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

// Manage configuration for pagespeed.  Compare to ApacheConfig.

#ifndef NGX_REWRITE_OPTIONS_H_
#define NGX_REWRITE_OPTIONS_H_

extern "C" {
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
}

#include <vector>

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "ngx_rewrite_driver_factory.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/ref_counted_ptr.h"
#include "pagespeed/kernel/base/stl_util.h"  // for STLDeleteElements
#include "pagespeed/system/system_rewrite_options.h"

#define NGX_PAGESPEED_MAX_ARGS 10

namespace net_instaweb {

class NgxRewriteDriverFactory;

class ScriptArgIndex {
 public:
  explicit ScriptArgIndex(ngx_http_script_compile_t* script, int index)
      : script_(script), index_(index) {
    CHECK(script != NULL);
    CHECK(index > 0 && index < NGX_PAGESPEED_MAX_ARGS);
  }

  virtual ~ScriptArgIndex() {}

  ngx_http_script_compile_t* script() { return script_; }
  int index() { return index_; }

 private:
  // Not owned.
  ngx_http_script_compile_t* script_;
  int index_;
};

// Refcounted, because the ScriptArgIndexes inside data_ can be shared between
// different rewriteoptions.
class ScriptLine : public RefCounted<ScriptLine> {
 public:
  explicit ScriptLine(StringPiece* args, int n_args,
                      RewriteOptions::OptionScope scope)
      : n_args_(n_args), scope_(scope) {
    for (int i = 0; i < n_args; i++) {
      args_[i] = args[i];
    }
  }

  virtual ~ScriptLine() {
    STLDeleteElements(&data_);
    data_.clear();
  }

  void AddScriptAndArgIndex(ngx_http_script_compile_t* script,
                            int script_index) {
    CHECK(script != NULL);
    CHECK(script_index < NGX_PAGESPEED_MAX_ARGS);
    data_.push_back(new ScriptArgIndex(script, script_index));
  }

  int n_args() { return n_args_; }
  StringPiece* args() { return args_; }
  RewriteOptions::OptionScope scope() { return scope_; }
  std::vector<ScriptArgIndex*>& data() { return data_; }

 private:
  StringPiece args_[NGX_PAGESPEED_MAX_ARGS];
  int n_args_;
  RewriteOptions::OptionScope scope_;
  std::vector<ScriptArgIndex*> data_;

  ScriptLine(const ScriptLine&) = delete;
  ScriptLine& operator=(const ScriptLine&) = delete;
};

class NgxRewriteOptions : public SystemRewriteOptions {
 public:
  // See rewrite_options::Initialize and ::Terminate
  static void Initialize();
  static void Terminate();

  NgxRewriteOptions(const StringPiece& description,
                    ThreadSystem* thread_system);
  explicit NgxRewriteOptions(ThreadSystem* thread_system);
  virtual ~NgxRewriteOptions() {}

  // args is an array of n_args StringPieces together representing a directive.
  // For example:
  //   ["RewriteLevel", "PassThrough"]
  // or
  //   ["EnableFilters", "combine_css,extend_cache,rewrite_images"]
  // or
  //   ["ShardDomain", "example.com", "s1.example.com,s2.example.com"]
  // Apply the directive, returning NGX_CONF_OK on success or an error message
  // on failure.
  //
  // pool is a memory pool for allocating error strings.
  // cf is only required when compile_scripts is true
  // when compile_scripts is true, the rewrite_options will be prepared
  // for replacing any script $variables encountered in args. when false,
  // script variables will be substituted using the prepared rewrite options.
  const char* ParseAndSetOptions(StringPiece* args, int n_args,
                                 ngx_pool_t* pool, MessageHandler* handler,
                                 NgxRewriteDriverFactory* driver_factory,
                                 OptionScope scope, ngx_conf_t* cf,
                                 ProcessScriptVariablesMode script_mode);
  bool ExecuteScriptVariables(ngx_http_request_t* r, MessageHandler* handler,
                              NgxRewriteDriverFactory* driver_factory);
  void CopyScriptLinesTo(NgxRewriteOptions* destination) const;
  void AppendScriptLinesTo(NgxRewriteOptions* destination) const;

  // Make an identical copy of these options and return it.
  virtual NgxRewriteOptions* Clone() const;

  // Returns a suitably down cast version of 'instance' if it is an instance
  // of this class, NULL if not.
  static const NgxRewriteOptions* DynamicCast(const RewriteOptions* instance);
  static NgxRewriteOptions* DynamicCast(RewriteOptions* instance);

  const GoogleString& statistics_path() const {
    return statistics_path_.value();
  }
  const GoogleString& global_statistics_path() const {
    return global_statistics_path_.value();
  }
  const GoogleString& console_path() const { return console_path_.value(); }
  const GoogleString& messages_path() const { return messages_path_.value(); }

  // the design record A1 Web-Bot-Auth (observe-only, default off).
  bool web_bot_auth() const { return web_bot_auth_.value(); }
  bool web_bot_auth_telemetry() const {
    return web_bot_auth_telemetry_.value();
  }
  // the design record Bar-A opt-in counter mode (experimental): "off" (default/empty) |
  // "private" | "public". Gates the /.well-known/webbotauth-counter endpoint.
  const GoogleString& web_bot_auth_public_counter() const {
    return web_bot_auth_public_counter_.value();
  }
  const GoogleString& web_bot_auth_directory_host() const {
    return web_bot_auth_directory_host_.value();
  }
  const GoogleString& web_bot_auth_verified_bots() const {
    return web_bot_auth_verified_bots_.value();
  }
  // Path to a local JWKS file (the signer's published key directory, copied
  // locally by the operator). A1 v1 verifies against this file; automatic
  // network refresh is a follow-up.
  const GoogleString& web_bot_auth_key_directory_file() const {
    return web_bot_auth_key_directory_file_.value();
  }
  // the design record A2 network warm-fetch (all default empty => off; A1 v1 behavior is
  // preserved exactly when unset). The operator maps the signer's directory_host
  // to a JWKS `Url` that a background thread fetches off-request, SSRF-guarded by
  // the `Allowlist` of https origins; resolved keys are cached and read
  // cache-only on the request path. Warm-fetch is active only when the url AND
  // the allowlist AND web_bot_auth_directory_host are all non-empty.
  const GoogleString& web_bot_auth_key_directory_url() const {
    return web_bot_auth_key_directory_url_.value();
  }
  const GoogleString& web_bot_auth_key_directory_allowlist() const {
    return web_bot_auth_key_directory_allowlist_.value();
  }
  const GoogleString& web_bot_auth_key_directory_refresh_sec() const {
    return web_bot_auth_key_directory_refresh_sec_.value();
  }

  // the design record A3 RSL-CAP enforcement (PAID, default OFF / empty). The nginx layer
  // maps the validator verdict to an inline 401/402; it NEVER settles/meters
  // money. All inputs are operator config (plane-split), never request-derived.
  bool rsl_cap_enforcement() const { return rsl_cap_enforcement_.value(); }
  // Path to a local JWKS file (the issuer's published key directory, copied
  // locally by the operator). v1 resolves keys synchronously from this file,
  // mirroring A1; the SSRF-guarded network-fetch provider is a tracked follow-up
  // (its async fetch cannot complete inline in an nginx phase handler).
  const GoogleString& rsl_cap_key_directory_file() const {
    return rsl_cap_key_directory_file_.value();
  }
  // Operator-mapped issuer directory host passed to the key provider's GetKey
  // (plane-split; ignored by the static-file provider, used by the follow-up
  // network provider). Never request-derived.
  const GoogleString& rsl_cap_directory_host() const {
    return rsl_cap_directory_host_.value();
  }
  // The license id and scope this route requires; a token must grant both to be
  // authorized (else 402). Empty means "no specific grant required".
  const GoogleString& rsl_cap_requested_license() const {
    return rsl_cap_requested_license_.value();
  }
  const GoogleString& rsl_cap_requested_scope() const {
    return rsl_cap_requested_scope_.value();
  }
  // Optional issuer pin (hardening): when non-empty, an otherwise-authorized
  // token whose `iss` != this value is rejected (401). Use when a directory_host
  // may serve more than one issuer.
  const GoogleString& rsl_cap_issuer() const { return rsl_cap_issuer_.value(); }
  // the design record A2 network warm-fetch for the RSL-CAP issuer directory (mirrors the
  // web_bot_auth_* options above; default empty => off, v1 static-file behavior).
  const GoogleString& rsl_cap_key_directory_url() const {
    return rsl_cap_key_directory_url_.value();
  }
  const GoogleString& rsl_cap_key_directory_allowlist() const {
    return rsl_cap_key_directory_allowlist_.value();
  }
  const GoogleString& rsl_cap_key_directory_refresh_sec() const {
    return rsl_cap_key_directory_refresh_sec_.value();
  }
  const GoogleString& admin_path() const { return admin_path_.value(); }
  const GoogleString& global_admin_path() const {
    return global_admin_path_.value();
  }
  const std::vector<RefCountedPtr<ScriptLine> >& script_lines() const {
    return script_lines_;
  }
  const bool& clear_inherited_scripts() const {
    return clear_inherited_scripts_;
  }

 private:
  // Helper methods for ParseAndSetOptions().  Each can:
  //  - return kOptionNameUnknown and not set msg:
  //    - directive not handled; continue on with other possible
  //      interpretations.
  //  - return kOptionOk and not set msg:
  //    - directive handled, all's well.
  //  - return kOptionValueInvalid and set msg:
  //    - directive handled with an error; return the error to the user.
  //
  // msg will be shown to the user on kOptionValueInvalid.  While it would be
  // nice to always use msg and never use the MessageHandler, some option
  // parsing code in RewriteOptions expects to write to a MessageHandler.  If
  // that happens we put a summary on msg so the user sees something, and the
  // detailed message goes to their log via handler.
  OptionSettingResult ParseAndSetOptions0(StringPiece directive,
                                          GoogleString* msg,
                                          MessageHandler* handler);

  virtual OptionSettingResult ParseAndSetOptionFromName1(
      StringPiece name, StringPiece arg, GoogleString* msg,
      MessageHandler* handler);

  // We may want to override 2- and 3-argument versions as well in the future,
  // but they are not needed yet.

  // Keeps the properties added by this subclass.  These are merged into
  // RewriteOptions::all_properties_ during Initialize().
  //
  // RewriteOptions uses static initialization to reduce memory usage and
  // construction time.  All NgxRewriteOptions instances will have the same
  // Properties, so we can build the list when we initialize the first one.
  static Properties* ngx_properties_;
  static void AddProperties();
  void Init();

  // Add an option to ngx_properties_
  template <class OptionClass>
  static void add_ngx_option(typename OptionClass::ValueType default_value,
                             OptionClass NgxRewriteOptions::* offset,
                             const char* id, StringPiece option_name,
                             OptionScope scope, const char* help,
                             bool safe_to_print) {
    AddProperty(default_value, offset, id, option_name, scope, help,
                safe_to_print, ngx_properties_);
  }

  Option<GoogleString> statistics_path_;
  Option<GoogleString> global_statistics_path_;
  Option<GoogleString> console_path_;
  Option<GoogleString> messages_path_;
  Option<GoogleString> admin_path_;
  Option<GoogleString> global_admin_path_;

  // the design record A1 Web-Bot-Auth options (default off / empty).
  Option<bool> web_bot_auth_;
  Option<bool> web_bot_auth_telemetry_;
  Option<GoogleString> web_bot_auth_public_counter_;
  Option<GoogleString> web_bot_auth_directory_host_;
  Option<GoogleString> web_bot_auth_verified_bots_;
  Option<GoogleString> web_bot_auth_key_directory_file_;
  // the design record A2 network warm-fetch options (default empty / off).
  Option<GoogleString> web_bot_auth_key_directory_url_;
  Option<GoogleString> web_bot_auth_key_directory_allowlist_;
  Option<GoogleString> web_bot_auth_key_directory_refresh_sec_;

  // the design record A3 RSL-CAP enforcement options (PAID, default off / empty).
  Option<bool> rsl_cap_enforcement_;
  Option<GoogleString> rsl_cap_key_directory_file_;
  Option<GoogleString> rsl_cap_directory_host_;
  Option<GoogleString> rsl_cap_requested_license_;
  Option<GoogleString> rsl_cap_requested_scope_;
  Option<GoogleString> rsl_cap_issuer_;
  // the design record A2 network warm-fetch options for the RSL-CAP issuer directory.
  Option<GoogleString> rsl_cap_key_directory_url_;
  Option<GoogleString> rsl_cap_key_directory_allowlist_;
  Option<GoogleString> rsl_cap_key_directory_refresh_sec_;

  bool clear_inherited_scripts_;
  std::vector<RefCountedPtr<ScriptLine> > script_lines_;

  // Helper for ParseAndSetOptions.  Returns whether the two directives equal,
  // ignoring case.
  bool IsDirective(StringPiece config_directive, StringPiece compare_directive);

  // Returns a given option's scope.
  RewriteOptions::OptionScope GetOptionScope(StringPiece option_name);

  // TODO(jefftk): support fetch proxy in server and location blocks.

  NgxRewriteOptions(const NgxRewriteOptions&) = delete;
  NgxRewriteOptions& operator=(const NgxRewriteOptions&) = delete;
};

}  // namespace net_instaweb

#endif  // NGX_REWRITE_OPTIONS_H_
