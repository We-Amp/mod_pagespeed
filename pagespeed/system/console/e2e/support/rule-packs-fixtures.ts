// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Rule packs status fixtures (rule_packs?json=1, schema 1), shaped like the
// module's own golden JSON: zero, one and two packs, a pack the server has
// not read yet, a duplicate id, a refused pack and overlap warnings.

import { ok } from "./fixtures";
import type { Responder } from "./mock-admin";

const SHA_A = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const SHA_B = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

const COUNTERS = {
  rule_pack_actions_applied: 4,
  rule_pack_pages_evaluated: 120,
  rule_pack_rows_logged: 12,
};

export function row(over: Record<string, unknown>): Record<string, unknown> {
  return {
    action: "insert",
    after: "",
    at: "2026-10-11T12:00:00Z",
    before: "",
    defect: "",
    header: false,
    host: "www.example.test",
    kind: "canonical",
    mode: "report",
    pack_id: "edge-seo",
    pack_version: "0.1.0",
    path: "/a",
    reason: "missing",
    removed: "",
    rule: "canonical-keep",
    sha256_prefix: "0123456789abcdef",
    ts_ms: 1791720000000,
    ...over,
  };
}

export const PACK_EDGE = {
  file: "/etc/packs/edge.json",
  id: "edge-seo",
  key_id: null,
  last_error: null,
  last_event_at: "2026-10-11T12:00:00Z",
  license: null,
  position: 0,
  publisher: "Example Publisher",
  sha256: SHA_A,
  state: "loaded",
  version: "0.1.0",
  warnings: [],
};

export const PACK_HOUSE = {
  file: "/etc/packs/house.json",
  id: "house-rules",
  key_id: "example-key",
  last_error: null,
  last_event_at: "2026-10-11T12:00:01Z",
  license: { expires: "2027-01-01", label: "Example licence", terms: "see the publisher" },
  position: 1,
  publisher: "House",
  sha256: SHA_B,
  state: "loaded",
  version: "2.0.0",
  warnings: ["rule x uses a deprecated kind"],
};

const ROWS_EDGE = [
  row({ at: "2026-10-11T12:00:02Z", ts_ms: 1791720002000, path: "/<script>alert(1)</script>", rule: "canonical-keep" }),
  row({ at: "2026-10-11T12:00:01Z", ts_ms: 1791720001000, path: "/b", action: "none", reason: "present", rule: "title-keep" }),
  row({ at: "2026-10-11T12:00:00Z", ts_ms: 1791720000000, path: "/a", host: "cdn.example.test", claimed_by: "edge-seo" }),
];

const ROWS_HOUSE = [
  row({ at: "2026-10-11T12:00:03Z", ts_ms: 1791720003000, pack_id: "house-rules", pack_version: "2.0.0", rule: "robots-meta", action: "replace", reason: "conflict", path: "/h" }),
];

interface Over {
  packs?: unknown[];
  files?: string[];
  rows?: unknown[];
  pack?: string | null;
  status?: string;
  error?: string | null;
  overlap?: string[];
  counters?: Record<string, number> | null;
  scope?: "vhost" | "global";
  enabled?: boolean;
  vhosts?: unknown[];
}

export function status(o: Over = {}): Record<string, unknown> {
  const packs = o.packs ?? [];
  const files = o.files ?? (packs as Array<{ file: string }>).map((p) => p.file);
  return {
    configuration: {
      filter_enabled: o.enabled ?? true,
      rule_pack_files: files,
      rule_pack_mode: "report",
      rule_pack_require_signed: false,
    },
    counters: o.counters === undefined ? COUNTERS : o.counters,
    decisions: {
      date: "2026-10-11",
      error: o.error ?? null,
      limit: 100,
      pack: o.pack === undefined ? null : o.pack,
      rows: o.rows ?? [],
      status: o.status ?? "ok",
    },
    generated_at: "2026-10-11T12:00:05Z",
    host: o.scope === "global" ? "" : "www.example.test:80",
    overlap_warnings: o.overlap ?? [],
    packs,
    schema: 1,
    scope: o.scope ?? "vhost",
    ...(o.scope === "global" ? { vhosts: o.vhosts ?? [] } : {}),
  };
}

/** No RulePackFile configured. */
export const ZERO = ok(status({ files: [], packs: [], pack: null, status: "no_pack_file", enabled: false, counters: null }));

/** Configured, but this server process has not read the file yet. */
export const NOT_READ = ok(
  status({
    packs: [{ ...PACK_EDGE, state: "none", id: null, version: null, publisher: null, sha256: null, last_event_at: null }],
    pack: null,
    status: "missing",
  }),
);

export const ONE = ok(status({ packs: [PACK_EDGE], pack: "edge-seo", rows: ROWS_EDGE }));

export const TWO_OVERLAP = ["rule pack overlap: title owned by edge-seo"];

/** The pack list of the two-pack case, plus a refused and a duplicate-id file. */
export const MANY_PACKS = [
  PACK_EDGE,
  PACK_HOUSE,
  {
    ...PACK_EDGE,
    file: "/etc/packs/edge-copy.json",
    position: 2,
    state: "duplicate-id",
    warnings: [],
  },
  {
    ...PACK_EDGE,
    file: "/etc/packs/bad.json",
    position: 3,
    state: "refused",
    id: null,
    version: null,
    publisher: null,
    sha256: null,
    last_error: "signature does not verify",
  },
];

/** Answers by ?pack=: the first pack's log by default, the other pack's when asked. */
export const TWO_BY_PACK: Responder = (_call, request) => {
  const requested = request.url.searchParams.get("pack");
  const house = requested === "house-rules";
  return ok(
    status({
      packs: MANY_PACKS,
      overlap: TWO_OVERLAP,
      pack: house ? "house-rules" : "edge-seo",
      rows: house ? ROWS_HOUSE : ROWS_EDGE,
    }),
  );
};

/** The whole-server console: rows of several hosts. */
export const GLOBAL_TWO: Responder = ok(
  status({ scope: "global", packs: MANY_PACKS.slice(0, 2), pack: "edge-seo", rows: ROWS_EDGE }),
);

/** One virtual host's configuration, as the whole-server answer lists it. */
export const VHOST_DEMO = {
  filter_enabled: true,
  host: "demo.example.test:80",
  packs: [PACK_EDGE],
  rule_pack_mode: "report",
  rule_pack_require_signed: false,
};

/** The whole-server console where only a virtual host configures a pack: its packs and its decisions are listed. */
export const GLOBAL_VHOST_ONLY: Responder = ok(
  status({ scope: "global", files: [], packs: [], pack: "edge-seo", rows: ROWS_EDGE, vhosts: [VHOST_DEMO] }),
);

/** The whole-server console where nothing configures a pack anywhere. */
export const GLOBAL_NONE: Responder = ok(
  status({ scope: "global", files: [], packs: [], status: "no_pack_file", enabled: false, counters: null, vhosts: [] }),
);
