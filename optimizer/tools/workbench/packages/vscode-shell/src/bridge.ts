// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import * as vscode from 'vscode';

// ---------------------------------------------------------------------------
// Message types (mirroring the VsCodeTransport protocol in @pagespeed/api-client)
// ---------------------------------------------------------------------------

/** Inbound message from the webview to the extension host. */
interface ApiRequestMessage {
  type: 'api-request';
  id: string;
  method: 'GET' | 'PATCH' | 'POST';
  path: string;
  params?: Record<string, string>;
  body?: object;
}

interface WsSubscribeMessage {
  type: 'ws-subscribe';
  id: string;
  path: string;
}

interface WsUnsubscribeMessage {
  type: 'ws-unsubscribe';
  id: string;
  path: string;
}

type BridgeMessage =
  | ApiRequestMessage
  | WsSubscribeMessage
  | WsUnsubscribeMessage;

/** Outbound message from the extension host back to the webview. */
interface ApiResponseMessage {
  type: 'api-response';
  id: string;
  ok: boolean;
  data?: unknown;
  error?: string;
}

interface WsDataMessage {
  type: 'ws-message';
  path: string;
  data: unknown;
}

// ---------------------------------------------------------------------------
// WebviewBridge
// ---------------------------------------------------------------------------

/**
 * Bridges postMessage communication between a VS Code Webview and the
 * PageSpeed worker management API.  All HTTP requests and WebSocket
 * subscriptions are proxied through the extension host so the webview
 * never makes direct network calls.
 */
export class WebviewBridge implements vscode.Disposable {
  private readonly disposables: vscode.Disposable[] = [];
  private readonly wsConnections = new Map<string, WebSocket>();

  constructor(
    private readonly webview: vscode.Webview,
    private readonly apiUrl: string,
    private readonly getToken: () => Promise<string | undefined>,
  ) {
    const listener = webview.onDidReceiveMessage(
      (msg: unknown) => this.handleMessage(msg as BridgeMessage),
    );
    this.disposables.push(listener);
  }

  // -- Message dispatch -----------------------------------------------------

  private async handleMessage(msg: BridgeMessage): Promise<void> {
    if (!msg || typeof msg !== 'object' || !('type' in msg)) return;

    switch (msg.type) {
      case 'api-request':
        await this.handleApiRequest(msg);
        break;
      case 'ws-subscribe':
        this.handleWsSubscribe(msg);
        break;
      case 'ws-unsubscribe':
        this.handleWsUnsubscribe(msg);
        break;
    }
  }

  // -- HTTP proxying --------------------------------------------------------

  private async handleApiRequest(msg: ApiRequestMessage): Promise<void> {
    try {
      const url = this.buildUrl(msg.path, msg.params);
      const token = await this.getToken();

      const headers: Record<string, string> = {};
      if (token) {
        headers['Authorization'] = `Bearer ${token}`;
      }

      const init: RequestInit = { method: msg.method, headers };

      if (msg.body && (msg.method === 'PATCH' || msg.method === 'POST')) {
        headers['Content-Type'] = 'application/json';
        // CSRF: required by the worker on mutating /v1/* requests; set on
        // every POST/PATCH to match the SPA's DirectTransport convention.
        headers['X-Requested-With'] = 'XMLHttpRequest';
        init.body = JSON.stringify(msg.body);
      }

      const res = await fetch(url, init);
      const data: unknown = await res.json();

      const response: ApiResponseMessage = {
        type: 'api-response',
        id: msg.id,
        ok: res.ok,
      };

      if (res.ok) {
        response.data = data;
      } else {
        const errBody = data as { error?: { message?: string } };
        response.error =
          errBody?.error?.message ?? `HTTP ${res.status} ${res.statusText}`;
      }

      void this.webview.postMessage(response);
    } catch (err) {
      const response: ApiResponseMessage = {
        type: 'api-response',
        id: msg.id,
        ok: false,
        error: err instanceof Error ? err.message : String(err),
      };
      void this.webview.postMessage(response);
    }
  }

  // -- WebSocket proxying ---------------------------------------------------

  private handleWsSubscribe(msg: WsSubscribeMessage): void {
    if (this.wsConnections.has(msg.path)) return;

    const wsUrl = this.buildWsUrl(msg.path);
    const ws = new WebSocket(wsUrl);

    ws.addEventListener('open', () => {
      void this.getToken().then((token) => {
        if (token && ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({ auth: token }));
        }
      });
    });

    ws.addEventListener('message', (ev: MessageEvent) => {
      try {
        const data: unknown = JSON.parse(String(ev.data));
        const outbound: WsDataMessage = {
          type: 'ws-message',
          path: msg.path,
          data,
        };
        void this.webview.postMessage(outbound);
      } catch {
        // Ignore unparseable frames.
      }
    });

    ws.addEventListener('close', () => {
      this.wsConnections.delete(msg.path);
    });

    ws.addEventListener('error', () => {
      // The close event will follow; clean-up happens there.
    });

    this.wsConnections.set(msg.path, ws);
  }

  private handleWsUnsubscribe(msg: WsUnsubscribeMessage): void {
    const ws = this.wsConnections.get(msg.path);
    if (ws) {
      ws.close(1000, 'unsubscribed');
      this.wsConnections.delete(msg.path);
    }
  }

  // -- Helpers --------------------------------------------------------------

  private buildUrl(
    path: string,
    params?: Record<string, string>,
  ): string {
    // Prevent absolute URL injection — path must be relative
    if (!path.startsWith('/') || path.includes('://')) {
      throw new Error(`Invalid API path: ${path}`);
    }
    const url = new URL(path, this.apiUrl);
    if (params) {
      for (const [k, v] of Object.entries(params)) {
        url.searchParams.set(k, v);
      }
    }
    return url.toString();
  }

  private buildWsUrl(path: string): string {
    // Prevent absolute URL injection — path must be relative
    if (!path.startsWith('/') || path.includes('://')) {
      throw new Error(`Invalid WebSocket path: ${path}`);
    }
    const wsProto = this.apiUrl.startsWith('https') ? 'wss' : 'ws';
    const hostAndPath = this.apiUrl.replace(/^https?:\/\//, '');
    return `${wsProto}://${hostAndPath}${path}`;
  }

  // -- Disposal -------------------------------------------------------------

  dispose(): void {
    for (const [, ws] of this.wsConnections) {
      ws.close(1000, 'bridge disposed');
    }
    this.wsConnections.clear();
    for (const d of this.disposables) {
      d.dispose();
    }
    this.disposables.length = 0;
  }
}
