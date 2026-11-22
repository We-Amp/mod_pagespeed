// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SYSTEM_SYSTEM_PROCESS_OPTIONS_H_
#define PAGESPEED_SYSTEM_SYSTEM_PROCESS_OPTIONS_H_

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// The process-scope options that SystemRewriteDriverFactory parses itself
// rather than RewriteOptions: their names, where each may be set, and how
// each value is checked.  Pure functions with no factory state, so that the
// factory (which applies the value) and the offline config linter (which only
// reports on it) run the same checks.
class SystemProcessOptions {
 public:
  // Which option a one-argument directive named.
  enum class Option {
    kStaticAssetPrefix,
    kUsePerVHostStatistics,
    kInstallCrashHandler,
    kNumRewriteThreads,
    kNumExpensiveRewriteThreads,
    kForceCaching,
    kListOutstandingUrlsOnError,
    kMessageBufferSize,
    kTrackOriginalContentLength,
  };

  // What Parse1 produced.  `apply` is false when the option was accepted but
  // must not be applied (a tolerated process-scope option below process
  // scope: "OK" there means "move on", not "applied").
  struct Value {
    Option option = Option::kStaticAssetPrefix;
    bool apply = false;
    bool bool_value = false;
    int int_value = 0;
    StringPiece string_value;
  };

  // The thread-count value that asks for the computed policy.
  static constexpr int kAutoThreadCount = 0;

  // Parses a one-argument process-scope option.  Returns kOptionNameUnknown
  // when `option` is not one of these, kOptionValueInvalid with *msg set when
  // the value or the scope is wrong, and kOptionOk otherwise.  Warnings go to
  // `handler`.
  static RewriteOptions::OptionSettingResult Parse1(
      StringPiece option, StringPiece arg, bool process_scope, Value* value,
      GoogleString* msg, MessageHandler* handler);

  // Parses CreateSharedMemoryMetadataCache's arguments, the only
  // two-argument process-scope option.  On kOptionOk with *apply true,
  // *size_kb holds the parsed size; creating the cache is the caller's job.
  static RewriteOptions::OptionSettingResult Parse2(
      StringPiece option, StringPiece arg1, StringPiece arg2,
      bool process_scope, bool* apply, int64* size_kb, GoogleString* msg,
      MessageHandler* handler);
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_SYSTEM_PROCESS_OPTIONS_H_
