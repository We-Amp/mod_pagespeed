#ifndef IIS_PSOL_REQUEST_CONTEXT_H_
#define IIS_PSOL_REQUEST_CONTEXT_H_

#include "pagespeed/system/system_request_context.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
namespace net_instaweb {

class AbstractMutex;
class Timer;

class IisPsolRequestContext : public SystemRequestContext {
 public:
  IisPsolRequestContext(AbstractMutex* logging_mutex, Timer* timer,
                    StringPiece hostname,int local_port, GoogleString local_ip_address);

  int local_port() const { return local_port_; }
  const GoogleString& local_ip_address() const { return local_ip_address_; }
  std::string check_third_party_cookie() { return check_third_part_cookie_; }
  void set_check_third_party_cookie(std::string value) { check_third_part_cookie_ = value; }


 protected:
  virtual ~IisPsolRequestContext();

 private:
  int local_port_;
  GoogleString local_ip_address_;
  std::string check_third_part_cookie_;

  IisPsolRequestContext(const IisPsolRequestContext&) = delete;
  IisPsolRequestContext& operator=(const IisPsolRequestContext&) = delete;
};

}  // namespace net_instaweb

#endif  // IIS_PSOL_REQUEST_CONTEXT_H_