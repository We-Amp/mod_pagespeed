// SPDX-License-Identifier: Apache-2.0
//
// Spike probe: static_rewriter with a fixed (mock) clock.
//   timer_probe <mode> [rewrite flags] input_dir output_dir name
// mode: real   real timer, real scheduler (what static_rewriter does)
//       mock   MockTimer + real Scheduler (the pairing suspected to hang)
//       mockS  MockTimer + MockScheduler (advances the mock clock when idle)

#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/url_async_fetcher.h"

#include "net/instaweb/rewriter/public/process_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/rewrite_gflags.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/automatic/static_rewriter.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/thread/scheduler.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/kernel/thread/mock_scheduler.h"
#include "net/instaweb/http/public/request_context.h"

namespace net_instaweb {
namespace {

enum Mode { kReal, kMock, kMockScheduler };

// A fetch that never answers: the rewrite of that resource can only end by
// its deadline, which is what makes the clock matter.
class NeverFetcher : public UrlAsyncFetcher {
 public:
  void Fetch(const GoogleString& url, MessageHandler* handler,
             AsyncFetch* fetch) override {}
};

bool hung_fetch = false;

class ProbeRewriter : public FileRewriter {
 public:
  ProbeRewriter(const ProcessContext& pc, const RewriteGflags* g, Mode mode)
      : FileRewriter(pc, g, false), mode_(mode) {}

  Timer* DefaultTimer() override {
    if (mode_ == kReal) return FileRewriter::DefaultTimer();
    // 2026-01-01T00:00:00Z, fixed.
    mock_ = new MockTimer(thread_system()->NewMutex(), 1767225600000LL);
    return mock_;
  }

  UrlAsyncFetcher* DefaultAsyncUrlFetcher() override {
    if (hung_fetch) return new NeverFetcher;
    return FileRewriter::DefaultAsyncUrlFetcher();
  }

  Scheduler* CreateScheduler() override {
    if (mode_ == kMockScheduler) {
      timer();  // make sure mock_ exists
      return new MockScheduler(thread_system(), mock_);
    }
    return RewriteDriverFactory::CreateScheduler();
  }

 private:
  Mode mode_;
  MockTimer* mock_ = nullptr;
};

int Run(int argc, char** argv) {
  if (argc < 2) return 2;
  Mode mode = kReal;
  if (strcmp(argv[1], "mock") == 0) mode = kMock;
  if (strcmp(argv[1], "mockS") == 0) mode = kMockScheduler;
  if (strncmp(argv[1], "hung-", 5) == 0) {
    hung_fetch = true;
    argv[1] += 5;
    if (strcmp(argv[1], "mock") == 0) mode = kMock;
    if (strcmp(argv[1], "mockS") == 0) mode = kMockScheduler;
  }
  // Drop the mode argument before gflags parsing.
  for (int i = 1; i + 1 < argc; ++i) argv[i] = argv[i + 1];
  --argc;

  ProcessContext process_context;
  SystemRewriteOptions::Initialize();
  RewriteDriverFactory::Initialize();
  RewriteGflags gflags(argv[0], &argc, &argv);
  if (argc != 4) {
    fprintf(stderr, "usage: timer_probe mode [flags] in out name\n");
    return 2;
  }
  ProbeRewriter factory(process_context, &gflags, mode);
  RewriteOptions* options = factory.default_options();
  if (!gflags.SetOptions(&factory, options)) return 2;
  ServerContext* server_context = factory.CreateServerContext();

  GoogleString name = argv[3];
  GoogleString url = StrCat("http://test.com/", name);
  GoogleString in_path = StrCat(argv[1], "/", name);
  GoogleString out_path = StrCat(argv[2], "/", name);
  GoogleString input, output;
  if (!factory.file_system()->ReadFile(in_path.c_str(), &input,
                                       factory.message_handler())) {
    return 2;
  }
  StringWriter writer(&output);
  RewriteDriver* driver = server_context->NewRewriteDriver(
      RequestContext::NewTestRequestContext(server_context->thread_system()));
  // With a fetch that never answers, wait by deadline, the way a server does.
  driver->set_fully_rewrite_on_flush(!hung_fetch);
  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kAccept, "image/webp");
  request_headers.Add(
      HttpAttributes::kUserAgent,
      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko)"
      " Chrome/42.0.2302.4 Safari/537.36");
  driver->SetRequestHeaders(request_headers);
  factory.set_filename_prefix(argv[2]);
  driver->SetWriter(&writer);
  if (!driver->StartParseId(url, in_path, kContentTypeHtml)) return 3;
  driver->ParseText(input);
  driver->FinishParse();
  if (!factory.file_system()->WriteFileAtomic(out_path, output,
                                              factory.message_handler())) {
    return 3;
  }
  // Leave without tearing down: the hung fetches would block the shutdown.
  fflush(nullptr);
  _exit(0);
}

}  // namespace
}  // namespace net_instaweb

int main(int argc, char** argv) { return net_instaweb::Run(argc, argv); }
