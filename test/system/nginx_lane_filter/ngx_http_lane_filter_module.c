/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 We-Amp B.V.
 *
 * A test-only filter for the HTML cache conformance lane. It stands in for
 * third-party filters an operator may run after this module's own header
 * filters (in the compressor slot, before the module's etag filter):
 *
 *   lane_filter_add_header NAME VALUE;  adds one response header line;
 *   lane_filter_rewrite_body TYPE;      for that media type, clears the
 *                                       length and appends bytes to the body;
 *   lane_filter_force_encoding on;      states Content-Encoding: gzip whatever
 *                                       the request's compression flags say
 *                                       (the body is left as it is).
 *
 * Never part of a release; the lane builds it against its own nginx source.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

typedef struct {
  ngx_array_t* add_headers; /* ngx_keyval_t */
  ngx_str_t rewrite_type;
  ngx_flag_t force_encoding;
} ngx_http_lane_filter_conf_t;

typedef struct {
  ngx_uint_t appended;
} ngx_http_lane_filter_ctx_t;

static ngx_int_t ngx_http_lane_filter_init(ngx_conf_t* cf);
static void* ngx_http_lane_filter_create_conf(ngx_conf_t* cf);
static char* ngx_http_lane_filter_merge_conf(ngx_conf_t* cf, void* parent,
                                             void* child);

static ngx_command_t ngx_http_lane_filter_commands[] = {
    {ngx_string("lane_filter_add_header"),
     NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE2,
     ngx_conf_set_keyval_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_lane_filter_conf_t, add_headers), NULL},
    {ngx_string("lane_filter_rewrite_body"),
     NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_lane_filter_conf_t, rewrite_type), NULL},
    {ngx_string("lane_filter_force_encoding"),
     NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_lane_filter_conf_t, force_encoding), NULL},
    ngx_null_command};

static ngx_http_module_t ngx_http_lane_filter_module_ctx = {
    NULL,                             /* preconfiguration */
    ngx_http_lane_filter_init,        /* postconfiguration */
    NULL,                             /* create main configuration */
    NULL,                             /* init main configuration */
    NULL,                             /* create server configuration */
    NULL,                             /* merge server configuration */
    ngx_http_lane_filter_create_conf, /* create location configuration */
    ngx_http_lane_filter_merge_conf   /* merge location configuration */
};

ngx_module_t ngx_http_lane_filter_module = {NGX_MODULE_V1,
                                            &ngx_http_lane_filter_module_ctx,
                                            ngx_http_lane_filter_commands,
                                            NGX_HTTP_MODULE,
                                            NULL,
                                            NULL,
                                            NULL,
                                            NULL,
                                            NULL,
                                            NULL,
                                            NULL,
                                            NGX_MODULE_V1_PADDING};

static ngx_http_output_header_filter_pt ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt ngx_http_next_body_filter;

static u_char kLaneAppended[] = "\n<!-- lane filter -->\n";

static ngx_table_elt_t* ngx_http_lane_filter_push_header(ngx_http_request_t* r,
                                                         ngx_str_t* key,
                                                         ngx_str_t* value) {
  ngx_table_elt_t* h = ngx_list_push(&r->headers_out.headers);
  if (h == NULL) {
    return NULL;
  }
  h->hash = 1;
  h->key = *key;
  h->value = *value;
  h->lowcase_key = NULL;
#if defined(nginx_version) && nginx_version >= 1023000
  h->next = NULL;
#endif
  return h;
}

