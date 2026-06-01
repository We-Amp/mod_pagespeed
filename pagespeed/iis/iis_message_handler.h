#ifndef IIS_MESSAGE_HANDLER_H_                                        
#define IIS_MESSAGE_HANDLER_H_                                        

#include <Windows.h>
#include <string>                         
#include "pagespeed/system/system_message_handler.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/message_handler.h"                 
#include "pagespeed/kernel/base/string_util.h"                                         

namespace net_instaweb {

	
	class AbstractMutex;
	class SharedCircularBuffer;
	class Writer;
	extern bool logToEventLog;
	extern bool logToEventLogSet;

	class IisMessageHandler : public SystemMessageHandler {
	public:                                                             
		explicit IisMessageHandler(Timer* timer, AbstractMutex* mutex); 
		virtual ~IisMessageHandler();
		//void set_buffer(SharedCircularBuffer* buff);
		// Dump contents of SharedCircularBuffer.
		//bool Dump(Writer* writer);
		//static void InstallCrashHandler();
		

	protected:                                                          
		virtual void MessageSImpl(MessageType type, const GoogleString& message);

		virtual void FileMessageSImpl(MessageType type, const char* filename,
			int line, const GoogleString& message);

	private:             
		BOOL WriteEventViewerLog(LPCSTR szNotification, MessageType type);
		std::string Format(const char * str, va_list args);
		HANDLE m_hEventLog;
		GoogleMessageHandler handler_;
		SharedCircularBuffer* buffer_;
		AbstractMutex* mutex_;
	};                                                                  
}                                                                     

#endif // IIS_MESSAGE_HANDLER_H_    