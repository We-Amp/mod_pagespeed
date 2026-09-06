// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from "vitest";
import {
  NEUTRAL_STAT_DESCRIPTION,
  STAT_DESCRIPTIONS,
  STAT_FAMILY_DESCRIPTIONS,
  describeStat,
  statDescriptionSource,
} from "./stat-descriptions";
import statNames from "../../../test-fixtures/stat-names.fixture.json";

describe("describeStat", () => {
  it("prefers an exact entry", () => {
    expect(describeStat("ipro_served")).toBe(STAT_DESCRIPTIONS["ipro_served"]);
    expect(statDescriptionSource("ipro_served")).toBe("exact");
  });

  it("falls back to the family for an unlisted member of a known family", () => {
    // Not an exact entry, but the recorder family covers it.
    const description = describeStat("ipro_recorder_brand_new_outcome");
    expect(description).toBe(familyText("ipro_recorder_"));
    expect(statDescriptionSource("ipro_recorder_brand_new_outcome")).toBe(
      "family",
    );
  });

  it("prefers the most specific family when families nest", () => {
    // Both "ipro_" and "ipro_recorder_" match; the longer prefix must win.
    expect(describeStat("ipro_recorder_brand_new_outcome")).not.toBe(
      familyText("ipro_"),
    );
    expect(describeStat("ipro_brand_new_outcome")).toBe(familyText("ipro_"));
  });

  it("falls back to a neutral description for an unknown counter", () => {
    expect(describeStat("totally_made_up_counter")).toBe(
      NEUTRAL_STAT_DESCRIPTION,
    );
    expect(statDescriptionSource("totally_made_up_counter")).toBe("none");
  });

  it("does not mistake an inherited object property for an entry", () => {
    // A counter named after something on Object.prototype must still get the
    // neutral text, not a function rendered as source.
    for (const name of ["toString", "constructor", "valueOf", "__proto__"]) {
      expect(describeStat(name), name).toBe(NEUTRAL_STAT_DESCRIPTION);
      expect(statDescriptionSource(name), name).toBe("none");
    }
  });

  it("never returns an empty description", () => {
    expect(describeStat("")).toBe(NEUTRAL_STAT_DESCRIPTION);
    expect(describeStat("x").length).toBeGreaterThan(0);
  });
});

describe("the description table", () => {
  it("has no empty or over-long entries", () => {
    for (const [name, text] of Object.entries(STAT_DESCRIPTIONS)) {
      expect(text.trim().length, `${name} is empty`).toBeGreaterThan(0);
      // Long enough to be a sentence, short enough for a tooltip and a
      // table cell.
      expect(text.length, `${name} is too long: ${text.length} chars`).toBeLessThanOrEqual(
        260,
      );
    }
    for (const [prefix, text] of STAT_FAMILY_DESCRIPTIONS) {
      expect(text.trim().length, `${prefix} is empty`).toBeGreaterThan(0);
      expect(text.length, `${prefix} is too long`).toBeLessThanOrEqual(320);
    }
  });

  it("writes prose, not counter names", () => {
    // Descriptions are searched alongside the names. An underscore in the
    // prose would be a counter name leaking in, which makes a name search
    // match unrelated rows.
    const offenders: string[] = [];
    for (const [name, text] of Object.entries(STAT_DESCRIPTIONS)) {
      if (text.includes("_")) offenders.push(name);
    }
    for (const [prefix, text] of STAT_FAMILY_DESCRIPTIONS) {
      if (text.includes("_")) offenders.push(prefix);
    }
    // The fallback is shown and searched like any other description.
    if (NEUTRAL_STAT_DESCRIPTION.includes("_")) {
      offenders.push("NEUTRAL_STAT_DESCRIPTION");
    }
    expect(offenders).toEqual([]);
  });

  it("lists every family prefix only once", () => {
    const prefixes = STAT_FAMILY_DESCRIPTIONS.map(([prefix]) => prefix);
    expect(prefixes).toEqual([...new Set(prefixes)]);
  });
});

describe("coverage of the counters a running server emits", () => {
  // test-fixtures/stat-names.fixture.json is captured from a running server.
  // Regenerate it with scripts/refresh-stat-fixture.sh when counters are added
  // or removed. It lives outside src/ deliberately: the console source hash
  // covers src/, so keeping the fixture out of it means refreshing the data
  // does not force a bundle rebuild.
  it("describes every counter in the captured statistics dump", () => {
    const misses = (statNames as string[]).filter(
      (name) => statDescriptionSource(name) === "none",
    );
    expect(
      misses,
      `${misses.length} counter(s) from the captured statistics dump have no ` +
        `description. Add an exact entry, or a family prefix that covers ` +
        `them, to stat-descriptions.ts:\n  ${misses.join("\n  ")}`,
    ).toEqual([]);
  });

  it("captured a realistic dump, not a stub", () => {
    expect((statNames as string[]).length).toBeGreaterThan(200);
  });
});

function familyText(prefix: string): string {
  const entry = STAT_FAMILY_DESCRIPTIONS.find(([p]) => p === prefix);
  if (!entry) throw new Error(`no family entry for ${prefix}`);
  return entry[1];
}
