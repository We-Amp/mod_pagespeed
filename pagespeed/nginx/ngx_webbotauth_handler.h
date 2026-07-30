// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// nginx <-> Web-Bot-Auth verifier glue. OBSERVE-ONLY
// UNLESS WebBotAuthBotDetection IS ON: classifies each request (human /
// signed-agent / verified-bot / unknown) via the RFC 9421 verifier
// (pagespeed/kernel/webbotauth) and exposes the verdict as the
// $x_verified_bot nginx variable. It NEVER blocks and NEVER enforces; it only
// labels. The feature is OFF by default (the WebBotAuth directive).
//
// The one exception to observe-only is the separate, also default-off
// WebBotAuthBotDetection directive: with it on, a request whose signature
// verified is additionally classified as an automated client by PageSpeed's
// own bot detection (RequestProperties::IsBot), which suppresses the
// instrumentation, critical-image and critical-CSS beacons and lazyload for
// that request, skips its background fetches when
// DisableBackgroundFetchesForBots is on (default off), and logs it as a bot.
// It still never blocks it and never alters the page's visible content.
// With WebBotAuthBotDetection off -- the default, and the behaviour every
// deployment that enabled WebBotAuth for telemetry alone keeps -- the verdict
// reaches nothing but the $x_verified_bot variable and the opt-in counters.
//
// This header is deliberately free of any webbotauth includes so that
// ngx_pagespeed.cc (compiled by nginx itself for the --add-module path) can
// reference the two entry points without pulling the verifier's headers into
// nginx's own compilation unit. All webbotauth includes live in the .cc.

#ifndef NGX_WEBBOTAUTH_HANDLER_H_
#define NGX_WEBBOTAUTH_HANDLER_H_

#include "ngx_pagespeed.h"  // ngx_http types + str helpers (+ LOG_* undef dance)

namespace net_instaweb {

class Statistics;
class ResponseHeaders;

// PREACCESS/PRECONTENT phase handler. When WebBotAuth is enabled, classifies
// the request and (when WebBotAuthTelemetry is enabled) increments the opt-in
// counter for verified/signed-agent requests. ALWAYS returns NGX_DECLINED so
// the request proceeds untouched -- observe-only, never blocks.
ngx_int_t ps_webbotauth_preaccess_handler(ngx_http_request_t* r);

// PREACCESS/PRECONTENT phase handler for the design record A3 RSL-CAP ENFORCEMENT (the
// PAID sibling of the observe-only A1 handler above). When RslCapEnforcement is
// enabled, it validates the request's Authorization: License capability token
// and maps the verdict to an inline status: authorized -> NGX_DECLINED (allow);
// no/invalid/expired/unknown-issuer/bad-signature token -> 401; valid identity
// but the requested license/scope is not granted -> 402. It NEVER settles,
// meters, escrows, or custodies money. Default OFF: returns NGX_DECLINED
// immediately (zero cost) unless the operator opts in.
ngx_int_t ps_rsl_cap_preaccess_handler(ngx_http_request_t* r);

// get_handler for the $x_verified_bot variable. Computes the verdict for the
// current request and emits the verdict token ("human" / "signed-agent" /
// "verified-bot" / "unknown"); for a verified bot it emits
// "<bot-name>, ed25519-verified". Sets not_found when WebBotAuth is disabled.
ngx_int_t ps_x_verified_bot_variable(ngx_http_request_t* r,
                                     ngx_http_variable_value_t* v,
                                     uintptr_t data);

// Reads back the verdict the preaccess handler already stored for this
// transaction and answers the single question PageSpeed's bot detection needs:
// did a Web Bot Auth signature actually verify? True only for signed-agent and
// verified-bot; false for human, for unknown (verification failed), and
// whenever the handler did not run -- including when WebBotAuth is off.
//
// Deliberately one-directional, mirroring DeviceProperties::IsBot: a valid
// signature proves the client is automated, but neither "no signature" nor
// "verification failed" is evidence of a human, so the false answer means
// "no opinion" and simply leaves the user-agent heuristic in charge. Reading
// "unknown" as bot would also hand any client a one-header way to switch off
// its own beaconing.
//
// Pure read of the stored value -- no crypto, no file read, no allocation on
// the request path. Callers must additionally check the
// WebBotAuthBotDetection option before acting on the answer.
bool ps_webbotauth_signature_verified(ngx_http_request_t* r);

// Registers the opt-in telemetry counters (verified/signed-agent requests +
// non-web-bot-auth "other signature" requests). Called from
// NgxRewriteDriverFactory::InitStats.
void ps_webbotauth_init_stats(Statistics* statistics);

// Records the index of the $x_verified_bot variable (from
// ngx_http_get_variable_index at postconfiguration) so the preaccess handler
// can store the computed verdict once. Called from ps_init.
void ps_webbotauth_set_var_index(ngx_int_t index);

// the design record Bar-A opt-in counter (EXPERIMENTAL, default off).
//
// Map (or, on first run, create) the shared memory-mapped counter file at
// `path` ONCE, in the master before workers fork; the MAP_SHARED region is then
// inherited by every forked worker. Idempotent and a no-op if `path` is empty or
// the file is already mapped. Called from ps_init_module.
void ps_webbotauth_counter_map(const GoogleString& path);

// Read the SECRET bearer token gating the exact counter document from the
// PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN environment variable into process memory.
// Logs nothing sensitive (the value is NEVER logged). Called from
// ps_init_child_process.
void ps_webbotauth_counter_read_token();

// Build the /.well-known/webbotauth-counter response for a GET/HEAD request that
// the router classified as RequestRouting::kWebBotAuthCounter (mode non-off).
// Returns true and fills *headers + *body with the coarse or token-gated exact
// document; returns false to HIDE the endpoint (mode private + no valid token),
// so the caller returns NGX_DECLINED and the request falls through to a normal
// 404. Reflects ZERO request data; carries no version/build identifiers.
bool ps_webbotauth_counter_build_response(ngx_http_request_t* r,
                                          ResponseHeaders* headers,
                                          GoogleString* body);

}  // namespace net_instaweb

#endif  // NGX_WEBBOTAUTH_HANDLER_H_
