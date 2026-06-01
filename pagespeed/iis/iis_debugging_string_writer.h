#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/message_handler.h"


namespace net_instaweb
{

class DebuggingStringWriter : public Writer
{
public:

DebuggingStringWriter::DebuggingStringWriter(std::string* s) : Writer()
{
	s_ = s;
}

DebuggingStringWriter::~DebuggingStringWriter() {}

bool DebuggingStringWriter::Write(const StringPiece& str, MessageHandler* handler) {
	//handler->Message(MessageType::kInfo, "write [%ld] bytes: [%.*s]"
	//	, str.size(), str.size(), str.data());
	s_->append(str.data(), str.size());		
	return true;
}

bool DebuggingStringWriter::Flush(MessageHandler* message_handler) {
  return true;
}

private:
	std::string * s_;
};


}