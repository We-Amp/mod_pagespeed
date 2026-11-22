// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/system_process_options.h"

#include "pagespeed/kernel/base/circular_buffer.h"
#include "pagespeed/system/optimization_thread_policy.h"

namespace net_instaweb {

namespace {

const char kStaticAssetPrefix[] = "StaticAssetPrefix";
const char kUsePerVHostStatistics[] = "UsePerVHostStatistics";
const char kInstallCrashHandler[] = "InstallCrashHandler";
const char kNumRewriteThreads[] = "NumRewriteThreads";
const char kNumExpensiveRewriteThreads[] = "NumExpensiveRewriteThreads";
const char kForceCaching[] = "ForceCaching";
const char kListOutstandingUrlsOnError[] = "ListOutstandingUrlsOnError";
const char kMessageBufferSize[] = "MessageBufferSize";
const char kTrackOriginalContentLength[] = "TrackOriginalContentLength";
const char kCreateSharedMemoryMetadataCache[] =
    "CreateSharedMemoryMetadataCache";

// The literal that asks for the computed policy by name.
const char kAutoThreadCountLiteral[] = "auto";

struct NamedOption {
  const char* name;
  SystemProcessOptions::Option option;
  bool strict;  // true: an error below process scope; false: ignored there.
};

const NamedOption kOptions[] = {
    {kStaticAssetPrefix, SystemProcessOptions::Option::kStaticAssetPrefix,
     true},
    {kUsePerVHostStatistics,
     SystemProcessOptions::Option::kUsePerVHostStatistics, true},
    {kInstallCrashHandler, SystemProcessOptions::Option::kInstallCrashHandler,
     true},
    {kNumRewriteThreads, SystemProcessOptions::Option::kNumRewriteThreads,
     true},
    {kNumExpensiveRewriteThreads,
     SystemProcessOptions::Option::kNumExpensiveRewriteThreads, true},
    {kForceCaching, SystemProcessOptions::Option::kForceCaching, false},
    {kListOutstandingUrlsOnError,
     SystemProcessOptions::Option::kListOutstandingUrlsOnError, false},
    {kMessageBufferSize, SystemProcessOptions::Option::kMessageBufferSize,
     false},
    {kTrackOriginalContentLength,
     SystemProcessOptions::Option::kTrackOriginalContentLength, false},
};

// Parses a NumRewriteThreads / NumExpensiveRewriteThreads argument.  Accepts
// `auto` and its alias `0` (both yielding kAutoThreadCount) and any positive
// integer; rejects everything else with a message naming the directive, which
// is what the operator sees at startup.
//
// A positive value above kMaxOptimizationThreadsPerPool is clamped rather than
// rejected, with a warning naming both the requested and the resolved count.
// An explicit count is an override, so it is not the policy's business to
// refuse it -- but installing thousands of threads per server process because
// a digit was mistyped, silently, is exactly what this policy exists to stop.
bool ParseThreadCountArgument(StringPiece option, StringPiece arg, int* count,
                              GoogleString* msg, MessageHandler* handler) {
  if (StringCaseEqual(arg, kAutoThreadCountLiteral)) {
    *count = SystemProcessOptions::kAutoThreadCount;
    return true;
  }
  int value = 0;
  if (!StringToInt(arg, &value)) {
    *msg = StrCat("'", option, "' must be a positive integer or '",
                  kAutoThreadCountLiteral, "'; got '", arg, "'.");
    return false;
  }
  if (value < 0) {
    *msg = StrCat("'", option, "' must not be negative; got '", arg, "'. Use '",
                  kAutoThreadCountLiteral,
                  "' (or 0) to size the pool automatically.");
    return false;
  }
  if (value > kMaxOptimizationThreadsPerPool) {
    GoogleString option_str;
    option.CopyToString(&option_str);
    handler->Message(kWarning,
                     "'%s' was set to %d, which is far more threads than any "
                     "server process can use; using %d instead. Leave it "
                     "unset, or set it to '%s', to size the pool from the CPU "
                     "budget.",
                     option_str.c_str(), value, kMaxOptimizationThreadsPerPool,
                     kAutoThreadCountLiteral);
    value = kMaxOptimizationThreadsPerPool;
  }
  *count = value;  // 0 is the alias for `auto`.
  return true;
}

}  // namespace

RewriteOptions::OptionSettingResult SystemProcessOptions::Parse1(
    StringPiece option, StringPiece arg, bool process_scope, Value* value,
    GoogleString* msg, MessageHandler* handler) {
  const NamedOption* named = nullptr;
  for (const NamedOption& candidate : kOptions) {
    if (StringCaseEqual(option, candidate.name)) {
      named = &candidate;
      break;
    }
  }
  if (named == nullptr) {
    return RewriteOptions::kOptionNameUnknown;
  }
  value->option = named->option;
  value->apply = false;

  // First check the scope.
  if (!process_scope) {
    if (named->strict) {
      *msg = StrCat("'", option, "' is global and can't be set at this scope.");
      return RewriteOptions::kOptionValueInvalid;
    }
    // msg is only printed to the user on error, so warnings must be logged.
    handler->Message(kWarning, "'%s' is global and is ignored at this scope",
                     option.as_string().c_str());
    // OK here means "move on" not "accepted and applied".
    return RewriteOptions::kOptionOk;
  }

  // Scope is ok and option is known.  Parse.
  switch (named->option) {
    case Option::kStaticAssetPrefix:
      value->string_value = arg;
      value->apply = true;
      return RewriteOptions::kOptionOk;

    // Most of our options take booleans.  The value is applied even when it
    // does not parse (as false), as it always has been; the result still
    // reports it as invalid.
    case Option::kUsePerVHostStatistics:
    case Option::kForceCaching:
    case Option::kInstallCrashHandler:
    case Option::kListOutstandingUrlsOnError:
    case Option::kTrackOriginalContentLength: {
      bool is_on = false;
      bool parsed = RewriteOptions::ParseFromString(arg, &is_on);
      value->bool_value = is_on;
      value->apply = true;
      return parsed ? RewriteOptions::kOptionOk
                    : RewriteOptions::kOptionValueInvalid;
    }

    // The two thread counts accept `auto` as a literal, with `0` as its
    // alias, and reject anything negative: a negative count used to convert
    // to size_t as SIZE_MAX on its way to the worker pools, which means
    // unbounded thread creation and a CHECK failure at child init.  Nothing
    // validated it, so the only symptom was the crash.
    case Option::kNumRewriteThreads:
    case Option::kNumExpensiveRewriteThreads: {
      int count = kAutoThreadCount;
      if (!ParseThreadCountArgument(option, arg, &count, msg, handler)) {
        return RewriteOptions::kOptionValueInvalid;
      }
      value->int_value = count;
      value->apply = true;
      return RewriteOptions::kOptionOk;
    }

    // An integer >= 0; 0 disables the message buffer.
    case Option::kMessageBufferSize: {
      int int_value = 0;
      bool parsed = RewriteOptions::ParseFromString(arg, &int_value);
      // The size is handed straight to the shared-memory circular buffer, so
      // reject what that buffer cannot represent: a negative value or one
      // past its capacity bound would otherwise fail silently at child init,
      // where a false return from InitSegment is dropped.
      if (parsed &&
          (int_value < 0 || int_value > CircularBuffer::kMaxCapacity)) {
        *msg = StrCat("MessageBufferSize must be between 0 and ",
                      CircularBuffer::kMaxCapacity, ".");
        return RewriteOptions::kOptionValueInvalid;
      }
      value->int_value = int_value;
      value->apply = true;
      return parsed ? RewriteOptions::kOptionOk
                    : RewriteOptions::kOptionValueInvalid;
    }
  }
  return RewriteOptions::kOptionNameUnknown;
}

RewriteOptions::OptionSettingResult SystemProcessOptions::Parse2(
    StringPiece option, StringPiece arg1, StringPiece arg2, bool process_scope,
    bool* apply, int64* size_kb, GoogleString* msg, MessageHandler* handler) {
  *apply = false;
  if (!StringCaseEqual(option, kCreateSharedMemoryMetadataCache)) {
    return RewriteOptions::kOptionNameUnknown;
  }
  if (!process_scope) {
    // msg is only printed to the user on error, so warnings must be logged.
    handler->Message(kWarning, "'%s' is global and is ignored at this scope",
                     option.as_string().c_str());
    // OK here means "move on" not "accepted and applied".
    return RewriteOptions::kOptionOk;
  }
  int64 kb = 0;
  if (!StringToInt64(arg2, &kb) || kb < 0) {
    *msg = "size_kb must be a positive 64-bit integer";
    return RewriteOptions::kOptionValueInvalid;
  }
  *size_kb = kb;
  *apply = true;
  return RewriteOptions::kOptionOk;
}

}  // namespace net_instaweb
