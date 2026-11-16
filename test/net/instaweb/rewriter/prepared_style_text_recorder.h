// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// For tests that check that the text a filter writes into a <style> element
// is text InlineStyleText::Prepare() returned.

#ifndef TEST_NET_INSTAWEB_REWRITER_PREPARED_STYLE_TEXT_RECORDER_H_
#define TEST_NET_INSTAWEB_REWRITER_PREPARED_STYLE_TEXT_RECORDER_H_

#include <mutex>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/inline_style_text.h"

namespace net_instaweb {

// Records every text Prepare() returns while it exists. One at a time.
class PreparedStyleTextRecorder {
 public:
  PreparedStyleTextRecorder() {
    instance_ = this;
    InlineStyleText::set_prepare_observer_for_testing(&Observe);
  }
  ~PreparedStyleTextRecorder() {
    InlineStyleText::set_prepare_observer_for_testing(nullptr);
    instance_ = nullptr;
  }

  // Whether Prepare() returned exactly `text`.
  bool WasPrepared(StringPiece text) {
    std::lock_guard<std::mutex> lock(mutex_);
    return prepared_.count(GoogleString(text.data(), text.size())) != 0;
  }

  // How often Prepare() refused its input.
  int refusals() {
    std::lock_guard<std::mutex> lock(mutex_);
    return refusals_;
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    prepared_.clear();
    refusals_ = 0;
  }

 private:
  static void Observe(StringPiece css, InlineStyleMode mode,
                      const GoogleString* text) {
    PreparedStyleTextRecorder* self = instance_;
    if (self == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(self->mutex_);
    if (text == nullptr) {
      ++self->refusals_;
    } else {
      self->prepared_.insert(*text);
    }
  }

  static inline PreparedStyleTextRecorder* instance_ = nullptr;
  std::mutex mutex_;
  StringSet prepared_;
  int refusals_ = 0;
};

}  // namespace net_instaweb

#endif  // TEST_NET_INSTAWEB_REWRITER_PREPARED_STYLE_TEXT_RECORDER_H_
