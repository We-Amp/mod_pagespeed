// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

export const meta = {
  name: 'verify-shutdown-matrix',
  description: 'Triage + adversarially verify the defect multi-sanitizer stress-matrix results (ASan/UBSan/TSan x filter-sets on the multi-site corpus)',
  phases: [
    { title: 'Gather' },
    { title: 'Triage' },
    { title: 'Completeness' },
    { title: 'Synthesize' },
  ],
}

// Pass the rig location at launch: Workflow({scriptPath, args:{host:'user@rig', worktree:'/path/to/checkout'}})
const RIG = (args && args.host) || 'localhost'        // ssh target of the stress rig
const RD = (args && args.results_dir) || '/tmp/matrix_results'
const WT = (args && args.worktree) || '.'            // local checkout for source triage

const GATHER_SCHEMA = {
  type: 'object',
  required: ['cells', 'findings'],
  properties: {
    cells: {
      type: 'array',
      items: {
        type: 'object',
        required: ['label', 'san', 'result', 'requests', 'san_hits'],
        properties: {
          label: { type: 'string' },
          san: { type: 'string' },
          config: { type: 'string' },
          result: { type: 'string' },
          exit: { type: 'string' },
          requests: { type: 'integer' },
          san_hits: { type: 'integer' },
          restart_health_issues: { type: 'integer' },
          notes: { type: 'string' },
        },
      },
    },
    findings: {
      type: 'array',
      items: {
        type: 'object',
        required: ['id', 'san_type', 'message'],
        properties: {
          id: { type: 'string' },
          cells: { type: 'array', items: { type: 'string' } },
          san_type: { type: 'string', enum: ['ubsan', 'asan', 'tsan', 'other'] },
          file: { type: 'string' },
          line: { type: 'integer' },
          message: { type: 'string' },
          count: { type: 'integer' },
        },
      },
    },
    overall_notes: { type: 'string' },
  },
}

const VERDICT_SCHEMA = {
  type: 'object',
  required: ['id', 'classification', 'is_shutdown_related', 'rationale'],
  properties: {
    id: { type: 'string' },
    classification: { type: 'string', enum: ['benign', 'real-bug', 'needs-human', 'suppressed-known'] },
    is_shutdown_related: { type: 'boolean' },
    rationale: { type: 'string' },
    recommended_action: { type: 'string' },
  },
}

const COMPLETENESS_SCHEMA = {
  type: 'object',
  required: ['per_cell', 'any_false_green'],
  properties: {
    per_cell: {
      type: 'array',
      items: {
        type: 'object',
        required: ['label', 'exercised'],
        properties: {
          label: { type: 'string' },
          exercised: { type: 'boolean' },
          evidence: { type: 'string' },
          concern: { type: 'string' },
        },
      },
    },
    any_false_green: { type: 'boolean' },
    summary: { type: 'string' },
  },
}

// --- Phase 1: gather everything off the rig -------------------------------------
phase('Gather')
const data = await agent(
  `You are gathering results of a mod_pagespeed shutdown-race stress MATRIX.
Run exactly this and read the output:

  ssh ${RIG} 'echo "===RESULTS==="; cat ${RD}/RESULTS.tsv; \
    echo "===SUMMARIES==="; for f in ${RD}/*.summary; do echo "## $(basename "$f")"; cat "$f"; done; \
    echo "===FINDINGS==="; for f in ${RD}/*.findings; do echo "## $(basename "$f")"; cat "$f"; done; \
    echo "===HARNESS==="; for f in ${RD}/*.harness.log; do echo "## $(basename "$f")"; grep -E "STRESS SUMMARY|requests=|crash/asan|RESULT:|not-healthy|graceful|teardown" "$f" | tail -10; done'

RESULTS.tsv columns: label, san, config, duration, exit, result, errlog_san_hits, requests.
Per-cell .summary lines: LABEL  RESULT  HARNESS_RC  ERRLOG_DELTA  SAN_HITS  REQUESTS.
.findings files hold deduped "<count> <line>" sanitizer findings (UBSan "file:line:col: runtime error:", ASan/TSan reports).

Build the structured object:
- One 'cells' entry per matrix cell (label, san, result PASS/FAIL/SKIP/NA, requests int, san_hits int, restart_health_issues int from "not-healthy" count, notes).
- One 'findings' entry per DISTINCT finding line across all .findings files. id = short slug (e.g. "ubsan-unicodetext-366"). Parse file+line from UBSan "path:line:col:" prefixes. san_type from which sanitizer. cells = which cell labels reported it. count = summed occurrences. If there are NO findings at all, return findings: [].
Return ONLY the structured object.`,
  { phase: 'Gather', schema: GATHER_SCHEMA }
)

const findings = (data.findings || [])
log(`gathered ${data.cells?.length || 0} cells, ${findings.length} distinct findings`)

