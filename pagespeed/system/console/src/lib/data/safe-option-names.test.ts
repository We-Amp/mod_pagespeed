// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { existsSync, readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { SAFE_OPTION_NAMES } from "./safe-option-names";

/** The module sources that register options, from the repository root. */
const SOURCES = [
  "net/instaweb/rewriter/rewrite_options.cc",
  "pagespeed/apache/apache_config.cc",
  "pagespeed/envoy/envoy_rewrite_options.cc",
  "pagespeed/nginx/ngx_rewrite_options.cc",
  "pagespeed/system/system_rewrite_options.cc",
];
const ROOT = new URL("../../../../../../", import.meta.url);
const paths = SOURCES.map((s) => new URL(s, ROOT));
const present = paths.every((p) => existsSync(p));

/** Every `kName[] = "Value"` constant in the option sources. */
function constants(): Map<string, string> {
  const out = new Map<string, string>();
  for (const p of paths) {
    for (const m of readFileSync(p, "utf8").matchAll(/\b(k[A-Z]\w*)\[\]\s*=\s*"([^"]*)"/g)) out.set(m[1], m[2]);
  }
  return out;
}

describe.skipIf(!present)("SAFE_OPTION_NAMES against the module's option sources", () => {
  it("names only options the module defines", () => {
    const defined = new Set(constants().values());
    for (const n of SAFE_OPTION_NAMES) expect(defined.has(n), n).toBe(true);
  });

  it("leaves out every option the module marks secret", () => {
    const k = constants();
    const secret: string[] = [];
    for (const p of paths) {
      for (const m of readFileSync(p, "utf8").matchAll(/MarkOptionSecret\([^,]+,\s*(?:\w+::)?(k[A-Z]\w*)\)/g)) {
        const name = k.get(m[1]);
        expect(name, m[1]).toBeDefined();
        secret.push(name as string);
      }
    }
    expect(secret.length).toBeGreaterThanOrEqual(7);
    for (const n of secret) expect(SAFE_OPTION_NAMES.has(n), n).toBe(false);
  });
});

describe("SAFE_OPTION_NAMES", () => {
  it("leaves out the options whose value can hold a credential or a private server's address", () => {
    for (const n of ["ProxyAuth", "RemoteConfigurationUrl", "MemcachedServers", "RedisServer", "TestProxy", "TestProxySlurp"]) {
      expect(SAFE_OPTION_NAMES.has(n), n).toBe(false);
    }
  });
  it("keeps the options an ordinary configuration sets", () => {
    for (const n of ["EnableRewriting", "FileCachePath", "InPlaceResourceOptimization", "LogDir", "ImageMaxRewritesAtOnce"]) {
      expect(SAFE_OPTION_NAMES.has(n), n).toBe(true);
    }
  });
});
