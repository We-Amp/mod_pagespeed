// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Disable SSR — the console is a client-only SPA that uses browser APIs
// (WebSocket, localStorage, sessionStorage) not available during SSR.
export const ssr = false;
