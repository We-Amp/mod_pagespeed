// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/apache/apache_daemon_html.h"

#include <cstdlib>

#include "net/instaweb/rewriter/public/option_context.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/apache/apache_conditional_stash.h"
#include "pagespeed/apache/apache_config.h"
#include "pagespeed/apache/apache_rewrite_driver_factory.h"
#include "pagespeed/apache/apache_server_context.h"
#include "pagespeed/apache/instaweb_context.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/system/site_cache_fragment.h"
// httpd after the module's own headers (see apache_httpd_includes.h).
#include "apr_strings.h"    // NOLINT
#include "http_protocol.h"  // NOLINT
#include "httpd.h"          // NOLINT

namespace net_instaweb {

const char kApacheMarkdownHeadersFilterName[] =
    "MOD_PAGESPEED_MARKDOWN_HEADERS";

namespace {

const char kContentEncoding[] = "Content-Encoding";
const char kContentRange[] = "Content-Range";

int CollectLine(void* rec, const char* /*key*/, const char* value) {
  static_cast<std::vector<GoogleString>*>(rec)->push_back(
      value == nullptr ? GoogleString() : GoogleString(value));
  return 1;
}

std::vector<GoogleString> Lines(const apr_table_t* table, const char* name) {
  std::vector<GoogleString> lines;
  if (table != nullptr) {
    apr_table_do(CollectLine, &lines, table, name, nullptr);
  }
  return lines;
}

bool Has(const apr_table_t* table, const char* name) {
  return table != nullptr && apr_table_get(table, name) != nullptr;
}

// A transport line one of the two tables carries (the first one found).
const char* FindLine(const request_rec* r, const char* name) {
  const char* value = apr_table_get(r->headers_out, name);
  if (value == nullptr) value = apr_table_get(r->err_headers_out, name);
  return value;
}

// The Markdown header filter's state: the lines to apply and whom to tell
// about a transport conflict.
struct MarkdownFilterContext {
  DaemonHtmlHeaderLines lines;
  DaemonHtmlEngine* engine = nullptr;
};

apr_status_t DeleteMarkdownFilterContext(void* data) {
  delete static_cast<MarkdownFilterContext*>(data);
  return APR_SUCCESS;
}

// Writes the set to a cleared request.  `keep_transport`: a Content-Encoding
// or Content-Range line found is kept (the body is already encoded).
void ApplyHeaders(request_rec* r, const DaemonHtmlHeaderLines& lines,
                  bool keep_transport, bool* transport_conflict) {
  *transport_conflict = false;
  const char* encoding = nullptr;
  const char* range = nullptr;
  if (keep_transport) {
    encoding = FindLine(r, kContentEncoding);
    if (encoding == nullptr) encoding = r->content_encoding;
    range = FindLine(r, kContentRange);
    if (encoding != nullptr) encoding = apr_pstrdup(r->pool, encoding);
    if (range != nullptr) range = apr_pstrdup(r->pool, range);
  }
  apr_table_clear(r->headers_out);
  apr_table_clear(r->err_headers_out);
  // The core header filter writes Content-Language from this, and
  // Content-Encoding from r->content_encoding.
  r->content_languages = nullptr;
  r->content_encoding = nullptr;
  // A "no-etag" note (FileETag None for the path) would drop the copy's own
  // entity tag in the core header filter.
  apr_table_unset(r->notes, "no-etag");
  bool has_type = false;
  for (const auto& [name, value] : lines) {
    if (StringCaseEqual(name, HttpAttributes::kContentType)) {
      ap_set_content_type(r, apr_pstrdup(r->pool, value.c_str()));
      has_type = true;
    } else if (StringCaseEqual(name, HttpAttributes::kContentLength)) {
      ap_set_content_length(r, apr_atoi64(value.c_str()));
    } else {
      apr_table_addn(r->headers_out, apr_pstrdup(r->pool, name.c_str()),
                     apr_pstrdup(r->pool, value.c_str()));
    }
  }
  if (!has_type) {
    // A 304 names no type; nothing of the page's type goes out with it.
    r->content_type = nullptr;
  }
  if (encoding != nullptr) {
    apr_table_setn(r->headers_out, kContentEncoding, encoding);
    *transport_conflict = true;
  }
  if (range != nullptr) {
    apr_table_setn(r->headers_out, kContentRange, range);
    *transport_conflict = true;
  }
}

}  // namespace

void ApacheDaemonHtmlInFlight::Enter() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++count_;
}

