// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#define _WINSOCKAPI_
#include <Windows.h>
#include "Shlobj.h"

#include "pagespeed/iis/iis_module_factory.h"
#include "pagespeed/iis/iis_http_module.h"
#include "pagespeed/iis/iis_rewrite_options.h"
#include "pagespeed/iis/iis_message_handler.h"
#include "pagespeed/iis/iis_configuration.h"
#include "pagespeed/iis/iis_config_util.h"
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
	// The %ProgramData% base config (tiers 1/2, canonical-first:
	// PageSpeed\ then legacy IISWebSpeed\, each preferring
	// pagespeed.config over iiswebspeed.config) is now resolved by the
	// single shared helper in iis_config_util so the factory, the
	// request-time merge (iis_misc.cpp), and the engage gate
	// (iis_http_module.cpp) can never diverge — that split-brain was the
	// config-drift defect this shared helper exists to eliminate. This base drives the process-level
	// global/root options and FileID1 change-detection below; site_root
	// (the per-site override, the design record tier 3) drives FileID2, so editing
	// EITHER the base or the authoritative per-site file re-inits the
	// process context.
	bool pd_config_exists = false;
	std::string config_path =
		iis_config_util::ResolveProgramDataConfig(&pd_config_exists);

	if (message_handler_) {
		// Effective winner: the per-site
		// override when present, else the %ProgramData% base.
		const bool site_config_exists =
			!site_root.empty() &&
			GetFileAttributesA(site_root.c_str()) != INVALID_FILE_ATTRIBUTES;
		const std::string& effective_config =
			site_config_exists ? site_root : config_path;
		message_handler_->Message(kInfo,
			"IisModuleFactory: resolved effective config: %s",
			effective_config.c_str());

		// Drift diagnostic: more than one config file present
		// with differing content. NeverOverwrite/Permanent mean the
		// installer never deleted the loser, so an operator who edited the
		// non-winning file sees no effect — name which file wins and which
		// is overridden.
		if (pd_config_exists && site_config_exists &&
			config_path != site_root &&
			!iis_config_util::ConfigContentsEqual(config_path, site_root)) {
			message_handler_->Message(kWarning,
				"IisModuleFactory: multiple pagespeed.config files present with "
				"differing content; per-site '%s' overrides machine-global '%s'. "
				"Edit the per-site file to change behavior.",
				site_root.c_str(), config_path.c_str());
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
