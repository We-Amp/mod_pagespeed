// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Capture API Route Handlers
//
// Factory functions that create RouteHandler lambdas for the browser
// capture endpoints:
//   POST /v1/capture/waterfall   - Network waterfall via CDP
//   POST /v1/capture/screenshot  - Viewport PNG screenshot via CDP
//
// Both endpoints require --enable-browser-analysis and a running
// Chrome process.  Returns 503 when Chrome is not available.

#ifndef PAGESPEED_SRC_WORKER_CAPTURE_HANDLERS_H_
#define PAGESPEED_SRC_WORKER_CAPTURE_HANDLERS_H_

#include "src/worker/http_server.h"

namespace pagespeed {

class BrowserAnalysisManager;

// Runtime context for capture API handlers.
// Provides access to browser infrastructure without coupling to
// the Worker class.
struct CaptureContext {
  // Browser analysis manager (may be nullptr when browser analysis
  // is disabled).  Provides access to Chrome/CdpClient.
  BrowserAnalysisManager* browser_manager;

  // Navigation timeout in milliseconds (default 30000).
  int navigation_timeout_ms = 30000;

  // When true, skip SSRF checks for private/loopback URLs.
  // Intended for Docker/development environments where the worker
  // needs to reach localhost or private-network services.
  bool allow_private_urls = false;
};

// Register all capture routes on the server.
//   POST /v1/capture/waterfall   - Network waterfall HAR-like JSON
//   POST /v1/capture/screenshot  - Viewport PNG screenshot (base64)
void RegisterCaptureRoutes(HttpServer& server, CaptureContext& ctx);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_CAPTURE_HANDLERS_H_
