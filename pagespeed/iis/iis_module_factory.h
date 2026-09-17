// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef IIS_MODULE_FACTORY_H_
#define IIS_MODULE_FACTORY_H_

#include <httpserv.h>
#include <unordered_map>
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "pagespeed/iis/iis_process_context.h"
#include "pagespeed/iis/iis_message_handler.h"



namespace net_instaweb {

class IisModuleFactory : public IHttpModuleFactory
{
public:
	IisModuleFactory(std::string module_path,PCWSTR app_pool_name, IisMessageHandler* handler);
	HRESULT GetHttpModule(OUT CHttpModule ** ppModule, IN IModuleAllocator * pAllocator);
	void Terminate();
	IisProcessContext* GetProcessContext(const GoogleString& site_app_id,std::string site_root, const GoogleString& app_url_base);
	
	MessageHandler *message_handler() {return message_handler_;}
	static HTTP_MODULE_ID module_id()
	{
		return module_id_;
	}
	static void set_module_id(HTTP_MODULE_ID module_id)
	{
		module_id_ = module_id;
	}
private:
	std::unordered_map<GoogleString, IisProcessContext*> site_contexts_;
	
	MessageHandler* message_handler_;
	CRITICAL_SECTION mycs;
	static HTTP_MODULE_ID module_id_;
	std::wstring app_pool_name_;
	std::string module_path_;
};
}

#endif