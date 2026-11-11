// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The captures of one running server (test-fixtures/rig/), as replies for
// the two consoles: pages opened with data of a real size, because a page
// that only shows a refusal lays out (and reads) trivially. The whole-server
// console gets the whole-server captures, the per-host console its own and,
// like a real one, the refusals of the whole-server pages. Leaves without a
// capture of that scope use the other scope's (same shape) or the spec
// fixtures.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import * as F from "./fixtures";
import { perHostReplies, wholeServerReplies } from "./layout";
import type { Leaf, Responder } from "./mock-admin";
import * as RP from "./rule-packs-fixtures";

type Replies = Partial<Record<Leaf, Responder>>;

const RIG = fileURLToPath(new URL("../../test-fixtures/rig/", import.meta.url));
const rig = (name: string): unknown => JSON.parse(readFileSync(`${RIG}${name}.json`, "utf8"));

export function rigWholeServerReplies(): Replies {
  return {
    ...wholeServerReplies(),
    config: F.ok(rig("config.global")),
    stats_json: F.ok(rig("stats_json.global")),
    histograms: F.ok(rig("histograms.vhost-a")),
    cache: F.ok(rig("cache.vhost-a")),
    rule_packs: RP.GLOBAL_TWO,
    "v1/daemon/health": F.ok(rig("daemon_health.global")),
    "v1/daemon/stats": F.ok(rig("daemon_stats.global")),
    "v1/daemon/cooldowns": F.ok(rig("daemon_cooldowns.global")),
    "v1/daemon/cache/urls": F.ok(rig("daemon_cache_urls.global")),
    "v1/daemon/logs": F.ok(rig("daemon_logs.global")),
  };
}

export function rigPerHostReplies(): Replies {
  return {
    ...perHostReplies(),
    config: F.ok({ ...(rig("config.vhost-a") as Record<string, unknown>), host: "site-a.test:80" }),
    stats_json: F.ok(rig("stats_json.vhost-a")),
    histograms: F.ok(rig("histograms.vhost-a")),
    cache: F.ok(rig("cache.vhost-a")),
    rule_packs: RP.ONE,
    "v1/daemon/health": F.ok(rig("daemon_health.global")),
    "v1/daemon/stats": F.ok(rig("daemon_stats.vhost-a")),
  };
}
