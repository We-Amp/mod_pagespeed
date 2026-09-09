// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef IIS_REWRITE_OPTIONS_H_
#define IIS_REWRITE_OPTIONS_H_

#include <string>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/system/system_rewrite_options.h"


#include "pagespeed/iis/iis_configuration.h"

extern bool INFO_URLS_LOCAL_ONLY;

namespace net_instaweb {

class ThreadSystem;

class IisRewriteOptions : public SystemRewriteOptions {
 public:
  // See rewrite_options::Initialize and ::Terminate
  static void Initialize();
  static void Terminate();

  IisRewriteOptions(ThreadSystem* thread_system);
  virtual ~IisRewriteOptions() {
  }

  const char* ParseAndSetOptions(
      std::vector<std::string> args, MessageHandler* handler, global_settings& global_config);

  // Returns the tightest scope an option may be set at, mirroring the nginx
  // port's NgxRewriteOptions::GetOptionScope. Names are matched
  // case-insensitively (option setting is case-insensitive). Used to reject
  // process/server-scoped directives that appear inside per-request (matched)
  // config blocks.
  RewriteOptions::OptionScope GetOptionScope(StringPiece option_name);

  virtual IisRewriteOptions* Clone() const;
  const GoogleString& statistics_path() const {
	  return statistics_path_.value();
  }
  const GoogleString& global_statistics_path() const {
	  return global_statistics_path_.value();
  }
  const GoogleString& console_path() const {
	  return console_path_.value();
  }
  const GoogleString& messages_path() const {
	  return messages_path_.value();
  }
  const GoogleString& admin_path() const {
	  return admin_path_.value();
  }
  const GoogleString& global_admin_path() const {
	  return global_admin_path_.value();
  }
  // Default-on; opt-out via `AutoCreateCachePath off` in
  // pagespeed.config. Read by IisProcessContext::GetServerContext after
  // the per-site options merge and consumed by
  // IisRewriteDriverFactory::EnsureDirectoryWritable. When off, the
  // legacy GetFileAttributesA existence check (kCachePathMissing) is
  // the only behaviour — no mkdir, no ACL touch.
  bool auto_create_cache_path() const {
	  return auto_create_cache_path_.value();
  }
  // default-on; opt-out via
  // `AutoCreateLogDir off` in pagespeed.config. Mirrors
  // auto_create_cache_path() but gates the LogDir parallel auto-create
  // flow. When off, the LogDir is left untouched at startup (no mkdir,
  // no ACL touch, no diagnostic page on absence — legacy behaviour).
  bool auto_create_log_dir() const {
	  return auto_create_log_dir_.value();
  }

 private:
  OptionSettingResult ParseAndSetOptions0(
      StringPiece directive, GoogleString* msg, MessageHandler* handler);

  virtual OptionSettingResult ParseAndSetOptionFromName1(
      StringPiece name, StringPiece arg,
      GoogleString* msg, MessageHandler* handler);

  bool SetBoolFlag(bool* v, StringPiece arg);
  static Properties* iis_properties_;
  static void AddProperties();
  void Init();

  // Add an option to ngx_properties_
  template<class OptionClass>
  static void add_iis_option(typename OptionClass::ValueType default_value,
	  OptionClass IisRewriteOptions::*offset,
	  const char* id,
	  StringPiece option_name,
	  OptionScope scope,
	  const char* help,
	  bool safe_to_print) {
	  AddProperty(default_value, offset, id, option_name, scope, help,
		  safe_to_print, iis_properties_);
  }

  Option<GoogleString> statistics_path_;
  Option<GoogleString> global_statistics_path_;
  Option<GoogleString> console_path_;
  Option<GoogleString> messages_path_;
  Option<GoogleString> admin_path_;
  Option<GoogleString> global_admin_path_;
  Option<bool> auto_create_cache_path_;
  Option<bool> auto_create_log_dir_;

  bool IsDirective(StringPiece config_directive, StringPiece compare_directive);

  IisRewriteOptions(const IisRewriteOptions&) = delete;
  IisRewriteOptions& operator=(const IisRewriteOptions&) = delete;
};

} // namespace net_instaweb

#endif  // iis_REWRITE_OPTIONS_H_