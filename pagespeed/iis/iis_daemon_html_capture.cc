// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/iis/iis_daemon_html_capture.h"

#include <utility>

#include "net/instaweb/public/global_constants.h"
#include "pagespeed/kernel/http/http_names.h"

namespace net_instaweb {

namespace {

// Lines this module owns on IIS.  The option headers are the ones the HTML
// path takes off the response before the rewriter reads it.
const char* const kModuleLines[] = {
    kModPagespeedHeader,
    kPageSpeedHeader,
    "X-Pagespeed-Init-Status",
    "Content-MD5",
    "PageSpeed",
    "PageSpeedFilters",
    "PageSpeedCssFlattenMaxBytes",
    "PageSpeedCssImageInlineMaxBytes",
    "PageSpeedCssInlineMaxBytes",
    "PageSpeedImageInlineMaxBytes",
    "PageSpeedJsInlineMaxBytes",
};

bool IsModuleLine(StringPiece name) {
  for (const char* module_line : kModuleLines) {
    if (StringCaseEqual(name, module_line)) return true;
  }
  return false;
}

}  // namespace

bool IisParseRequestStartMs(const wchar_t* value, size_t length,
                            int64* out_ms) {
  if (value == nullptr || length == 0 || length > 18) return false;
  int64 ms = 0;
  for (size_t i = 0; i < length; ++i) {
    const wchar_t c = value[i];
    if (c < L'0' || c > L'9') return false;
    ms = ms * 10 + static_cast<int64>(c - L'0');
  }
  if (ms <= 0) return false;
  *out_ms = ms;
  return true;
}

DaemonHtmlHeaderLines IisDaemonHtmlViewLines(const DaemonHtmlHeaderLines& raw) {
  DaemonHtmlHeaderLines lines;
  lines.reserve(raw.size());
  for (const auto& line : raw) {
    if (!IsModuleLine(line.first)) lines.push_back(line);
  }
  return lines;
}

IisDaemonHtmlCapture::IisDaemonHtmlCapture(
    std::unique_ptr<DaemonHtmlRecording> recording)
    : recording_(std::move(recording)) {}

IisDaemonHtmlCapture::~IisDaemonHtmlCapture() {
  // A capture dropped unfinished reports nothing: the recording's own
  // destructor releases what it holds.
}

void IisDaemonHtmlCapture::SetOrigin(int status,
                                     const DaemonHtmlHeaderLines& lines,
                                     bool body_inflated) {
  status_ = status;
  recording_->set_status(status);
  DaemonHtmlHeaderLines origin;
  for (const auto& line : IisDaemonHtmlViewLines(lines)) {
    if (StringCaseEqual(line.first, HttpAttributes::kTransferEncoding)) {
      continue;
    }
    if (body_inflated &&
        (StringCaseEqual(line.first, HttpAttributes::kContentEncoding) ||
         StringCaseEqual(line.first, HttpAttributes::kContentLength))) {
      continue;
    }
    origin.push_back(line);
  }
  recording_->SetOriginHeaders(std::move(origin));
}

void IisDaemonHtmlCapture::AddBody(const char* data, size_t size) {
  // Nothing after the handler's last send is part of its body.
  if (finished_ || body_broken_ || body_ended_) return;
  recording_->AddBody(data, size);
}

void IisDaemonHtmlCapture::NoteBodyEnd() {
  if (finished_ || body_broken_ || body_ended_) return;
  body_ended_ = true;
  recording_->set_body_ended();
}

void IisDaemonHtmlCapture::NoteBodyBroken() { body_broken_ = true; }

void IisDaemonHtmlCapture::NoteAborted() { aborted_ = true; }

void IisDaemonHtmlCapture::NoteModuleView(const DaemonHtmlHeaderLines& lines) {
  // Only the first: the engine keeps the latest baseline it is given.
  if (finished_ || module_view_taken_) return;
  module_view_taken_ = true;
  recording_->SetBaselineHeaders(IisDaemonHtmlViewLines(lines));
}

bool IisDaemonHtmlCapture::needs_final_view() const {
  return !finished_ && recording_->needs_final_headers();
}

void IisDaemonHtmlCapture::NoteFinalView(int status,
                                         const DaemonHtmlHeaderLines& lines) {
  if (!needs_final_view()) return;
  if (status != status_) aborted_ = true;
  recording_->NoteFinalHeaders(IisDaemonHtmlViewLines(lines));
}

bool IisDaemonHtmlCapture::Finish() {
  if (finished_) return false;
  const bool final_view_missing = recording_->needs_final_headers();
  finished_ = true;
  recording_->Finish(aborted_);
  return final_view_missing;
}

}  // namespace net_instaweb
