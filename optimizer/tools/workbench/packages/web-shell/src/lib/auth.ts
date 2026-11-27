// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

const TOKEN_KEY = 'pagespeed_api_token';

const isBrowser = typeof sessionStorage !== 'undefined';

export function getStoredToken(): string | null {
  return isBrowser ? sessionStorage.getItem(TOKEN_KEY) : null;
}

export function storeToken(token: string): void {
  if (isBrowser) sessionStorage.setItem(TOKEN_KEY, token);
}

export function clearToken(): void {
  if (isBrowser) sessionStorage.removeItem(TOKEN_KEY);
}
