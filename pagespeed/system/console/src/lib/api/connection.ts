// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Whether the console can reach the server at all. Fed by AdminApiClient on
 * every request; read by the reconnect banner and the top bar. The daemon
 * proxy's own failure codes are panel states (the optimizer is down, the
 * server is not), so they never count here.
 */

export type ConnectionStatus = "connected" | "reconnecting";

export interface ConnectionView {
  status: ConnectionStatus;
  /** Consecutive requests without an answer from the server. */
  failures: number;
  /** Epoch ms of the last request the server answered; null before the first. */
  lastAnsweredAt: number | null;
}

/** A request that got no HTTP answer (the server or the network is down). */
export class NetworkError extends Error {
  constructor() {
    super("cannot reach the server");
    this.name = "NetworkError";
    Object.setPrototypeOf(this, NetworkError.prototype);
  }
}

/**
 * `status` null: no answer at all. A 502/503/504 on a module leaf comes from
 * a gateway in front of the module; on a v1/daemon leaf it is the proxy
 * describing the optimizer.
 */
export function isConnectionFailure(path: string, status: number | null): boolean {
  if (status === null) return true;
  if (path.startsWith("/v1/daemon/")) return false;
  return status === 502 || status === 503 || status === 504;
}

/** The proxy's own reason codes for a `/v1/daemon/` leaf's error body. */
const DAEMON_REASON_CODES = new Set([
  "daemon_unreachable",
  "daemon_not_configured",
  "endpoint_unsupported_by_daemon",
  "response_too_large",
]);

/**
 * The proxy's reason code from a `/v1/daemon/` error body, or null when the
 * body does not carry one -- e.g. a gateway in front of the whole server
 * answering for every leaf, module and daemon alike, with a body of its own.
 */
function daemonReasonCode(body: string | undefined): string | null {
  if (body === undefined) return null;
  try {
    const stripped = body.replace(/^\)\]\}'?\s*\n/, "");
    const parsed = JSON.parse(stripped) as { error?: unknown };
    return typeof parsed.error === "string" && DAEMON_REASON_CODES.has(parsed.error) ? parsed.error : null;
  } catch {
    return null;
  }
}

const INITIAL: ConnectionView = { status: "connected", failures: 0, lastAnsweredAt: null };

export class ConnectionMonitor {
  private current: ConnectionView = INITIAL;
  private readonly listeners = new Set<(v: ConnectionView) => void>();

  get view(): ConnectionView {
    return this.current;
  }

  /**
   * `body` is the leaf's own response body (when read). A 5xx on a
   * `/v1/daemon/` leaf whose body does not name one of the proxy's reason
   * codes is neutral: it is a gateway in front of the whole server, not the
   * module's own proxy describing the optimizer, so it is neither a failure
   * nor an answer -- recording either would flicker the banner and the top
   * bar on every sample of an outage that a gateway is fronting.
   */
  record(path: string, status: number | null, now: number, body?: string): void {
    const prev = this.current;
    if (status !== null && status >= 500 && status <= 599 && path.startsWith("/v1/daemon/") && daemonReasonCode(body) === null) {
      return;
    }
    if (isConnectionFailure(path, status)) {
      this.current = { ...prev, status: "reconnecting", failures: prev.failures + 1 };
    } else {
      this.current = { status: "connected", failures: 0, lastAnsweredAt: now };
    }
    if (this.current.status !== prev.status || this.current.failures !== prev.failures) {
      for (const fn of this.listeners) fn(this.current);
    }
  }

  /** Calls `fn` with the current view at once, then on every status or failure-count change. */
  subscribe(fn: (v: ConnectionView) => void): () => void {
    this.listeners.add(fn);
    fn(this.current);
    return () => {
      this.listeners.delete(fn);
    };
  }

  reset(): void {
    this.current = INITIAL;
  }
}

export const connection = new ConnectionMonitor();
