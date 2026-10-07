// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Kept in its own file: before the fix these inputs never return, which
// hangs the whole test file instead of failing one case.

import { describe, expect, it } from "vitest";
import { parseCacheCohorts, parseCacheSummary } from "./cache-summary";

describe("the cache summary parser always ends", () => {
  it.each(["A((", "A(,(", "A(b=(", "(", ")))", "A(b(c", "A(=)", "A(b=,c=(", "A(" + "(".repeat(200)])(
    "on %j",
    (input) => {
      expect(() => parseCacheSummary(input)).not.toThrow();
      expect(() => parseCacheCohorts(`x:${input}\ny:${input}`)).not.toThrow();
    },
  );
});
