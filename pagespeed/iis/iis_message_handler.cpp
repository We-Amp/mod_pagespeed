// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/iis/iis_message_handler.h"
#include "pagespeed/iis/iis_event_log.h"                                      
#include "pagespeed/iis/iis_global_constants.h"
#include "pagespeed/kernel/sharedmem/shared_circular_buffer.h"
#define _WINSOCKAPI_
#include <Windows.h>
#include <signal.h>





namespace net_instaweb {                                              
	
	bool logToEventLog=false;
	bool logToEventLogSet=false;
	bool logToEventLogParsed=false;

	IisMessageHandler::IisMessageHandler(Timer* timer, AbstractMutex* mutex)
		: SystemMessageHandler(timer, mutex)
		 , buffer_(NULL),
		  mutex_(mutex)
	{
	}

	IisMessageHandler::~IisMessageHandler() {
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
	// The module's ONE event-log facility owns the policy: severity
	// filtering, the per-distinct-text latch and the per-process cap all
	// live there, so every handler instance (there is one per site
	// context) sees the same latches.
	WriteModuleEvent(logToEventLogParsed
	                     ? (logToEventLog ? EventLogDirective::kOn
	                                      : EventLogDirective::kOff)
	                     : EventLogDirective::kNotReadYet,
	                 type, szNotification);
	return TRUE;
}
}                                                                     

