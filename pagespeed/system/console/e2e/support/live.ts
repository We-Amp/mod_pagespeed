// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Where the live project finds the console: CONSOLE_LIVE_URL names the origin
// of a running server (default: a local dev or test server on :8080). The
// nightly sets it to the Apache test server.
export const LIVE_ORIGIN = (process.env.CONSOLE_LIVE_URL ?? "http://localhost:8080").replace(/\/+$/, "");

/** The console's per-vhost admin path on the live server, with a trailing slash. */
export const LIVE_BASE = `${LIVE_ORIGIN}/pagespeed_admin/`;
