// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Implementation of the observe-only nginx Web-Bot-Auth (RFC 9421) wiring.
// See ngx_webbotauth_handler.h. the design record Amendment A1: the FREE verifier --
// no 401/402, no enforcement, no RSL-CAP, no metering beyond an opt-in counter.
//
// A1 v1 scope: signer keys come from an operator-LOCAL JWKS file
// (WebBotAuthKeyDirectoryFile) and are resolved SYNCHRONOUSLY in-memory. The
// network-fetch directory provider is deliberately NOT used here because its
// async fetch cannot complete inline in an nginx phase handler; automatic
// network refresh of the directory is a tracked follow-up.

#include "ngx_webbotauth_handler.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "ngx_rewrite_options.h"
#include "ngx_server_context.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/webbotauth/classifier.h"
#include "pagespeed/kernel/webbotauth/key_directory.h"
#include "pagespeed/kernel/webbotauth/rsl_cap_token.h"
#include "pagespeed/kernel/webbotauth/rsl_cap_validator.h"
#include "pagespeed/kernel/webbotauth/static_key_directory.h"
#include "pagespeed/kernel/webbotauth/verifier.h"

namespace net_instaweb {

const char kWebBotAuthVerifiedSignedRequests[] =
    "web_bot_auth_verified_signed_requests";

void ps_webbotauth_init_stats(Statistics* statistics) {
  statistics->AddVariable(kWebBotAuthVerifiedSignedRequests);
}

namespace {

// The index of the $x_verified_bot variable, obtained once at postconfiguration
// (ps_init) so the preaccess handler can store the computed verdict directly,
// making the variable get-handler a no-recompute reader on the common path.
ngx_int_t g_xvb_var_index = NGX_ERROR;

// Maximum size of the local JWKS key-directory file we will read.
const int64_t kMaxKeyDirectoryFileBytes = int64_t{256} * 1024;

// Hardening: cap the Authorization header length BEFORE parsing an
// RSL-CAP token, so an abusive multi-KB header cannot drive parser work. A real
// RSL-CAP token is well under this; operators can also bound it upstream via
// large_client_header_buffers. Oversized -> treated as no valid token (401).
const size_t kMaxRslCapAuthHeaderBytes = size_t{8} * 1024;

// Case-insensitively find request header `name` (length `name_len`); returns
// its value as a StringPiece backed by the request (valid for the synchronous
// duration of the request), or an empty StringPiece if absent.
StringPiece FindRequestHeader(ngx_http_request_t* r, const char* name,
                              size_t name_len) {
  ngx_list_part_t* part = &r->headers_in.headers.part;
  ngx_table_elt_t* h = static_cast<ngx_table_elt_t*>(part->elts);
  for (ngx_uint_t i = 0;; i++) {
    if (i >= part->nelts) {
      if (part->next == nullptr) {
        break;
      }
      part = part->next;
      h = static_cast<ngx_table_elt_t*>(part->elts);
      i = 0;
    }
    if (h[i].key.len == name_len &&
        ngx_strncasecmp(h[i].key.data,
                        reinterpret_cast<u_char*>(const_cast<char*>(name)),
                        name_len) == 0) {
      return StringPiece(reinterpret_cast<char*>(h[i].value.data),
                         h[i].value.len);
    }
  }
  return StringPiece();
}

// Parse the operator verified-bot registry: "keyid=name,keyid2=name2".
void ParseVerifiedBots(StringPiece spec, webbotauth::VerifiedBotRegistry* reg) {
  StringPieceVector pairs;
  SplitStringPieceToVector(spec, ",", &pairs, true /* omit_empty */);
  for (size_t i = 0; i < pairs.size(); ++i) {
    StringPiece pair = pairs[i];
    StringPiece::size_type eq = pair.find('=');
    if (eq == StringPiece::npos) {
      continue;
    }
    StringPiece keyid = pair.substr(0, eq);
    StringPiece name = pair.substr(eq + 1);
    if (!keyid.empty()) {
      reg->Register(keyid, name);
    }
  }
}

// Classify the current request. Returns the verdict; sets *bot_name_out and
// *keyid_out when applicable. The common human path (no Signature-Input header)
// short-circuits BEFORE any file read or allocation, so it is allocation-free
// and never touches the filesystem. Keys are resolved synchronously from the
// operator-local JWKS file (no network).
webbotauth::Verdict ClassifyRequest(ngx_http_request_t* r,
                                    NgxServerContext* server_context,
                                    NgxRewriteOptions* options,
                                    GoogleString* bot_name_out,
                                    GoogleString* keyid_out) {
  StringPiece sig_input =
      FindRequestHeader(r, "Signature-Input", sizeof("Signature-Input") - 1);
  // No signature at all -> human. Short-circuit so the common path neither
  // reads the key-directory file nor allocates the verifier inputs.
  if (sig_input.empty()) {
    return webbotauth::Verdict::kHuman;
  }

  StringPiece signature =
      FindRequestHeader(r, "Signature", sizeof("Signature") - 1);
  StringPiece user_agent =
      FindRequestHeader(r, "User-Agent", sizeof("User-Agent") - 1);

  // Load the operator-local signer key directory (A1 v1: local file, no fetch).
  // An empty/unreadable directory yields an empty provider -> kNotFound ->
  // Verdict::kUnknown for a signed request, the honest result when no key is
  // available. The path comes ONLY from operator config, never the request.
  GoogleString jwks_doc;
  const GoogleString& kd_file = options->web_bot_auth_key_directory_file();
  if (!kd_file.empty()) {
    FileSystem* fs = server_context->file_system();
    if (fs != nullptr) {
      fs->ReadFile(kd_file.c_str(), kMaxKeyDirectoryFileBytes, &jwks_doc,
                   server_context->message_handler());
    }
  }

  webbotauth::RequestView req;
  req.method = str_to_string_piece(r->method_name);
  req.authority = ps_determine_host(r);
  req.path = str_to_string_piece(r->uri);
  req.signature_input = sig_input;
  req.signature = signature;
  req.user_agent = user_agent;
  // directory_host is unused by the static provider (single local directory).

  webbotauth::VerifiedBotRegistry registry;
  ParseVerifiedBots(options->web_bot_auth_verified_bots(), &registry);

  webbotauth::StaticKeyDirectory local_provider(jwks_doc);

  // A2 warm-fetch: when the operator has configured a remote key directory (URL
  // + SSRF allowlist + directory_host all set), resolve keys CACHE-ONLY from the
  // shared blocking cache the background warmer populates, then fall back to the
  // local file. The cache-only read never fetches on the event loop. With no
  // remote directory configured this is byte-identical to A1 v1 (local file).
  webbotauth::KeyDirectoryProvider* provider = &local_provider;
  webbotauth::CachedKeyDirectoryProvider cache_only(
      nullptr, server_context->metadata_cache(), /*realm=*/"wba",
      /*read_through=*/false);
  std::unique_ptr<webbotauth::ChainedKeyDirectoryProvider> chained;
  if (!options->web_bot_auth_key_directory_url().empty() &&
      !options->web_bot_auth_key_directory_allowlist().empty() &&
      !options->web_bot_auth_directory_host().empty() &&
      server_context->metadata_cache() != nullptr) {
    // The cache key is (directory_host, keyid); the warmer writes under the same
    // operator-configured host, so the request lookup must use it too.
    req.directory_host = options->web_bot_auth_directory_host();
    std::vector<webbotauth::KeyDirectoryProvider*> chain;
    chain.push_back(&cache_only);
    chain.push_back(&local_provider);
    chained.reset(new webbotauth::ChainedKeyDirectoryProvider(chain));
    provider = chained.get();
  }

  // Use the engine's injectable Timer (unix seconds) rather than time(), so the
  // clock is the single mockable time source the rest of the engine uses.
  const int64_t now_unix_sec =
      server_context->timer()->NowMs() / Timer::kSecondMs;
  webbotauth::VerifyResult result =
      webbotauth::VerifyAndClassify(req, provider, registry, now_unix_sec);
  if (keyid_out != nullptr) {
    *keyid_out = result.keyid;
  }
  if (bot_name_out != nullptr) {
    *bot_name_out = result.bot_name;
  }
  return result.verdict;
}

// Render the verdict as the $x_verified_bot token. A verified bot is emitted as
// "<bot-name>, ed25519-verified"; otherwise the bare verdict token.
GoogleString FormatVerdict(webbotauth::Verdict verdict,
                           const GoogleString& bot_name) {
  if (verdict == webbotauth::Verdict::kVerifiedBot) {
    return StrCat(bot_name, ", ed25519-verified");
  }
  return webbotauth::VerdictToken(verdict);
}

// Store `out` into an nginx variable value, allocated from the request pool.
void SetNgxVarValue(ngx_http_request_t* r, ngx_http_variable_value_t* v,
                    const GoogleString& out) {
  u_char* data = static_cast<u_char*>(ngx_pnalloc(r->pool, out.size()));
  if (data == nullptr) {
    v->not_found = 1;
    return;
  }
  ngx_memcpy(data, out.data(), out.size());
  v->valid = 1;
  v->no_cacheable = 0;
  v->not_found = 0;
  v->len = static_cast<unsigned>(out.size());
  v->data = data;
}

}  // namespace

void ps_webbotauth_set_var_index(ngx_int_t index) { g_xvb_var_index = index; }

ngx_int_t ps_webbotauth_preaccess_handler(ngx_http_request_t* r) {
  NgxServerContext* server_context = ps_get_server_context(r);
  if (server_context == nullptr) {
    return NGX_DECLINED;
  }
  NgxRewriteOptions* options = server_context->config();
  // Default OFF: zero-cost fast path when the operator has not enabled it.
  if (options == nullptr || !options->web_bot_auth()) {
    return NGX_DECLINED;
  }

  // Compute the verdict EXACTLY ONCE per request here.
  GoogleString bot_name;
  GoogleString keyid;
  webbotauth::Verdict verdict =
      ClassifyRequest(r, server_context, options, &bot_name, &keyid);

  if (options->web_bot_auth_telemetry() &&
      (verdict == webbotauth::Verdict::kSignedAgent ||
       verdict == webbotauth::Verdict::kVerifiedBot)) {
    Statistics* stats = server_context->statistics();
    if (stats != nullptr) {
      Variable* v = stats->GetVariable(kWebBotAuthVerifiedSignedRequests);
      if (v != nullptr) {
        v->Add(1);
      }
    }
  }

  // Store the verdict into the indexed $x_verified_bot value so the variable
  // get-handler reads it instead of recomputing (counter and variable now
  // reflect the same single computation).
  if (g_xvb_var_index != NGX_ERROR) {
    ngx_http_variable_value_t* vv = &r->variables[g_xvb_var_index];
    SetNgxVarValue(r, vv, FormatVerdict(verdict, bot_name));
  }

  // Observe-only: never block, never alter the request.
  return NGX_DECLINED;
}

ngx_int_t ps_rsl_cap_preaccess_handler(ngx_http_request_t* r) {
  NgxServerContext* server_context = ps_get_server_context(r);
  if (server_context == nullptr) {
    return NGX_DECLINED;
  }
  NgxRewriteOptions* options = server_context->config();
  // Default OFF: zero-cost fast path when the operator has not enabled
  // enforcement. This is the PAID, demand-gated sibling of A1; it stays inert
  // until explicitly turned on.
  if (options == nullptr || !options->rsl_cap_enforcement()) {
    return NGX_DECLINED;
  }

  // Read the capability token from the Authorization header. Absent/oversized
  // are both treated as "no valid token" -> 401 (the size cap runs BEFORE any
  // parse so an abusive header cannot drive parser work).
  StringPiece auth =
      FindRequestHeader(r, "Authorization", sizeof("Authorization") - 1);
  if (auth.size() > kMaxRslCapAuthHeaderBytes) {
    return NGX_HTTP_UNAUTHORIZED;
  }

  // Resolve issuer keys from the operator-local JWKS file (v1: synchronous, no
  // network), mirroring A1. The path comes ONLY from operator config, never the
  // request. The SSRF-guarded NetFetchKeyDirectory
  // implements the same KeyDirectoryProvider interface and drops in here once
  // an async continuation exists -- it is deferred for the SAME reason A1 defers
  // it: an async fetch cannot complete inline in an nginx phase handler.
  GoogleString jwks_doc;
  const GoogleString& kd_file = options->rsl_cap_key_directory_file();
  if (!kd_file.empty()) {
    FileSystem* fs = server_context->file_system();
    if (fs != nullptr) {
      fs->ReadFile(kd_file.c_str(), kMaxKeyDirectoryFileBytes, &jwks_doc,
                   server_context->message_handler());
    }
  }

  webbotauth::StaticKeyDirectory local_provider(jwks_doc);

  // A2 warm-fetch (mirrors A1): prefer cache-only remote keys then the local
  // file when a remote RSL-CAP issuer directory is configured. The validator
  // passes rsl_cap_directory_host() to GetKey, the same host the warmer writes
  // under, so the cache key matches. Default-off => local file only (v1).
  webbotauth::KeyDirectoryProvider* provider = &local_provider;
  webbotauth::CachedKeyDirectoryProvider cache_only(
      nullptr, server_context->metadata_cache(), /*realm=*/"rsl",
      /*read_through=*/false);
  std::unique_ptr<webbotauth::ChainedKeyDirectoryProvider> chained;
  if (!options->rsl_cap_key_directory_url().empty() &&
      !options->rsl_cap_key_directory_allowlist().empty() &&
      !options->rsl_cap_directory_host().empty() &&
      server_context->metadata_cache() != nullptr) {
    std::vector<webbotauth::KeyDirectoryProvider*> chain;
    chain.push_back(&cache_only);
    chain.push_back(&local_provider);
    chained.reset(new webbotauth::ChainedKeyDirectoryProvider(chain));
    provider = chained.get();
  }

  webbotauth::RslCapValidator validator(provider);
  webbotauth::RslCapToken token;
  // Use the engine's injectable Timer (unix seconds) rather than time(), so the
  // clock is the single mockable time source the rest of the engine uses.
  const int64_t now_unix_sec =
      server_context->timer()->NowMs() / Timer::kSecondMs;
  webbotauth::RslCapStatus status = validator.Validate(
      auth, options->rsl_cap_requested_license(),
      options->rsl_cap_requested_scope(), options->rsl_cap_directory_host(),
      now_unix_sec, &token);

  // Issuer pin (hardening): when an issuer is configured, an otherwise-authorized
  // token whose iss does not match is rejected as an unknown issuer (401). The
  // core parses iss but leaves it unbound; this binds it at the policy layer.
  const GoogleString& want_iss = options->rsl_cap_issuer();
  if (status == webbotauth::RslCapStatus::kAuthorized && !want_iss.empty() &&
      token.iss != want_iss) {
    status = webbotauth::RslCapStatus::kUnknownIssuer;
  }

  // The verdict->status mapping is a kernel free function (unit-tested without
  // nginx). 0 means "allow" (NGX_DECLINED); 402/401 are returned verbatim and
  // match NGX_HTTP_UNAUTHORIZED (401) and the prior kHttpPaymentRequired (402).
  const int http_status = webbotauth::RslCapStatusToHttpStatus(status);
  return http_status == 0 ? NGX_DECLINED : http_status;
}

ngx_int_t ps_x_verified_bot_variable(ngx_http_request_t* r,
                                     ngx_http_variable_value_t* v,
                                     uintptr_t /*data*/) {
  NgxServerContext* server_context = ps_get_server_context(r);
  if (server_context == nullptr || server_context->config() == nullptr ||
      !server_context->config()->web_bot_auth()) {
    v->not_found = 1;
    return NGX_OK;
  }

  // On the common path the preaccess handler has already stored the indexed
  // value, so this handler is reached only on edge cases (e.g. subrequests
  // whose phase pipeline did not run the preaccess handler). Recompute once.
  GoogleString bot_name;
  GoogleString keyid;
  webbotauth::Verdict verdict = ClassifyRequest(
      r, server_context, server_context->config(), &bot_name, &keyid);
  SetNgxVarValue(r, v, FormatVerdict(verdict, bot_name));
  return NGX_OK;
}

}  // namespace net_instaweb
