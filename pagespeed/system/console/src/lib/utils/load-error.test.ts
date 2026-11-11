// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { ApiError } from "$lib/api/client";
import { NetworkError } from "$lib/api/connection";
import { describeLoadError, loadErrorKind, loadErrorStatus } from "./load-error";

describe("loadErrorStatus", () => {
  it("reads the status of an API error", () => {
    expect(loadErrorStatus(new ApiError(404, "Unknown admin page: rule_packs"))).toBe(404);
  });

  it("reads the status a message string names", () => {
    expect(loadErrorStatus("HTTP 503: Service Unavailable")).toBe(503);
    expect(loadErrorStatus(new Error("HTTP 403: Forbidden"))).toBe(403);
  });

  it("is null without a status", () => {
    expect(loadErrorStatus(new NetworkError())).toBeNull();
    expect(loadErrorStatus("something else")).toBeNull();
    expect(loadErrorStatus("HTTP 4040: no")).toBeNull();
    expect(loadErrorStatus(null)).toBeNull();
    expect(loadErrorStatus(undefined)).toBeNull();
  });
});

describe("loadErrorKind", () => {
  it.each([
    [404, "not-found"],
    [401, "unauthorized"],
    [403, "refused"],
    [400, "rejected"],
    [410, "rejected"],
    [429, "rejected"],
    [500, "server"],
    [502, "server"],
    [503, "server"],
    [599, "server"],
    [200, "unreadable"],
    [302, "other"],
  ] as const)("maps HTTP %i to %s", (status, kind) => {
    expect(loadErrorKind(new ApiError(status, "x"))).toBe(kind);
  });

  it("maps a request without an answer", () => {
    expect(loadErrorKind(new NetworkError())).toBe("no-answer");
    // Config.svelte once kept only the message.
    expect(loadErrorKind("cannot reach the server")).toBe("no-answer");
  });

  it("maps anything else to other", () => {
    expect(loadErrorKind(new Error("Failed to fetch dynamically imported module"))).toBe("other");
    expect(loadErrorKind("")).toBe("other");
    expect(loadErrorKind(null)).toBe("other");
    expect(loadErrorKind({})).toBe("other");
  });
});

describe("describeLoadError", () => {
  it("titles the failure with the noun and keeps the transport message apart", () => {
    const text = describeLoadError(new ApiError(404, "Unknown admin page: rule_packs"), "the rule packs");
    expect(text.kind).toBe("not-found");
    expect(text.title).toBe("Could not load the rule packs.");
    expect(text.raw).toBe("HTTP 404: Unknown admin page: rule_packs");
    expect(text.cause).toContain("does not know this view");
  });

  it("accepts a plain string", () => {
    const text = describeLoadError("HTTP 500: Internal Server Error", "the configuration");
    expect(text.kind).toBe("server");
    expect(text.title).toBe("Could not load the configuration.");
    expect(text.raw).toBe("HTTP 500: Internal Server Error");
  });

  it("has no raw message when there is none", () => {
    expect(describeLoadError(null, "this page").raw).toBeNull();
    expect(describeLoadError("", "this page").raw).toBeNull();
    expect(describeLoadError(new Error(""), "this page").raw).toBeNull();
  });

  it("never puts a status code or the transport message in the title or the cause", () => {
    const errors: unknown[] = [
      new ApiError(401, "Unauthorized"),
      new ApiError(403, "Forbidden"),
      new ApiError(404, "Unknown admin page: stats_json"),
      new ApiError(429, "busy"),
      new ApiError(500, "statistics unavailable"),
      new ApiError(503, "Service Unavailable"),
      new ApiError(200, "Invalid JSON response: <html>"),
      new NetworkError(),
      "HTTP 404: Not Found",
      new Error("boom"),
    ];
    for (const error of errors) {
      const text = describeLoadError(error, "the statistics");
      for (const line of [text.title, text.cause]) {
        expect(line).not.toMatch(/HTTP|\d{3}/);
        if (text.raw !== null) expect(line).not.toContain(text.raw);
      }
      expect(text.cause.length).toBeGreaterThan(0);
    }
  });
});
