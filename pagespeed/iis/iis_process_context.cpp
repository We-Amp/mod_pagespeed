#include "pagespeed/iis/iis_process_context.h"

#include "pagespeed/automatic/proxy_fetch.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/iis/iis_rewrite_driver_factory.h"
#include "pagespeed/iis/iis_message_handler.h"
#include "pagespeed/iis/iis_global_constants.h"
#include "pagespeed/iis/iis_rewrite_options.h"
//#include "win_thread/win_thread_system.h"
#include "pagespeed/kernel/base/stdio_file_system.h"

#include "pagespeed/system/system_caches.h"
#include "pagespeed/system/system_thread_system.h"

#include "pagespeed/iis/iis_misc.h"

#include "pagespeed/iis/log_message_handler.h"
#include "pagespeed/iis/iis_configuration.h"
#include "pagespeed/iis/iis_server_context.h"

#include "pagespeed/kernel/base/atomic_int32.h"
#include "pagespeed/kernel/sharedmem/inprocess_shared_mem.h"

namespace net_instaweb {                                              

extern std::string modulePath;


ProcessContext* IisProcessContext::PSOL_PROCESS_CONTEXT = NULL;

IisProcessContext::IisProcessContext(
	const GoogleString& site_app_id,
	HTTP_MODULE_ID module_id, 
	DWORD page_size, 
	IisRewriteOptions* root_options,
	std::wstring app_pool_name,
	global_settings settings
	)  
	: site_app_id_(site_app_id),
	module_id_(module_id)
	, message_handler_( new GoogleMessageHandler() ) // TODO(oschaaf): want IisMessageHandler here.
	, page_size_(page_size)
	, root_options_(root_options)
	, app_pool_name_(app_pool_name)
	, settings_(settings)
	, server_context_mutex_(NULL)
	, ok_(true)
	, server_context_(NULL)
	
{	
	stopped_.set_value(0);
	reference_count_=1;
	driver_factory_ = NULL;
}

IisServerContext* IisProcessContext::GetServerContext(const GoogleString& site_app_id, const char * hostname,
	unsigned int port, const char * config_path)
{
	if (!ok_) { 
		CHECK(false) << "Forbidden code path - the process context is not OK. Check it before requesting a server context.";
	}

	if (driver_factory_ == NULL) {
		auto ts = new SystemThreadSystem();
		server_context_mutex_ = ts->NewMutex();
		driver_factory_ = new IisRewriteDriverFactory(this, app_pool_name_, ts, new InProcessSharedMem(ts));
		driver_factory_->Init();
	} else {
		// Might be NULL from a previous attempt.
		return server_context_;
	}
	//DebugBreak();
	ScopedMutex lock(server_context_mutex_);

	if (server_context_ == NULL)
	{
		IisServerContext* server_context;
		std::list<std::string> paths;
		paths.push_back(config_path);
		std::map<std::string,std::string> input;

		std::string foopath("/");
		input["path"]=std::string(foopath.data(),foopath.length());
		input["fullpath"]=std::string(foopath.data(),foopath.length());
		input["hostname"]=std::string(hostname);
		input["config"]=std::string("base");		 
		
		ConfigFactory *cf = new ConfigFactory();
		IisRewriteOptions * options = new IisRewriteOptions(driver_factory_->thread_system());
		cf->GetConfig(paths, input, *options, driver_factory_->message_handler(), settings());
		delete cf;
		if (!logToEventLogSet) 
		logToEventLog=false; // the options parser will have set logToEventLogSet

		message_handler_->Message( kInfo, "create server context");

		server_context = driver_factory_->MakeIisServerContext(site_app_id, port);
		IisRewriteOptions* server_options = root_options_->Clone();
		
		// If we have not file cache path, we postfix it with site-id here to make it unique accross sites
		// to ensure we don't overwrite shmem snapshats as different sites/processes dump them.
		if (options->file_cache_path().empty() && !root_options_->file_cache_path().empty()) {
			GoogleString server_cache_path = StrCat(root_options_->file_cache_path(), "/", site_app_id);
			options->set_file_cache_path(server_cache_path);
		}

		server_options->Merge(*options);
		delete options;
		
		server_context->global_options()->Merge(*server_options);
		delete server_options;	

		// oschaaf: beware: do not remove the line below - global_options() needs to be called!
		server_context->global_options();
		std::vector<SystemServerContext*> server_contexts;
		server_contexts.push_back(server_context);
		GoogleString error_message;
		int error_index = -1;
		Statistics* global_statistics = NULL;
		driver_factory_->PostConfig(
			server_contexts, &error_message, &error_index, &global_statistics);
		if (error_index != -1) {
			driver_factory_->message_handler()->Message(
				kError, "IIS WebSpeed is not enabled. %s", error_message.c_str());
			return NULL;
		}
		if (global_statistics == NULL) {
			IisRewriteDriverFactory::InitStats(driver_factory_->statistics());
		}
		driver_factory_->SetServerContextMessageHandler(server_context);
		driver_factory_->RootInit();
		driver_factory_->ChildInit();

		// Let's verify the cache path for good measure.
		auto cache_path = server_context->global_system_rewrite_options()->file_cache_path();
		bool cache_path_ok = false;
		if (cache_path.size() > 0) {
			GoogleString tc = cache_path;
			if (tc[0]=='/' && tc[cache_path.size() - 1] != '/') {
				tc += "/";
			}
			else
				if (tc[cache_path.size() - 1] != '\\') {
					tc += "\\";
				}
			auto tempfile=driver_factory_->file_system()->OpenTempFile(tc, driver_factory_->message_handler());
			if (tempfile) {
				cache_path_ok = true;
				GoogleString fn = tempfile->filename();
				if (tempfile->Write("test", driver_factory_->message_handler())) {
					if (!tempfile->Flush(driver_factory_->message_handler())) 
						cache_path_ok = false;
				}
				else {
					cache_path_ok = false;
				}
				if (!driver_factory_->file_system()->Close(tempfile, driver_factory_->message_handler()))
					cache_path_ok = false;
				driver_factory_->file_system()->RemoveFile(fn.c_str(), driver_factory_->message_handler());				
			}
			/*auto exists = driver_factory_->file_system()->Exists(cache_path.c_str(), driver_factory_->message_handler());
			if (exists.is_true()) { 
				cache_path_ok = true;
			} else {*/
			if (!cache_path_ok) {
				driver_factory_->message_handler()->Message( kError, "Server context cache path not writeable: %s",cache_path.c_str() );
			}
		}
		//driver_factory_->message_handler()->Message(kInfo, "global default options:\r\n[%s]", driver_factory_->default_options()->OptionsToString().c_str());
		//driver_factory_->message_handler()->Message(kInfo,
		//	"Rewrite options for ServerContext for site-id [%s]:\n[%s]",
		//	Integer64ToString((int64)site_id).c_str(), server_context->global_options()->OptionsToString().c_str());

		if (!cache_path_ok) {
			driver_factory_->message_handler()->Message(kError, "Unable to start server context: %s", cache_path.c_str());
			ok_ = false;
			driver_factory_->ShutDown();
			return NULL;
		}
		fetch_factory_ = new ProxyFetchFactory(server_context);
		server_context->set_fetch_factory(fetch_factory_);
		driver_factory_->StartThreads();
		server_context_ = server_context;
	}
	return server_context_;
}

void IisProcessContext::Shutdown()
{
	// TODO: looks like barrierincrement has become unnessecary
	if (stopped_.BarrierIncrement(1) == 1) 
	{
		server_context_mutex_->Lock();
		message_handler_->Message( net_instaweb::kInfo, "delete driver factory");
	
		if (driver_factory_ != NULL) 
		{
			delete driver_factory_;
			driver_factory_ = NULL;
		}
		if ( fetch_factory_ != NULL ) {
			message_handler_->Message( net_instaweb::kInfo, "delete fetch factory");
			delete fetch_factory_;
			fetch_factory_ = NULL;
		}
		if (root_options_ != NULL)
		{
			delete root_options_;
			root_options_ = NULL;
		}


		message_handler_->Message(kInfo, "Iis Process Context terminated");
		delete message_handler_;
		message_handler_ = NULL;


		server_context_mutex_->Unlock();
		delete server_context_mutex_;
		server_context_mutex_= NULL;
	}
}

IisProcessContext::~IisProcessContext() 
{
}	

}                                                                     

