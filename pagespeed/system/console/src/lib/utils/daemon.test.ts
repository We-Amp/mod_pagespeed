// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from "vitest";
import { ApiError } from "$lib/api/client";
import {
  checkResult,
  cooldownReasonLabel,
  counterRows,
  daemonUnavailableReason,
  errorCode,
  fieldValue,
  isDaemonUnavailable,
  normalizeCooldowns,
  objectEntries,
  wholeServerConsoleOnlyError,
} from "./daemon";

describe("wholeServerConsoleOnlyError", () => {
  it("reads back as the module's own whole_server_console_only 403, with no network request involved", () => {
    const error = wholeServerConsoleOnlyError();
    expect(error).toBeInstanceOf(ApiError);
    expect((error as ApiError).status).toBe(403);
    expect(errorCode(error)).toBe("whole_server_console_only");
  });
});

describe("isDaemonUnavailable", () => {
  it("treats 502 and 404 as the daemon-unavailable empty state", () => {
    expect(isDaemonUnavailable(new ApiError(502, "Bad Gateway"))).toBe(true);
    expect(isDaemonUnavailable(new ApiError(404, "Unknown admin page"))).toBe(true);
  });

  it("treats other errors as real errors", () => {
    expect(isDaemonUnavailable(new ApiError(500, "oops"))).toBe(false);
    expect(isDaemonUnavailable(new Error("network down"))).toBe(false);
    expect(isDaemonUnavailable(null)).toBe(false);
  });

  it("treats 503 not-configured and 501 unsupported as unavailable with a reason", () => {
    expect(isDaemonUnavailable(new ApiError(503, "daemon_not_configured"))).toBe(true);
    expect(
      daemonUnavailableReason(new ApiError(503, "{\"error\":\"daemon_not_configured\"}")),
    ).toBe("not configured on this server");
    expect(
      daemonUnavailableReason(
        new ApiError(501, "{\"error\":\"endpoint_unsupported_by_daemon\"}"),
      ),
    ).toBe("this optimizer version does not provide this panel");
  });
});

describe("objectEntries", () => {
  it("returns entries for a plain object", () => {
    expect(objectEntries({ a: 1 })).toEqual([["a", 1]]);
  });

  it("degrades non-objects to no entries", () => {
    expect(objectEntries(undefined)).toEqual([]);
    expect(objectEntries(null)).toEqual([]);
    expect(objectEntries("ok")).toEqual([]);
    expect(objectEntries([1, 2])).toEqual([]);
  });
});

describe("fieldValue", () => {
  it("formats numbers with locale grouping", () => {
    expect(fieldValue(1234567)).toBe((1234567).toLocaleString());
  });

  it("renders booleans as yes/no", () => {
    expect(fieldValue(true)).toBe("yes");
    expect(fieldValue(false)).toBe("no");
  });

  it("passes strings through and dashes the empty/ absent ones", () => {
    expect(fieldValue("ok")).toBe("ok");
    expect(fieldValue("")).toBe("\u2014");
    expect(fieldValue(undefined)).toBe("\u2014");
    expect(fieldValue(null)).toBe("\u2014");
  });

  it("renders objects as compact inert JSON text", () => {
    expect(fieldValue({ ok: true })).toBe('{"ok":true}');
    expect(fieldValue("<script>alert(1)</script>")).toBe(
      "<script>alert(1)</script>",
    );
  });
});

describe("normalizeCooldowns", () => {
  it("passes a bare array through", () => {
    const list = [{ url: "https://a/", reason: "processing" }];
    expect(normalizeCooldowns(list)).toEqual(list);
  });

  it("unwraps an object carrying the list", () => {
    const list = [{ url: "https://a/", reason: "revalidation" }];
    expect(normalizeCooldowns({ cooldowns: list })).toEqual(list);
  });

  it("treats absent and malformed shapes as an empty list", () => {
    expect(normalizeCooldowns(null)).toEqual([]);
    expect(normalizeCooldowns(undefined)).toEqual([]);
    expect(normalizeCooldowns({})).toEqual([]);
    expect(normalizeCooldowns({ cooldowns: "nope" } as never)).toEqual([]);
  });

  it("drops non-object entries rather than throwing", () => {
    expect(normalizeCooldowns([null, { url: "https://a/" }, 42] as never)).toEqual([
      { url: "https://a/" },
    ]);
  });
});

describe("cooldownReasonLabel", () => {
  it("maps the daemon's reason codes to short plain-language labels", () => {
    expect(cooldownReasonLabel("processing")).toBe("Processing");
    expect(cooldownReasonLabel("write_failure")).toBe("Write failed");
    expect(cooldownReasonLabel("revalidation")).toBe("Revalidating");
  });

  it("has no label for an absent, empty, unrecognised or non-string reason", () => {
    expect(cooldownReasonLabel("unknown")).toBeNull();
    expect(cooldownReasonLabel("some_future_reason")).toBeNull();
    expect(cooldownReasonLabel("")).toBeNull();
    expect(cooldownReasonLabel(undefined)).toBeNull();
    expect(cooldownReasonLabel(null)).toBeNull();
    expect(cooldownReasonLabel({ code: "processing" })).toBeNull();
    expect(cooldownReasonLabel(42)).toBeNull();
  });
});

describe("counterRows", () => {
  it("flattens numeric leaves, dot-joining nested keys", () => {
    expect(
      counterRows({
        images: { rewrites: 10, dropped: 2 },
        bytes_saved: 4096,
      }),
    ).toEqual([
      { name: "images.rewrites", value: 10 },
      { name: "images.dropped", value: 2 },
      { name: "bytes_saved", value: 4096 },
    ]);
  });

  it("skips non-numeric leaves", () => {
    expect(counterRows({ note: "n/a", ok: 1 })).toEqual([{ name: "ok", value: 1 }]);
  });

  it("treats an absent block as no rows", () => {
    expect(counterRows(undefined)).toEqual([]);
  });
});

describe("checkResult", () => {
  it("reads the optimizer's {pass} objects as Pass or Fail", () => {
    expect(checkResult({ pass: true })).toEqual({ text: "Pass", pass: true });
    expect(checkResult({ pass: false })).toEqual({ text: "Fail", pass: false });
    expect(checkResult({ pass: false, detail: "cache volume not writable" })).toEqual({
      text: "Fail: cache volume not writable",
      pass: false,
    });
  });
  it("reads a bare boolean too", () => {
    expect(checkResult(true)).toEqual({ text: "Pass", pass: true });
    expect(checkResult(false)).toEqual({ text: "Fail", pass: false });
  });
  it("anything else is shown as a value, neither pass nor fail", () => {
    expect(checkResult("degraded")).toEqual({ text: "degraded", pass: null });
    expect(checkResult({ pass: "yes" })).toEqual({ text: '{"pass":"yes"}', pass: null });
    expect(checkResult(undefined)).toEqual({ text: "—", pass: null });
  });
});
