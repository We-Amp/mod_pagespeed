
#include "pagespeed/iis/iis_psol_request_context.h"
#include "base/logging.h"
//#include "net/instaweb/http/public/meta_data.h"
#include "pagespeed/kernel/base/timer.h"

namespace net_instaweb {

class Timer;

IisPsolRequestContext::IisPsolRequestContext(AbstractMutex* logging_mutex, Timer* timer,
                                     StringPiece hostname,int local_port, GoogleString local_ip_address)
    : SystemRequestContext(logging_mutex, timer, hostname, local_port, local_ip_address),
      local_port_(local_port),
	  local_ip_address_(local_ip_address) 
	  ,check_third_part_cookie_()
{
}

IisPsolRequestContext::~IisPsolRequestContext() {
}

}  // namespace net_instaweb