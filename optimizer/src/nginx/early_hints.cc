// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - HTTP 103 Early Hints Implementation

#include "src/nginx/early_hints.h"

#include <cstring>

namespace pagespeed {

ngx_int_t SendEarlyHints(ngx_http_request_t* r,
                         const std::vector<PreloadResource>& resources) {
  if (resources.empty()) {
    return NGX_OK;
  }

  // Build the combined Link header value.
  std::string link_value = FormatLinkHeader(resources);
  if (link_value.empty()) {
    return NGX_OK;
  }

#if defined(nginx_version) && nginx_version >= 1029000
  // nginx 1.29.0 and later: hand the Link header to nginx's own
  // informational-response filter chain.  Each protocol (HTTP/1.1, HTTP/2,
  // HTTP/3) frames the 103 itself, so the hint reaches clients on every
  // protocol and never corrupts a multiplexed connection.  The chain is
  // entered directly (not through ngx_http_send_early_hints) because that
  // wrapper is also gated by the operator's "early_hints" directive, which
  // controls passing a backend's 103, not this module's own hints.
  if (r != r->main) {
    return NGX_OK;
  }

  // The filters emit whatever is in r->headers_out.headers.  Stage the Link
  // header in a private list and restore the real one afterwards so no other
  // header leaks into the 103 and nothing staged leaks into the response.
  ngx_list_t saved_headers = r->headers_out.headers;
  if (ngx_list_init(&r->headers_out.headers, r->pool, 1,
                    sizeof(ngx_table_elt_t)) != NGX_OK) {
    r->headers_out.headers = saved_headers;
    return NGX_ERROR;
  }

  ngx_int_t rc = NGX_ERROR;
  ngx_table_elt_t* h =
      static_cast<ngx_table_elt_t*>(ngx_list_push(&r->headers_out.headers));
  u_char* value = static_cast<u_char*>(ngx_pnalloc(r->pool, link_value.size()));
  u_char* lowcase = static_cast<u_char*>(ngx_pnalloc(r->pool, sizeof("link")));
  if (h != nullptr && value != nullptr && lowcase != nullptr) {
    ngx_memcpy(lowcase, "link", sizeof("link"));
    ngx_memcpy(value, link_value.data(), link_value.size());
    h->hash = 1;
    ngx_str_set(&h->key, "Link");
    h->value.data = value;
    h->value.len = link_value.size();
    h->lowcase_key = lowcase;
    rc = ngx_http_top_early_hints_filter(r);
  }

  r->headers_out.headers = saved_headers;

  if (rc == NGX_ERROR) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "pagespeed: failed to send 103 Early Hints");
    return NGX_ERROR;
  }

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                 "pagespeed: sent 103 Early Hints over HTTP/%d",
                 r->http_version >= NGX_HTTP_VERSION_20 ? 2 : 1);
  return NGX_OK;
#else
  // Older nginx has no API for informational responses (1xx other than 100
  // Continue).  Write the 103 directly to the connection for HTTP/1.1 only.
  //
  // The response format:
  //   HTTP/1.1 103 Early Hints\r\n
  //   Link: <url>; rel=preload; as=style\r\n
  //   \r\n

  // HTTP/1.0 has no 1xx.  HTTP/2 and HTTP/3 are framed protocols: writing raw
  // "HTTP/1.1 103" bytes to them corrupts the stream, so without nginx's own
  // 103 support those clients get no hint.
  if (r->http_version != NGX_HTTP_VERSION_11) {
    return NGX_OK;
  }

  // Format the response line + headers.
  std::string response = "HTTP/1.1 103 Early Hints\r\nLink: ";
  response.append(link_value);
  response.append("\r\n\r\n");

  // Allocate an nginx buffer and write directly to the connection.
  ngx_buf_t* b = ngx_create_temp_buf(r->pool, response.size());
  if (b == nullptr) {
    return NGX_ERROR;
  }

  b->last = ngx_cpymem(b->pos, response.data(), response.size());
  b->flush = 1;

  // Pool-allocate chain (never stack-allocate — if send_chain
  // performs a partial write, nginx's write event handler will
  // reference the chain later, after this function returns).
  ngx_chain_t* out =
      static_cast<ngx_chain_t*>(ngx_palloc(r->pool, sizeof(ngx_chain_t)));
  if (out == nullptr) {
    return NGX_ERROR;
  }
  out->buf = b;
  out->next = nullptr;

  // Write to the connection's send chain.  This bypasses the normal
  // filter chain since we need to send the 103 before the main
  // response headers.
  //
  // send_chain returns the *unsent* remainder of the chain.  A single call
  // can perform a partial write under socket back-pressure (returning a
  // non-NULL, non-error chain); dropping that remainder truncates the 103 and
  // corrupts the stream once the main 200 response follows.  Drive the write
  // through FlushWithRetry so the full buffer is drained before we report
  // success.  send_chain advances b->pos for the bytes it accepted, so the
  // not-yet-sent count is (b->last - b->pos).
  ngx_connection_t* c = r->connection;
  ngx_chain_t* cl = out;
  FlushStatus status = FlushWithRetry([&]() -> long {
    cl = c->send_chain(c, cl, 0);
    if (cl == NGX_CHAIN_ERROR) {
      return -1;
    }
    if (cl == nullptr) {
      return 0;  // fully sent
    }
    return static_cast<long>(b->last - b->pos);  // bytes still pending
  });
  if (status != FlushStatus::kComplete) {
    ngx_log_error(
        NGX_LOG_ERR, r->connection->log, 0,
        "pagespeed: failed to send 103 Early Hints (%s)",
        status == FlushStatus::kError ? "send error" : "write would block");
    return NGX_ERROR;
  }

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                 "pagespeed: sent 103 Early Hints (%uz bytes)",
                 response.size());

  return NGX_OK;
#endif  // nginx_version >= 1029000
}

}  // namespace pagespeed
