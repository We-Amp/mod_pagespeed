// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

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

DebuggingStringWriter::~DebuggingStringWriter() override {}

bool DebuggingStringWriter::Write(const StringPiece& str, MessageHandler* handler) override {
	//handler->Message(MessageType::kInfo, "write [%ld] bytes: [%.*s]"
	//	, str.size(), str.size(), str.data());
	s_->append(str.data(), str.size());		
	return true;
}

bool DebuggingStringWriter::Flush(MessageHandler* message_handler) override {
  return true;
}

private:
	std::string * s_;
};


}