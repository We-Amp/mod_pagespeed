#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Honest two-goal JS parseability check for the fuzz-artifact classifier
// (tools/ci/classify-js-fuzz-artifacts.sh). Replaces `node --check` as the
// validity axis: node v24's CJS/ambiguous --check path has an ESM-detection
// fallback that returns rc=0 whenever an import/export token breaks the CJS
// parse — even for input unparseable in BOTH goals (the minifier-rewrite
// triage; minimal
// proof: `export&\n&` passes --check silently while execution, vm.Script and
// .mjs --check all reject; `a&\n&` without the export token is honestly
// rejected). That quirk misrouted an entire nightly needs-triage bucket.
//
// A file counts as parseable when ANY serving goal accepts it:
//   * script goal, raw            — vm.Script(src)
//   * script goal, CJS-wrapped    — node's module wrapper legalizes
//                                   top-level `return` (CJS servability)
//   * module goal                 — vm.SourceTextModule when the runtime
//                                   offers it, else `node --check` on a .mjs
//                                   copy (honest for the module goal: the
//                                   fallback quirk only affects the
//                                   CJS/ambiguous path)
//
// Usage: js-parse-check.cjs <file>
//   rc 0 iff parseable in at least one goal; prints "script=0|1 module=0|1".
//   Generosity is deliberate: the classifier fails toward triage, so
//   accepting a dubious goal is the safe direction.
"use strict";

const fs = require("fs");
const vm = require("vm");

if (process.argv.length !== 3) {
  console.error("usage: js-parse-check.cjs <file>");
  process.exit(2);
}
const src = fs.readFileSync(process.argv[2], "utf8");

let script = false;
try {
  new vm.Script(src, {});
  script = true;
} catch {}
if (!script) {
  // CJS wrapper: node's CommonJS goal wraps sources in a function, which
  // legalizes top-level `return` (and only that class of difference).
  try {
    new vm.Script(
      "(function(require,module,exports,__filename,__dirname){" + src + "\n})",
      {}
    );
    script = true;
  } catch {}
}

let module_ = false;
if (vm.SourceTextModule) {
  try {
    new vm.SourceTextModule(src, {});
    module_ = true;
  } catch {}
} else {
  const os = require("os");
  const path = require("path");
  const cp = require("child_process");
  const tmp = path.join(os.tmpdir(), "jspc-" + process.pid + ".mjs");
  fs.writeFileSync(tmp, src);
  const r = cp.spawnSync(process.execPath, ["--check", tmp], {
    stdio: "pipe",
  });
  fs.unlinkSync(tmp);
  module_ = r.status === 0;
}

console.log(`script=${script ? 1 : 0} module=${module_ ? 1 : 0}`);
process.exit(script || module_ ? 0 : 1);
