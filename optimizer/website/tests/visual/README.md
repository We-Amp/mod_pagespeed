# Scan UI visual suite

Opt-in screenshots of the scanner pages (`/analyze/` and `/ai-readability/`) in
every state, for review by a person or a second model. It asserts nothing about
pixels, so it never gates CI and is not part of `npx playwright test`.

## Run

```
cd website
VISUAL=1 npx playwright test --project=visual
```

Without `VISUAL=1` the `visual` project does not exist. Viewports: desktop
1280x800 and mobile 390x844, dark colour scheme, reduced motion, animations
disabled, `Date` fixed, fonts awaited, every request to a host other than the
local server refused. The scan API, both PageSpeed Insights strategies and the
contact endpoint are answered from `tests/fixtures/scan/` by `mocks.ts`, with a
per-source delay or a gate (`mocks.release(source)`) so the loading and partial
states are captured on purpose.

## Output

`test-output/<run>/visual/{page}-{NN}-{state}-{viewport}.png`, for example
`analyze-01-landing-mobile.png`. The state number matches the list in
`scan-ui.visual.spec.ts`; states without a body yet are registered as skipped,
each with the mock timing it needs. `<run>` is the port (or `PW_RUN_ID`), so concurrent runs do not
overwrite each other; the directory is git-ignored.

## Fixtures

`report-full.json` (grade C, every lens present, all eight gates fire),
`report-clean.json` (grade A, nothing fires), `report-blocked.json` (risk lenses
blocked or errored), `psi-mobile.json`, `psi-desktop.json` and `psi-429.html` (nginx's default 429 page, which is what the proxy's own rate limit returns).
`fixtures.test.ts` (run by `npx vitest run`) checks them against the gate
predicates in `src/lib/scan/demand.mjs`. The PSI files are trimmed real responses, relabelled for https://shop.example.com/, with a set of flagged audits added and the scores set to 0.46 (mobile) and 0.81 (desktop) so the Speed panel has fixes to show.

## Review checklist

- hierarchy readable in 5 s
- nothing above the fold beyond hero, box and strip
- no overflow or clipped text at 390 px
- status never colour-only
- focus ring visible on tiles/chips (separate keyboard screenshots of states 4 and 8)
- copy on screen equals the table
- no leftover fragment of the retired inline scan forms

Output of a review: a written review with findings marked blocking /
non-blocking. Acceptance: no blocking findings.
