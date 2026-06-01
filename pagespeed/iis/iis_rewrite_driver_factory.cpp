#include "pagespeed/iis/iis_rewrite_driver_factory.h"
#include <cstdio>
#include <stdlib.h>


#include "pagespeed/kernel/http/content_type.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/std_timer.h"
#include "pagespeed/kernel/cache/lru_cache.h"
#include "pagespeed/kernel/sharedmem/inprocess_shared_mem.h"
#include "pagespeed/kernel/base/md5_hasher.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/cache/threadsafe_cache.h"
#include "pagespeed/kernel/thread/scheduler_thread.h"
#include "pagespeed/kernel/sharedmem/shared_circular_buffer.h"
#include "pagespeed/kernel/thread/slow_worker.h"
#include "pagespeed/kernel/sharedmem/shared_mem_statistics.h"
#include "pagespeed/kernel/sharedmem/shared_mem_lock_manager.h"
#include "pagespeed/system/system_caches.h"
#include "pagespeed/automatic/proxy_interface.h"

#include "pagespeed/iis/util.h"
#include "pagespeed/iis/iis_async_url_fetcher.h"
#include "pagespeed/iis/iis_message_handler.h"
#include "pagespeed/iis/iis_rewrite_options.h"
#include "pagespeed/iis/iis_server_context.h"
#include "pagespeed/iis/iis_global_constants.h"
#include "pagespeed/iis/iis_process_context.h"

#include "pagespeed/iis/log_message_handler.h"
#include "pagespeed/iis/iis_request_context.h"
#include "pagespeed/kernel/util/hashed_nonce_generator.h"

#include <bcrypt.h>

namespace net_instaweb {
	  // namespace

	class CacheInterface;	
	class FileSystem;
	class Hasher;
	class MessageHandler;
	class Statistics;
	class Timer;
	class UrlAsyncFetcher;
	class UrlFetcher;
	class Writer;

	IisRewriteDriverFactory::IisRewriteDriverFactory(
		IisProcessContext* process_context,
		std::wstring app_pool_name, 
		SystemThreadSystem* thread_system,
		AbstractSharedMem* shm_runtime)
		: SystemRewriteDriverFactory(*IisProcessContext::PSOL_PROCESS_CONTEXT,
		thread_system, 
		//WEAMPKS: 1.9 NULL is default shared memory
		shm_runtime,
		"foo.com", 
		80),
	 app_pool_name_(app_pool_name),
	 use_per_vhost_statistics_(true),
	 use_native_fetcher_(process_context->settings()->use_native_fetcher),
	 //iis_url_async_fetcher_(NULL),
	 iis_shared_circular_buffer_(NULL),
	 iis_message_handler_(new IisMessageHandler(timer(), this->thread_system()->NewMutex())),
	 iis_html_parse_message_handler_(new IisMessageHandler(timer(), this->thread_system()->NewMutex())),
	 site_app_id_(process_context->site_app_id()), shut_down_(false)
	{
		InitializeDefaultOptions();
		// TOD(oschaaf): 1.9 -> def. beacon url? maintain  it?>
		//default_options()->set_beacon_url("/");
		SystemRewriteOptions* system_options = (SystemRewriteOptions*)(
			default_options());
		system_options->set_avoid_renaming_introspective_javascript(true);
		// We tune down the 50 MB default shared mem cache, because on IIS we use one per process.
		system_options->set_default_shared_memory_cache_kb(1024*5);
		system_options->set_lru_cache_byte_limit(16384);
		system_options->set_lru_cache_kb_per_process(1024*10);
		set_message_buffer_size(process_context->settings()->message_buffer_size);
		set_message_handler(iis_message_handler_);
		set_html_parse_message_handler(iis_html_parse_message_handler_);

		// We can't support convert_meta_tags as we don't see the response body before we send out the headers.
		system_options->ForbidFiltersByCommaSeparatedList("convert_meta_tags", NULL);
		// we need to seal the system_options to prevent a debug dcheck
		
		system_options->ComputeSignature();
		created_at_ = time(0);
		GoogleString err;
		// TODO(oschaaf): 1.9!
		// TODO(oschaaf): -> https moet configureerbaar zijn, opties:		

		//TODO(oschaaf): release. name should be prefixed with app-pool id
		// we need to know if we are the one that is creating the segment, instead of attaching
		// it, because the datastructure(s) that live in it need to be initialised
		//std::wstring mutex_name(app_pool_name_);
		//mutex_name.append(L"_iispeed_factory_startup");
		/*
		std::wstring mutex_name(L"_iispeed_factory_startup");

		SECURITY_ATTRIBUTES sa;
		SECURITY_DESCRIPTOR sd;
		InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
		SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
		sa.nLength = sizeof(sa);
		sa.lpSecurityDescriptor = &sd;
		HANDLE mutex = CreateMutex(&sa,true,mutex_name.c_str());
		DWORD last_err = GetLastError();
		bool is_root = last_err != ERROR_ALREADY_EXISTS;
		message_handler()->Message(kInfo, ">>>>>>>>>>> iispeed_factory_startup for pool [%s] - [%s]"
			, ws2s(app_pool_name_).c_str(),  is_root ? "true" : "false");
			*/
	}

