#define _WINSOCKAPI_
#include <Windows.h>
#include "Shlobj.h"

#include "pagespeed/iis/iis_module_factory.h"
#include "pagespeed/iis/iis_http_module.h"
#include "pagespeed/iis/iis_rewrite_options.h"
#include "pagespeed/iis/iis_message_handler.h"
#include "pagespeed/iis/iis_configuration.h"
#include "pagespeed/iis/log_message_handler.h"




namespace net_instaweb {
	extern ConfigFactory *cf;

	IisModuleFactory::IisModuleFactory(std::string module_path,PCWSTR app_pool_name /*IisProcessContext* process_context*/, IisMessageHandler* handler)
	: app_pool_name_(app_pool_name)
	, message_handler_(handler)
	, module_path_(module_path)
	
{
	InitializeCriticalSection(&mycs);
}


void GetFileID(std::string file,struct FileCheck &fc)
{
	fc.CreateFileID(file);
}
bool FileIDChanged(std::string file,struct FileCheck &fc)
{
	return fc.FileIDChanged(file);
}

HTTP_MODULE_ID IisModuleFactory::module_id_=0;

IisProcessContext* IisModuleFactory::GetProcessContext(const GoogleString& site_app_id,std::string site_root, const GoogleString& app_url_base)
{
	//BFAL 
	//need readers/writers lock
	EnterCriticalSection(&mycs);
	//mutex_->Lock();
	auto current_time=GetTickCount64();
	auto process_context=site_contexts_[site_app_id.c_str()];
	IisProcessContext *old_context=NULL;


	// Config-path resolution.
	//
	// Canonical-vs-fallback order (directory, not filename):
	//   1. %ProgramData%\We-Amp\PageSpeed\        (canonical 1.1+)
	//   2. %ProgramData%\We-Amp\IISWebSpeed\      (legacy fallback)
	//
	// Within each directory we still try CONFIGFILE_PRIMARY
	// ("pagespeed.config") before CONFIGFILE_FALLBACK
	// ("iiswebspeed.config") — that two-name fallback is the
	// upgrade-from-IISpeed-1.0 contract and is independent of the
	// directory move below.
	//
	// Why canonical-first: fresh 1.1 installs file
	// pagespeed.config into PageSpeed\ alongside the cache + logs
	// subdirectories — one canonical product directory. The MSI no
	// longer creates IISWebSpeed\ at all (Product.wxs IISWEBSPEEDDIR
	// declaration was removed).
	//
	// Why the IISWebSpeed\ fallback stays: upgrade-from-IISpeed and
	// upgrade-from-1.1-pre-this-change customers retain their config
	// at IISWebSpeed\ via NeverOverwrite="yes" on Product.wxs:228.
	// Detecting existence with GetFileAttributesA on each candidate is
	// cheap (one syscall per startup) and avoids a deferred-CA
	// filesystem migration during the MSI transaction.
	//
	// TODO(oschaaf): deduplicate this code accross the code base
	// and speed it up / cache it (only needs to be determined at startup).
	CHAR szPath[MAX_PATH];
	std::string config_path;
	if (SUCCEEDED(SHGetFolderPathA(NULL,
		CSIDL_COMMON_APPDATA,
		NULL,
		0,
		szPath)))
	{
		const std::string pdata(szPath);
		// Candidate directories in canonical-first order. Trailing
		// backslash included so the CONFIGFILE_* append below joins
		// cleanly.
		static const char* const kDirs[] = {
			"\\We-Amp\\PageSpeed\\",      // canonical 1.1+
			"\\We-Amp\\IISWebSpeed\\",    // legacy upgrade fallback
		};
		bool found = false;
		for (const char* dir : kDirs) {
			std::string candidate = pdata + dir + CONFIGFILE_PRIMARY;
			if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
				config_path = candidate;
				found = true;
				break;
			}
			candidate = pdata + dir + CONFIGFILE_FALLBACK;
			if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
				config_path = candidate;
				found = true;
				break;
			}
		}
		if (!found) {
			// Nothing exists yet — return the canonical path so the
			// downstream parser surfaces a sensible "not found"
			// message pointing at where the config _should_ live.
			config_path = pdata + kDirs[0] + CONFIGFILE_PRIMARY;
		}
		if (message_handler_) {
			message_handler_->Message(kInfo,
				"IisModuleFactory: resolved config path: %s",
				config_path.c_str());
		}
	}

	if (process_context)
	{		
		if (process_context->lastchecktime<(current_time-100))
		{
			if (process_context->FileID1.FileIDChanged(config_path)
				||
				process_context->FileID2.FileIDChanged(site_root )		
			)
			{
				OutputDebugStringA("FileID changed!");
				site_contexts_[site_app_id.c_str()]=NULL;
				old_context=process_context;
				process_context=NULL;
			}
			else
			{
				process_context->lastchecktime=current_time;
			}
		}
	}
	if (!process_context)
	{
		SYSTEM_INFO sysInfo;
		HRESULT hr = S_OK;
		GetSystemInfo(&sysInfo);
		DWORD m_dwPageSize = sysInfo.dwPageSize;

		// TODO(oschaaf): FIXME
		IisRewriteOptions * options = new IisRewriteOptions(NULL);
		std::map<std::string,std::string> input;

		std::string foopath("/");
		std::list<std::string> paths;
		paths.push_back(config_path);

		input["path"]=std::string(foopath.data(),foopath.length());
		input["fullpath"]=std::string(foopath.data(),foopath.length());
		input["config"]=std::string("base");
		OutputDebugStringA("Parse root configuration");
		global_settings global_config;


		if (cf->GetConfig(paths, input, *options, message_handler_, &global_config))
		{
			process_context= new IisProcessContext(site_app_id,module_id_, m_dwPageSize, options, app_pool_name_, global_config);
			process_context->FileID1.CreateFileID(config_path);
			process_context->FileID2.CreateFileID(site_root );
			process_context->lastchecktime=current_time;
			OutputDebugStringA("Parse root configuration done");
			site_contexts_[site_app_id.c_str()]=process_context;
		} else {
		} 
		
	}
	// read
	if (old_context) old_context->ReleaseRef();
	if (process_context) process_context->AddRef();
	LeaveCriticalSection(&mycs);
	return process_context;
}

HRESULT IisModuleFactory::GetHttpModule(
	OUT CHttpModule		** ppModule, 
	IN IModuleAllocator * pAllocator
	)
{
	UNREFERENCED_PARAMETER( pAllocator );
	IisHttpModule* pModule = new IisHttpModule(this);

	if (!pModule)
	{
		return HRESULT_FROM_WIN32( ERROR_NOT_ENOUGH_MEMORY );
	}
	else
	{
		*ppModule = pModule;
		pModule = NULL;
		return S_OK;
	}            
}

void IisModuleFactory::Terminate()
{
	DeleteCriticalSection(&mycs);
	message_handler_->Message(kInfo, "IisModuleFactory::Terminate  -> delete process context");

	for (auto key=site_contexts_.begin();key!=site_contexts_.end();key++)
	{
		auto current=*key;
		current.second->ReleaseRef();
	}
	site_contexts_.clear();
	
	message_handler_->Message(kInfo, "IisModuleFactory::Terminate delete message handler");
	//delete message_handler_;
	//message_handler_ = NULL;
	delete this;
} 


}
