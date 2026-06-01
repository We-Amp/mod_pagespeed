#include "pagespeed/iis/iis_message_handler.h"                                      
#include "pagespeed/iis/iis_global_constants.h"
#include "pagespeed/kernel/sharedmem/shared_circular_buffer.h"
#define _WINSOCKAPI_
#include <Windows.h>
#include <signal.h>





namespace net_instaweb {                                              
	
	bool logToEventLog=true;
	bool logToEventLogSet=false;
	static bool loggedError=false;
	static bool loggedWarning=false;
	static bool loggedInfo=false;

	IisMessageHandler::IisMessageHandler(Timer* timer, AbstractMutex* mutex)
		: SystemMessageHandler(timer, mutex)
		 , buffer_(NULL),
		  mutex_(mutex)
	{         
		
		m_hEventLog = RegisterEventSourceA( NULL, "IISpeed" );
		// TODO(oschaaf): seems message handlers are created per request. that's not a good idea
		// and may drag on performance later on. Find out where they get constructed, and fix it.
		//		OutputDebugStringA("created event log");
		
	}

	IisMessageHandler::~IisMessageHandler() { 
		if (NULL != m_hEventLog)
		{
			DeregisterEventSource( m_hEventLog );
			m_hEventLog = NULL;
		}
	}

	//void IisMessageHandler::set_buffer(SharedCircularBuffer* buff) {
		//ScopedMutex lock(mutex_.get());
	//	buffer_ = buff;
	//}


	//bool IisMessageHandler::Dump(Writer* writer) {
		// Can't dump before SharedCircularBuffer is set up.
		//if (buffer_ == NULL) {
		//	return false;
		//}
		//return buffer_->Dump(writer, &handler_);
	//}

	const char * GetMessageLevel(MessageType type) {
		switch(type) {
		case kInfo:
			return "INFO";
		case kWarning:
			return "WARN";
		case kError:
			return "ERR ";// space is padding for format
		default:
			return "????";
		}
	}

	void                                                                  
		IisMessageHandler::MessageSImpl(MessageType type, const GoogleString& message) {

		const char * buf = message.c_str();
		OutputDebugStringA(buf);
		WriteEventViewerLog(buf, type);
		AddMessageToBuffer(type, message);
	}
	                                                                     

	void                                                                  
		IisMessageHandler::FileMessageSImpl(MessageType type, const char* filename,
			int line, const GoogleString& message) {
		const char * buf = message.c_str();
		OutputDebugStringA(buf);
		WriteEventViewerLog(buf, type);
		AddMessageToBuffer(type, message);
	
	}                                                                   


	std::string                                                           
		IisMessageHandler::Format(const char * str, va_list args) {          
			std::string buffer;                                               
			// Ignore the name of this routine: it formats with vsnprintf.                                                               
			// See base/stringprintf.cc.                                                                                                 
			StringAppendV(&buffer, str, args);                                
			return buffer;                                                    
	}                                                                     
	BOOL IisMessageHandler::WriteEventViewerLog(LPCSTR szNotification, MessageType type)
	{
		return true;
		if (type == net_instaweb::kInfo)
				return true;
		WORD category = EVENTLOG_ERROR_TYPE;

		switch(type)
		{
		case net_instaweb::kInfo:
			category = EVENTLOG_INFORMATION_TYPE;
			break;
		case net_instaweb::kFatal: /*can only map this to error?*/
		case net_instaweb::kError:
			category = EVENTLOG_ERROR_TYPE;
			break;
		case net_instaweb::kWarning:
			category = EVENTLOG_WARNING_TYPE;
			break;
		}

		if (m_hEventLog )
		{
			if ((logToEventLog || type==net_instaweb::kFatal) )
			{
				return ReportEventA(m_hEventLog,category, 0, 0,NULL, 1, 0, &szNotification, NULL );
			}			
			else
			{
				
				if(type==net_instaweb::kInfo && !loggedInfo)
				{
					loggedInfo=true;
					const char *eventLogWarning="Eventlogging is turned off for IIS WebSpeed. Further events in this session will be suppressed, you can turn logging in the eventlog back on by adding\nIISpeed UseEventLog on\nto the IISpeed.config. You can also see the log at http://<hostname>/iispeed_message from the local machine.";
				
					return ReportEventA(m_hEventLog,category, 0, 0,NULL, 1, 0, &eventLogWarning, NULL );
				}
				if (type==net_instaweb::kError && !loggedError)
				{
					loggedError=true;
					const char *eventLogWarning="Eventlogging is turned off for IIS WebSpeed. We did however encounter an error, further errors in this session will be suppressed, you can turn logging in the eventlog back on by adding\nIISpeed UseEventLog on\nto the IISpeed.config. You can also see the error log at http://<hostname>/iispeed_message from the local machine.";
				
					return ReportEventA(m_hEventLog,category, 0, 0,NULL, 1, 0, &eventLogWarning, NULL );
				}
				if (type==net_instaweb::kWarning && !loggedWarning)
				{
					loggedWarning=true;
					const char *eventLogWarning="Eventlogging is turned off for IIS WebSpeed. We did however encounter a warning, further warnings in this session will be suppressed, you can turn logging in the eventlog back on by adding\nIISpeed UseEventLog on\nto the IISpeed.config. You can also see the error log at http://<hostname>/iispeed_message from the local machine.";
				
					return ReportEventA(m_hEventLog,category, 0, 0,NULL, 1, 0, &eventLogWarning, NULL );
				}
			}
		}
		return FALSE;
	}
}                                                                     