// --- Phase 2: triage each distinct finding (parallel) ----------------------
phase('Triage')
let verdicts = []
if (findings.length) {
  verdicts = await parallel(findings.map((f) => () =>
    agent(
      `Triage ONE sanitizer finding from the mod_pagespeed shutdown-race matrix. Be adversarial and precise.

Finding id: ${f.id}
Sanitizer: ${f.san_type}
File:line: ${f.file || '?'}:${f.line || '?'}
Message: ${f.message}
Seen in cells: ${(f.cells || []).join(', ')}   occurrences: ${f.count || '?'}

Read the actual source at ${WT}/${f.file || ''} around line ${f.line || ''} (and callers if needed) to judge it.

Classify:
- 'benign': UB/race that cannot corrupt state or crash in practice (e.g. "null pointer passed as argument declared nonnull" for a 0-length memcpy/memmove; unsigned-overflow in a hash; a known-benign protobuf byte race already in tools/tsan_suppressions.txt). Explain WHY it's harmless.
- 'real-bug': a genuine defect (OOB, real UAF, signed-overflow with consequence, a race on shared mutable state).
- 'suppressed-known': matches an existing suppression / already-known benign pattern.
- 'needs-human': cannot determine from source alone.

CRITICAL question: is_shutdown_related — is this the worker-thread-during-static-destruction UAF class that is about (LogMessage/spdlog, CSS kClass/kId statics, or any static touched by a rewrite worker at teardown)? If so it is high-priority regardless of sanitizer.

recommended_action: one line (e.g. "none — benign", "add immortal-static fix", "add tsan suppression", "file follow-up").`,
      { phase: 'Triage', label: `triage:${f.id}`, schema: VERDICT_SCHEMA }
    ).then((v) => ({ ...v, finding: f })).catch(() => null)
  ))
  verdicts = verdicts.filter(Boolean)
} else {
  log('no sanitizer findings to triage')
}

// --- Phase 3: completeness critic (guard against false-green) ---------------
phase('Completeness')
const completeness = await agent(
  `You are the COMPLETENESS critic for the shutdown-race stress matrix. A PASS only means
something if the cell actually exercised the target paths. Given these per-cell stats:

${JSON.stringify(data.cells, null, 2)}

For EACH cell decide 'exercised' (bool): did it drive real load and teardown stress?
- requests should be non-trivial (hundreds+ of real 200s; note ASan/TSan are slow so counts are lower — TSan lowest).
- A SKIP (module missing) or NA/health-fatal cell is NOT exercised — flag it.
- The matrix used gentle chaos (restart ~150s, reload ~75s, flush ~40s); long cells should have seen multiple restarts/teardowns.
You may SSH to ${RIG} to spot-check a cell's harness log under ${RD}/<label>.harness.log if a number looks off
(e.g. confirm rewrites fired: grep for '.pagespeed.' served, or graceful-stop teardowns).
Set any_false_green=true if any cell reports PASS but did not actually exercise the rewrite/teardown paths.`,
  { phase: 'Completeness', schema: COMPLETENESS_SCHEMA }
)

// --- Phase 4: synthesize ----------------------------------------------------
phase('Synthesize')
const realBugs = verdicts.filter((v) => v.classification === 'real-bug')
const shutdownRelated = verdicts.filter((v) => v.is_shutdown_related)
const report = await agent(
  `Write the final verdict for the defect multi-sanitizer stress matrix as concise markdown.

Cells: ${JSON.stringify(data.cells)}
Finding verdicts: ${JSON.stringify(verdicts.map((v) => ({ id: v.id, san: v.finding?.san_type, classification: v.classification, is_shutdown_related: v.is_shutdown_related, rationale: v.rationale, action: v.recommended_action })))}
Completeness: ${JSON.stringify(completeness)}
Real bugs: ${realBugs.length}  Shutdown-related findings: ${shutdownRelated.length}

Produce:
1. **Headline verdict** — does the shutdown-UAF fix hold across the matrix (ASan/UBSan/TSan x filter-sets, multi-site corpus, long+gentle chaos)? Any NEW real or shutdown-related finding?
2. **Per-sanitizer result** — ASan (the authoritative UAF check), UBSan (catalog of UB sites + benign/real triage), TSan (note the best-effort/die_after_fork + uninstrumented-apache caveat — low confidence).
3. **Completeness** — were all cells real (no false-greens)?
4. **Actions** — only if real-bug or shutdown-related: what to change (immortal-static fix branch, lock-free) and re-verify; otherwise state "no code change — fix holds".
5. Keep disclosure discipline: this is product-internal verification; no customer-facing claims.
Be honest about limits (TSan confidence, UBSan benign-UB noise). Return the markdown only.`,
  { phase: 'Synthesize' }
)

return { cells: data.cells, verdicts, completeness, report }
