// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { writable } from 'svelte/store';
import type { Readable } from 'svelte/store';
import type { ApiTransport, Unsubscribe } from './transport.js';

/** Shape of the VS Code webview API injected by the extension host. */
export interface VsCodeApi {
  postMessage(message: unknown): void;
}

/** Message posted from the webview to the extension host. */
interface OutboundRequest {
  type: 'api-request';
  id: string;
  method: 'GET' | 'PATCH' | 'POST';
  path: string;
  params?: Record<string, string>;
  body?: object;
}

interface OutboundSubscribe {
  type: 'ws-subscribe';
  id: string;
  path: string;
}

interface OutboundUnsubscribe {
  type: 'ws-unsubscribe';
  id: string;
  path: string;
}

/** Message posted from the extension host back to the webview. */
interface InboundResponse {
  type: 'api-response';
  id: string;
  ok: boolean;
  data?: unknown;
  error?: string;
}

interface InboundWsMessage {
  type: 'ws-message';
  path: string;
  data: unknown;
}

interface InboundConnectionStatus {
  type: 'connection-status';
  connected: boolean;
  error?: string;
}

type InboundMessage = InboundResponse | InboundWsMessage | InboundConnectionStatus;

/** Default request timeout in milliseconds. */
const DEFAULT_TIMEOUT_MS = 10_000;

/**
 * Transport implementation for the VS Code extension webview, using
 * postMessage to communicate with the extension host which proxies
 * requests to the PageSpeed worker.
 */
export class VsCodeTransport implements ApiTransport {
  private readonly vscode: VsCodeApi;
  private readonly timeoutMs: number;

  private readonly _connected = writable(false);
  private readonly _error = writable<string | null>(null);

  /** Pending request callbacks keyed by correlation ID. */
  private pending = new Map<
    string,
    { resolve: (data: unknown) => void; reject: (err: Error) => void }
  >();

  /** Active WebSocket subscription handlers keyed by path. */
  private subscriptions = new Map<string, Set<(msg: unknown) => void>>();

  /** Reference to the global message listener so we can remove it. */
  private messageListener: ((ev: MessageEvent) => void) | null = null;

  get connected(): Readable<boolean> {
    return this._connected;
  }

  get error(): Readable<string | null> {
    return this._error;
  }

  constructor(vscode: VsCodeApi, options?: { timeoutMs?: number }) {
    this.vscode = vscode;
    this.timeoutMs = options?.timeoutMs ?? DEFAULT_TIMEOUT_MS;
  }

  // -- ApiTransport implementation ------------------------------------------

  async connect(): Promise<void> {
    // Install the global message listener.
    if (!this.messageListener) {
      this.messageListener = (ev: MessageEvent) => {
        this.handleMessage(ev.data as InboundMessage);
      };
      globalThis.addEventListener('message', this.messageListener);
    }

    // Ask the extension host to connect and do a health probe.
    try {
      await this.get('/v1/health');
      this._connected.set(true);
      this._error.set(null);
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err);
      this._error.set(msg);
      this._connected.set(false);
      throw err;
    }
  }

  disconnect(): void {
    this._connected.set(false);

    // Clean up all pending requests.
    for (const [, { reject }] of this.pending) {
      reject(new Error('Transport disconnected'));
    }
    this.pending.clear();

    // Unsubscribe all WebSocket paths.
    for (const [path] of this.subscriptions) {
      const id = crypto.randomUUID();
      const msg: OutboundUnsubscribe = { type: 'ws-unsubscribe', id, path };
      this.vscode.postMessage(msg);
    }
    this.subscriptions.clear();

    // Remove the message listener.
    if (this.messageListener) {
      globalThis.removeEventListener('message', this.messageListener);
      this.messageListener = null;
    }
  }

  async get<T>(path: string, params?: Record<string, string>): Promise<T> {
    return this.request<T>('GET', path, params);
  }

  async patch<T>(path: string, body: object): Promise<T> {
    return this.request<T>('PATCH', path, undefined, body);
  }

  async post<T>(path: string, body: object): Promise<T> {
    return this.request<T>('POST', path, undefined, body);
  }

  subscribe(path: string, handler: (msg: unknown) => void): Unsubscribe {
    let handlers = this.subscriptions.get(path);
    const isNew = !handlers;

    if (!handlers) {
      handlers = new Set();
      this.subscriptions.set(path, handlers);
    }
    handlers.add(handler);

    // Only send the subscribe message for the first handler on this path.
    if (isNew) {
      const id = crypto.randomUUID();
      const msg: OutboundSubscribe = { type: 'ws-subscribe', id, path };
      this.vscode.postMessage(msg);
    }

    return () => {
      handlers!.delete(handler);
      if (handlers!.size === 0) {
        this.subscriptions.delete(path);
        const id = crypto.randomUUID();
        const msg: OutboundUnsubscribe = { type: 'ws-unsubscribe', id, path };
        this.vscode.postMessage(msg);
      }
    };
  }

  // -- Internal helpers -----------------------------------------------------

  private request<T>(
    method: 'GET' | 'PATCH' | 'POST',
    path: string,
    params?: Record<string, string>,
    body?: object,
  ): Promise<T> {
    const id = crypto.randomUUID();

    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`Request timed out after ${this.timeoutMs}ms: ${method} ${path}`));
      }, this.timeoutMs);

      this.pending.set(id, {
        resolve: (data: unknown) => {
          clearTimeout(timer);
          resolve(data as T);
        },
        reject: (err: Error) => {
          clearTimeout(timer);
          reject(err);
        },
      });

      const msg: OutboundRequest = { type: 'api-request', id, method, path };
      if (params) msg.params = params;
      if (body) msg.body = body;
      this.vscode.postMessage(msg);
    });
  }

  private handleMessage(msg: InboundMessage): void {
    if (!msg || typeof msg !== 'object' || !('type' in msg)) return;

    switch (msg.type) {
      case 'api-response': {
        const entry = this.pending.get(msg.id);
        if (!entry) return;
        this.pending.delete(msg.id);
        if (msg.ok) {
          entry.resolve(msg.data);
        } else {
          entry.reject(new Error(msg.error ?? 'Unknown error'));
        }
        break;
      }

      case 'ws-message': {
        const handlers = this.subscriptions.get(msg.path);
        if (handlers) {
          for (const h of handlers) {
            h(msg.data);
          }
        }
        break;
      }

      case 'connection-status': {
        this._connected.set(msg.connected);
        this._error.set(msg.error ?? null);
        break;
      }
    }
  }
}