void ApacheDaemonHtmlInFlight::Leave() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (--count_ == 0) idle_.notify_all();
}

void ApacheDaemonHtmlInFlight::WaitIdle() {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock, [this] { return count_ == 0; });
}

GoogleString ApacheDaemonHtmlSiteId(const server_rec* server,
                                    ApacheServerContext* context) {
  const GoogleString* fragment =
      context->apache_factory()->site_cache_fragment(server);
  return SiteIdFromServerFragment(
      fragment != nullptr ? *fragment
                          : context->global_config()->cache_fragment());
}

bool ApacheDaemonHtmlMainRequest(const request_rec* r) {
  return r->main == nullptr && r->prev == nullptr;
}

void FillApacheDaemonHtmlRequestFacts(request_rec* r,
                                      ApacheServerContext* context,
                                      const RewriteOptions* options,
                                      const GoogleUrl& url,
                                      DaemonHtmlRequestFacts* facts) {
  const apr_table_t* in = r->headers_in;
  facts->method = r->method == nullptr ? "" : r->method;
  if (url.IsWebValid()) {
    url.Scheme().CopyToString(&facts->scheme);
  }
  // The host as the client sent it, a port included; for a request target
  // in absolute form httpd's own reading of it.
  const char* host = apr_table_get(in, HttpAttributes::kHost);
  if (host == nullptr) host = r->hostname;
  facts->host = host == nullptr ? "" : host;
  // The request target's path and query, as received.  Not unparsed_uri: a
  // target in absolute form carries the scheme and host there.
  facts->path = r->parsed_uri.path == nullptr ? "" : r->parsed_uri.path;
  if (r->parsed_uri.query != nullptr) {
    StrAppend(&facts->path, "?", r->parsed_uri.query);
  }
  facts->site = ApacheDaemonHtmlSiteId(r->server, context);
  facts->url_agreed =
      ApacheDaemonHtmlMainRequest(r) && InstawebContext::InPlaceUrlAgreed(r);
  facts->has_authorization = Has(in, HttpAttributes::kAuthorization) ||
                             Has(in, HttpAttributes::kProxyAuthorization);
  facts->authenticated_user = r->user != nullptr;
  facts->has_cookie =
      Has(in, HttpAttributes::kCookie) || Has(in, HttpAttributes::kCookie2);
  facts->accept_lines = Lines(in, HttpAttributes::kAccept);
  const std::vector<GoogleString> cache_control =
      Lines(in, HttpAttributes::kCacheControl);
  facts->has_cache_control = !cache_control.empty();
  facts->cache_control = JoinCollection(cache_control, ", ");
  const std::vector<GoogleString> pragma = Lines(in, HttpAttributes::kPragma);
  facts->has_pragma = !pragma.empty();
  facts->pragma = JoinCollection(pragma, ", ");
  // The conditionals as they arrived: the post_read_request hook may have
  // removed them from headers_in.
  const ClientConditionalStash* stash = ApacheClientConditionalStash(r);
  if (stash != nullptr) CopyClientConditionals(*stash, facts);
  facts->has_if_match = Has(in, "If-Match");
  facts->has_if_unmodified_since = Has(in, "If-Unmodified-Since");
  facts->has_range = Has(in, "Range");
  facts->has_if_range = Has(in, "If-Range");
  facts->has_origin = Has(in, HttpAttributes::kOrigin);
  // When the request entered httpd, so never later than the origin's fetch.
  facts->capture_start_ms = apr_time_as_msec(r->request_time);
  if (options != nullptr) {
    facts->options_enabled_and_allowed = options->enabled() &&
                                         url.IsWebValid() &&
                                         options->IsAllowed(url.Spec());
    if (OptionContext::Compute(*options, &facts->option_context,
                               &facts->option_signature) !=
        OptionContextStatus::kOk) {
      facts->option_context.clear();
      facts->option_signature.clear();
    }
  }
}

ApacheDaemonHtmlPort::ApacheDaemonHtmlPort(request_rec* r,
                                           DaemonHtmlEngine* engine,
                                           ApacheDaemonHtmlInFlight* in_flight)
    : r_(r), engine_(engine), in_flight_(in_flight) {}

ApacheDaemonHtmlPort::~ApacheDaemonHtmlPort() = default;

