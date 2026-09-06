// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef IIS_SERVER_CONTEXT_H_
#define IIS_SERVER_CONTEXT_H_

#include "pagespeed/system/system_server_context.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

	class IisRewriteDriverFactory;
	class IisRewriteOptions;
	class ProxyFetchFactory;
	class SharedMemStatistics;

	class IisServerContext : public SystemServerContext {
	public:
		IisServerContext(IisRewriteDriverFactory* factory, const GoogleString& site_app_id, unsigned int port);
		virtual ~IisServerContext();
		virtual bool ProxiesHtml() const { return true; }

		const GoogleString& site_app_id() { return site_app_id_; }
		ProxyFetchFactory* fetch_factory() { return fetch_factory_; }
		void set_fetch_factory(ProxyFetchFactory* x) { fetch_factory_ = x; }
		IisRewriteOptions* config();

	private:

		GoogleString site_app_id_;
		ProxyFetchFactory* fetch_factory_;
		IisRewriteDriverFactory* iis_factory_;
 		IisServerContext(const IisServerContext&) = delete;
 		IisServerContext& operator=(const IisServerContext&) = delete;
	};

}  // namespace net_instaweb

#endif  // IIS_SERVER_CONTEXT_H_