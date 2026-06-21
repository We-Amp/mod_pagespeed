// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// nginx <-> Web-Bot-Auth verifier glue. OBSERVE-ONLY:
// classifies each request (human / signed-agent / verified-bot / unknown) via
// the RFC 9421 verifier (pagespeed/kernel/webbotauth) and exposes the verdict
// as the $x_verified_bot nginx variable. It NEVER blocks and NEVER enforces;
// it only labels. The feature is OFF by default (the WebBotAuth directive).
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

// Registers the opt-in telemetry counter. Called from
// NgxRewriteDriverFactory::InitStats.
void ps_webbotauth_init_stats(Statistics* statistics);

// Records the index of the $x_verified_bot variable (from
// ngx_http_get_variable_index at postconfiguration) so the preaccess handler
// can store the computed verdict once. Called from ps_init.
void ps_webbotauth_set_var_index(ngx_int_t index);

}  // namespace net_instaweb

#endif  // NGX_WEBBOTAUTH_HANDLER_H_
