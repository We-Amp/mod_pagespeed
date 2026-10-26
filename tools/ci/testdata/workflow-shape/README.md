# Workflow-shape gate fixtures

Fixtures for `tools/ci/test_check_workflow_shape.sh`. Each subdirectory is a
tree root passed to `tools/ci/check-workflow-shape.sh`:

- `good/` must pass (exit 0). It exercises a thin caller with an aggregate
  gate job, a `ci-dev` caller, a reusable workflow with every
  routing-expression variant, the four fleet guards (the fleet-iis one
  included), a hosted report job with a write permission, and the allowlist path (`legacy.yml` is listed
  in `good/tools/ci/workflow-shape-allowlist.txt`).
- `bad-no-top-perms/` (a reusable workflow without a top-level `permissions:`
  block), `bad-write-quoted/` and `bad-write-single-quoted/` (a quoted `write`
  on a fleet job) and `bad-write-flow/` (the one-line flow form) cover the
  gaps of rule f.
- `good/` also has a job whose guard is prefixed by `!cancelled()` and `needs.<job>.result == 'success'` terms (a job that depends on a build runner pair); `bad-guard-status-drift/` and `bad-guard-status-missing/` break that form.
- `bad-env-fleet/` and `bad-secret-fleet/` (an environment, or a secret, on
  a fleet-routed job) cover rule g; `good/` has a hosted job with both.
  `bad-secret-toplevel-env/`, `bad-secret-inherit/`, `bad-secret-index/`,
  `bad-secret-tojson/` and `bad-secret-uses-job/` cover the other holes of rule g;
  `good/` has a hosted job with both and a call that passes a secret to the
  allowed callee.
- `bad-guard-iis-only-term/` and `bad-guard-iis-no-fork/` break the fleet-iis
  guard; `bad-guard-iis-class/` routes to fleet-iis with the plain fleet-only
  guard; `bad-guard-hyperv-class/` does the same for fleet-hyperv.
- `good/` also has jobs on the switch routing expression (no guard, or the
  path filter alone) and the `!= 'false'` forms of the fleet-only and fleet-iis
  guards; `bad-switch-guard/` (a guard on a switch-routed job),
  `bad-switch-drift/`, `bad-switch-iis-class/` (the switch expression on a
  class with no hosted twin), `bad-switch-path-drift/` and
  `bad-guard-only-switch-drift/` (the switch form without the fork term) break them.
  `good/` also has a fleet-iis job whose guard is narrowed by a path filter;
  `bad-guard-iis-path-drift/` (the narrowing without the full guard) breaks it.
- `bad-*/` must each fail (exit 1) and name the rule in the message.

