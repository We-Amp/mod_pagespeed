#ifndef IIS_REWRITE_DRIVER_FACTORY_H_
#define IIS_REWRITE_DRIVER_FACTORY_H_

#include "pagespeed/kernel/base/scoped_ptr.h"
#include "pagespeed/kernel/base/md5_hasher.h"
#include "pagespeed/system/system_rewrite_driver_factory.h"
#include <vector>

namespace net_instaweb {

class AbstractSharedMem;
class AprMemCache;
class AsyncCache;
class CacheInterface;
class AbstractSharedMem;
class IisAsyncUrlFetcher;
class IisProcessContext;
class IisRewriteOptions;
class IisServerContext;
class IisMessageHandler;
class SharedCircularBuffer;
class SharedMemStatistics;
class SharedMemRefererStatistics;
class SlowWorker;
class Statistics;
class StaticAssetManager;

class IisRewriteDriverFactory : public SystemRewriteDriverFactory {
 public:

  IisRewriteDriverFactory(IisProcessContext* process_context, std::wstring app_pool_name, SystemThreadSystem* thread_system, AbstractSharedMem* shm_runtime);
  virtual ~IisRewriteDriverFactory();

  virtual Hasher* NewHasher();
  virtual UrlAsyncFetcher* AllocateFetcher(SystemRewriteOptions* config);
  virtual MessageHandler* DefaultHtmlParseMessageHandler();
  virtual MessageHandler* DefaultMessageHandler();
  virtual FileSystem* DefaultFileSystem();
  virtual Timer* DefaultTimer();
  void StartThreads();
  //virtual QueuedWorkerPool* CreateWorkerPool(WorkerPoolCategory name);
  virtual NamedLockManager* DefaultLockManager();
  virtual NonceGenerator* DefaultNonceGenerator();
  virtual RewriteOptions* NewRewriteOptions();

  ServerContext* NewServerContext();
  virtual void ShutDown();

  // We use a beacon handler to collect data for critical images,
  // css, etc., so filters should be configured accordingly.
  virtual bool UseBeaconResultsInFilters() const {
    return true;
  }

  virtual void NonStaticInitStats(Statistics* statistics) {
    InitStats(statistics);
  }  
  // Provides an optional hook for customizing the RewriteDriver object
  // using the options set on it. This is called before
  // RewriteDriver::AddFilters() and AddPlatformSpecificRewritePasses().
  virtual void ApplyPlatformSpecificConfiguration(RewriteDriver* driver) {;}
  // Provides an optional hook for adding rewrite passes to the HTML filter
  // chain.  This should be used for filters that are specific to a particular
  // RewriteDriverFactory implementation.
  virtual void AddPlatformSpecificRewritePasses(RewriteDriver* driver);
  static void InitStats(Statistics* statistics);
  IisServerContext* MakeIisServerContext(const GoogleString& site_app_id, unsigned int port);
  virtual void ShutDownMessageHandlers();
  virtual void SetCircularBuffer(SharedCircularBuffer* buffer);
  
  virtual ServerContext* NewDecodingServerContext();
	  

  void SetServerContextMessageHandler(ServerContext* server_context);
	bool use_per_vhost_statistics() const {
		return use_per_vhost_statistics_;
	}
	void set_use_per_vhost_statistics(bool x) {
		use_per_vhost_statistics_ = x;
	}
	bool use_native_fetcher() {
		return use_native_fetcher_;
	}
	void set_use_native_fetcher(bool x) {
		use_native_fetcher_ = x;
	} 

	int expires();
	void reset_expired();
protected:
  virtual QueuedWorkerPool* CreateWorkerPool(WorkerPoolCategory pool,
                                             StringPiece name);

  virtual void ShutDownFetchers();
private:
  std::wstring app_pool_name_;
  bool use_per_vhost_statistics_;
  bool use_native_fetcher_;
  GoogleString site_app_id_;

  // Owned by the superclass.
  // TODO(jefftk): merge the nginx and apache ways of doing this.
  SharedCircularBuffer* iis_shared_circular_buffer_;
  IisMessageHandler* iis_message_handler_;
  IisMessageHandler* iis_html_parse_message_handler_;
  typedef std::set<IisMessageHandler*> IisMessageHandlerSet;
  IisMessageHandlerSet server_context_message_handlers_;
  std::vector<IisAsyncUrlFetcher*> native_fetchers_;
  bool shut_down_;
  time_t created_at_;
  DISALLOW_COPY_AND_ASSIGN(IisRewriteDriverFactory);
};

} // namespace net_instaweb

#endif  // IIS_REWRITE_DRIVER_FACTORY_H_