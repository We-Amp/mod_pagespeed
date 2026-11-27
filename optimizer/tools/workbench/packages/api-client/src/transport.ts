// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import type { Readable } from 'svelte/store';

/** Callback to unsubscribe from a WebSocket stream. */
export type Unsubscribe = () => void;

/**
 * Shell-agnostic transport for communicating with the PageSpeed worker API.
 * Web console uses DirectTransport (fetch + WebSocket).
 * VS Code extension uses VsCodeTransport (postMessage bridge).
 */
export interface ApiTransport {
  get<T>(path: string, params?: Record<string, string>): Promise<T>;
  /** Fetch binary content as a Response (for raw bytes / blob URLs). */
  getBlob?(path: string, params?: Record<string, string>): Promise<Response>;
  patch<T>(path: string, body: object): Promise<T>;
  post<T>(path: string, body: object): Promise<T>;
  subscribe(path: string, handler: (msg: unknown) => void): Unsubscribe;
  readonly connected: Readable<boolean>;
  readonly error: Readable<string | null>;
  connect(): Promise<void>;
  disconnect(): void;
}
