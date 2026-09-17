// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/iis/iis_psol_request_context.h"
#include "base/logging.h"
//#include "net/instaweb/http/public/meta_data.h"
#include "pagespeed/kernel/base/timer.h"

namespace net_instaweb {

class Timer;

IisPsolRequestContext::IisPsolRequestContext(AbstractMutex* logging_mutex, Timer* timer,
                                     StringPiece hostname,int local_port, GoogleString local_ip_address,
                                     GoogleString local_scheme)
    : SystemRequestContext(logging_mutex, timer, hostname, local_port, local_ip_address),
      local_port_(local_port),
	  local_ip_address_(local_ip_address)
	  ,check_third_part_cookie_()
{
	// Transport scheme of the incoming connection, consumed by
	// LoopbackRouteFetcher when munging unknown-origin URLs (the loopback
	// fetch must speak the connection's scheme, not the URL's).
	set_local_scheme(local_scheme);
}

IisPsolRequestContext::~IisPsolRequestContext() {
}

}  // namespace net_instaweb