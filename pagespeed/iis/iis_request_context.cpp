// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/iis/iis_request_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "pagespeed/iis/iis_final_view_module.h"
#include "pagespeed/system/in_place_resource_recorder.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/iis/iis_server_context.h"

namespace net_instaweb
{

IisInnerRequestContext::~IisInnerRequestContext()
{
	pagespeedRequestPointer.reset(nullptr);
}


void IisInnerRequestContext::NoteDaemonHtmlModuleView(IHttpResponse* response)
{
	if (daemon_html_capture_ == nullptr || response == nullptr) {
		return;
	}
	const HTTP_RESPONSE* raw = response->GetRawHttpResponse();
	if (raw != nullptr) {
		daemon_html_capture_->NoteModuleView(IisResponseHeaderLines(*raw));
	}
}

void IisInnerRequestContext::NoteDaemonHtmlBodyEnded()
{
	if (daemon_html_capture_ == nullptr || daemon_html_commit_started_) {
		return;
	}
	daemon_html_commit_started_ = true;
	server_context_->NoteDaemonHtmlCommitStarted();
}

namespace {

// Test equipment: while the file PAGESPEED_TEST_HTML_COMMIT_HOLD_FILE names
// exists, a commit waits (at most a minute), so a test can show that a read
// made before the commit ran finds nothing.  Unset in production.
void HoldDaemonHtmlCommitForTest()
{
	static const std::string hold_file = [] {
		char buffer[MAX_PATH] = {0};
		const DWORD n = GetEnvironmentVariableA(
			"PAGESPEED_TEST_HTML_COMMIT_HOLD_FILE", buffer, MAX_PATH);
		return (n > 0 && n < MAX_PATH) ? std::string(buffer, n) : std::string();
	}();
	if (hold_file.empty()) {
		return;
	}
	for (int i = 0; i < 600; ++i) {
		if (GetFileAttributesA(hold_file.c_str()) == INVALID_FILE_ATTRIBUTES) {
			return;
		}
		Sleep(100);
	}
}

}  // namespace

void IisInnerRequestContext::CleanupStoredContext()
{
	// The response is done with: the HTML cache engine classifies what was
	// recorded of it, and stores it or removes a refused entry, inline.
	// The final view is missing when the final-view module entry did not
	// run for this request's site.
	if (daemon_html_capture_ != nullptr) {
		if (daemon_html_commit_started_) {
			HoldDaemonHtmlCommitForTest();
		}
		server_context_->TraceDaemonHtml("commit-begin", daemon_html_trace_url_, "");
		if (daemon_html_capture_->Finish()) {
			server_context_->NoteDaemonHtmlFinalViewMissing();
		}
		{
			Statistics* stats = server_context_->statistics();
			server_context_->TraceDaemonHtml("commit-end", daemon_html_trace_url_,
				StrCat("stored=", Integer64ToString(stats->GetVariable("daemon_html_stored")->Get()),
					" purge_race_refused=",
					Integer64ToString(stats->GetVariable("daemon_html_purge_race_refused")->Get())));
		}
		daemon_html_capture_.reset();
		if (daemon_html_commit_started_) {
			server_context_->NoteDaemonHtmlCommitFinished();
		}
	}
	if (markdown_answer_ != nullptr) {
		// Kept for the final-view registration, which never took it.
		server_context_->NoteDaemonHtmlMarkdownNotFinalized(
			"the PageSpeedFinalView module entry did not run; add it at the top of "
			"the site's modules list, as the installer does");
		markdown_answer_.reset();
	}
	// IIS is done with the request, but the rewriter may still be producing
	// output for it on its own threads: cut the fetch off from the request
	// first, so nothing it does from here on can reach IIS.
	if (base_fetch_ != nullptr)
	{
		base_fetch_->DetachFromRequest();
	}
	if (proxy_fetch_ != nullptr)
	{
		//CHECK(false);
		//TODO (oschaaf): ?!!!
		//proxy_fetch_->Done(true);
		proxy_fetch_ = nullptr;
	}
	if (driver_ != nullptr) {
		driver_->Cleanup();
		driver_ = nullptr;
	}

	if (recorder_ != nullptr) {
		//log("IPRO: recorder CLEANUP");
		recorder_->Fail();
		recorder_->DoneAndSetHeaders(nullptr, false);  // Deletes recorder.
		recorder_ = nullptr;
	}

	if (base_fetch_ != nullptr)
	{
		base_fetch_->ReleaseRef();
		base_fetch_ = nullptr;
	}
	if (this->process_context_)
	{
		this->process_context_->ReleaseRef();
	}
	delete gurl_;
	gurl_ = nullptr;
	delete this;
}


} 