	void IisRewriteDriverFactory::ShutDownFetchers() {
		message_handler()->Message(net_instaweb::kInfo, "IisRewriteDriverFactory::ShutDownFetchers()");
		for (int i = 0; i < native_fetchers_.size(); i++) {
			native_fetchers_[i]->ShutDown();
		}
		native_fetchers_.clear();
	}

	IisRewriteDriverFactory::~IisRewriteDriverFactory() {
		message_handler()->Message( net_instaweb::kInfo, "IisRewriteDriverFactory::~IisRewriteDriverFactory()");
		ShutDown();
		//Sleep(100);
		if (iis_shared_circular_buffer_ != NULL) {
			//delete iis_shared_circular_buffer_;
			iis_shared_circular_buffer_ = NULL;
		}
	}

	void IisRewriteDriverFactory::ShutDown() {
		if (!shut_down_) {
			shut_down_ = true;
			SystemRewriteDriverFactory::ShutDown();
		}
	}

	ServerContext* IisRewriteDriverFactory::NewDecodingServerContext() {
		ServerContext* sc = new IisServerContext(this, site_app_id_, 9999 /* port doesn't really matter*/);
		InitStubDecodingServerContext(sc);
		return sc;
	}

	Hasher* IisRewriteDriverFactory::NewHasher() {
		return new MD5Hasher;
	}

	NonceGenerator* IisRewriteDriverFactory::DefaultNonceGenerator() {
		// Use Windows BCrypt API instead of /dev/urandom (which doesn't exist).
		const int kKeySize = 64;
		char key[kKeySize];
		NTSTATUS status = BCryptGenRandom(
			nullptr,
			reinterpret_cast<PUCHAR>(key),
			kKeySize,
			BCRYPT_USE_SYSTEM_PREFERRED_RNG);
		if (status != 0) {
			uint64_t seed = static_cast<uint64_t>(timer()->NowUs());
			memcpy(key, &seed, sizeof(seed));
			for (int i = sizeof(seed); i < kKeySize; i++) {
				key[i] = static_cast<char>(seed ^ (seed >> (i % 8)));
			}
		}
		return new HashedNonceGenerator(
			hasher(),
			StringPiece(key, kKeySize),
			thread_system()->NewMutex());
	}

	UrlAsyncFetcher* IisRewriteDriverFactory::AllocateFetcher(
		SystemRewriteOptions* config) {
		if (use_native_fetcher_) {
			message_handler()->Message(kInfo, "Allocating native fetcher");
			IisAsyncUrlFetcher* f = new IisAsyncUrlFetcher();
			native_fetchers_.push_back(f);
			return f;
		}
		else {
			message_handler()->Message(kInfo, "Allocating default fetcher");
			// WinHTTP is the only fetcher on Windows (no curl)
			IisAsyncUrlFetcher* f = new IisAsyncUrlFetcher();
			native_fetchers_.push_back(f);
			return f;
		}
	}

	MessageHandler* IisRewriteDriverFactory::DefaultHtmlParseMessageHandler() {
		return iis_html_parse_message_handler_;
	}
	 
	MessageHandler* IisRewriteDriverFactory::DefaultMessageHandler() {
		return iis_message_handler_;
	}

	FileSystem* IisRewriteDriverFactory::DefaultFileSystem() {
		return new StdioFileSystem();
	}

	Timer* IisRewriteDriverFactory::DefaultTimer() {
		return new StdTimer;
	}


	

	void IisRewriteDriverFactory::AddPlatformSpecificRewritePasses(RewriteDriver* driver) 
	{ 
	}


