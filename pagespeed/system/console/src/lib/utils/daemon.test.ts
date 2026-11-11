// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from "vitest";
import { ApiError } from "$lib/api/client";
import { ALERT_RULES, type AlertSnapshot } from "$lib/alerts";
import { availabilityText } from "./overview";
import {
  OPTIMIZER_NOT_ANSWERING_FIX,
  optimizerAbsence,
  optimizerAbsenceText,
  checkLabel,
  checkResult,
  cooldownReasonLabel,
  counterRows,
  daemonUnavailableReason,
  errorCode,
  fieldValue,
  isDaemonUnavailable,
  normalizeCooldowns,
  objectEntries,
  optimizerUnavailableNote,
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

describe("daemonUnavailableReason", () => {
  it("a 503 the optimizer sent itself is a refresh without an answer, not 'unreachable'", () => {
    expect(daemonUnavailableReason(new ApiError(503, "Service Unavailable"))).toBe(
      "the optimizer did not answer this refresh",
    );
    expect(daemonUnavailableReason(new ApiError(503, '{"error":"busy"}'))).toBe(
      "the optimizer did not answer this refresh",
    );
  });

  it("keeps 'unreachable' for the proxy's 502 and for anything it cannot name", () => {
    expect(daemonUnavailableReason(new ApiError(502, "daemon_unreachable"))).toBe("unreachable");
    expect(daemonUnavailableReason(new ApiError(502, "Bad Gateway"))).toBe("unreachable");
    expect(daemonUnavailableReason(new ApiError(404, "Unknown admin page"))).toBe("unreachable");
    expect(daemonUnavailableReason(new Error("network down"))).toBe("unreachable");
  });
});

describe("optimizerAbsence", () => {
  const notConfigured = new ApiError(503, "daemon_not_configured");
  const unreachable = new ApiError(502, "daemon_unreachable");
  const outdated = new ApiError(501, "endpoint_unsupported_by_daemon");
  const noLeaf = new ApiError(404, "Unknown admin page: v1/daemon/stats");

  it("names one reason per status", () => {
    expect(optimizerAbsence(notConfigured, notConfigured)).toBe("not-configured");
    expect(optimizerAbsence(unreachable, unreachable)).toBe("not-answering");
    expect(optimizerAbsence(outdated, outdated)).toBe("optimizer-outdated");
    expect(optimizerAbsence(noLeaf, noLeaf)).toBe("module-unsupported");
  });

  it("reads the proxy's code from a raw JSON body too", () => {
    expect(optimizerAbsence(new ApiError(503, '{"error":"daemon_not_configured"}'))).toBe("not-configured");
  });

  it("a 503 the optimizer sent itself is a refresh without an answer, not a stopped optimizer", () => {
    const busy = new ApiError(503, "Service Unavailable");
    expect(optimizerAbsence(busy, busy)).toBe("did-not-answer");
    expect(optimizerAbsence(busy, unreachable)).toBe("not-answering");
    expect(optimizerAbsence(notConfigured, busy)).toBe("did-not-answer");
    expect(optimizerAbsence(noLeaf, busy)).toBe("did-not-answer");
  });

  it("leaves that disagree: the most telling answer wins, in either order", () => {
    expect(optimizerAbsence(noLeaf, unreachable)).toBe("not-answering");
    expect(optimizerAbsence(unreachable, noLeaf)).toBe("not-answering");
    expect(optimizerAbsence(noLeaf, notConfigured)).toBe("not-configured");
    expect(optimizerAbsence(outdated, noLeaf)).toBe("optimizer-outdated");
  });

  it("is null for anything that is not an 'unavailable' answer", () => {
    expect(optimizerAbsence()).toBeNull();
    expect(optimizerAbsence(null, null)).toBeNull();
    expect(optimizerAbsence(new ApiError(500, "boom"), new Error("network down"))).toBeNull();
    expect(optimizerAbsence(new ApiError(429, "busy"))).toBeNull();
  });

  it("agrees with isDaemonUnavailable on which statuses count", () => {
    for (const status of [400, 403, 404, 429, 500, 501, 502, 503, 504]) {
      const error = new ApiError(status, "x");
      expect(optimizerAbsence(error) !== null, String(status)).toBe(isDaemonUnavailable(error));
    }
  });
});

describe("optimizerAbsenceText", () => {
  const kinds = ["not-configured", "not-answering", "did-not-answer", "optimizer-outdated", "module-unsupported"] as const;

  it("titles the three reasons in words", () => {
    expect(optimizerAbsenceText("not-configured").title).toBe("No optimizer configured");
    expect(optimizerAbsenceText("not-answering").title).toBe("Optimizer not answering");
    expect(optimizerAbsenceText("did-not-answer").title).toBe("Optimizer did not answer");
    expect(optimizerAbsenceText("optimizer-outdated").title).toBe("This build has no optimizer endpoints");
    expect(optimizerAbsenceText("module-unsupported").title).toBe("This build has no optimizer endpoints");
  });

  it("never prints a transport status or a reason code", () => {
    for (const kind of kinds) {
      const { title, detail } = optimizerAbsenceText(kind);
      expect(`${title} ${detail}`).not.toMatch(/HTTP|\b\d{3}\b|daemon_|_unsupported/);
    }
  });

  it("links to a documentation page under the product site", () => {
    for (const kind of kinds) {
      const { doc } = optimizerAbsenceText(kind);
      if (kind === "did-not-answer") {
        expect(doc).toBeNull();
        continue;
      }
      expect(doc?.label).not.toBe("");
      expect(doc?.href).toMatch(/^https:\/\/[a-z.]+\/docs\/[a-z-]+\/#[a-z-]+$/);
    }
  });

  it("not configured says what the Overview card says", () => {
    expect(optimizerAbsenceText("not-configured").detail).toContain(availabilityText("not-configured").detail);
  });

  it("not answering carries the Overview finding's message, fix and documentation link", () => {
    const rule = ALERT_RULES.find((r) => r.id === "daemon-unreachable");
    expect(rule?.fix).toBe(OPTIMIZER_NOT_ANSWERING_FIX);
    const text = optimizerAbsenceText("not-answering");
    expect(text.detail).toContain(rule?.message({} as AlertSnapshot, null, []));
    expect(text.detail).toContain(OPTIMIZER_NOT_ANSWERING_FIX);
    expect(text.doc?.href).toBe(rule?.doc);
  });

  it("a refresh without an answer says what the Overview card says, and prescribes nothing", () => {
    const text = optimizerAbsenceText("did-not-answer");
    expect(text.detail).toBe(availabilityText("transient").detail);
    expect(text.detail).not.toContain(OPTIMIZER_NOT_ANSWERING_FIX);
  });

  it("a module without the endpoints says what the Overview card says", () => {
    expect(optimizerAbsenceText("module-unsupported").detail).toContain(availabilityText("not-configured").detail);
  });

  it("an outdated optimizer says what the Overview card says", () => {
    expect(optimizerAbsenceText("optimizer-outdated").detail).toBe(availabilityText("unsupported").detail);
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

describe("optimizerUnavailableNote", () => {
  const note = (status: number, body = "") => optimizerUnavailableNote(new ApiError(status, body));

  it("gives a title and a sentence that say the same thing", () => {
    expect(note(502, '{"error":"daemon_unreachable"}')).toEqual({
      title: "Optimizer not answering",
      detail: "The optimizer is configured but not answering. Its data appears here once it answers.",
    });
    // A 503 the optimizer sent itself: it is running, so nothing calls it unreachable.
    const busy = note(503, "busy");
    expect(busy.title).toBe("Optimizer did not answer");
    expect(busy.detail).toBe("The optimizer did not answer this refresh; the next refresh tries again.");
    expect(`${busy.title} ${busy.detail}`).not.toMatch(/unreachable/i);
  });

  it("tells a missing optimizer, an old one and an old module apart", () => {
    expect(note(503, '{"error":"daemon_not_configured"}').title).toBe("No optimizer configured");
    expect(note(501, '{"error":"endpoint_unsupported_by_daemon"}').detail).toMatch(/Update the optimizer package/);
    expect(note(404).detail).toBe("This web server's module does not serve the optimizer's data.");
  });

  it("reads anything else as not answering, and never prints the error", () => {
    for (const error of [null, new Error("boom"), new ApiError(500, "HTTP 500: stack trace")]) {
      const text = optimizerUnavailableNote(error);
      expect(text.title).toBe("Optimizer not answering");
      expect(text.detail).not.toMatch(/HTTP|boom|stack/);
    }
  });
});

describe("checkLabel", () => {
  it("names the checks the console knows in plain words", () => {
    expect(checkLabel("cache_configured")).toBe("Cache directory configured");
    expect(checkLabel("cache_open")).toBe("Cache opened");
  });

  it("reads an unknown id as words, and never as a property of the table", () => {
    expect(checkLabel("chrome_running")).toBe("Chrome running");
    expect(checkLabel("a")).toBe("A");
    expect(checkLabel("constructor")).toBe("Constructor");
    expect(checkLabel("__proto__")).toBe("Proto");
    expect(checkLabel("")).toBe("");
    expect(checkLabel("___")).toBe("___");
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