The bad trees violate the gate on purpose; the token scan excludes this
directory (see the exclusion list in `check-workflow-shape.sh`).
- `good-queue-guard/` is the accepted merge-queue guard layout of `pipeline-pr.yml`: the guard step right after the checkout step, the fixed keys, `if:`, env and `run:` block, and `vendor` with `needs: changes` and no `if:`. Each `bad-queue-guard-*` tree breaks one rule of the `queue-guard-reads-default-branch` check (the queued tree's copy, an extra or trailing command, an interpolated, truncated, globbed, braced, `find`-ed or sourced read of the script, a `GITHUB_ENV` write, an absent, renamed or misplaced step, a neutering or quoted key, a changed `if:`, a step not right after the checkout, env on the checkout, a job-level or extra step env key, `PATH`/`BASH_ENV`/`GIT_CONFIG*`, and a `vendor` job without `needs: changes`, with an `if:`, or missing). `good/` carries the guard step in its `changes` job too.
- `good/` carries the merge-queue guard step in its `changes` job in the one accepted form (`if: github.event_name == 'merge_group'`, a `run:` block of `set -euo pipefail`, the optional default-branch fetch and `git show "origin/${DEFAULT_BRANCH}:tools/ci/queue_fork_guard.sh" | bash`). The `bad-queue-guard-*` trees each break the `queue-guard-reads-default-branch` check: the queued tree's copy, an extra command, trailing text or comment, an interpolated path, a second read of the script elsewhere, an absent or renamed step, the step in the wrong job, a neutering key (`continue-on-error`) and a changed `if:`.
- `good/` carries the merge-queue guard step in its `changes` job in the one accepted form (`if: github.event_name == 'merge_group'`, a `run:` block of `set -euo pipefail`, the optional default-branch fetch and `git show "origin/${DEFAULT_BRANCH}:tools/ci/queue_fork_guard.sh" | bash`). The `bad-queue-guard-*` trees each break the `queue-guard-reads-default-branch` check: the queued tree's copy, an extra command, trailing text or comment, an interpolated path, a second read of the script elsewhere, an absent or renamed step, the step in the wrong job, a neutering key (`continue-on-error`) and a changed `if:`.
- `good/` carries a fleet pipeline (`pipeline-nightly.yml`) with the `event-guard` job in the one accepted form, every fleet job reaching it through `needs`, and a `!cancelled()` job with a success term on a reaching job. The `bad-event-guard-*` trees each break the `event-guard` check: the job absent, a fleet job with no path to it, an extra command in its step, `merge_group` in its allowed list, a fleet job with `if: always()`, and a `!cancelled()` fleet job without a success term on a reaching job.
- The `bad-event-guard-*` trees for the ref and actor checks drop or widen one of them (`refs-widened`, `actor-check-removed`, `queue-ref-check-removed`, `fork-check-removed`); `relay-failure`, `relay-or` and `relay-multiline` put a hosted relay with `failure()`, `success() || failure()` or a multi-line `always()` between the guard and a fleet job, and `good/pipeline-nightly.yml` has a relay in the required form `needs.event-guard.result == 'success' && (!cancelled() && true)`.
- `bad-event-guard-unnamed-queue-ref` and `bad-event-guard-unnamed-wide-ref` put a bad `ALLOWED_REFS` entry (a queue ref, a `refs/heads/*` glob) in a pipeline file the gate does not name in its lists. `bad-event-guard-relay-or-always`, `-relay-or-failure`, `-relay-case`, `-relay-literal`, `-relay-plain-multiline` and `-relay-quoted-if` put a hosted relay with a non-success shape of `if:` (`... || always()`, `... || failure()`, `Always()`, a literal block, a plain multi-line value, a quoted `'if'` key) between the guard and a fleet job, and `-relay-quoted-coe` does the same with a quoted `'continue-on-error'` key; `good/pipeline-nightly.yml` has a relay in the required form `needs.event-guard.result == 'success' && (!cancelled() && true)`.
- `bad-event-guard-relay-guard-or-always`, `-relay-guard-or-failure`, `-relay-negated-guard`, `-relay-guard-eq-false`, `-relay-always-space`, `-relay-not-equal` and `-relay-quoted-paren` put a hosted relay whose `if:` has `always(`, `failure(`, `cancelled(`, `||`, `!` or `== false` but is not exactly `needs.event-guard.result == 'success' && (<inner>)` between the guard and a fleet job: `guard && false || always()`, the same with `failure()`, `!cancelled() && !(guard)`, `guard == false`, `always ()` with a space, `needs.event-guard.result != 'failure'` (safe in practice, rejected because it negates), and a group closed early behind a parenthesis in a string literal. `good-event-guard-relay-paren` has the accepted form `needs.event-guard.result == 'success' && (github.event_name == 'schedule' || always())`.
- `bad-event-guard-relay-success-eq-zero`, `-relay-success-lt` and `-relay-success-format` compare or wrap `success()` (an explicit status function turns off the implicit one); `-relay-key-space` and `-relay-quoted-key-space` write the key as `if :` and `'if' :`; `-relay-five-space` continues a plain `if:` on a line indented five spaces with `|| always()`; `-relay-literal-hash` hides `|| always()` behind a `#`-led line inside a literal block; `-relay-block-clip` holds the accepted form in a `|` block, whose kept final newline makes it a string. `bad-yaml-alias`, `bad-yaml-anchor` and `bad-yaml-merge-key` put an alias on a relay `if:`, an anchor on a `needs:` value and a `<<:` merge key in a job. `good-event-guard-relay-bare-success` has a relay with a bare `${{ success() }}`.
- `good-queue-guard/` and every queue-guard fixture carry the push-ref step and the dispatch-actor step before the guard step. `bad-queue-guard-push-step-*`, `-dispatch-step-*` and `-steps-misordered` drop, rename, widen, add a command to or reorder them.
- Every queue-guard fixture and `good/` carry the `Decide the fleet scope` step right after the merge-queue guard step, and the `fleet` and `heavy` outputs, so each fails only for its own defect (the test rejects a non-fleet-scope fixture that also trips `fleet-scope`). `good-queue-guard/` also has `vendor` with `if: ${{ needs.changes.outputs.fleet == 'true' }}`. The `bad-fleet-scope-*` trees break the `fleet-scope` check: an extra command, a renamed step, an extra env key, the outputs missing, a step that runs the tree's `tools/ci/pr_tier_scope.sh`, the inline `merge_group|workflow_dispatch)` branch removed, and the step before the merge-queue guard or after the diff step. `bad-queue-guard-vendor-if-always`, `-if-heavy` and `-if-not-false` give `vendor` an `if:` other than the fleet form.
