/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "test/pagespeed/kernel/base/mock_message_handler.h"

#include <map>
#include <utility>

#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/writer.h"

namespace net_instaweb {

MockMessageHandler::MockMessageHandler(AbstractMutex* mutex) : mutex_(mutex) {}

MockMessageHandler::~MockMessageHandler() {}

void MockMessageHandler::MessageSImpl(MessageType type,
                                      const GoogleString& message) {
  ScopedMutex hold_mutex(mutex_.get());
  if (ShouldPrintMessage(message)) {
    internal_handler_.MessageSImpl(type, message);
    GoogleString type_str = MessageTypeToString(type);
    StringPiece type_char(type_str.data(), 1);
    // A message may itself contain embedded "\n"s (SystemMessageHandler's
    // real AddMessageToBuffer joins a multi-line message into one Write());
    // prefix every line with the type char, same as the real handler, so
    // every buffered line -- not just the first -- starts with one, which
    // is what ReformatMessage()/GetMessageType() assume of each split line.
    StringPieceVector lines;
    SplitStringPieceToVector(message, "\n", &lines, false);
    StrAppend(&buffer_, type_char, "[Wed Jan 01 00:00:00 2014] ");
    StrAppend(&buffer_, "[", type_str, "] [00000] ");
    StrAppend(&buffer_, lines.empty() ? StringPiece() : lines[0], "\n");
    int64 newline_count = 1;
    for (int i = 1, n = lines.size(); i < n; ++i) {
      StrAppend(&buffer_, type_char, lines[i], "\n");
      ++newline_count;
    }
    lines_written_ += newline_count;
  } else {
    ++skipped_message_counts_[type];
  }
  ++message_counts_[type];
}

void MockMessageHandler::FileMessageSImpl(MessageType type,
                                          const char* filename, int line,
                                          const GoogleString& message) {
  ScopedMutex hold_mutex(mutex_.get());
  if (ShouldPrintMessage(message)) {
    internal_handler_.FileMessageSImpl(type, filename, line, message);
    GoogleString type_str = MessageTypeToString(type);
    StringPiece type_char(type_str.data(), 1);
    StringPieceVector lines;
    SplitStringPieceToVector(message, "\n", &lines, false);
    StrAppend(&buffer_, type_char, "[Wed Jan 01 00:00:00 2014] ");
    StrAppend(&buffer_, "[", type_str, "] [00000] ");
    StrAppend(&buffer_, "[", filename, ":", IntegerToString(line), "] ");
    StrAppend(&buffer_, lines.empty() ? StringPiece() : lines[0], "\n");
    int64 newline_count = 1;
    for (int i = 1, n = lines.size(); i < n; ++i) {
      StrAppend(&buffer_, type_char, lines[i], "\n");
      ++newline_count;
    }
    lines_written_ += newline_count;
  } else {
    ++skipped_message_counts_[type];
  }
  ++message_counts_[type];
}

int MockMessageHandler::MessagesOfType(MessageType type) const {
  ScopedMutex hold_mutex(mutex_.get());
  return MessagesOfTypeImpl(message_counts_, type);
}

int MockMessageHandler::SkippedMessagesOfType(MessageType type) const {
  ScopedMutex hold_mutex(mutex_.get());
  return MessagesOfTypeImpl(skipped_message_counts_, type);
}

int MockMessageHandler::MessagesOfTypeImpl(const MessageCountMap& counts,
                                           MessageType type) const {
  MessageCountMap::const_iterator i = counts.find(type);
  if (i != counts.end()) {
    return i->second;
  } else {
    return 0;
  }
}

int MockMessageHandler::TotalMessages() const {
  ScopedMutex hold_mutex(mutex_.get());
  return TotalMessagesImpl(message_counts_);
}

int MockMessageHandler::TotalSkippedMessages() const {
  ScopedMutex hold_mutex(mutex_.get());
  return TotalMessagesImpl(skipped_message_counts_);
}

int MockMessageHandler::TotalMessagesImpl(const MessageCountMap& counts) const {
  int total = 0;
  for (MessageCountMap::const_iterator i = counts.begin(); i != counts.end();
       ++i) {
    total += i->second;
  }
  return total;
}

int MockMessageHandler::SeriousMessages() const {
  ScopedMutex hold_mutex(mutex_.get());
  int num = TotalMessagesImpl(message_counts_) -
            MessagesOfTypeImpl(message_counts_, kInfo);
  return num;
}

void MockMessageHandler::set_mutex(AbstractMutex* mutex) {
  mutex_->DCheckUnlocked();
  mutex_.reset(mutex);
}

void MockMessageHandler::AddPatternToSkipPrinting(const char* pattern) {
  patterns_to_skip_.Allow(GoogleString(pattern));
}

bool MockMessageHandler::ShouldPrintMessage(const StringPiece& msg) {
  return !patterns_to_skip_.Match(msg, false);
}

bool MockMessageHandler::Dump(Writer* writer) {
  if (buffer_ == "") {
    return false;
  }
  return (writer->Write(buffer_, &internal_handler_));
}

bool MockMessageHandler::DumpWithCount(Writer* writer, int64* lines_written) {
  {
    ScopedMutex hold_mutex(mutex_.get());
    *lines_written = lines_written_;
  }
  return Dump(writer);
}

}  // namespace net_instaweb
