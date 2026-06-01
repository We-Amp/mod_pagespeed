#include "pagespeed/iis/iis_rewrite_options.h"
#include "net/instaweb/public/version.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/timer.h"

#include "pagespeed/kernel/base/message_handler.h"
#include "net/instaweb/rewriter/public/file_load_policy.h"

#include "pagespeed/kernel/base/stdio_file_system.h"

#include "pagespeed/iis/iis_message_handler.h"
#include "pagespeed/iis/iis_global_constants.h"
#include "pagespeed/iis/iis_rewrite_driver_factory.h"

namespace {
// Local helper: joins a vector of strings with a separator character.
GoogleString JoinString(const std::vector<std::string>& args, char sep) {
  GoogleString result;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i > 0) result.push_back(sep);
    result.append(args[i]);
  }
  return result;
}
}  // namespace


bool INFO_URLS_LOCAL_ONLY = true;

namespace net_instaweb {

	
namespace {

const char kStatisticsPath[] = "StatisticsPath";
const char kGlobalStatisticsPath[] = "GlobalStatisticsPath";
const char kConsolePath[] = "ConsolePath";
const char kMessagesPath[] = "MessagesPath";
const char kAdminPath[] = "AdminPath";
const char kGlobalAdminPath[] = "GlobalAdminPath";
const char kAutoCreateCachePath[] = "AutoCreateCachePath";
const char kAutoCreateLogDir[] = "AutoCreateLogDir";

// TODO(oschaaf): 1.9 -> use these options server only stuff below.

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
  "NativeFetcherMaxKeepaliveRequests"
};

// Options that can only be used in the main (http) option scope.
const char* const main_only_options[] = {
  "UseNativeFetcher",
  "NativeFetcherMaxKeepaliveRequests"
};

}  // namespace


RewriteOptions::Properties* IisRewriteOptions::iis_properties_ = NULL;

IisRewriteOptions::IisRewriteOptions(ThreadSystem* thread_system) 
	: SystemRewriteOptions(thread_system) {
	
  Init();
}

void IisRewriteOptions::Init() {
  DCHECK(iis_properties_ != NULL)
      << "Call IisRewriteOptions::Initialize() before construction";
  InitializeOptions(iis_properties_);
}

void IisRewriteOptions::AddProperties() {
	// IISpeed-specific options.
	add_iis_option(
		"/pagespeed_statistics", &IisRewriteOptions::statistics_path_, "nsp", kStatisticsPath,
		kServerScope, "Set the statistics path. Ex: /iispeed_statistics",
		false);
	add_iis_option(
		"/pagespeed_global_statistics", &IisRewriteOptions::global_statistics_path_, "ngsp",
		kGlobalStatisticsPath, kProcessScopeStrict,
		"Set the global statistics path. Ex: /iispeed_pagespeed_global_statistics",
		false);
	add_iis_option(
		"/pagespeed_console", &IisRewriteOptions::console_path_, "ncp", kConsolePath, kServerScope,
		"Set the console path. Ex: /pagespeed_console", false);
	add_iis_option(
		"/pagespeed_message", &IisRewriteOptions::messages_path_, "nmp", kMessagesPath,
		kServerScope, "Set the messages path.  Ex: /iispeed_message",
		false);
	add_iis_option(
		"/pagespeed_admin", &IisRewriteOptions::admin_path_, "nap", kAdminPath,
		kServerScope, "Set the admin path.  Ex: /pagespeed_admin", false);
	add_iis_option(
		"/pagespeed_global_admin", &IisRewriteOptions::global_admin_path_, "ngap", kGlobalAdminPath,
		kProcessScopeStrict, "Set the global admin path.  Ex: /pagespeed_global_admin",
		false);
	// the design record §5: default-on auto-create of the per-site cache subdir
	// under either C:\ProgramData\We-Amp\PageSpeed\cache\ or
	// C:\ProgramData\We-Amp\IISWebSpeed\cache\ (hardcoded prefix scope
	// in the factory override). Operator opt-out via
	// `pagespeed AutoCreateCachePath off`.
	add_iis_option(
		true, &IisRewriteOptions::auto_create_cache_path_, "nacp",
		kAutoCreateCachePath, kProcessScopeStrict,
		"Auto-create the per-site cache subdirectory if missing (on|off, default on).",
		false);
	// the design record §Operational + the referenced issue: default-on auto-create of
	// the LogDir under either C:\ProgramData\We-Amp\PageSpeed\logs\ or
	// C:\ProgramData\We-Amp\IISWebSpeed\logs\ (hardcoded prefix scope
	// in the factory override). Operator opt-out via
	// `pagespeed AutoCreateLogDir off`. ACL grant on the conditional
	// suspenders leg is RX+W (no DELETE), narrower than the cache grant,
	// mirroring Product.wxs GrantLogAcl.
	add_iis_option(
		true, &IisRewriteOptions::auto_create_log_dir_, "nacl",
		kAutoCreateLogDir, kProcessScopeStrict,
		"Auto-create the LogDir if missing (on|off, default on).",
		false);

	MergeSubclassProperties(iis_properties_);
	IisRewriteOptions dummy_config(NULL);
  
  dummy_config.set_default_x_header_value(kModPagespeedVersion);
}