static ngx_int_t ngx_http_lane_header_filter(ngx_http_request_t* r) {
  static ngx_str_t encoding_key = ngx_string("Content-Encoding");
  static ngx_str_t encoding_value = ngx_string("gzip");
  ngx_http_lane_filter_conf_t* conf;
  ngx_uint_t i;

  if (r != r->main) {
    return ngx_http_next_header_filter(r);
  }
  conf = ngx_http_get_module_loc_conf(r, ngx_http_lane_filter_module);

  if (conf->add_headers != NULL) {
    ngx_keyval_t* kv = conf->add_headers->elts;
    for (i = 0; i < conf->add_headers->nelts; i++) {
      if (ngx_http_lane_filter_push_header(r, &kv[i].key, &kv[i].value) ==
          NULL) {
        return NGX_ERROR;
      }
    }
  }

  if (conf->force_encoding) {
    ngx_table_elt_t* h =
        ngx_http_lane_filter_push_header(r, &encoding_key, &encoding_value);
    if (h == NULL) {
      return NGX_ERROR;
    }
    r->headers_out.content_encoding = h;
  }

  if (conf->rewrite_type.len > 0 && r->headers_out.content_type.len > 0 &&
      !r->header_only) {
    size_t len = r->headers_out.content_type_len;
    if (len == 0 || len > r->headers_out.content_type.len) {
      len = r->headers_out.content_type.len;
    }
    if (len == conf->rewrite_type.len &&
        ngx_strncasecmp(r->headers_out.content_type.data,
                        conf->rewrite_type.data, len) == 0) {
      ngx_http_lane_filter_ctx_t* ctx =
          ngx_pcalloc(r->pool, sizeof(ngx_http_lane_filter_ctx_t));
      if (ctx == NULL) {
        return NGX_ERROR;
      }
      ngx_http_set_ctx(r, ctx, ngx_http_lane_filter_module);
      ngx_http_clear_content_length(r);
    }
  }
  return ngx_http_next_header_filter(r);
}

static ngx_int_t ngx_http_lane_body_filter(ngx_http_request_t* r,
                                           ngx_chain_t* in) {
  ngx_http_lane_filter_ctx_t* ctx;
  ngx_chain_t* cl;

  ctx = ngx_http_get_module_ctx(r, ngx_http_lane_filter_module);
  if (ctx == NULL || ctx->appended || in == NULL) {
    return ngx_http_next_body_filter(r, in);
  }
  for (cl = in; cl != NULL; cl = cl->next) {
    ngx_buf_t* b;
    ngx_chain_t* add;

    if (!cl->buf->last_buf) {
      continue;
    }
    b = ngx_calloc_buf(r->pool);
    add = ngx_alloc_chain_link(r->pool);
    if (b == NULL || add == NULL) {
      return NGX_ERROR;
    }
    b->pos = kLaneAppended;
    b->last = b->pos + sizeof(kLaneAppended) - 1;
    b->memory = 1;
    b->last_buf = 1;
    cl->buf->last_buf = 0;
    if (ngx_buf_size(cl->buf) == 0) {
      cl->buf->sync = 1;
    }
    add->buf = b;
    add->next = cl->next;
    cl->next = add;
    ctx->appended = 1;
    break;
  }
  return ngx_http_next_body_filter(r, in);
}

static void* ngx_http_lane_filter_create_conf(ngx_conf_t* cf) {
  ngx_http_lane_filter_conf_t* conf =
      ngx_pcalloc(cf->pool, sizeof(ngx_http_lane_filter_conf_t));
  if (conf == NULL) {
    return NULL;
  }
  conf->add_headers = NGX_CONF_UNSET_PTR;
  conf->force_encoding = NGX_CONF_UNSET;
  return conf;
}

static char* ngx_http_lane_filter_merge_conf(ngx_conf_t* cf, void* parent,
                                             void* child) {
  ngx_http_lane_filter_conf_t* prev = parent;
  ngx_http_lane_filter_conf_t* conf = child;
  ngx_conf_merge_ptr_value(conf->add_headers, prev->add_headers, NULL);
  ngx_conf_merge_str_value(conf->rewrite_type, prev->rewrite_type, "");
  ngx_conf_merge_value(conf->force_encoding, prev->force_encoding, 0);
  return NGX_CONF_OK;
}

static ngx_int_t ngx_http_lane_filter_init(ngx_conf_t* cf) {
  ngx_http_next_header_filter = ngx_http_top_header_filter;
  ngx_http_top_header_filter = ngx_http_lane_header_filter;
  ngx_http_next_body_filter = ngx_http_top_body_filter;
  ngx_http_top_body_filter = ngx_http_lane_body_filter;
  return NGX_OK;
}
