// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Where the scanner's API calls go. The site's own path is the only target
// for visitors; a different base is honoured only while the page itself is
// served from this machine (local development against a render service).

export const DEFAULT_API_BASE = '/ai-readability/api';

const LOCAL_HOSTS = new Set(['localhost', '127.0.0.1', '[::1]', '::1']);

export function resolveApiBase(search: string, hostname: string): string {
  if (!LOCAL_HOSTS.has(hostname)) return DEFAULT_API_BASE;
  const value = new URLSearchParams(search).get('api');
  return value || DEFAULT_API_BASE;
}