	void IisRewriteDriverFactory::StartThreads() 
	{
	  SchedulerThread* thread = new SchedulerThread(thread_system(), scheduler());
	  bool ok = thread->Start();
	  CHECK(ok) << "Unable to start scheduler thread";
	  defer_cleanup(thread->MakeDeleter());
	}


	NamedLockManager* IisRewriteDriverFactory::DefaultLockManager() {
		CHECK(false);
		return NULL;
	}

	RewriteOptions* IisRewriteDriverFactory::NewRewriteOptions() {
		IisRewriteOptions* options = new IisRewriteOptions(thread_system());
		options->SetRewriteLevel(RewriteOptions::kCoreFilters);
		return options;
	}

	IisServerContext* IisRewriteDriverFactory::MakeIisServerContext(const GoogleString& site_app_id, unsigned int port) {
		IisServerContext* rm = new IisServerContext(this, site_app_id, port);
		this->uninitialized_server_contexts_.insert(rm);
		return rm;
	}

	// oschaaf: this gets called multiple times - however, if it doesn't get
	// called, we shouldn't set up the buffer at all. (message buffer size would be 0)
	void IisRewriteDriverFactory::SetCircularBuffer(
		SharedCircularBuffer* buffer) {

		iis_shared_circular_buffer_ = buffer;
		iis_message_handler_->set_buffer(buffer);
		iis_html_parse_message_handler_->set_buffer(buffer);
		/* old code for when we had real shared mem.
		if (iis_shared_circular_buffer_ == NULL && buffer != NULL) {
			iis_shared_circular_buffer_ = new SharedCircularBuffer(
											shared_mem_runtime(),
											1024 * 128,
											filename_prefix().as_string(),
											hostname_identifier());
			iis_shared_circular_buffer_->InitSegment(true, message_handler());
			iis_shared_circular_buffer_->InitSegment(false, message_handler());
			iis_message_handler_->set_buffer(iis_shared_circular_buffer_);
			iis_html_parse_message_handler_->set_buffer(iis_shared_circular_buffer_);
		}*/
	}


	// oschaaf: note that we completely ignore the underlying sharedcircular buffer
	// from the SystemRewriteDriverFactory, which gets initialized badly due to
	// RootInit/ChildInit methodology
	void IisRewriteDriverFactory::SetServerContextMessageHandler(
		ServerContext* server_context) {
	  IisMessageHandler* handler = new IisMessageHandler(timer(), this->thread_system()->NewMutex());
	  handler->set_buffer(iis_shared_circular_buffer_);
	  server_context_message_handlers_.insert(handler);
	  defer_cleanup(new Deleter<IisMessageHandler>(handler));
	  server_context->set_message_handler(handler);
	}

	ServerContext* IisRewriteDriverFactory::NewServerContext() {
	  LOG(DFATAL) << "MakeIisServerContext should be used instead";
	  return NULL;
	}
	void IisRewriteDriverFactory::ShutDownMessageHandlers() {
	  iis_message_handler_->set_buffer(NULL);
	  iis_html_parse_message_handler_->set_buffer(NULL);
	  for (IisMessageHandlerSet::iterator p =
			   server_context_message_handlers_.begin();
		   p != server_context_message_handlers_.end(); ++p) {
		(*p)->set_buffer(NULL);
	  }
	  server_context_message_handlers_.clear();
	}

	void IisRewriteDriverFactory::InitStats(Statistics* statistics) {
	  SystemRewriteDriverFactory::InitStats(statistics);
	  IisServerContext::InitStats(statistics);
	}
	QueuedWorkerPool* IisRewriteDriverFactory::CreateWorkerPool(
		WorkerPoolCategory pool, StringPiece name) {
		SYSTEM_INFO sysinfo;
		GetSystemInfo( &sysinfo );

		auto numCPU = sysinfo.dwNumberOfProcessors;
	  switch (pool) {
		case kHtmlWorkers:

			return new QueuedWorkerPool(__max(4,numCPU), name, thread_system());
		case kRewriteWorkers:
			return new QueuedWorkerPool(__max(4,numCPU>>1), name, thread_system());
		case kLowPriorityRewriteWorkers:
		  return new QueuedWorkerPool(__max(4,numCPU>>1),
									  name,
									  thread_system());
		default:
		  return RewriteDriverFactory::CreateWorkerPool(pool, name);
	  }
	}


}  // namespace net_instaweb