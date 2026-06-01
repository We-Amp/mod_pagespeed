
#ifndef LOG_MESSAGE_HANDLER_H_
#define LOG_MESSAGE_HANDLER_H_


namespace net_instaweb {

class IisMessageHandler;

namespace log_message_handler {

void Install(IisMessageHandler* handler);
void Deinstall();

}  // namespace log_message_handler

}  // namespace net_instaweb

#endif  // LOG_MESSAGE_HANDLER_H_