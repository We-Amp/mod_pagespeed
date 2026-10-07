// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { ConnectionMonitor, NetworkError, isConnectionFailure, type ConnectionView } from "./connection";

describe("isConnectionFailure", () => {
  it("no answer at all is a lost connection, on any path", () => {
    expect(isConnectionFailure("/stats_json", null)).toBe(true);
    expect(isConnectionFailure("/v1/daemon/health", null)).toBe(true);
  });
  it("a gateway error on a module leaf is a lost connection", () => {
    for (const status of [502, 503, 504]) expect(isConnectionFailure("/stats_json", status)).toBe(true);
  });
  it("any other answer means the server is there", () => {
    for (const status of [200, 404, 429, 500, 501]) expect(isConnectionFailure("/stats_json", status)).toBe(false);
  });
  it("the daemon proxy's own 5xx codes are panel states, not a lost connection", () => {
    for (const status of [501, 502, 503, 504]) expect(isConnectionFailure("/v1/daemon/stats", status)).toBe(false);
  });
});

describe("NetworkError", () => {
  it("says the server cannot be reached and is an Error", () => {
    const e = new NetworkError();
    expect(e).toBeInstanceOf(Error);
    expect(e).toBeInstanceOf(NetworkError);
    expect(e.message).toBe("cannot reach the server");
  });
});

describe("ConnectionMonitor", () => {
  it("starts connected", () => {
    expect(new ConnectionMonitor().view).toEqual({ status: "connected", failures: 0, lastAnsweredAt: null });
  });

  it("the first request without an answer flips to reconnecting; an answer flips back", () => {
    const m = new ConnectionMonitor();
    m.record("/stats_json", 200, 1000);
    m.record("/stats_json", null, 2000);
    expect(m.view).toEqual({ status: "reconnecting", failures: 1, lastAnsweredAt: 1000 });
    m.record("/v1/daemon/health", null, 3000);
    expect(m.view.failures).toBe(2);
    m.record("/stats_json", 500, 4000);
    expect(m.view).toEqual({ status: "connected", failures: 0, lastAnsweredAt: 4000 });
  });

  it("a daemon proxy 502 leaves a connected console connected", () => {
    const m = new ConnectionMonitor();
    m.record("/v1/daemon/stats", 502, 1000);
    expect(m.view.status).toBe("connected");
  });

  it("a daemon-leaf 5xx naming the proxy's own reason code answers as the proxy", () => {
    const m = new ConnectionMonitor();
    m.record("/v1/daemon/health", null, 500); // start reconnecting
    m.record("/v1/daemon/stats", 502, 2000, JSON.stringify({ error: "daemon_unreachable" }));
    expect(m.view).toEqual({ status: "connected", failures: 0, lastAnsweredAt: 2000 });
  });

  it("the logs leaf's over-cap answer is the proxy's own reason code too", () => {
    const m = new ConnectionMonitor();
    m.record("/v1/daemon/health", null, 500); // start reconnecting
    m.record("/v1/daemon/logs", 502, 2000, JSON.stringify({ error: "response_too_large" }));
    expect(m.view).toEqual({ status: "connected", failures: 0, lastAnsweredAt: 2000 });
  });

  it("a daemon-leaf 5xx with no reason code (a gateway fronting the whole server) is neutral", () => {
    const m = new ConnectionMonitor();
    m.record("/stats_json", null, 1000); // reconnecting, failures: 1
    const before = m.view;
    m.record("/v1/daemon/stats", 504, 2000, JSON.stringify({ error: "gateway timeout" }));
    expect(m.view).toEqual(before);
    m.record("/v1/daemon/health", 502, 3000); // no body at all
    expect(m.view).toEqual(before);
  });

  it("a daemon-leaf 5xx that is neutral notifies no listener", () => {
    const m = new ConnectionMonitor();
    const seen: ConnectionView[] = [];
    m.subscribe((v) => seen.push(v));
    m.record("/v1/daemon/stats", 503, 1000, JSON.stringify({ error: "not the proxy" }));
    expect(seen).toHaveLength(1); // only the subscribe-time callback
  });

  it("listeners get the current view at once, then only status or failure changes", () => {
    const m = new ConnectionMonitor();
    const seen: ConnectionView[] = [];
    const off = m.subscribe((v) => seen.push(v));
    expect(seen).toHaveLength(1);
    m.record("/stats_json", 200, 1000);
    m.record("/stats_json", 200, 2000);
    expect(seen).toHaveLength(1);
    m.record("/stats_json", null, 3000);
    expect(seen).toHaveLength(2);
    expect(seen[1].status).toBe("reconnecting");
    off();
    m.record("/stats_json", 200, 4000);
    expect(seen).toHaveLength(2);
  });

  it("reset() returns to the initial view", () => {
    const m = new ConnectionMonitor();
    m.record("/stats_json", null, 1000);
    m.reset();
    expect(m.view).toEqual({ status: "connected", failures: 0, lastAnsweredAt: null });
  });
});
