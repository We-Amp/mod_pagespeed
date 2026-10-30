// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef IISCONFIGH
#define IISCONFIGH
#define CONFIGFILE_PRIMARY "pagespeed.config"
#define CONFIGFILE_FALLBACK "iiswebspeed.config"

#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <windows.h>
#include <sys/stat.h>
#include <iostream>
#include <set>
#include <map>
#include <list>
#include <string>
#include <array>
#include <net/instaweb/rewriter/public/rewrite_options.h>
#include "pagespeed/iis/iis_message_handler.h"

// Helper: find config file, preferring pagespeed.config over iiswebspeed.config
inline std::string FindConfigFile(const std::string& dir) {
    std::string primary = dir + CONFIGFILE_PRIMARY;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(primary.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { FindClose(h); return primary; }
    return dir + CONFIGFILE_FALLBACK;
}

class ConfigurationFile;

namespace net_instaweb {

struct global_settings { 
	global_settings() 
		: use_native_fetcher(false)
		, use_per_vhost_statistics(true)
		, message_buffer_size(1024*128)
		, shm_cache_size_kb(0)
		, num_rewrite_threads(0)
		, num_expensive_rewrite_threads(0)
		//, rate_limit_background_fetches(true)
		//, force_caching(false)
		//, list_outstanding_urls_on_error(false)
		//, track_original_content_length(false)
	{
	}
	bool use_native_fetcher;
	bool use_per_vhost_statistics;
	int message_buffer_size;
	//bool rate_limit_background_fetches;
	//bool force_caching;
	//bool list_outstanding_urls_on_error;
	//bool track_original_content_length;
	int shm_cache_size_kb;
	// NumRewriteThreads / NumExpensiveRewriteThreads; 0 means "use the built-in
	// size" (the directive's "auto" and its default).
	int num_rewrite_threads;
	int num_expensive_rewrite_threads;
};

}

class ConfigFactory
{
protected:
	std::map<std::string,ConfigurationFile *> configurationFiles;	
	SRWLOCK rwlock;

public:
	ConfigFactory();
	~ConfigFactory();
	bool GetConfig(std::list<std::string> paths,std::map<std::string,std::string> &input,net_instaweb::RewriteOptions &rwo,net_instaweb::MessageHandler *mh,
		net_instaweb::global_settings* global_config);
	ConfigurationFile *GetConfiguration(std::string path,net_instaweb::MessageHandler *mh,net_instaweb::global_settings& global_config);
	
};

// Sets every field of *out (MessageBufferSize, UseNativeFetcher,
// UsePerVHostStatistics, the shared-memory size and the pool sizes) from the
// machine-wide file alone. Reads the file afresh, never from a cache, so every
// site context of a process, and a context rebuilt after a file edit, gets the
// same values. |machine_path| may be empty, which leaves the defaults.
void ReadMachineWideSettings(const std::string& machine_path,
                             net_instaweb::MessageHandler* quiet,
                             net_instaweb::global_settings* out);

// Sets out->num_rewrite_threads and out->num_expensive_rewrite_threads from
// the machine-wide file and the site's own file (either path may be empty; the
// site's value wins). Reads the files afresh, never from a cache, and logs
// nothing to the real message handler (|quiet| gets any parse messages).
void ReadPoolThreadSettings(const std::string& machine_path,
                            const std::string& site_path,
                            net_instaweb::MessageHandler* quiet,
                            net_instaweb::global_settings* out);
#endif 