void ApacheDaemonHtmlPort::MarkNoCompress() {
  if (!marked_no_compress_) {
    prior_no_gzip_ = apr_table_get(r_->subprocess_env, "no-gzip");
    prior_no_brotli_ = apr_table_get(r_->subprocess_env, "no-brotli");
    marked_no_compress_ = true;
  }
  apr_table_setn(r_->subprocess_env, "no-gzip", "1");
  apr_table_setn(r_->subprocess_env, "no-brotli", "1");
}

void ApacheDaemonHtmlPort::UndoNoCompress() {
  if (!marked_no_compress_) return;
  marked_no_compress_ = false;
  if (prior_no_gzip_ == nullptr) {
    apr_table_unset(r_->subprocess_env, "no-gzip");
  } else {
    apr_table_setn(r_->subprocess_env, "no-gzip", prior_no_gzip_);
  }
  if (prior_no_brotli_ == nullptr) {
    apr_table_unset(r_->subprocess_env, "no-brotli");
  } else {
    apr_table_setn(r_->subprocess_env, "no-brotli", prior_no_brotli_);
  }
}

bool ApacheDaemonHtmlPort::ClearAndEmitAllowlisted(
    const DaemonHtmlServeResult& result, bool head) {
  // Nothing may have gone out yet, and the answer must be one the engine
  // builds; anything else falls through with the request untouched.
  if (r_->sent_bodyct != 0 || r_->bytes_sent != 0 ||
      !ApacheDaemonHtmlMainRequest(r_) ||
      (result.status != HTTP_OK && result.status != HTTP_NOT_MODIFIED)) {
    UndoNoCompress();
    return false;
  }
  in_flight_->Enter();
  ApacheRemoveContentFilters(r_);
  bool unused = false;
  ApplyHeaders(r_, result.headers, /*keep_transport=*/false, &unused);
  r_->status = result.status;
  r_->status_line = nullptr;

  MarkdownFilterContext* filter_context = new MarkdownFilterContext;
  filter_context->lines = result.headers;
  filter_context->engine = engine_;
  apr_pool_cleanup_register(r_->pool, filter_context,
                            DeleteMarkdownFilterContext, apr_pool_cleanup_null);
  ap_add_output_filter(kApacheMarkdownHeadersFilterName, filter_context, r_,
                       r_->connection);

  apr_bucket_alloc_t* alloc = r_->connection->bucket_alloc;
  apr_bucket_brigade* bb = apr_brigade_create(r_->pool, alloc);
  if (!head && result.status == HTTP_OK && !result.body.empty()) {
    // Copied: the result goes away with the handler's stack.
    APR_BRIGADE_INSERT_TAIL(
        bb, apr_bucket_heap_create(result.body.data(), result.body.size(),
                                   nullptr, alloc));
  }
  APR_BRIGADE_INSERT_TAIL(bb, apr_bucket_eos_create(alloc));
  // Once the headers are in the chain the answer is this one, whatever the
  // network does with it: a failed write is a client that went away.
  ap_pass_brigade(r_->output_filters, bb);
  in_flight_->Leave();
  return true;
}

void ApacheDaemonHtmlPort::RunBlocking(DaemonHtmlCommit commit) {
  in_flight_->Enter();
  commit.Run();
  in_flight_->Leave();
}

void ApacheRemoveContentFilters(request_rec* r) {
  ap_filter_t* filter = r->output_filters;
  while (filter != nullptr) {
    ap_filter_t* next = filter->next;
    if (filter->r == r && filter->frec->ftype < AP_FTYPE_PROTOCOL &&
        !StringCaseEqual(filter->frec->name,
                         kApacheMarkdownHeadersFilterName)) {
      ap_remove_output_filter(filter);
    }
    filter = next;
  }
}

void ApacheApplyMarkdownHeaders(request_rec* r,
                                const DaemonHtmlHeaderLines& lines,
                                bool* transport_conflict) {
  ApplyHeaders(r, lines, /*keep_transport=*/true, transport_conflict);
}

apr_status_t ApacheMarkdownHeadersFilter(ap_filter_t* filter,
                                         apr_bucket_brigade* bb) {
  MarkdownFilterContext* context =
      static_cast<MarkdownFilterContext*>(filter->ctx);
  if (context != nullptr) {
    bool conflict = false;
    ApacheApplyMarkdownHeaders(filter->r, context->lines, &conflict);
    if (conflict && context->engine != nullptr) {
      context->engine->NoteTransportConflict();
    }
  }
  ap_remove_output_filter(filter);
  return ap_pass_brigade(filter->next, bb);
}

}  // namespace net_instaweb
