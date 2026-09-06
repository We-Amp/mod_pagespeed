// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from "vitest";
import { ApiError } from "$lib/api/client";
import {
  counterRows,
  fieldValue,
  formatBytes,
  formatUptime,
  isDaemonUnavailable,
  normalizeCooldowns,
  objectEntries,
} from "./daemon";

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

describe("formatUptime", () => {
  it("renders days, hours and minutes", () => {
    expect(formatUptime(2 * 86400 + 5 * 3600 + 3 * 60 + 7)).toBe("2d 5h 3m");
  });

  it("renders hours and minutes below a day", () => {
    expect(formatUptime(3 * 3600 + 41 * 60)).toBe("3h 41m");
  });

  it("renders minutes and seconds below an hour", () => {
    expect(formatUptime(60 + 9)).toBe("1m 9s");
  });

  it("renders bare seconds below a minute", () => {
    expect(formatUptime(12)).toBe("12s");
    expect(formatUptime(0)).toBe("0s");
  });

  it("renders a dash for absent or unusable input", () => {
    expect(formatUptime(undefined)).toBe("\u2014");
    expect(formatUptime(NaN)).toBe("\u2014");
    expect(formatUptime(-5)).toBe("\u2014");
  });
});

describe("formatBytes", () => {
  it("renders small sizes in bytes", () => {
    expect(formatBytes(512)).toBe("512 B");
  });

  it("scales to the largest whole unit", () => {
    expect(formatBytes(2048)).toBe("2.0 KB");
    expect(formatBytes(5 * 1024 * 1024)).toBe("5.0 MB");
  });

  it("renders a dash for absent or unusable input", () => {
    expect(formatBytes(undefined)).toBe("\u2014");
    expect(formatBytes(NaN)).toBe("\u2014");
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