void IisRewriteOptions::Initialize() {
  if (Properties::Initialize(&iis_properties_)) {
    SystemRewriteOptions::Initialize();
    AddProperties();
  }
}

void IisRewriteOptions::Terminate() {
  if (Properties::Terminate(&iis_properties_)) {
    SystemRewriteOptions::Terminate();
  }
}

bool IisRewriteOptions::IsDirective(StringPiece config_directive,
                                    StringPiece compare_directive) {
  return StringCaseEqual(config_directive, compare_directive);
}

RewriteOptions::OptionSettingResult IisRewriteOptions::ParseAndSetOptions0(
    StringPiece directive, GoogleString* msg, MessageHandler* handler) {
  if (IsDirective(directive, "diagnose")) {
	  // oschaaf: note that this turns on diagnose globally - which might not always make sense
	  // when it was configured at the server level.
    REDUCE_LOG=false;
  } else if (IsDirective(directive, "on")) {
	  set_enabled(EnabledEnum::kEnabledOn);
  } else if (IsDirective(directive, "off")) {
	  set_enabled(EnabledEnum::kEnabledOff);
  } else if (IsDirective(directive, "unplugged")) {
    set_enabled(RewriteOptions::kEnabledUnplugged);
  } else {
    return RewriteOptions::kOptionNameUnknown;
  }
  return RewriteOptions::kOptionOk;
}


RewriteOptions::OptionSettingResult
    IisRewriteOptions::ParseAndSetOptionFromName1(
        StringPiece name, StringPiece arg,
        GoogleString* msg, MessageHandler* handler) {
  // FileCachePath needs error checking.
  if (StringCaseEqual(name, "UseEventLog"))
  {
	  
	  if (StringCaseEqual(arg,"on"))
	  {
		logToEventLogSet=true;
		logToEventLog=true;	
	  }
	  else
      if (StringCaseEqual(arg,"off"))
	  {
		  logToEventLogSet=true;
		  logToEventLog=false;
	  }
	  return RewriteOptions::kOptionOk;
  }
  else
  if (StringCaseEqual(name, kFileCachePath)) {
    // On Windows, paths start with a drive letter (e.g., "C:\..."), not "/".
    // Accept both forward-slash and drive-letter paths.
    if (!StringCaseStartsWith(arg, "/") &&
        !(arg.size() >= 2 && isalpha(arg[0]) && arg[1] == ':')) {
      *msg = "must start with a slash or drive letter";
      return RewriteOptions::kOptionValueInvalid;
    }
  }

  return SystemRewriteOptions::ParseAndSetOptionFromName1(
      name, arg, msg, handler);
}

bool IisRewriteOptions::SetBoolFlag(bool* v, StringPiece arg) {
  if (IsDirective(arg, "on")) {
	 *v=true;
	 return true;
  } else if (IsDirective(arg, "off")) {
	 *v=false;
	 return true;
  } 
  return false;
}

