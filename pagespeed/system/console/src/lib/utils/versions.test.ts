// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it, vi } from "vitest";
import { ApiError } from "$lib/api/client";
import {
  isDirtyBuild,
  moduleTag,
  optimizerVersions,
  optimizerVersionsOnce,
  optimizerVersionsStatus,
  optimizerVersionText,
} from "./versions";

describe("moduleTag", () => {
  it("takes the leading tag from a describe stamp", () => {
    expect(moduleTag("v1.16.0-86-g0cdb738df-dirty")).toBe("v1.16.0");
    expect(moduleTag("v1.16.0-86-g0cdb738df")).toBe("v1.16.0");
    expect(moduleTag("v1.16.0")).toBe("v1.16.0");
  });
  it("falls back to the whole stamp outside a tag", () => {
    expect(moduleTag("680aa4709")).toBe("680aa4709");
    expect(moduleTag("dev")).toBe("dev");
  });
});

describe("isDirtyBuild", () => {
  it("spots the dirty suffix only", () => {
    expect(isDirtyBuild("v1.16.0-86-g0cdb738df-dirty")).toBe(true);
    expect(isDirtyBuild("v1.16.0-dirty")).toBe(true);
    expect(isDirtyBuild("v1.16.0-86-g0cdb738df")).toBe(false);
    expect(isDirtyBuild("dev")).toBe(false);
  });
});

describe("optimizerVersions", () => {
  it("reads both fields from a health answer", () => {
    expect(
      optimizerVersions({ version: "2.1.0", git_commit: "affafd3" }),
    ).toEqual({ version: "2.1.0", commit: "affafd3" });
  });
  it("tolerates absent, empty and non-object answers", () => {
    expect(optimizerVersions({})).toEqual({ version: null, commit: null });
    expect(optimizerVersions({ version: "" })).toEqual({
      version: null,
      commit: null,
    });
    expect(optimizerVersions(null)).toEqual({ version: null, commit: null });
    expect(optimizerVersions("2.1.0")).toEqual({ version: null, commit: null });
  });
});

describe("optimizerVersionsOnce", () => {
  it("answers from the first read", async () => {
    const result = await optimizerVersionsOnce(async () => ({
      version: "2.1.0",
      git_commit: "affafd3",
    }));
    expect(result).toEqual({ version: "2.1.0", commit: "affafd3" });
  });
  it("retries once after a failure, then gives up as unknown", async () => {
    let calls = 0;
    const read = async () => {
      calls += 1;
      if (calls === 1) throw new Error("busy");
      return { version: "2.1.0" };
    };
    expect(await optimizerVersionsOnce(read, async () => {})).toEqual({
      version: "2.1.0",
      commit: null,
    });
    let failures = 0;
    const alwaysFails = async () => {
      failures += 1;
      throw new Error("down");
    };
    expect(await optimizerVersionsOnce(alwaysFails, async () => {})).toEqual({
      version: null,
      commit: null,
    });
    expect(failures).toBe(2);
  });
});

describe("optimizerVersionsStatus", () => {
  it("a real version and commit are shown at once, with no retry", async () => {
    const wait = vi.fn().mockResolvedValue(undefined);
    const status = await optimizerVersionsStatus(
      async () => ({ version: "2.0.41", git_commit: "52344ff" }),
      wait,
    );
    expect(status).toEqual({ version: "2.0.41", commit: "52344ff", unavailable: null });
    expect(wait).not.toHaveBeenCalled();
  });

  it("a non-object or version-less answer is running without a version", async () => {
    expect(await optimizerVersionsStatus(async () => null)).toEqual({
      version: null,
      commit: null,
      unavailable: null,
    });
    expect((await optimizerVersionsStatus(async () => ({ version: { major: 2 } }))).version).toBeNull();
  });

  it("a 429 retries once and shows what the retry finds", async () => {
    let calls = 0;
    const wait = vi.fn().mockResolvedValue(undefined);
    const status = await optimizerVersionsStatus(async () => {
      calls++;
      if (calls === 1) throw new ApiError(429, "busy");
      return { version: "2.0.41" };
    }, wait);
    expect(status).toEqual({ version: "2.0.41", commit: null, unavailable: null });
    expect(calls).toBe(2);
    expect(wait).toHaveBeenCalledTimes(1);
  });

  it("a busy or unreachable network twice says so, not 'Checking'", async () => {
    const wait = vi.fn().mockResolvedValue(undefined);
    const busy = await optimizerVersionsStatus(async () => {
      throw new ApiError(429, "busy");
    }, wait);
    expect(busy.unavailable).toBe("Not available right now");
    const network = await optimizerVersionsStatus(async () => {
      throw new Error("network error");
    }, wait);
    expect(network.unavailable).toBe("Not available right now");
  });

  it("a non-transient failure (no optimizer configured) is reported at once, with no retry", async () => {
    const wait = vi.fn().mockResolvedValue(undefined);
    const status = await optimizerVersionsStatus(async () => {
      throw new ApiError(404, "Unknown admin page: v1/daemon/health");
    }, wait);
    expect(status.unavailable).toBe("Not configured");
    expect(wait).not.toHaveBeenCalled();
  });
});

describe("optimizerVersionText", () => {
  it("reads checking, a version with its commit, running without one, or the reason", () => {
    expect(optimizerVersionText(null)).toBe("Checking…");
    expect(optimizerVersionText({ version: "2.0.41", commit: "52344ff", unavailable: null })).toBe(
      "2.0.41 (52344ff)",
    );
    expect(optimizerVersionText({ version: "2.0.41", commit: null, unavailable: null })).toBe("2.0.41");
    expect(optimizerVersionText({ version: null, commit: null, unavailable: null })).toBe(
      "Running (version not reported)",
    );
    expect(optimizerVersionText({ version: null, commit: null, unavailable: "Not configured" })).toBe(
      "Not configured",
    );
  });
});
