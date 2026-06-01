#ifndef IIS_PROCESS_CONTEXT_H_                                        
#define IIS_PROCESS_CONTEXT_H_                                        


#include <atomic>
#include <string> 
#include <vector>

#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <Windows.h>

#include <httpserv.h>

#include "net/instaweb/rewriter/public/process_context.h"
#include "pagespeed/automatic/proxy_fetch.h"
#include "pagespeed/iis/iis_configuration.h"
#include "pagespeed/iis/FileCheck.h"

namespace net_instaweb 
{
	class IisRewriteDriverFactory;
	class IisRewriteOptions;
	class IisServerContext;
	struct global_settings;

	class IisProcessContext
	{
	public:
		IisProcessContext(
			const GoogleString& site_app_id,
			HTTP_MODULE_ID module_id, 
			DWORD page_size, 
			IisRewriteOptions* root_options, /*we take ownership*/
			std::wstring app_pool_name,
			global_settings settings);
		virtual ~IisProcessContext();

		HTTP_MODULE_ID module_id() { return module_id_; };
		IisRewriteDriverFactory* driver_factory() { return driver_factory_; }; 
		MessageHandler* handler() { return message_handler_; };
		IisRewriteOptions* root_options() { return root_options_; };
		DWORD page_size() { return page_size_; }
		//RequestContext *CreatePagespeedRequestContext() {return new RequestContext(thread_system_->NewMutex());}
		IisServerContext* GetServerContext(const GoogleString& site_app_id, const char * hostname, unsigned int port, const char * config_path);
		void Shutdown();
		global_settings *  settings() { return &settings_; } 
		void AddRef() { reference_count_.fetch_add(1); }
		void ReleaseRef() { if ( reference_count_.fetch_sub(1) == 1 )	{this->Shutdown();delete this;} }
		bool ok() { return ok_; };

		ULONGLONG lastchecktime;
		struct FileCheck FileID1;
		struct FileCheck FileID2;
		GoogleString site_app_id() { return site_app_id_; }

		static ProcessContext* PSOL_PROCESS_CONTEXT;

	private:
		const GoogleString site_app_id_;
		std::atomic<DWORD> reference_count_;
		HTTP_MODULE_ID module_id_;
		IisRewriteDriverFactory* driver_factory_; 
		MessageHandler * message_handler_;
		IisRewriteOptions* root_options_;
		ProxyFetchFactory* fetch_factory_;
		IisServerContext* server_context_;
		DWORD page_size_;
		// KS: needed for request context
		AbstractMutex *loggingMutex_;
		std::wstring app_pool_name_;
		AtomicInt32 stopped_;
		global_settings settings_;
		AbstractMutex* server_context_mutex_;
		bool ok_;
	};
}                                                                     

#endif // IIS_PROCESS_CONTEXT_H_    