const char*
IisRewriteOptions::ParseAndSetOptions(
     std::vector<std::string> args, MessageHandler* handler, global_settings& global_config) {
  int n_args = args.size();
  CHECK_GE(n_args, 1);

  StringPiece directive = args[0];

  // Remove initial "ModPagespeed" if there is one.
  StringPiece mod_pagespeed("ModPagespeed");
  if (StringCaseStartsWith(directive, mod_pagespeed)) {
    directive.remove_prefix(mod_pagespeed.size());
  }

  GoogleString msg;
  OptionSettingResult result;
  if (n_args == 1) {
    result = ParseAndSetOptions0(directive, &msg, handler);
  } else if (n_args == 2) {
    StringPiece arg = args[1];
    if (IsDirective(directive, "UsePerVHostStatistics")) {
		if (!SetBoolFlag(&global_config.use_per_vhost_statistics,arg)) {
			msg = "Failed to set UsePerVHostStatistics value";
			result = RewriteOptions::kOptionValueInvalid;
		} else {
			result = RewriteOptions::kOptionOk;
		}
    } /* else if (IsDirective(directive, "InstallCrashHandler")) {
		// Not applicable
    } */ else if (IsDirective(directive, "MessageBufferSize")) {
      int message_buffer_size;
      bool ok = StringToInt(arg.as_string(), &message_buffer_size);
      if (ok && message_buffer_size >= 0) {
		  global_config.message_buffer_size = message_buffer_size;
        result = RewriteOptions::kOptionOk;
      } else {
		msg = "Failed to set MessageBufferSize value";
        result = RewriteOptions::kOptionValueInvalid;
      }
    } else if (IsDirective(directive, "UseNativeFetcher")) {
		if (!SetBoolFlag(&global_config.use_native_fetcher,arg)) {
			msg = "Failed to set UseNativeFetcher value";
			result = RewriteOptions::kOptionValueInvalid;
		} else {
			result = RewriteOptions::kOptionOk;
		}
    } else if (IsDirective(directive, "InfoUrlsLocalOnly")) {
		if (!SetBoolFlag(&INFO_URLS_LOCAL_ONLY, arg)) {
			msg = "Failed to set InfoUrlsLocalOnly value";
			result = RewriteOptions::kOptionValueInvalid;
		} else {
			result = RewriteOptions::kOptionOk;
		}	
	}/* else if (IsDirective(directive, "RateLimitBackgroundFetches")) {
		if (!SetBoolFlag(&global_config.rate_limit_background_fetches, arg)) {
			msg = "Failed to set RateLimitBackgroundFetches value";
			result = RewriteOptions::kOptionValueInvalid;
		} else {
			result = RewriteOptions::kOptionOk;
		}	
    }  else if (IsDirective(directive, "ForceCaching")) {
		if (!SetBoolFlag(&global_config.force_caching, arg)) {
			msg = "Failed to set ForceCaching value";
			result = RewriteOptions::kOptionValueInvalid;
		} else {
			result = RewriteOptions::kOptionOk;
		}	
    } else if (IsDirective(directive, "ListOutstandingUrlsOnError")) {
		if (!SetBoolFlag(&global_config.list_outstanding_urls_on_error, arg)) {
			msg = "Failed to set ListOutstandingUrlsOnError value";
			result = RewriteOptions::kOptionValueInvalid;
		} else {
			result = RewriteOptions::kOptionOk;
		}	
    } else if (IsDirective(directive, "TrackOriginalContentLength")) {
		if (!SetBoolFlag(&global_config.track_original_content_length, arg)) {
			msg = "Failed to set TrackOriginalContentLength value";
			result = RewriteOptions::kOptionValueInvalid;
		} else {
			result = RewriteOptions::kOptionOk;
		}	
    } */else {
      result = ParseAndSetOptionFromName1(directive, args[1], &msg, handler);
    }
  } else if (n_args == 3) {
    if (StringCaseEqual(directive, "CreateSharedMemoryMetadataCache")) {
      int64 kb = 0;
      if (!StringToInt64(args[2], &kb) || kb < 0) {
        result = RewriteOptions::kOptionValueInvalid;
        msg = "size_kb must be a positive 64-bit integer";
      } else {
		  global_config.shm_cache_size_kb = kb;
		  result = kOptionOk;
        //bool ok = driver_factory->caches()->CreateShmMetadataCache(
        //    args[1].as_string(), kb, &msg);
        //result = ok ? kOptionOk : kOptionValueInvalid; 
      }
    } else {
      result = ParseAndSetOptionFromName2(directive, args[1], args[2],
                                          &msg, handler);
    }
  } else if (n_args == 4) {
    result = ParseAndSetOptionFromName3(
        directive, args[1], args[2], args[3], &msg, handler);
  } else {
    return "unknown option";
  }

  if (msg.size()) {
		handler->Message(kWarning, "Error handling config line [%s]: [%s]", JoinString(args, ' ').c_str(), msg.c_str());
  }

  switch (result) {
    case RewriteOptions::kOptionOk:
      return NULL;
    case RewriteOptions::kOptionNameUnknown:
		handler->Message(kWarning, JoinString(args, ' ').c_str());
      return "unknown option";
    case RewriteOptions::kOptionValueInvalid: {
		handler->Message(kWarning, JoinString(args, ' ').c_str());
      return "Invalid value"; 
    }
  }

  CHECK(false);
  return NULL;
}

IisRewriteOptions* IisRewriteOptions::Clone() const {
  IisRewriteOptions* options = new IisRewriteOptions(this->thread_system());
  options->Merge(*this);
  return options;
}


}  // namespace net_instaweb