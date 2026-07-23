import { describe, it, expect } from "vitest";
import { resolveCanManageLicense } from "./license-utils";

describe("resolveCanManageLicense", () => {
  it("trusts the backend is_global flag when present (custom global admin path)", () => {
    // Operator serves the global admin console at a renamed path, so the
    // URL heuristic reports false — the authoritative backend flag must win.
    expect(resolveCanManageLicense({ is_global: true }, false)).toBe(true);
  });

  it("hides management when the backend reports is_global: false", () => {
    // Per-server admin: backend flag says no, URL heuristic happens to say yes.
    expect(resolveCanManageLicense({ is_global: false }, true)).toBe(false);
  });

  it("falls back to the URL heuristic when is_global is absent (older backend)", () => {
    expect(resolveCanManageLicense({}, true)).toBe(true);
    expect(resolveCanManageLicense({}, false)).toBe(false);
  });

  it("falls back to the URL heuristic while status is loading (null/undefined)", () => {
    expect(resolveCanManageLicense(null, true)).toBe(true);
    expect(resolveCanManageLicense(undefined, false)).toBe(false);
  });
});
