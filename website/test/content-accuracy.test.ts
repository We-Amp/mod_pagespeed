// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// content-accuracy.test.ts — ANTI-DRIFT guard for drift-prone product facts in
// human-facing site copy. Prevents the recurrence of the product-fact drift
// that mps2 PR #705 (2026-06-15 sitewide accuracy sweep) removed..
//
// WHAT DRIFTED (and what this guard forbids coming back)
//   PR #705 corrected a class of confidently-wrong product statements that had
//   spread across .md/.mdx content and .astro pages:
//     - a phantom 2.0 "v1.0.0" version label (2.0 GA is v2.0.0 / current
//       website manifest semver 2.0.21 — there is NO authoritative v1.0.0; the
//       roadmap is "2.x" / "a future 2.x release");
//     - ModPageSpeed 2.0 described as a loadable / standalone nginx module
//       installable from apt/yum (it is NOT — 2.0 BUNDLES nginx 1.30.2 in its
//       Docker reverse proxy and ships via Docker + Helm + NuGet;
//       the standalone 2.0 nginx module is DEFERRED);
//     - the 1.15 native nginx module's supported nginx range stated as
//       "nginx 1.26+" (it is per-distro, pinned to each distro's STOCK nginx:
//       Deb11/Ubu22.04=1.18.0 … Deb13=1.26.3 — a closed pinned matrix, not an
//       open ">=1.26" range);
//     - an Apache el9 arm64 overclaim ("AlmaLinux/RHEL 9 (amd64 + arm64)") —
//       the yum repo is x86_64 ONLY on el9; arm64 is direct-download;
//     - (guarded since the 2026-06-15 hardening) ModPageSpeed 2.0 stated to
//       require ".NET 9" — the middleware targets net8.0 + net10.0 (ASPNETCORE_TFMS).
//
// HOW THIS GUARD WORKS
//   (1) DENYLIST — a small set of TIGHTLY-SCOPED regexes, each targeting one
//       specific wrong CLAIM while deliberately NOT matching the legitimate
//       neighbouring text that lives in the clean tree today (e.g. "1.18 to
//       1.26", "el9 (x86_64; arm64 via direct download)", "before 2.0.14",
//       libpng/Envoy dependency versions like "1.0.0"). The scan surface is the
//       SIX BUCKETS defined at SCAN_BUCKETS below (content .md/.mdx, pages
//       .astro/.ts, data .ts/.mjs, layouts, llms .tmpl templates, and the
//       generated public/ agent surface); each file is scanned line-by-line and
//       a hit FAILS with file:line:matchedText.
//   (2) PER-RULE, CLAUSE-SCOPED EXEMPTIONS — exemption is OPT-IN per rule
//       (`negationExempt` for corrective-prose tolerance, `exemptIf` for a
//       rule-specific licensing term), and BOTH are tested against the CLAUSE
//       CONTAINING THE MATCH, never the whole line. Commit 39221716 inverted the
//       old global line-level negation default: a true statement in one clause
//       must not disarm a false claim in another ("Install 2.0 from apt. 1.15
//       packages are signed too." still flags). The denylist targets AFFIRMATIVE
//       restatements of the wrong fact, NOT the corrective copy that PR #705
//       itself wrote. This is precision-over-recall by design: the guard is a
//       REGRESSION NET (§Consequences), not an exhaustive semantic
//       checker. `exemptionMustNotDisarm` probes machine-enforce the scoping.
//   (3) CANONICAL-SOURCE consistency — assert the single-sources-of-truth
//       (product-facts.mjs + the release manifests) still hold the
//       ground-truth values, so the denylist's baseline never silently rots.
//   (4) CORPUS TESTS — a fixed corpus of (a) real corrective phrasings that MUST
//       NOT be flagged (the false positives an adversarial review found), and
//       (b) engine-tag refs ("v1.1.0+r11") that MUST NOT be flagged (§5).
//   (5) MATCHER UNIT TESTS — each denylist regex is asserted to FLAG a known
//       wrong sample (and variants) AND to NOT flag a known-good sample, proving
//       the matcher discriminates without having to mutate real content.
//
// The baseline 2.0 version is DERIVED from the manifest at runtime (NOT
// hardcoded) — the manifest legitimately changes (it is 2.0.21 today).
//
// This test asserts copy/contract only; it never modifies any file.

import { readFileSync, readdirSync, statSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { parse as parseYaml } from 'yaml';
import { describe, it, expect } from 'vitest';
import * as facts from '../src/data/product-facts.mjs';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const CONTENT_DIR = path.join(WEBSITE_ROOT, 'src/content');
const REL_2_0 = path.join(CONTENT_DIR, 'releases-2.0/release.yaml');
const REL_1_1 = path.join(CONTENT_DIR, 'releases-1.1/release.yaml');
const REL_2_1 = path.join(CONTENT_DIR, 'releases-2.1/release.yaml');

// ---------------------------------------------------------------------------
// Canonical manifests (single sources of truth). Parsed once.
// ---------------------------------------------------------------------------
type ReleaseManifest = {
  release: { semver: string; revision: number | null; tag: string };
};
const manifest2_0 = parseYaml(readFileSync(REL_2_0, 'utf8')) as ReleaseManifest;
const manifest1_1 = parseYaml(readFileSync(REL_1_1, 'utf8')) as ReleaseManifest;
const manifest2_1 = parseYaml(readFileSync(REL_2_1, 'utf8')) as ReleaseManifest;

// Current-line semver DERIVED at runtime — never hardcode (it changes at
// every 2.1 release, same reason V2_SEMVER below is derived).
const V2_1_SEMVER = manifest2_1.release.semver; // e.g. "2.1.0"

// Baseline 2.0 semver DERIVED at runtime — never hardcode (it changes).
const V2_SEMVER = manifest2_0.release.semver; // e.g. "2.0.21"

// ---------------------------------------------------------------------------
// File collection.
//
// SCAN SURFACE. The original guard scanned only src/content/**.{md,mdx} and
// src/pages/**.astro. That left published surfaces unguarded: the two API/feed
// routes are .ts, every product CLAIM originates in src/data/**, the shared
// layout emits meta/JSON-LD prose, and the llms.txt / ai-plugin.json family is
// generated from .tmpl sources and served verbatim to agents. All of those are
// PUBLISHED, so all of them are in scope.
//
// NO PRE-PROCESSING. Lines are scanned RAW, comments included. In src/data the
// comments are load-bearing fact records ("1.15 ships AVIF via four opt-in
// filters") — a claim that drifts in a comment drifts in the reader's head and
// then in the copy. Scanning them is intended, not an accident. If a real false
// positive ever appears, REWORD THE COMMENT — do not add a comment opt-out. One
// was tried (`notInComment`) and reverted: it cost the rule that set it 41.7% of
// product-facts.mjs (the canonical fact record) and 15.6% of src/data/**, to
// accommodate exactly ONE line. Rewording that one line costs nothing and keeps
// every rule's eyes open across the whole 57k-line scan surface.
// ---------------------------------------------------------------------------

// EXCLUSIONS — each with the reason it is NOT scanned. Matched against the
// path RELATIVE to WEBSITE_ROOT (posix separators).
const SCAN_EXCLUSIONS: Array<{ test: (rel: string) => boolean; why: string }> = [
  {
    // Archived Google-era documentation mirror. It is a HISTORICAL RECORD of
    // what the upstream project said, not a claim we are making today; the
    // guard's whole premise (this text asserts our current product facts) does
    // not hold for it, and "correcting" it would falsify the archive.
    test: (rel) => rel.startsWith('public/1.0/'),
    why: 'public/1.0/** — archived Google-era doc mirror; historical record, not our claim',
  },
  {
    // Large machine payloads (pricing snapshots, demo metrics, example
    // fixtures). No prose to drift, and megabytes of JSON per rule per run.
    test: (rel) => /^src\/data\/[^/]+\.json$/.test(rel),
    why: 'src/data/*.json — large machine payloads, no prose',
  },
  {
    // The guard's own fixtures are DELIBERATELY wrong (every rule's `bad`
    // sample lives here). Scanning them would make the guard flag itself.
    test: (rel) => rel.startsWith('test/'),
    why: "test/** — the guard's own known-bad fixtures would self-flag",
  },
  {
    // Presentational only. Components render claims that arrive as PROPS from
    // the pages and data modules already in scope, so the claim is caught at
    // its source rather than at each render site.
    //
    // EXCEPT src/components/home/: the home page's sections. They are the
    // home page's own copy, split out of src/pages/index.astro one file per
    // section, so they carry product claims as literals and stay in scope
    // (the "home" bucket below).
    test: (rel) => rel.startsWith('src/components/') && !rel.startsWith('src/components/home/'),
    why: 'src/components/** except home/ — presentational; claims arrive as props from scanned pages',
  },
];

function isExcluded(abs: string): boolean {
  const rel = path.relative(WEBSITE_ROOT, abs).split(path.sep).join('/');
  return SCAN_EXCLUSIONS.some((x) => x.test(rel));
}

// `honorExclusions` is false ONLY for the canary fixtures, which live under
// test/** (excluded by design) but must still be walked by this exact function
// so the canaries actually exercise the real walker.
function walk(dir: string, exts: string[], honorExclusions = true): string[] {
  let out: string[] = [];
  let entries: string[];
  try {
    entries = readdirSync(dir);
  } catch {
    return out; // dir absent — nothing to scan
  }
  for (const name of entries) {
    const full = path.join(dir, name);
    if (honorExclusions && isExcluded(full)) continue;
    const st = statSync(full);
    if (st.isDirectory()) {
      out = out.concat(walk(full, exts, honorExclusions));
    } else if (exts.some((e) => name.endsWith(e))) {
      out.push(full);
    }
  }
  return out;
}

// Explicit files (not a directory walk): the generated agent-facing surface.
function existingFiles(rels: string[]): string[] {
  return rels
    .map((r) => path.join(WEBSITE_ROOT, r))
    .filter((f) => {
      try {
        return statSync(f).isFile();
      } catch {
        return false;
      }
    });
}

// SCAN BUCKETS + FLOORS. `walk()` returns [] on a missing directory, so a path
// typo or a moved tree would silently degrade this guard to green while
// scanning nothing. A PER-BUCKET floor makes that failure loud: one global
// "> 0" would stay satisfied by src/content alone even if every other bucket
// vanished. Floors are set below today's real counts — they catch a bucket
// COLLAPSING, they are not a ratchet on content volume.
//
// PER-EXTENSION floors alongside the bucket total. A bucket total is blind to
// losing one whole EXTENSION inside a mixed bucket: fault injection showed
// content dropping every .mdx (110 files, floor 100), pages dropping every .ts
// (62, floor 50) and data dropping every .mjs (11, floor 10) — all three stayed
// GREEN. That last one means losing product-facts.mjs, the single most
// load-bearing file in the scan set, was invisible. So each mixed bucket also
// asserts a minimum per extension.
type ScanBucket = {
  name: string;
  files: string[];
  floor: number;
  extFloors?: Record<string, number>;
};

const SCAN_BUCKETS: ScanBucket[] = [
  {
    name: 'content',
    files: walk(CONTENT_DIR, ['.md', '.mdx']),
    floor: 100,
    // The docs-1.1 collection and docs/release-notes-2-1.mdx retired into
    // the single converged docs/release-notes.mdx page, dropping two more
    // .mdx files sitewide; the real count is 6 as of this change.
    extFloors: { '.mdx': 5 },
  },
  {
    // .astro pages plus the two PUBLISHED .ts routes (api/product.json.ts,
    // blog/rss.xml.ts) — both emit agent- and reader-facing product prose.
    name: 'pages',
    files: walk(path.join(WEBSITE_ROOT, 'src/pages'), ['.astro', '.ts']),
    floor: 50,
    extFloors: { '.ts': 2 },
  },
  {
    name: 'data',
    files: walk(path.join(WEBSITE_ROOT, 'src/data'), ['.ts', '.mjs']),
    floor: 10,
    // >= 1 .mjs === product-facts.mjs is still being scanned.
    extFloors: { '.mjs': 1 },
  },
  { name: 'layouts', files: walk(path.join(WEBSITE_ROOT, 'src/layouts'), ['.astro']), floor: 1 },
  {
    // The home page's sections (hero, the seven numbered sections, sponsor,
    // closing): page copy that lives under src/components/. Ten files today;
    // the floor catches the directory moving or the exclusion above swallowing
    // it again.
    name: 'home',
    files: walk(path.join(WEBSITE_ROOT, 'src/components/home'), ['.astro']),
    floor: 8,
  },
  {
    // One template is left: the AI-plugin manifest. llms.txt and llms-full.txt
    // are generated from the docs collection (scanned as content), the release
    // manifests and src/data/agent-files.mjs (scanned as data).
    name: 'templates',
    files: walk(path.join(WEBSITE_ROOT, 'scripts/llms-templates'), ['.tmpl']),
    floor: 1,
  },
  {
    name: 'public',
    files: existingFiles([
      'public/llms.txt',
      'public/llms-full.txt',
      'public/.well-known/ai-plugin.json',
    ]),
    floor: 3,
  },
  {
    // The install recipes for coding agents (README.md plus one per surface)
    // repeat distributions, .NET versions, RIDs, package names and headers as
    // literals, so they get the same denylist as the docs.
    name: 'recipes',
    files: walk(path.join(WEBSITE_ROOT, 'public/recipes'), ['.md']),
    floor: 7,
  },
];

const CONTENT_FILES = SCAN_BUCKETS.find((b) => b.name === 'content')!.files;
const SCAN_FILES = SCAN_BUCKETS.flatMap((b) => b.files);

// ---------------------------------------------------------------------------
// (2) NEGATION TOLERANCE IS OPT-IN, PER RULE.
//
//   This used to be the inverse: a global ~25-token NEGATION_CONTRAST list
//   disarmed ANY rule whenever ANY negation token appeared anywhere on the
//   line. That default was backwards, and the evidence was in the rules
//   themselves — every rule authored against REAL observed drift (f1, f2, f3)
//   had to set `hardFalse: true` to escape it, because the wrong claims those
//   rules target are themselves negation-shaped ("1.15 does not ship AVIF",
//   "AVIF is 2.0-only"). A blanket token list also disarms on an INCIDENTAL
//   negation elsewhere in the sentence, which is a silent hole by construction.
//
//   Now: negation exempts NOTHING by default. A rule that genuinely needs
//   corrective-prose tolerance sets `negationExempt`, and that regex carries the
//   SPECIFIC corrective phrasing for THAT claim — not a generic token list. Only
//   three rules need it (a, b2, b3); f1/f2/f3 need nothing, and c/d/e are
//   already cleared by their own regex directionality and `exemptIf`.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// (1) DENYLIST. Each rule is one tightly-scoped regex + human-readable why,
// plus a known-bad sample (must flag), a known-good sample (must not flag), and
// optional extra variants exercised by the matcher tests.
//
// SCOPING NOTES (false positives deliberately avoided — verified against the
// clean PR-#705 tree AND an adversarial false-positive corpus, see FP_CORPUS):
//   - Rule a does NOT bare-match "1.0.0": libpng 1.6.x, Envoy and other
//     dependency versions legitimately use x.y.z including "1.0.0". It only
//     matches phantom 2.0 version-label phrasings.
//   - Rule b3 is FORWARD-only ("ModPageSpeed 2.0" is the SUBJECT of the apt/yum
//     claim): the reverse direction ("1.15 ships apt and yum … 2.0 ships Docker")
//     is the common legitimate contrast and must not fire.
//   - Rule c keys on the OPEN-range expression ("1.26+", "1.26 or newer", ">=1.26"),
//     NOT "1.18 to 1.26" / "stock nginx 1.26.3".
//   - Rule d keys on the explicit "(amd64 + arm64)" overclaim attached to el9 /
//     RHEL 9 / AlmaLinux 9, OR an affirmative verb tying arm64 INTO the el9 yum
//     repo — NOT a correct arch-split description ("yum is x86_64 only; arm64 via
//     apt/direct download"), which the token order + the negation exemption clear.
// ---------------------------------------------------------------------------
// The slice of product-facts.mjs a rule anchor reads. A test can hand an anchor
// a MUTATED copy to prove it notices a changed fact.
type FactsView = Pick<
  typeof facts,
  'V1_LINE' | 'V2_LINE' | 'IMAGE_FORMAT_SUPPORT' | 'CAPABILITY_SUPPORT'
>;
// The machine-checked fact an exclusivity rule enforces. `describes` says it in
// words; `holds` reads the capability tables and must return true while the
// fact is true. When a table changes so that `holds` goes false, the anchor test
// fails and names the rule: re-verify the rule (and its `why`) before touching
// the table to make it pass.
type RuleAnchor = { describes: string; holds: (f: FactsView) => boolean };

function formatEditions(f: FactsView, format: string): string[] {
  const row = (f.IMAGE_FORMAT_SUPPORT as Array<{ format: string; editions: object }>).find(
    (r) => r.format === format,
  );
  return Object.keys(row?.editions ?? {});
}
function capabilityEditionsOf(f: FactsView, capability: string): string[] {
  const row = (f.CAPABILITY_SUPPORT as Array<{ capability: string; editions: object }>).find(
    (r) => r.capability === capability,
  );
  return Object.keys(row?.editions ?? {});
}
// "Not 2.0-only": the capability is carried by the 1.15 line AND by 2.0.
const onBothLines = (editions: string[], f: FactsView) =>
  editions.includes(f.V1_LINE) && editions.includes(f.V2_LINE);

const ANCHOR_AVIF: RuleAnchor = {
  describes: 'AVIF is encoded on the 1.15 line and on 2.0 (IMAGE_FORMAT_SUPPORT)',
  holds: (f) => onBothLines(formatEditions(f, 'AVIF'), f),
};
const ANCHOR_SHARED_CAPABILITIES: RuleAnchor = {
  describes:
    'zero-copy serving and variant-aware caching ship on the 1.15 line and on 2.0 (CAPABILITY_SUPPORT)',
  holds: (f) =>
    onBothLines(capabilityEditionsOf(f, 'zero-copy-serving'), f) &&
    onBothLines(capabilityEditionsOf(f, 'variant-aware-caching'), f),
};

type DenyRule = {
  id: string;
  why: string;
  // The fact this rule enforces, tied to product-facts.mjs. REQUIRED for the
  // exclusivity rules (f1-f4, see ANCHORED_RULE_IDS): see the anchor tests.
  anchor?: RuleAnchor;
  // Opt OUT of boundWindows(): the rule's claim is a deliberate cross-sentence
  // adjacency ("X is granted to 2.0. Then 1.15 is described by WebP-only
  // filters"), so its windows must be allowed to run past a sentence end.
  // Every such rule says why next to the flag.
  crossSentence?: boolean;
  re: RegExp;
  bad: string; // sample that MUST be flagged (re matches AND not corrective)
  good: string; // sample that MUST NOT be flagged
  // Per-rule extra exemption: a rule-specific term that LICENSES the claim.
  // Tested against the CLAUSE CONTAINING THE MATCH, not the whole line.
  exemptIf?: RegExp;
  // Opt IN to corrective-prose (negation/contrast) tolerance — §2. Set ONLY
  // when this rule's own targeted claim has a legitimate DENIAL form in real
  // copy, and make the regex carry that SPECIFIC phrasing. Never a generic
  // negation-token list: that is the backwards default this replaced. Also
  // CLAUSE-SCOPED — a denial in the neighbouring clause is about a different
  // claim and must not disarm this one.
  negationExempt?: RegExp;
  // WHOLE-FILE exemption, by path relative to the website root. Reserved for a
  // surface that is a HISTORICAL RECORD rather than a claim about the product
  // as it is today (an archived release history, a frozen doc set, the legal
  // pages that enumerate predecessor lines by name). A clause-scoped exemption
  // cannot express "this whole document speaks in the past tense", and widening
  // the regex until such a document passes would blind the rule everywhere
  // else. Every rule that sets this pins the exact list of files it exempts in
  // a test, so the exemption cannot silently widen to a live copy surface.
  exemptFile?: (rel: string) => boolean;
  // The exact scan-surface files `exemptFile` is allowed to cover. Asserted to
  // match, so adding a file to the tree (or broadening the predicate) fails
  // loudly instead of quietly removing a page from the rule's eyes.
  exemptFileExpectation?: string[];
  // Tighten the text this rule's exemptions are tested against to the
  // COMMA-DELIMITED PART of the clause the match starts in.
  //
  // A comma is deliberately NOT a clause boundary for the rule REGEXES: one
  // claim runs straight through a comma ("the worker, which ships only as an
  // image"). But it is exactly the seam an EXEMPTION gets abused across,
  // because bolting a true statement on after a comma is the most natural
  // sentence a writer produces:
  //   "ModPageSpeed 2.0 is the current product, see the migration guide."
  //   "The optimizer worker is not in the yum repository on Linux, and the
  //    IIS package ships from the 1.15 packaging channel."
  // Each is false in its first part and exempt only because of its second —
  // and the second is copy the programme itself mandates, so the disarm is
  // not hypothetical. Opt IN per rule; every rule that sets this pins the
  // abuse with `exemptionMustNotDisarm` probes.
  exemptScope?: 'clause' | 'comma-part';
  variantsBad?: string[]; // extra affirmative reintroductions that MUST be flagged
  variantsGood?: string[]; // extra legitimate phrasings that MUST NOT be flagged
  // ADVERSARIAL PROBES for the exemptions. Sentences that carry an exemption
  // term NATURALLY but are FALSE, so the exemption must not disarm the rule.
  // Each entry is asserted to be flagged. This is what turns "the exemption is
  // too broad" from a silent hole into a red test.
  exemptionMustNotDisarm?: string[];
  // A RATCHET for a rule that lands before the copy it forbids is gone: the
  // known hits, as {file: count}, that the scan reports as warnings instead
  // of failures while a separate change removes them. A file with MORE hits
  // than listed fails (new text), a file that is not listed fails, and a
  // listed file with FEWER hits fails too, naming the count to lower it to,
  // so the list only shrinks and cannot outlive the copy it was written for.
  pending?: Record<string, number>;
  // EXACT-LINE exemptions: one line of one file, matched character for
  // character, with the reason. Narrower than exemptIf (no other line, and
  // no edit of this one, is covered) and pinned by a test that each entry
  // still occurs exactly once in its file.
  exemptLines?: Array<{ file: string; line: string; why: string }>;
};

// A '.' is a sentence or clause boundary only when whitespace or the end of
// the text follows it. A dot inside a token (1.15.x, 2.x, rc.5, nginx.conf,
// ASP.NET) never ends anything, and neither does the dot of a common
// abbreviation (e.g., i.e., vs., cf.), which is followed by a space.
const DOT_IN_TOKEN = String.raw`\.(?=\S)|(?<=\b(?:e\.g|i\.e|vs|cf))\.`;

// A gap INSIDE a rule regex that stops at a clause boundary instead of running
// to its character limit. A rule that pairs a subject with an object across a
// wide [^\n]{0,80} window otherwise matches a subject in sentence one against
// an object in sentence two — "The optimizer worker runs as its own service.
// The module is a loadable nginx module." is two true statements, not a claim
// that the worker is loadable. A '.' followed by non-whitespace is a
// token dot, not a boundary (see DOT_IN_TOKEN), so "mod_pagespeed 2.5.x" never
// splits a clause. A '.' after a digit but before a space ends the sentence: "…landed in
// 2.0.16. The module…" is two sentences, which matters once a wrapped paragraph
// is scanned as one block.
const CLAUSE_GAP = new RegExp(`(?:[^;:—!?.\\n]|${DOT_IN_TOKEN})`).source;

// CLAUSE_GAP plus the colon. A colon normally ends a clause, and for most
// rules it should: what follows an "Install it:" is a new statement. But a
// colon-introduced LIST is one statement, not two — "mod_pagespeed:
// Apache, nginx, IIS and Envoy" names its subject once and then enumerates.
// Used ONLY by rule `j`'s list branches, which additionally require the colon
// itself to sit between the subject and the list.
const COLON_LIST_GAP = new RegExp(`(?:[^;—!?.\\n]|${DOT_IN_TOKEN})`).source;

// The part names used as a rule SUBJECT. The prose name, the literal package /
// binary names, and the bare "the worker" — which is what the copy actually
// writes ("the worker rewrites it", "the worker process runs out-of-process"),
// so a list without it guards a vocabulary the site does not use.
//
// "the worker process" IS this subject — that is how the ASP.NET Core docs
// name it — so only the noun's other senses are excluded: a worker thread or
// pool, nginx's own `worker_processes`, and "the worker image", which names
// the container rather than the process running inside it. The browser's web
// and service workers need no guard: neither reads as "the worker".
//
// A filesystem path is not the worker as a subject: `/etc/default/pagespeed-optimizer`
// names a config file, and the sentence around it is about that file. The
// (?<!\/) guard keeps a path component out of every rule that uses this subject.
// This surfaced when block scanning joined a wrapped paragraph that said
// "from `/etc/default/pagespeed-optimizer` -- the default binary path exists only
// in the container images", which is true and is not a packaging claim.
const WORKER_SUBJECT =
  /(?:(?<!\/)(?:optimi[sz]er worker|pagespeed-optimizer|factory_worker|the worker(?!_)(?!\s+(?:thread|threads|pool|pools|image|images)\b)))/
    .source;

// The current product as a rule SUBJECT: the name alone or "mod_pagespeed 2".
// The retired "2.1" label is no longer a subject: rule k denies it. Never the
// predecessor ("mod_pagespeed 1.15"), never an exact release ("mod_pagespeed
// 2.3.0" in a release-notes row is a fact about that release), and never a
// filename ("mod_pagespeed.so").
const CURRENT_PRODUCT_SUBJECT = /mod_pagespeed(?: 2)?(?![\w.-]|\s+\d)/.source;

// SENTENCE-BOUNDED WINDOWS. Many rule regexes pair a subject with a qualifier
// across a bounded proximity window written [^\n]{0,N}. On a single physical
// line the window was cut short by the line break; on a joined block (see BLOCK
// SCANNING) the line break is gone, so the same window would reach from one
// sentence into the next and pair "AVIF" in one true sentence with "2.0" in the
// following one. boundWindows() rewrites every such window to stop at a
// sentence end instead. The length bound N is untouched, so the window stays
// bounded; a '.' inside a token (see DOT_IN_TOKEN) is not a sentence end.
// Applied once to every rule below (after DENYLIST), so no rule can forget it.
const SENTENCE_GAP = new RegExp(`(?:[^.!?\\n]|${DOT_IN_TOKEN})`).source;
function boundWindows(re: RegExp): RegExp {
  return new RegExp(re.source.split('[^\\n]').join(SENTENCE_GAP), re.flags);
}

function escapeForRegExp(literal: string): string {
  return literal.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

// The blog posts whose TITLE carries a retired product name. A title is
// what was published on its date: it is indexed under that string, quoted
// verbatim in the generated agent files, and rasterized into the post's social
// card. Renaming one would rewrite the record and break three surfaces that
// mirror it, so the titles stay and the rule steps over them — by exact
// string, so nothing else is covered. The BODIES of these posts are not
// exempt.
const FROZEN_POST_TITLES = [
  // The predecessor line's own release post, published under that number on
  // its date. The label branch of rule `i` reaches titles carrying
  // "mod_pagespeed 1.15" as of this hardening, and this is the one such title
  // in the tree that is a dated record rather than a live label.
  'mod_pagespeed 1.15: what six years of stewardship look like',
  'ModPageSpeed 2.0 for ASP.NET Core: optimization middleware via NuGet',
  'Run ModPageSpeed 2.0 with Docker Compose',
  "Migrate from Google's mod_pagespeed / ngx_pagespeed to ModPageSpeed 2.0",
  'Cache key derivation in ModPageSpeed 2.0: host-scoped keys and single-pass variant fallback',
  'Benchmarking ModPageSpeed 2.0: real numbers on real sites',
];

// …and only in TITLE or LINK position: `title: '<it>'` in a post's own
// frontmatter, or `[<it>](…)` where another page cites the post. The same
// words in running prose are not a citation and stay flagged. A title is
// trimmed at its first sentence period ("… for ASP.NET Core: …") because the
// clause scope the exemption is tested against ends there.
const FROZEN_TITLE_CITATION = FROZEN_POST_TITLES.map(
  (title) => `(?:title:\\s*["']|\\[)${escapeForRegExp(title.split(/\.(?!\d)/)[0])}`,
);

// The hits of rule `k` that the rename has not reached yet (see `pending` on
// DenyRule). Shared with scripts/check-built-content.mjs, which reads the
// `pages` half.
const RETIRED_LABEL_PENDING_FILE = path.join(WEBSITE_ROOT, 'scripts/retired-label-pending.json');
const RETIRED_LABEL_PENDING: Record<string, number> = existsSync(RETIRED_LABEL_PENDING_FILE)
  ? (JSON.parse(readFileSync(RETIRED_LABEL_PENDING_FILE, 'utf8')).sources ?? {})
  : {};

// The lines that keep the "mod_pagespeed 2" shorthand (rule l): history and
// records only, one exact line each.
const SHORTHAND_ALLOW_FILE = path.join(WEBSITE_ROOT, 'scripts/product-name-shorthand-allow.json');
const SHORTHAND_ALLOW_LINES: Array<{ file: string; line: string; why: string }> = JSON.parse(
  readFileSync(SHORTHAND_ALLOW_FILE, 'utf8'),
).lines;

const DENYLIST: DenyRule[] = [
  {
    id: 'a-phantom-2.0-v1.0.0-label',
    why: '2.0 GA is v2.0.0 (manifest semver derived at runtime); there is no authoritative "v1.0.0". Roadmap is "2.x" / "a future 2.x release".',
    // Requires the "v" in "post-v1.0": bare "post-1.0" is legitimate
    // stabilization-after-1.x copy ("the post-1.0 cleanup of the Envoy shim").
    re: /\bpost-v1\.0\b|\b2\.0 v1\.0\b|\bNot in v1\.0\.0\b|\bv1\.0\.0 (?:roadmap|release|of)\b|\bModPageSpeed 2\.0 1\.0(?:\.0)?\b/i,
    // Corrective form: copy that DENIES the phantom label exists. Anchored on
    // the denial sitting directly on "v1.0.0" ("there is no v1.0.0 release",
    // "not a v1.0.0"), never on a stray "no"/"not" elsewhere in the sentence.
    negationExempt: /\b(?:no|not an?)\s+v1\.0\.0\b/i,
    exemptionMustNotDisarm: [
      // The denial governs the SECOND clause (and a different product);
      // the phantom label in clause 1 is still false.
      'Planned for the post-v1.0 milestone. There is no v1.0.0 of Cyclone yet.',
    ],
    bad: 'Planned for the post-v1.0 milestone of ModPageSpeed 2.0.',
    good: 'Bundled libpng 1.6.58 and an Envoy 1.0.0 shim; before 2.0.14 the default differed.',
    variantsBad: ['Shipped in v1.0.0 of the 2.0 line.', 'Deferred to ModPageSpeed 2.0 1.0.0.'],
    variantsGood: [
      'Upgrade to 2.0.16+ and confirm; the Envoy 1.0.0 shim is internal.',
      'There is no v1.0.0 release of ModPageSpeed 2.0; GA was v2.0.0.', // corrective
      'The post-1.0 cleanup of the Envoy shim landed in 2.0.16.', // legit stabilization copy (no "v")
    ],
  },
  {
    id: 'b1-worker-as-loadable-nginx-module',
    // RULE: the optimizer worker is a separate process that optimizes off the
    // request path. It is never loaded into the web server. The IN-PROCESS
    // part — the module — is the loadable Apache/nginx module, so the phrase
    // itself is correct copy and misleads only when the WORKER is its subject:
    // a reader told to load the worker into nginx goes looking for a module
    // that does not exist, and misses the service that actually does the work.
    why: 'The optimizer worker runs as its own process alongside the web server; it is not something you load into nginx. The module is the loadable part, so this rule requires the worker as the subject — the bare phrase is correct copy about the module.',
    re: new RegExp(
      [
        `\\b${WORKER_SUBJECT}\\b${CLAUSE_GAP}{0,80}?(?:drop-in nginx (?:image )?optimization module|loadable nginx module|drop-in module for nginx|nginx module you (?:load|compile|install))`,
        `(?:drop-in nginx (?:image )?optimization module|loadable nginx module|drop-in module for nginx)${CLAUSE_GAP}{0,40}?\\b${WORKER_SUBJECT}\\b`,
      ].join('|'),
      'i',
    ),
    bad: 'The optimizer worker is a drop-in nginx image optimization module you load_module.',
    good: 'The module is a drop-in nginx image optimization module; the optimizer worker runs beside it as its own process.',
    variantsBad: [
      'The optimizer worker is a loadable nginx module.',
      'The optimizer worker is a drop-in module for nginx.',
      'Install pagespeed-optimizer as a drop-in nginx optimization module.',
      // The bare name the copy actually uses — this read as a no-subject
      // sentence until "the worker" joined WORKER_SUBJECT.
      'The worker is a loadable nginx module.',
    ],
    variantsGood: [
      // The module IS a drop-in nginx module — the bare phrase must not fire
      // (no worker subject):
      'A drop-in nginx image optimization module — install the module from the signed apt/yum repository.',
      'The drop-in nginx optimization module for Apache, nginx and IIS: mod_pagespeed.',
      // Both parts named, each doing its own job — the clause boundary keeps
      // the module's phrase away from the worker:
      'The optimizer worker runs as its own service; the module is the loadable nginx module.',
      'A drop-in nginx image optimization module: install the module, then the optimizer worker.',
      // "the worker image" is the container, not the process — the bare-name
      // subject must not reach it.
      'The worker image ships the optimizer worker; the module is the loadable nginx module.',
    ],
  },
  {
    id: 'b2-worker-near-load_module-or-dynamic-module',
    // RULE: `load_module` loads the in-process module into nginx. The
    // optimizer worker is started by its own service unit and talks to the
    // module over a socket, so copy that puts the worker next to load_module,
    // or calls it a dynamic module, sends a reader hunting for a directive
    // that does not exist for it.
    why: 'The optimizer worker is started as its own service, not loaded into an existing nginx with load_module. Only the module is loaded that way.',
    re: new RegExp(
      [
        `\\b${WORKER_SUBJECT}\\b${CLAUSE_GAP}{0,80}?(?:load_module|dynamic module on an? existing nginx|(?:dynamic|loadable)(?: nginx)? module)`,
        `(?:load_module|dynamic module on an? existing nginx)${CLAUSE_GAP}{0,80}?\\b${WORKER_SUBJECT}\\b`,
      ].join('|'),
      'i',
    ),
    // Corrective forms for THIS claim, all anchored on the denied object
    // (load_module / dynamic module / nginx config) rather than a bare token:
    //   "the optimizer worker does not use a load_module directive"
    //   "there is no load_module line for the worker" / "has no load_module directive"
    //   "the worker needs no nginx config"
    //   "the worker is not installed as a dynamic module"
    // plus the Unlike/Whereas contrast where the module owns that clause.
    // CLAUSE_GAP, not [^\n]: a gap that runs to its character limit reaches
    // past the end of the denial it is supposed to license and picks up a
    // `load_module` belonging to the next statement. Paired with
    // `exemptScope` below, which stops the same reach at a comma.
    negationExempt: new RegExp(
      [
        `\\b(?:does not use|do(?:es)? not need|has no|have no|is no|there is no|needs? no|is not installed as|is not loaded (?:as|with|into)|not use)\\b${CLAUSE_GAP}{0,40}?(?:load_module|dynamic module|loadable module|nginx config)`,
        `\\b(?:Unlike|Whereas)\\b${CLAUSE_GAP}{0,80}?\\bthe module\\b`,
        `\\b(?:Unlike|Whereas)\\b${CLAUSE_GAP}{0,80}?\\bload_module\\b`,
      ].join('|'),
      'i',
    ),
    // "Whereas the module ships from apt, the optimizer worker is a dynamic
    // module you load_module." — the contrast governs the part before the
    // comma; the claim after it is still false. Scoping the exemption to the
    // match's own comma-part is what stops a leading "Whereas the module …,"
    // from switching this rule off for the rest of the sentence.
    exemptScope: 'comma-part',
    exemptionMustNotDisarm: [
      // The corrective statement is about the container image, in clause 2;
      // the load_module claim in clause 1 is still false.
      'Add the optimizer worker with a load_module directive. The worker image needs no nginx config.',
      // The Unlike/the-module contrast governs clause 2 only.
      'Install the optimizer worker as a dynamic module on an existing nginx. Unlike the module, it needs no restart.',
      // A COMMA, not a semicolon: the true denial in part two does not
      // license the dynamic-module claim in part one.
      'The optimizer worker is a dynamic module, and there is no load_module line for the module.',
      // …and the contrast leading the sentence governs only its own part —
      // prefixing "Whereas the module …," must not switch the rule off for
      // everything after the comma.
      'Whereas the module ships from apt, the optimizer worker is a dynamic module you load_module.',
    ],
    bad: 'Install the optimizer worker as a dynamic module on an existing nginx with load_module.',
    good: 'The nginx module adds a load_module line; the package auto-includes it.',
    variantsBad: [
      'The optimizer worker plugs into your existing nginx as a dynamic module.',
      'Add the optimizer worker with a load_module directive.',
      'Load pagespeed-optimizer as a dynamic nginx module.',
      // The bare name, which the subject list previously did not know.
      'The worker installs as a dynamic module on an existing nginx with load_module.',
    ],
    variantsGood: [
      // Corrective / contrast — must be exempt by negation/contrast:
      'The optimizer worker does not use a load_module directive — it runs as its own service.',
      "Unlike the module's load_module line, the optimizer worker needs no nginx config.",
      // Both parts in one sentence, each described correctly — the clause
      // boundary keeps load_module attached to the module:
      'Add the module with load_module; the optimizer worker starts from its own unit file.',
      // A worker THREAD is a different thing that shares the noun.
      'The worker thread pool in your application is unrelated to the load_module line.',
    ],
  },
  {
    id: 'b3-worker-not-from-apt-or-yum',
    // RULE: the polarity of this claim INVERTED when the two parts became one
    // product. The `pagespeed-optimizer` worker now ships from the same signed
    // apt/yum repository as the module, and the two install and upgrade as a
    // matching pair (releases-2.1/release.yaml `artifacts.docker` + the native
    // deb/rpm note; docs/getting-started.md; docs/release-notes-2-1.mdx). So
    // the misleading statement is no longer "the engine installs from apt" —
    // it is the DENIAL carried over from when the out-of-process engine was
    // container-only: "the worker ships only as an image", "there is no apt
    // package for the worker". That sends a reader to Docker for a part their
    // package manager already installs, and splits one product back into two
    // install stories.
    why: 'The pagespeed-optimizer worker ships from the same signed apt/yum repository as the module — the two install and upgrade as a matching pair, alongside the container images and the Helm chart. Copy that denies the worker has native packages, or scopes it to containers only, sends readers down an install path they do not need.',
    re: new RegExp(
      [
        // "the optimizer worker is not in the apt repository" / "has no deb package"
        `\\b${WORKER_SUBJECT}\\b${CLAUSE_GAP}{0,60}?\\b(?:is not|isn['’]t|are not|aren['’]t|does not|doesn['’]t|do not|don['’]t|cannot|can['’]t|won['’]t|will not|never|no)\\b${CLAUSE_GAP}{0,40}?\\b(?:apt|yum|deb|rpm|signed repositor\\w+|native packages?)\\b`,
        // "the optimizer worker ships only as a container image".
        // `only` must be the free-standing adverb, not the tail of a
        // hyphenated compound: "without the worker, providing HTML-only
        // optimizations (critical CSS, image dimensions…)" is a true sentence
        // about the middleware, and its "HTML-only" plus a later "image" is
        // not a packaging claim.
        `\\b${WORKER_SUBJECT}\\b${CLAUSE_GAP}{0,60}?(?<!-)\\bonly\\b${CLAUSE_GAP}{0,30}?\\b(?:Docker|container|image|Helm|NuGet)\\b`,
        // …and the same claim compounded the other way round, which is how
        // it is usually written: "pagespeed-optimizer is container-only".
        // The gap is tempered against a negation token because, unlike the
        // branches above, this shape has a common TRUE form that denies it
        // ("the optimizer worker is not a container-only component"), and the
        // denial sits between the subject and the compound.
        `\\b${WORKER_SUBJECT}\\b(?:(?!\\b(?:not|never|no|isn['’]t|aren['’]t)\\b)${CLAUSE_GAP}){0,60}?\\b(?:Docker|container|image|Helm|NuGet)[- ]only\\b`,
        // "there is no apt package for the optimizer worker"
        `\\bno\\b${CLAUSE_GAP}{0,25}?\\b(?:apt|yum|deb|rpm)\\b${CLAUSE_GAP}{0,40}?\\b${WORKER_SUBJECT}\\b`,
      ].join('|'),
      'i',
    ),
    // Windows has no apt/yum at all, so a denial scoped to Windows/IIS states a
    // platform fact rather than the worker's packaging. Nothing else licenses
    // the denial — the probes below hold the exemption to the clause it sits in.
    exemptIf: /\b(?:Windows|IIS|MSI)\b/i,
    // …and to the denial's OWN comma-part within that clause. The programme
    // mandates putting "The IIS package ships from the 1.15 packaging
    // channel." beside platform statements, so a bare Windows/IIS token is
    // available to disarm this rule in almost any sentence a writer would
    // naturally produce — one comma instead of the semicolon the first probe
    // uses was enough. The exempting platform now has to sit with the denial,
    // not merely later in the same sentence.
    exemptScope: 'comma-part',
    exemptionMustNotDisarm: [
      // "IIS" is real here but governs the SECOND clause; the packaging denial
      // in clause 1 still misleads.
      'The optimizer worker is not in the apt repository; the IIS package ships from the 1.15 packaging channel.',
      // Span-widening probe: the regex window reaches past the boundary, so the
      // scope must key on where the match STARTS.
      'The optimizer worker has no deb package. Windows is served by an MSI.',
      // The same disarm through a COMMA instead of that semicolon — and with
      // the exact sentence the programme mandates putting next to platform
      // statements, which is what made this the easiest exemption to abuse.
      'The optimizer worker is not in the yum repository on Linux, and the IIS package ships from the 1.15 packaging channel.',
      // A trailing contrast that merely NAMES the IIS module is not a denial
      // scoped to Windows either.
      'pagespeed-optimizer has no deb package, unlike the IIS module.',
    ],
    bad: 'The optimizer worker is not in the apt and yum repositories — it ships only as a container image.',
    good: 'The module and the `pagespeed-optimizer` worker install and upgrade as a matching pair from the signed apt/yum repository.',
    variantsBad: [
      'The optimizer worker ships only as a container image.',
      'The pagespeed-optimizer worker for 2.0.x ships only as a container image.',
      'There is no apt package for the optimizer worker — use Docker.',
      'The optimizer worker is not available from the signed repository.',
      'pagespeed-optimizer does not ship as a deb or rpm package.',
      // The compound form of the container-only claim.
      'pagespeed-optimizer is container-only; it is not in the signed repository.',
      // The bare name, which the subject list previously did not know.
      'The worker ships only as a container image.',
    ],
    variantsGood: [
      // The true packaging story, in the shapes real copy uses:
      'Both parts install from the signed apt/yum repository: the module, and the pagespeed-optimizer worker beside it.',
      'The optimizer worker also ships as a container image, for deployments that prefer one.',
      // Windows genuinely has no apt/yum — the denial is a platform fact:
      'The optimizer worker has no apt or yum packages on Windows; the IIS package ships from the 1.15 packaging channel.',
      // A denial whose subject is the ASP.NET Core middleware, not the worker:
      'The ASP.NET Core middleware ships from NuGet and never from apt or yum.',
      // The denial OF the container-only claim is the corrective copy:
      'The optimizer worker is not a container-only component; it installs from the signed apt and yum repositories.',
      // "only" as the tail of a hyphenated compound is not the packaging
      // adverb — this is a true sentence about the middleware:
      'The middleware also runs in standalone mode without the worker, providing HTML-only optimizations (critical CSS, image dimensions, preload hints).',
    ],
  },
  {
    id: 'c-nginx-1.26-plus-as-module-range',
    why: 'The 1.15 native module is per-distro, pinned to each distro\'s STOCK nginx (1.18.0 … 1.26.3). It is NOT an open "nginx 1.26+" / "1.26 or newer" range.',
    re: /(?:mod_pagespeed 1\.15|1\.15 (?:nginx )?module|native module)[\s\S]{0,60}?nginx 1\.26(?:\+|\s*(?:or newer|or later|and later|and up|minimum))|nginx 1\.26(?:\+|\s*(?:or newer|or later|and later|and up|minimum))[\s\S]{0,60}?(?:mod_pagespeed 1\.15|1\.15 (?:nginx )?module|native module)|nginx >=?\s?1\.26\b/i,
    bad: 'The mod_pagespeed 1.15 native module supports nginx 1.26+ on every distro.',
    good: 'The mod_pagespeed 1.15 native module is pinned per distro, stock nginx 1.18 to 1.26.3.',
    variantsBad: [
      'The 1.15 module supports nginx 1.26 or newer.',
      'The native module requires nginx 1.26+ (stable).',
    ],
    variantsGood: [
      // Debian 13 trixie's stock nginx IS 1.26.3 — describing that per-distro build
      // as "nginx 1.26 (stable channel)" is accurate and closed, not an open range:
      'On Debian 13 the 1.15 native module is built for nginx 1.26 (stable channel).',
    ],
  },
  {
    id: 'd-apache-el9-arm64-overclaim',
    why: 'Apache yum repo on el9 (AlmaLinux/RHEL 9) is x86_64 ONLY; arm64 is via direct download. amd64+arm64 is the apt (Debian/Ubuntu) repo only.',
    // Branch 1: the explicit "(amd64 + arm64)" overclaim attached to el9 / RHEL 9
    // / AlmaLinux 9 (the exact PR-#705 finding + synonyms). Branch 2: an
    // affirmative verb tying arm64 INTO the el9 yum repo. Branch 3: arm64
    // in/from/via the el9 yum repo. The negation/contrast exemption + the tight
    // proximity clear correct arch-split descriptions.
    // Inter-token gaps use [^.;\n] so a clause boundary (`.` or `;`) stops a
    // match — "el9 yum carries x86_64; arm64 from apt" must not chain "carries"
    // (which governs x86_64) to the arm64 in the NEXT clause.
    re: /(?:AlmaLinux\/)?(?:RHEL ?9|el9|AlmaLinux ?9) ?\(amd64 ?\+ ?arm64\)|(?:el9|RHEL ?9|AlmaLinux ?9)\b[^.;\n]{0,40}?\byum\b[^.;\n]{0,25}?(?:include|includes|have|has|offers?|ships?|provides?|carr(?:y|ies)|supports?|gets?)\b[^.;\n]{0,15}?\barm64\b|\barm64\b[^.;\n]{0,20}?\b(?:in|from|via)\b[^.;\n]{0,20}?(?:el9|RHEL ?9|AlmaLinux ?9)\b[^.;\n]{0,20}?\byum\b/i,
    // Corrective form: copy stating arm64 is NOT in the el9 yum repo. Branch 3
    // ("arm64 in/from/via … el9 … yum") matches the shape of that denial, so it
    // needs the exemption. Anchored on the negation directly governing arm64's
    // presence — an affirmative overclaim carries no such denial.
    // (Beyond the a/b2/b3 set the task predicted: FP_CORPUS "Note: arm64 RPMs
    // are not in the RHEL 9 yum repo — fetch them from the release archive
    // instead." forced this.)
    negationExempt: /\barm64\b[^\n]{0,30}?\b(?:not|never)\b[^\n]{0,15}?\b(?:in|from|via)\b/i,
    exemptionMustNotDisarm: [
      // The denial is about the APT repo, in clause 2. The el9-yum overclaim in
      // clause 1 stands and must flag. (Doubles as a span-widening probe.)
      'The el9 yum repo also offers arm64 RPMs. arm64 is not in the apt repo.',
    ],
    bad: 'Apache ships from the yum repo for AlmaLinux/RHEL 9 (amd64 + arm64).',
    good: 'The signed yum repo (AlmaLinux/RHEL 9) ships x86_64; for arm64 / aarch64 RPMs, use the direct-download fallback below.',
    variantsBad: [
      'el9 (amd64 + arm64) RPMs are signed in the yum repo.',
      'The el9 yum repo also offers arm64 RPMs.',
    ],
    variantsGood: [
      // Correct arch-split descriptions — must NOT fire (token order / clause boundary / x86_64-only):
      'The yum repo serves x86_64 RPMs on RHEL 9; arm64 (aarch64) is available only through the apt channel on Debian/Ubuntu.',
      'On RHEL 9, the yum repo carries x86_64 packages; for arm64, build from source or use the apt-based images.',
      'On RHEL 9 the signed yum repo is x86_64 only, while the apt repo also offers arm64 builds for Debian and Ubuntu.',
      'The el9 yum repo carries x86_64; arm64 ships from the apt repo only.',
    ],
  },
  {
    id: 'e-2.0-dotnet-9-requirement',
    why: 'The ModPageSpeed 2.0 ASP.NET Core middleware targets net8.0 + net10.0 (ASPNETCORE_TFMS) — .NET 8 or .NET 10, NOT .NET 9. Only a REQUIREMENT claim is wrong; a "skipped .NET 9"/"newer than .NET 9" comparison is correct.',
    // Requires an UNAMBIGUOUS requirement-verb tying 2.0 to .NET 9
    // (targets/requires/needs/built for), or ".NET 9 or newer"/"9.0+". The bare
    // "on"/"runs on"/"supports"/"built on" verbs were dropped: they collide with
    // accurate copy ("2.0 runs on your servers", "supports .NET 9 apps as a
    // sidecar", "builds on .NET 9-era tooling") that does NOT claim 2.0's own
    // runtime is .NET 9. A skip/comparison ("skipped .NET 9", "newer than .NET 9")
    // carries no requirement verb, so the regex's own directionality clears it —
    // rule e needs no exemption of its own.
    re: /ModPageSpeed 2\.0[\s\S]{0,40}?(?:targets?|requires?|needs?|built for)\s+\.NET ?9\b|ModPageSpeed 2\.0[\s\S]{0,40}?\.NET ?9(?:\.0)?\s*(?:\+|or (?:newer|later))|ModPageSpeed 2\.0[\s\S]{0,40}?\b9\.0\+/i,
    bad: 'ModPageSpeed 2.0 targets .NET 9 and later.',
    good: 'ModPageSpeed 2.0 targets .NET 8 and .NET 10 (net8.0 and net10.0).',
    variantsBad: ['Run ModPageSpeed 2.0 on .NET 9 or newer.', 'ModPageSpeed 2.0 requires .NET 9.'],
    variantsGood: [
      // Corrective / comparison / coexistence — must NOT fire:
      'ModPageSpeed 2.0 does not support .NET 9; it targets net8.0 and net10.0.',
      'ModPageSpeed 2.0 skipped .NET 9: it targets the net8.0 and net10.0 LTS releases.',
      'ModPageSpeed 2.0.x runs on net10.0, which is newer than .NET 9.',
      'ModPageSpeed 2.0.x runs on .NET 9 hosts too, even though the package targets net8.0 and net10.0.',
      'ModPageSpeed 2.0.x supports .NET 9 apps as a sidecar while the middleware itself targets net8.0/net10.0.',
      'ModPageSpeed 2.0 builds on .NET 9-era runtime improvements, shipping for net8.0 and net10.0.',
    ],
  },
  {
    id: 'f1-avif-exclusive-to-2.0',
    anchor: ANCHOR_AVIF,
    why: 'mod_pagespeed 1.15 SHIPS AVIF encoding (Apache, nginx, IIS native). AVIF is NOT exclusive to ModPageSpeed 2.0 — see IMAGE_FORMAT_SUPPORT in src/data/product-facts.mjs. Still 2.0-only: SVG auto-vectorization, Jpegli, ML-predicted quality, variant-aware caching.',
    // No negationExempt: the wrong claim is often itself negation-shaped ("this
    // package does not do AVIF — AVIF is 2.0 only"), so ANY negation tolerance
    // would neuter this rule. Exemptions live in exemptIf instead.
    // All branches are DIRECTIONAL — AVIF must be the SUBJECT being scoped to
    // 2.0. That keeps correct copy that mentions a real 2.0-only feature and
    // then AVIF ("…remain 2.0-only; AVIF now ships across 1.15 and 2.0") clear.
    re: new RegExp(
      [
        // "AVIF is ModPageSpeed 2.0 only" / "AVIF: 2.0-only"
        /\bAVIF\b[^\n]{0,60}?\b(?:ModPageSpeed )?2\.0[ -]only\b/.source,
        // "AVIF is available only in / only on / only from 2.0"
        /\bAVIF\b[^\n]{0,60}?\bonly\b[^\n]{0,20}?\b(?:in|on|from|with|via)\b[^\n]{0,20}?(?:ModPageSpeed |the )?2\.0\b/
          .source,
        // "Only ModPageSpeed 2.0 produces AVIF"
        /\bonly\b[^\n]{0,20}?(?:ModPageSpeed |the )?2\.0\b[^\n]{0,35}?\bAVIF\b/.source,
        // "AVIF is exclusive to 2.0"
        /\bAVIF\b[^\n]{0,40}?\bexclusive(?:ly)?\b[^\n]{0,25}?2\.0\b/.source,
        // "AVIF ships only here" (a 2.0-page "here")
        /\bAVIF\b[^\n]{0,30}?\bonly here\b/.source,
        // "AVIF requires / needs ModPageSpeed 2.0"
        /\bAVIF\b[^\n]{0,30}?\b(?:requires?|needs?)\b[^\n]{0,20}?(?:ModPageSpeed |the )?2\.0\b/
          .source,
        // The vs/* contrast cell: WebP scoped to both lines, AVIF fenced to 2.0
        // ("…(1.15 + 2.0); AVIF in 2.0", "…across 1.15 and 2.0, AVIF from the
        // 2.0 worker"). The connector must sit DIRECTLY after AVIF, so
        // "On 1.15 AVIF is opt-in; in 2.0 it is on by default" does not fire.
        /\b1\.15\b[^\n]{0,90}?\bAVIF\b[\s,—-]{0,3}\b(?:in|from)\b\s+(?:the |ModPageSpeed )?2\.0\b/
          .source,
      ].join('|'),
      'i',
    ),
    // A claim correctly scoped to the ARCHIVED open-source build (which
    // genuinely never had AVIF) is accurate and must survive.
    // NARROWED: bare \bGoogle\b is gone. The entire site is about Google
    // heritage, so that token appears in ordinary marketing prose and disarmed
    // f1 wherever it did — "Google never shipped AVIF, and AVIF remains
    // exclusive to ModPageSpeed 2.0" passed while being false. What actually
    // licenses the claim is the ARCHIVE marker (archived / 1.13.35.2 /
    // 1.14.36.1 / upstream), not the word Google. Note "original" is
    // deliberately NOT an archive marker: "Unlike Google's original, AVIF is
    // 2.0 only" is false and must flag.
    exemptIf: /1\.13\.35\.2|1\.14\.36\.1|\barchived\b|\bupstream\b/i,
    exemptionMustNotDisarm: [
      // "Google" appears naturally; the exclusivity claim is still false.
      'Google never shipped AVIF, and AVIF remains exclusive to ModPageSpeed 2.0',
      "Unlike Google's original, AVIF is ModPageSpeed 2.0 only",
    ],
    bad: 'AVIF is ModPageSpeed 2.0 only.',
    good: 'AVIF encoding is available across 1.15 and 2.0.',
    variantsBad: [
      'This package does not do AVIF — AVIF is ModPageSpeed 2.0 only.',
      'AVIF ships only here.',
      'AVIF (e.g. for product photos) is ModPageSpeed 2.0 only.',
      'AVIF is exclusive to ModPageSpeed 2.0.',
      'AVIF is available only in ModPageSpeed 2.0.',
      'AVIF requires ModPageSpeed 2.0.',
      'Only ModPageSpeed 2.0 produces AVIF.',
      'WebP + responsive variants at origin (1.15 + 2.0); AVIF in 2.0',
      'WebP conversion across 1.15 and 2.0, AVIF from the 2.0 worker.',
      'SVG auto-vectorization, Jpegli, and AVIF are 2.0-only.',
    ],
    variantsGood: [
      // Correctly scoped to Google's archived 1.13.35.2 build — still true:
      "Google's archived 1.13.35.2 build never had AVIF; the maintained 1.15 line does.",
      'AVIF was only in ModPageSpeed 2.0 back when the archived Google build was current.',
      // SVG auto-vectorization really is 2.0-only — must not fire:
      'SVG auto-vectorization is ModPageSpeed 2.0 only.',
      'SVG auto-vectorization, Jpegli, and ML-predicted quality remain 2.0-only; AVIF now ships across 1.15 and 2.0.',
      // The package-scoped "no AVIF" statements — still true (see f2):
      'This package does not do AVIF; use the IIS module on Windows.',
      // Correct edition scoping (what editionClause() renders):
      'WebP and AVIF across 1.15 and 2.0, SVG in 2.0.',
      'On 1.15 AVIF is opt-in; in 2.0 it is on by default.',
    ],
  },
  {
    id: 'f2-1.15-does-not-ship-avif',
    anchor: ANCHOR_AVIF,
    why: 'The inverse form: denying that mod_pagespeed 1.15 does AVIF. 1.15 ships AVIF encoding on Apache, nginx (standard and lite) and the native IIS module (opt-in via four filters outside rewrite_images/CoreFilters). The old "ROADMAP lists AVIF as Planned" claim is stale.',
    // No negationExempt: every phrasing of this claim IS a negation, so any
    // negation tolerance would zero the rule. The legitimate "X has no AVIF"
    // statements (Google's archived build, the NuGet package) are in exemptIf.
    re: new RegExp(
      [
        // "1.15 does not ship / cannot produce / will never encode AVIF"
        /\b1\.1(?:5)?\b[^\n]{0,60}?\b(?:does not|doesn['’]t|do not|don['’]t|cannot|can['’]t|won['’]t|will not|never)\b[^\n]{0,25}?\b(?:ship|ships|do|does|support|supports|have|has|produce|produces|emit|emits|generate|generates|include|includes|encode|encodes|output|outputs)\b[^\n]{0,20}?\bAVIF\b/
          .source,
        // "no AVIF in / for mod_pagespeed 1.15"
        /\bno AVIF\b[^\n]{0,30}?\b(?:in|on|for|from)\b[^\n]{0,25}?(?:mod_pagespeed )?1\.1(?:5)?\b/
          .source,
        // "1.15 lacks AVIF"
        /\b1\.1(?:5)?\b[^\n]{0,40}?\blacks?\b[^\n]{0,20}?\bAVIF\b/.source,
        // "AVIF is not available / not supported in 1.15"
        /\bAVIF\b[^\n]{0,40}?\bnot (?:available|supported|present|shipped|implemented)\b[^\n]{0,30}?1\.1(?:5)?\b/
          .source,
        // The stale ROADMAP claim: AVIF "planned" / "on the roadmap" for 1.15
        /\b1\.1(?:5)?\b[^\n]{0,60}?\bAVIF\b[^\n]{0,30}?\b(?:is |as )?(?:still )?(?:planned|on the roadmap)\b/
          .source,
      ].join('|'),
      'i',
    ),
    // Exempt the denials that remain TRUE: Google's archived open-source build,
    // and the WeAmp.PageSpeed.AspNetCore NuGet package (a 2.0 product whose
    // win-x64 build lacks libaom, so it does not inherit AVIF from 1.15).
    // NOT exempt any more: the ASP.NET Core Sidecar. It bundles nginx with the
    // pagespeed module, so it carries the same opt-in AVIF filters -- "the
    // Sidecar does not do AVIF" is now FALSE and must be caught. The old blanket
    // /packages?/ exemption is gone with it: it re-opened the same hole by
    // matching "the published sidecar packages ...".
    // NARROWED twice:
    //   - \bngx_pagespeed\b removed. It re-opened the exact hole the /packages?/
    //     removal closed: this rule's own `why` says 1.15 ships AVIF ON NGINX,
    //     and ngx_pagespeed IS that nginx module. "ngx_pagespeed 1.15 does not
    //     ship AVIF" passed while being false.
    //   - \bGoogle\b removed, same reasoning as f1: the archive markers license
    //     the denial, the word Google does not.
    // What remains is genuinely exempt: the archived open-source build, and the
    // WeAmp.PageSpeed.AspNetCore NuGet package (a 2.0 product whose win-x64
    // build lacks libaom, so it does not inherit AVIF from 1.15).
    exemptIf: /1\.13\.35\.2|1\.14\.36\.1|\barchived\b|\bupstream\b|\bAspNetCore\b|\bNuGet\b/i,
    exemptionMustNotDisarm: [
      // ngx_pagespeed IS the 1.15 nginx module, and it ships AVIF.
      'ngx_pagespeed 1.15 does not ship AVIF',
    ],
    bad: 'mod_pagespeed 1.15 does not ship AVIF.',
    good: 'mod_pagespeed 1.15 ships AVIF on Apache, nginx, and the native IIS module.',
    variantsBad: [
      'The 1.15 module cannot produce AVIF.',
      'There is no AVIF in mod_pagespeed 1.15.',
      '1.15 lacks AVIF support.',
      'AVIF is not supported in 1.15.',
      'For 1.15, AVIF is still planned.',
      'mod_pagespeed 1.15.x does not ship AVIF.',
      '1.15.0-rc.5 does not ship AVIF.',
    ],
    variantsGood: [
      // Google's archived open-source build genuinely never had AVIF:
      "Google's archived 1.13.35.2 build does not ship AVIF.",
      'The archived open-source line never had an AVIF encoder; 1.15 added one.',
      // Package-scoped denial that is STILL true (2.0 product, win-x64 has no libaom):
      'The WeAmp.PageSpeed.AspNetCore NuGet package does not get AVIF from 1.15.',
      // Unrelated 2.0-only features:
      'mod_pagespeed 1.15 does not do SVG auto-vectorization; that is 2.0-only.',
      // Correct:
      'mod_pagespeed 1.15 ships AVIF, but the four AVIF filters are opt-in.',
      // The Sidecar bundles nginx + the pagespeed module, so it DOES do AVIF:
      'The ASP.NET Core sidecar transcodes AVIF through the same opt-in filters.',
    ],
  },
  {
    id: 'f3-zero-copy-and-variant-cache-are-not-2.0-only',
    anchor: ANCHOR_SHARED_CAPABILITIES,
    why: "Zero-copy serving and the variant-aware Cyclone cache are NOT 2.0-only. mod_pagespeed 1.15 ships zero-copy serving on nginx, Apache and IIS as of v1.15.0+r19 (opt-in via CycloneZeroCopy / CycloneZeroCopyServe), and 1.15 already varies its cache on client capability -- image format, mobile UA, Save-Data and small-screen. What is genuinely 2.0-only is NARROWER: tablet/desktop viewport classes, pixel density (Sec-CH-DPR), transfer-encoding alternates, and PROACTIVE generation of the full variant matrix. Re-anchor exclusivity onto those, never onto 'variant-aware caching' or 'zero-copy' as whole categories. This class of error already shipped once: AVIF was replaced as the 2.0 differentiator by claims that were themselves false. NOTE: the widened scan surfaced ONE real false positive, a corrective comment in src/data/product-facts.mjs that told future authors 'NOT 2.0-only, never add them to a 2.0-only list: variant-aware caching'. A per-rule comment opt-out was tried and REVERTED — blinding f3 across every comment line in the canonical fact record to spare one line is a terrible trade. The comment was reworded instead ('Ships on BOTH lines, never fence to 2.0: ...'), which states the same facts without the exclusivity shape. Reword; never blind the rule.",
    // No negationExempt: exclusivity claims are negation-shaped ("ship ONLY
    // here", "1.15 does NOT have it"), so negation tolerance would neuter this
    // rule to zero hits against live false copy -- exactly as for f1/f2.
    re: new RegExp(
      [
        // "variant-aware caching and zero-copy serving ship only here / only in 2.0"
        /\b(?:variant-aware|zero-copy)\b[^\n]{0,80}?\b(?:ships?|available|found|exists?)\b[^\n]{0,20}?\bonly\b/
          .source,
        // "only 2.0 has variant-aware caching / zero-copy serving"
        /\bonly\b[^\n]{0,40}?\b2\.0\b[^\n]{0,60}?\b(?:variant-aware|zero-copy)\b/.source,
        // "variant-aware caching / zero-copy is 2.0-only"
        /\b(?:variant-aware|zero-copy)\b[^\n]{0,60}?\b(?:is |are )?2\.0[-\s]only\b/.source,
        // "1.15 does not have / lacks zero-copy or the variant-aware cache"
        /\b1\.1(?:5)?\b[^\n]{0,60}?\b(?:does not|doesn['’]t|cannot|can['’]t|lacks?|without)\b[^\n]{0,30}?\b(?:variant-aware|zero-copy)\b/
          .source,
      ].join('|'),
      'i',
    ),
    // 2.0 genuinely owns the NARROWER properties, so exclusivity phrased around
    // those is legitimate. Also exempt the ASP.NET Core middleware, which really
    // does copy through the managed response stream rather than serving zero-copy.
    // NARROWED: \bviewport\b, \bdensity\b and bare \bmiddleware\b are gone.
    // They are THE TOPIC'S OWN VOCABULARY — the sentences this rule exists to
    // catch are about viewport/density-scoped variant caching, so those tokens
    // sit in the false claims as readily as in the true ones and disarmed f3 on
    // ~13% of relevant lines. "Viewport-aware variants and zero-copy serving
    // ship only in 2.0" passed while being false. The genuinely-2.0-only
    // properties that DO license an exclusivity claim are the narrower
    // mechanisms below (Sec-CH-DPR, proactive generation, warmup,
    // transfer-encoding alternates), and none of the true phrasings this rule
    // was measured against relies on the dropped tokens.
    // \bmiddleware\b survives only in its subject-changing sense: the AspNetCore
    // middleware really does COPY through the managed response stream rather
    // than serve zero-copy, so a denial scoped to it is true.
    exemptIf: new RegExp(
      [
        /\bSec-CH-DPR\b/.source,
        /\bproactive(?:ly)?\b/.source,
        /\bwarmup\b/.source,
        /\btransfer[-\s]encoding\b/.source,
        /\bAspNetCore\b/.source,
        // "the middleware copies / copy path" — the subject really is the
        // copying middleware, not zero-copy serving as a category.
        /\bmiddleware\b[^\n]{0,40}?\bcop(?:y|ies|ying)\b/.source,
        /\bcop(?:y|ies|ying)\b[^\n]{0,40}?\bmiddleware\b/.source,
      ].join('|'),
      'i',
    ),
    exemptionMustNotDisarm: [
      // Carries "viewport"; still a false exclusivity claim (1.15 has zero-copy).
      'Viewport-aware variants and zero-copy serving ship only in 2.0',
      // Carries "middleware"; the 1.15 denial is still false.
      'mod_pagespeed 1.15 lacks zero-copy serving; the middleware adds it',
    ],
    bad: 'Variant-aware caching and zero-copy serving ship only here.',
    good: 'Viewport- and density-aware variants, generated proactively rather than on demand, ship only here.',
    variantsBad: [
      'Only ModPageSpeed 2.0 has variant-aware caching.',
      'Zero-copy serving is 2.0-only.',
      'mod_pagespeed 1.15 lacks zero-copy serving.',
      'The 1.15 module does not have a variant-aware cache.',
    ],
    variantsGood: [
      // TRUE: 1.15 has both.
      'mod_pagespeed 1.15 reads from Cyclone zero-copy; in 2.0 the same cache is shared with the worker.',
      'Zero-copy serving is opt-in on 1.15 and available on nginx, Apache, and IIS.',
      // TRUE: the narrower 2.0-only properties.
      'Proactive generation of the full variant matrix ships only in 2.0.',
      'Viewport class and pixel density enter the cache key only in 2.0.',
      // TRUE: the ASP.NET Core middleware really does copy.
      'The AspNetCore middleware copies through the managed response stream instead of serving zero-copy.',
    ],
  },
  {
    id: 'f4-implicit-exclusivity-by-scoping',
    // The adjacency branch reads "2.0 gets X; 1.15 uses <WebP-only filters>."
    // up to the end of the text, and a sentence period is part of that tail.
    crossSentence: true,
    anchor: {
      describes: `${ANCHOR_AVIF.describes}; ${ANCHOR_SHARED_CAPABILITIES.describes}`,
      holds: (f) => ANCHOR_AVIF.holds(f) && ANCHOR_SHARED_CAPABILITIES.holds(f),
    },
    why:
      "IMPLICIT exclusivity: a capability is fenced to 2.0 without the word 'only' or 'exclusive', so f1/f2/f3 all miss it. This is the shape the two motivating defects actually had. " +
      "(1) PARENTHETICAL FENCE — src/data/psi-mps-mapping.ts:250 and :649 read 'WebP (and AVIF on 2.0)'. The parenthetical scopes AVIF to 2.0 while WebP stays unscoped, so the reader concludes 1.15 has WebP but not AVIF. False: 1.15 ships AVIF too. Note the LINE never names 1.15 — the 1.1 scoping sits on neighbouring lines (availableIn, snippet11) — so a line-based rule must key on the fence itself, not on a 1.15 co-mention. " +
      '(2) ADJACENCY — src/data/cwv-pillars.ts:157 granted WebP+AVIF to 2.0 and then described 1.15 purely by WebP-producing filters (recompress_images + convert_jpeg_to_webp), granting it no AVIF. Same false implication, built entirely out of true-sounding parts. ' +
      'Target capabilities: AVIF, zero-copy serving, variant-aware caching.',
    re: (() => {
      // The capabilities that are NOT 2.0-exclusive and so must not be fenced.
      const CAP = /(?:AVIF|zero-copy(?:\s+serving)?|variant-aware(?:\s+caching)?)/.source;
      return new RegExp(
        [
          // (1) PARENTHETICAL FENCE: "WebP (and AVIF on 2.0)", "[plus AVIF in 2.0]".
          // The capability is bracketed off and tied to 2.0, which fences it away
          // from whatever the unbracketed subject is.
          //
          // TIGHTENED (an FP probe found 8 of 11 plausible ACCURATE sentences
          // firing here — routine release-note and blog shapes). Four guards:
          //   (i)  INCLUSION MARKER EXCLUDED (also/too/as well/both/alike).
          //        A bracket that is its own clause ("(AVIF is opt-in in 2.0
          //        as well)", "(variant-aware caching in 2.0 too)") is ordinary
          //        prose, and the inclusion word is precisely what says so: it
          //        asserts 2.0 ALSO has the capability, the opposite of fencing
          //        it there. An optional connector (and/plus/with/+) marks the
          //        appendage form but is NOT required — requiring it was tried
          //        and let a bare "WebP (AVIF on 2.0)" through, which is the
          //        motivating defect with one word removed.
          //   (ii) 2.0 must not be a VERSION PREFIX — "(AVIF in 2.0.21)",
          //        "(AVIF from 2.0.4 onward)", "(zero-copy on 2.0.14+)" are
          //        release refs, not fences. Same digit-flanked-period guard
          //        isClauseBoundary() uses.
          //   (iii) A MARKDOWN LINK "[AVIF in 2.0](/blog/…)" is linking, not
          //        fencing: a ']' followed by '(' does not close a fence.
          //   (iv) A bracket that CO-MENTIONS 1.15/1.1 is by definition not a
          //        fence, in EITHER order. Previously only the 1.15-first order
          //        was tolerated (by the {0,25} window), so "(zero-copy serving
          //        in 2.0 and 1.15)" fired while "(and AVIF on 1.15 and 2.0)"
          //        passed — pure ordering luck.
          `[([](?![^)\\]\\n]*\\b1\\.1(?:5)?\\b)(?![^)\\]\\n]*\\b(?:also|too|as well|both|alike)\\b)(?:(?:and|plus|with|\\+)\\s*)?${CAP}\\b[^)\\]\\n]{0,25}?\\b(?:on|in|from|with)\\b\\s*(?:ModPageSpeed\\s+)?2\\.0\\b(?!\\.\\d)[^)\\]\\n]{0,10}(?:\\)|\\](?!\\())`,
          // (2) ADJACENCY: 2.0 is granted the capability, then 1.15 is described
          // with a capability-granting verb and an image/format capability that
          // is NOT this one — and the capability never reappears for 1.15.
          `\\b2\\.0\\b[^\\n]{0,250}?\\b${CAP}\\b[^\\n]{0,500}?\\b(?:mod_pagespeed\\s+)?1\\.1(?:5)?\\b[^\\n]{0,60}?\\b(?:uses?|ships?|does|has|have|provides?|offers?|supports?|adds?|relies on)\\b(?=(?:(?!${CAP})[^\\n])*(?:WebP|convert_|recompress_|_images))(?:(?!${CAP})[^\\n])*$`,
        ].join('|'),
        'i',
      );
    })(),
    // No negationExempt: like f1/f2/f3, the claim shape is not a denial —
    // fencing is done affirmatively, so negation tolerance has nothing to add.
    bad: 'ModPageSpeed transcodes JPEG/PNG/GIF to WebP (and AVIF on 2.0), recompresses with quality-aware encoders.',
    good: 'mod_pagespeed transcodes JPEG/PNG/GIF to WebP and AVIF across 1.15 and 2.0.',
    variantsBad: [
      // (1) parenthetical fences, incl. the real :649 line:
      'JPEG/PNG/GIF transcoded to WebP (and AVIF on 2.0) when the client advertises support via `Accept`.',
      'Images are converted to WebP [plus AVIF in 2.0] on the fly.',
      'Serves responsive variants (with zero-copy serving on 2.0).',
      // (2) adjacency — capability granted to 2.0, 1.15 left with WebP filters:
      'ModPageSpeed 2.0 transcodes images to WebP and AVIF based on the client Accept header; mod_pagespeed 1.15 uses recompress_images + convert_jpeg_to_webp.',
    ],
    variantsGood: [
      // Both lines granted the capability — no fence:
      'WebP and AVIF across 1.15 and 2.0, SVG in 2.0.',
      '2.0 transcodes to WebP and AVIF; 1.15 ships the same AVIF filters opt-in.',
      // A genuinely 2.0-only capability may be fenced — SVG/Jpegli are not targets:
      'Images are converted to WebP (and SVG on 2.0).',
      'Vector output (Jpegli in 2.0) is a 2.0 addition.',
      // AVIF fenced to BOTH lines is not a fence at all:
      'JPEG/PNG/GIF transcoded to WebP (and AVIF on 1.15 and 2.0).',
    ],
  },
  {
    id: 'g-2.1-no-native-module',
    why: 'releases-2.1/release.yaml declares native deb/rpm packages for Apache and nginx, shipped from the signed apt/yum repository (the same channel mod_pagespeed 1.15 uses, per its GA-alias-naming note). A claim that the converged line has no standalone/native module is the exact regression a review round caught and fixed in installation-module.md; it must not resurface.',
    // The subject is the "2.1" line key or the current product by name (the
    // name alone once the retired label is gone from the copy).
    re: new RegExp(
      `(?:2\\.1\\b|${CURRENT_PRODUCT_SUBJECT})` +
        /[\s\S]{0,40}?(?:has no|does not have|doesn't have|lacks|is without)\s+(?:an?\s+)?(?:standalone|native)\s+(?:Apache(?:\/nginx| or nginx)?|nginx)\s+module/
          .source,
      'i',
    ),
    bad: 'mod_pagespeed has no standalone nginx module yet.',
    good: 'mod_pagespeed ships a native module for Apache and nginx, from the signed apt/yum repository.',
    variantsBad: [
      'mod_pagespeed 2.1 has no standalone nginx module yet.',
      'ModPageSpeed 2.1 does not have a native Apache or nginx module.',
      '2.1 has no native nginx module.',
      'mod_pagespeed 2 lacks a native Apache module.',
    ],
    variantsGood: [
      'The native Apache and nginx module ships from the signed packages.modpagespeed.com repository — the same channel mod_pagespeed 1.15 uses.',
      'mod_pagespeed ships a native module for Apache and nginx today.',
      'Since 2.1.0, mod_pagespeed ships a native module for Apache and nginx.',
    ],
  },
  {
    id: 'h-two-maintained-lines-phrasing',
    // RULE: the product is one continuously updated line, not two products a
    // reader must still choose between. A claim that "two" continuations,
    // successors, lines, products, or editions are "(actively) maintained" in
    // parallel, a bare "two product lines", or a bare either/or ("2.0 or
    // 1.15" / "1.15 or 2.0", including the converged "2.1" spelling),
    // restates the pre-convergence framing and misleads a reader into
    // thinking there is still a choice to make. Legitimate copy that
    // describes what converged (e.g. "converges the two lines … into one
    // product") is licensed via the exemptIf on "converges"/"converged" — a
    // promise that the lines *will* converge is not a licence.
    why: 'The product is a single converged line; asserting that two actively maintained continuations/successors/lines/products/editions still coexist, offering a bare "2.0 or 1.15" / "2.1 or 1.15" choice, or naming "two product lines", misleads readers into thinking they must still pick between two products.',
    re: /\btwo\b[^\n]{0,30}?(?:actively[- ])?maintained\b[^\n]{0,20}?(?:continuations?|successors?|lines?|products?|editions?)\b|\btwo\b[^\n]{0,20}?product lines?\b|\b(?:2\.[01] or 1\.15|1\.15 or 2\.[01])\b/i,
    exemptIf: /\bconverge[sd]\b/i,
    exemptionMustNotDisarm: [
      'Although the lines will converge, two actively maintained products exist today.',
      'Convergence is planned, but two actively maintained successors exist right now.',
    ],
    bad: 'We-Amp shipped two actively maintained continuations of mod_pagespeed.',
    good: 'mod_pagespeed converges the two lines We-Amp shipped through 2026 into one continuously updated product.',
    variantsBad: [
      'Two actively maintained successors exist today.',
      'Whether it runs 1.15 or 2.0, the same optimizations apply.',
      'Choose 2.1 or 1.15 depending on your platform.',
      'There are two product lines to choose from.',
    ],
    variantsGood: [
      'mod_pagespeed converges the two previously maintained lines into one always-updated release.',
    ],
  },
  {
    id: 'j-envoy-offered-on-the-current-line',
    // RULE: the module runs in Apache, nginx and IIS. The Envoy port belongs
    // to the predecessor line, was never shipped, and is excluded from CI —
    // which is why Envoy appears in no port list in the fact record. Naming it
    // in the same clause as the current product, or as the module, offers a
    // reader a front end they cannot deploy. A dated post describing the
    // predecessor's four front ends is a record and names neither subject.
    why: "The module's platforms are Apache, nginx and IIS. The Envoy port is not shipped and is excluded from CI, so naming Envoy in a clause about mod_pagespeed or about the module offers a deployment target that does not exist.",
    re: new RegExp(
      [
        `(?:${CURRENT_PRODUCT_SUBJECT}|the (?:native |in-process )?module)\\b${CLAUSE_GAP}{0,160}?\\bEnvoy\\b`,
        `\\bEnvoy\\b${CLAUSE_GAP}{0,160}?(?:${CURRENT_PRODUCT_SUBJECT}|the (?:native |in-process )?module)\\b`,
        // A colon-introduced list is the site's own idiom for naming
        // platforms — a card subline, a `description:` front-matter string, a
        // comparison row — and CLAUSE_GAP treats ':' as a boundary, so the
        // subject and the platform after the colon never met. These two
        // branches cross a colon, and ONLY a colon: the subject has to sit
        // directly in front of it ("mod_pagespeed: Apache, nginx, IIS and
        // Envoy"), or Envoy has to head the list that introduces the subject
        // ("Envoy users: install mod_pagespeed").
        `(?:${CURRENT_PRODUCT_SUBJECT}|the (?:native |in-process )?module)\\b\\s*:${COLON_LIST_GAP}{0,120}?\\bEnvoy\\b`,
        `\\bEnvoy\\b${COLON_LIST_GAP}{0,40}?:${COLON_LIST_GAP}{0,120}?(?:${CURRENT_PRODUCT_SUBJECT}|the (?:native |in-process )?module)\\b`,
      ].join('|'),
      'i',
    ),
    bad: 'The native module is available as an HTTP filter for Envoy.',
    good: 'The module runs in-process in Apache, nginx and IIS.',
    variantsBad: [
      'mod_pagespeed runs on Apache and nginx, plus an experimental Envoy port.',
      'mod_pagespeed 2 runs on Apache and nginx, plus an experimental Envoy port.',
      'The module in 2.x also runs inside Envoy.',
      'Envoy filter chain | mod_pagespeed (experimental)',
      'You can load the module into Envoy as an HTTP filter.',
      // Colon-introduced platform lists — the site's own card/front-matter
      // idiom, and invisible to the rule until the list branches were added.
      'mod_pagespeed: Apache, nginx, IIS and Envoy.',
      'Envoy users: install mod_pagespeed and enable the filter.',
    ],
    variantsGood: [
      // The predecessor line's four front ends, as a dated record — neither
      // the current product nor "the module" is the subject:
      '1.15 shipped one test framework across Apache, nginx, Envoy and IIS.',
      'We-Amp has shipped optimization code across Apache, nginx, Envoy and IIS since 2010.',
      // A clause boundary keeps a neighbouring Envoy sentence out of scope:
      'The module runs in Apache, nginx and IIS. An experimental Envoy port exists on the predecessor line.',
      // The dependency, not the front end:
      'libcurl links against the same BoringSSL that Envoy links against.',
      // The same colon list, naming only the platforms that exist:
      'mod_pagespeed: Apache, nginx and IIS.',
      // A colon list in the NEXT sentence — the clause boundary still holds:
      'The module runs in Apache, nginx and IIS. Envoy users: see the predecessor line.',
    ],
  },
  {
    id: 'i-retired-product-name-as-current-label',
    // The "<label> runs as ... . See [pricing]" branch pairs a subject with a
    // call to action in a LATER sentence by design (up to 300 characters): the
    // rule's own text says "a line that also tells the reader where to get it".
    // On a joined block that scope is the paragraph, which is the same idea.
    crossSentence: true,
    // RULE: the product is mod_pagespeed — one product in two parts, the
    // module and the optimizer worker. "ModPageSpeed 2.0", the bare camel
    // "ModPageSpeed" and "mod_pagespeed 1.15" are names the product no longer
    // goes by. Each survives only where it records something that was true
    // when it was written, or names a literal: a version (2.0.x, v2.0, an
    // image tag), a dated announcement, the state a reader is migrating FROM,
    // the fixed phrase "the 2.0 re-architecture", the JSON-LD alternate name
    // that keeps the old query findable, an identifier spelled that way in the
    // world (a WordPress plugin's display name, a systemd or repo-config
    // field), or one of the five dated post titles the blog still carries.
    // Anywhere else — a present-tense product subject, an install or run
    // imperative, a page title, an H1, a JSON-LD name, a call to action — it
    // offers a reader a product they cannot install under that name, and
    // splits one product back into a shelf of them.
    why: 'The product is mod_pagespeed, one product in two parts (the module and the optimizer worker). A retired name used as the current product\'s label, or as something to install, tells readers to go looking for a product that is not on offer. Allowed: version facts (2.0.x, v2.0, image tags), dated announcements, migration source states, the phrase "the 2.0 re-architecture", the JSON-LD alternate name, literal identifiers, and the dated post titles the blog keeps.',
    // CASE-SENSITIVE on purpose: this rule guards a SPELLING. Lower-cased,
    // "modpagespeed" is the domain name and "mod_pagespeed" is the current
    // product, so an /i flag would flag both wherever they appear. The
    // exemptions below carry their own case handling.
    re: new RegExp(
      [
        // Present-tense product subject: "ModPageSpeed 2.0 is / runs / ships …".
        /\bModPageSpeed 2\.0\s+(?:is|are|runs?|ships?|optimi[sz]es?|supports?|generates?|serves?|deploys?|handles?|uses?|adds?|keeps?|transcodes?|caches?|has|have)\b/
          .source,
        // Imperative — "Run / Install / Upgrade to <retired name>" — for either
        // retired line name. A Markdown link opener directly before the verb
        // means the phrase is a cited post title ("[Run ModPageSpeed 2.0 with
        // Docker Compose](/blog/…)"), not an instruction to the reader.
        /(?<!\[)\b(?:[Rr]un|[Ii]nstall|[Dd]eploy|[Tt]ry|[Gg]et|[Uu]pgrade to|[Ss]witch to|[Mm]igrate to|[Cc]hoose)\s+(?:ModPageSpeed 2\.0|mod_pagespeed 1\.1(?:5)?)\b/
          .source,
        // Label position: a page title, an H1 or a JSON-LD name carrying the
        // retired name is the product's name as search engines and readers see
        // it first.
        /(?:title=|title:|\bname:|"name":|headline:|<h1[^>]*>)[^\n]{0,60}(?:ModPageSpeed 2\.0|mod_pagespeed 1\.1(?:5)?)/
          .source,
        // The current product spelled with the retired camel.
        /\bModPageSpeed 2\.1\b/.source,
        // The camel brand as a bare product name. A version directly after it —
        // literal, or interpolated from the release manifest — makes it a
        // version fact, which the branches above judge on their own terms.
        /\bModPageSpeed\b(?!\s*(?:2\.0|\$?\{))/.source,
        // The retired abbreviation.
        /\bMPS ?2\.0\b/.source,
        // The predecessor line name as a present-tense product claim on a line
        // that also tells the reader where to get it. A dated post recording
        // what 1.15 shipped is a record; the same sentence next to a download,
        // pricing or install link is an offer, and the offer is the current product.
        /\bmod_pagespeed 1\.1(?:5)?\s+(?:is|are|runs?|ships?|optimi[sz]es?|supports?|serves?|handles?|transcodes?|caches?)\b[^\n]{0,300}?(?:\/download\/|\/pricing\/|\/license\/|[Dd]ownload|[Ii]nstall)/
          .source,
      ].join('|'),
    ),
    // Each exemption has to sit in the retired name's OWN comma-part, not
    // merely somewhere later in the sentence. Every allow-list term below is
    // a word a writer reaches for anyway, so appending one after a comma —
    // "ModPageSpeed 2.0 is the current product, see the migration guide for
    // details." — turned all four families into an off switch for the rule.
    // The probes at `exemptionMustNotDisarm` carry one comma-joined form per
    // family.
    exemptScope: 'comma-part',
    // ALLOW-LIST, scoped as above. Each entry is a form in which a retired
    // name states a fact rather than names the product on offer.
    exemptIf: new RegExp(
      [
        /\b2\.0\.[\dx]/.source, // a semver or a "2.0.x" version range
        /\bv2\.0\b/.source, // a release tag
        /ghcr\.io/.source, // a container image reference
        /\bre-architecture\b/.source, // the one sanctioned surviving noun phrase
        // Migration copy: the state a reader is upgrading FROM, and the guide
        // that covers it. Deliberately not bare "migrat" — "Migrate to
        // ModPageSpeed 2.0" is an offer, not a source state, and must flag.
        /[Mm]igrat(?:e|ed|ing|ion)\s+(?:from|guide|path)|migration guide/.source,
        /alternateName/.source, // the JSON-LD bridge that keeps the old query findable
        // Identifiers spelled that way in the world, not prose:
        /WeAmp Cache Control for ModPageSpeed/.source, // the plugin's display name
        /Settings (?:&rarr;|→) ModPageSpeed/.source, // the plugin's settings menu
        /(?:name|Description)=ModPageSpeed/.source, // repo-config and unit-file fields
        // The five dated post titles the blog keeps, in title or link
        // position. They are indexed, quoted verbatim in the agent files, and
        // rasterized into social cards; renaming one would rewrite what was
        // published on its date and break three surfaces that mirror it.
        ...FROZEN_TITLE_CITATION,
      ].join('|'),
    ),
    // Whole documents that are a record of a past line rather than a claim
    // about the product on offer. Pinned file-by-file below.
    exemptFile: (rel) =>
      /^src\/content\/docs\/release-notes(?:-\d+-\d+)?\.mdx$/.test(rel) ||
      /^src\/pages\/(?:privacy|terms|license)\.astro$/.test(rel) ||
      rel === 'public/llms-full.txt',
    exemptFileExpectation: [
      // The generated full agent file embeds the release-notes pages below
      // verbatim (every docs page, in fact). Its own prose is scanned at its
      // sources — the docs bodies, src/data/agent-files.mjs — and
      // test/sync/llms-generated.test.ts gates the generated index and the
      // non-archival pages for retired product framing.
      'public/llms-full.txt',
      // Release history: every entry is what shipped, under the name it
      // shipped under, on the date it shipped. The docs-1.1 collection and
      // the frozen /1.1/docs/[slug] route that also carried this exemption
      // are retired; the history is the release-notes index plus one page
      // per line (2.2, 2.1, the 2.0 worker, the 1.15 and 1.1 module).
      'src/content/docs/release-notes-1-1.mdx',
      'src/content/docs/release-notes-1-15.mdx',
      'src/content/docs/release-notes-2-0.mdx',
      'src/content/docs/release-notes-2-1.mdx',
      'src/content/docs/release-notes-2-2.mdx',
      'src/content/docs/release-notes-2-3.mdx',
      'src/content/docs/release-notes-2-4.mdx',
      'src/content/docs/release-notes-2-5.mdx',
      'src/content/docs/release-notes-2-6.mdx',
      'src/content/docs/release-notes-2-7.mdx',
      'src/content/docs/release-notes-2-8.mdx',
      'src/content/docs/release-notes-2-9.mdx',
      'src/content/docs/release-notes.mdx',
      // The legal pages, which enumerate the predecessor lines by name because
      // the terms they state apply to each of them.
      'src/pages/license.astro',
      'src/pages/privacy.astro',
      'src/pages/terms.astro',
    ],
    exemptLines: [
      {
        file: 'src/data/software-jsonld.ts',
        line: "  'ModPageSpeed',",
        why: 'the camel spelling as an entry of PRODUCT_ALTERNATE_NAMES: the JSON-LD alternate name that keeps the old query findable (the alternateName exemption, for a list one entry per line)',
      },
    ],
    bad: 'ModPageSpeed 2.0 optimizes whole sites from its worker behind nginx.',
    good: 'mod_pagespeed optimizes whole sites: the optimizer worker, introduced by the 2.0 re-architecture, does the heavy work behind the module.',
    variantsBad: [
      'Run ModPageSpeed 2.0 locally with Docker and measure the difference.',
      'title="Security — ModPageSpeed 2.0"',
      'ModPageSpeed is a self-hosted optimizer.',
      'MPS 2.0 generates up to 37 variants per image.',
      'ModPageSpeed 2.1 is the converged product.',
      'Install mod_pagespeed 1.15 for nginx on Debian 12.',
      'mod_pagespeed 1.15 runs as an nginx or Apache module. It optimizes out of the box. See [pricing](/pricing/).',
      // The label branch's own headline case, which it did not cover while it
      // named only the 2.0 spelling — and which is exactly what a naming pass
      // over titles, H1s and JSON-LD names is for.
      "title: 'mod_pagespeed 1.15 — the drop-in module'",
      "name: 'mod_pagespeed 1.15'",
    ],
    variantsGood: [
      'A volume created by 2.0 is migrated in place on first start.',
      'ModPageSpeed 2.0 went GA on 2026-05-17; its engine continues as the optimizer worker.',
      'The optimizer worker was introduced by the 2.0 re-architecture.',
      'Upgrading from ModPageSpeed 2.0? See the migration guide.',
      "alternateName: 'ModPageSpeed',",
      'The WeAmp Cache Control for ModPageSpeed plugin is live on WordPress.org.',
      'Description=ModPageSpeed Factory Worker',
      'Requires ModPageSpeed 2.0.x or later.',
      'The IIS package ships from the 1.15 packaging channel.',
      // A dated record of what the predecessor line shipped, with no offer
      // attached — the 1.15 branch needs the call to action.
      'mod_pagespeed 1.15 ships an experimental RSL-CAP gate for nginx, added in 2026.',
      // The current product, spelled the way it is spelled:
      'mod_pagespeed runs as an Apache, nginx or IIS module. Install it from the signed repository.',
      // The predecessor line's dated release post, in title position: a
      // record of what was published on its date, not a live label.
      "title: 'mod_pagespeed 1.15: what six years of stewardship look like'",
    ],
    exemptionMustNotDisarm: [
      // The sanctioned history phrase governs the SECOND sentence; the claim
      // in the first is still an offer of a product that is not on offer.
      'ModPageSpeed 2.0 is the product you install today. The 2.0 re-architecture is why it is fast.',
      // "migration guide" sits in clause 2.
      'Install ModPageSpeed 2.0 in production; the migration guide covers the upgrade.',
      // The JSON-LD bridge is clause 1; clause 2 uses the retired brand as the
      // product's name.
      "alternateName: 'ModPageSpeed 2.0'; ModPageSpeed is the product you install today.",
      // A frozen post title is cited in clause 1; clause 2 is an offer.
      'See [Run ModPageSpeed 2.0 with Docker Compose](/blog/run-with-docker-compose/); ModPageSpeed 2.0 ships as the product today.',
      // The same title's words in running prose are not a citation.
      'Run ModPageSpeed 2.0 with Docker Compose today and see the difference.',
      // A real version fact in clause 2 does not license the imperative in
      // clause 1.
      'Upgrade to ModPageSpeed 2.0 today; the 2.0.41 release is current.',
      // "Migrate to" is an offer, not a source state, so the migration
      // exemption must not reach it.
      'Migrate to ModPageSpeed 2.0 and install it from Docker Hub.',
      // The same four exemption families again, each bolted on after a COMMA
      // instead of the period or semicolon the probes above use. The comma
      // form is the more natural sentence, so it is the one that gets
      // written — and it defeated all four.
      'ModPageSpeed 2.0 is the current product, see the migration guide for details.',
      'Install ModPageSpeed 2.0 today, version 2.0.41.',
      'ModPageSpeed 2.0 is the product you install today, introduced by the 2.0 re-architecture.',
      'ModPageSpeed 2.0 ships today, alternateName aside.',
      // The frozen-title exemption is citation-position only: the dated title
      // in clause one does not license a live label in clause two.
      "title: 'mod_pagespeed 1.15: what six years of stewardship look like'; name: 'mod_pagespeed 1.15'",
    ],
  },
  {
    id: 'k-retired-2.1-product-label',
    // RULE (owner ruling, 2026-10-20): the product is plain "mod_pagespeed".
    // "2.1" is retired as a product label. "The current mod_pagespeed" is the
    // form for contrasting with the 1.x lineage (rule l), and an exact three-part version,
    // rendered from the release manifest, is for release facts (download,
    // install output, release notes, advisories). The name followed by "2.1"
    // as a label tells a reader there is a 2.1 product to go and find, and
    // dates every page that says it at the next release.
    //
    // Allowed, because none of them puts the label after the name: an exact
    // version ("2.1.0"), version-scoped phrases ("since 2.1.0", "2.1.0 and
    // later"), identifiers (the releases-2.1 collection, getRelease('2.1'),
    // the /docs/release-notes-2-1/ and /docs/migrating-to-2-1/ slugs), and the
    // two records exempted by file below.
    //
    // The second branch catches the label assembled from the constants,
    // `${PRODUCT_NAME} ${CURRENT_LINE}` or its {{…}} template form, which is
    // where most of the generated copy gets it from.
    //
    // The rename itself is a separate change: `pending` lists the hits that
    // were there when this rule landed (scripts/retired-label-pending.json,
    // whose `pages` half feeds the built-output lint's rule of the same name).
    why: 'The product is mod_pagespeed, with no version (owner ruling, 2026-10-20). "2.1" is retired as a product label: write "mod_pagespeed", "the current mod_pagespeed" to contrast with the 1.x line, and an exact three-part version from the release manifest where the version is the point. Allowed: exact versions (2.1.0), version-scoped phrases that do not follow the name ("since 2.1.0"), identifiers and slugs, and the 2.1.0 release notes and announcement post.',
    re: new RegExp(
      [
        // The label in prose, any case, underscore or space; not the start of
        // a longer version ("2.1.0", "2.10").
        /\bmod[_ ]pagespeed 2\.1(?!\d|\.\d)/.source,
        // The label assembled from the constants.
        /\bPRODUCT_(?:DISPLAY_)?NAME\}{1,2}\s*\$?\{{1,2}\s*CURRENT_LINE\b/.source,
      ].join('|'),
      'i',
    ),
    // Records of what shipped under the label, on the date it shipped.
    exemptFile: (rel) =>
      rel === 'src/content/docs/release-notes-2-1.mdx' ||
      rel === 'src/content/blog/mod-pagespeed-2-1-one-product-again.md' ||
      rel === 'public/llms-full.txt',
    exemptFileExpectation: [
      // The generated full agent file embeds every docs page verbatim; its
      // prose is scanned at its sources (the docs bodies, agent-files.mjs).
      'public/llms-full.txt',
      // The dated announcement of the 2.1.0 convergence.
      'src/content/blog/mod-pagespeed-2-1-one-product-again.md',
      // The 2.1.0 release notes.
      'src/content/docs/release-notes-2-1.mdx',
    ],
    pending: RETIRED_LABEL_PENDING,
    bad: 'mod_pagespeed 2.1 runs as an nginx or Apache module.',
    good: 'mod_pagespeed runs as an nginx or Apache module; 2.1.0 converged the module and the optimizer worker into one product.',
    variantsBad: [
      'Install mod_pagespeed 2.1 on Debian 12.',
      "title: 'Security hardening guide — mod_pagespeed 2.1'",
      "name: 'mod_pagespeed 2.1',",
      'MOD_PAGESPEED 2.1 is open source.',
      'Upgrade to mod pagespeed 2.1 in place.',
      'The mod_pagespeed 2.1 worker warms hot URLs.',
      'Run mod_pagespeed 2.1.',
      'const PRODUCT = `${PRODUCT_NAME} ${CURRENT_LINE}`;',
      '{{PRODUCT_NAME}} {{CURRENT_LINE}} is open source.',
    ],
    variantsGood: [
      'mod_pagespeed 2.1.0 converged the module line and the optimizer worker.',
      'mod_pagespeed 2.5.0 is the current release.',
      'The current mod_pagespeed is a drop-in for 1.14 and 1.15 configurations.',
      'Since 2.1.0 the worker drops privileges at startup.',
      'The 2.1.0 notes are at /docs/release-notes-2-1/.',
      "const rel = await getRelease('2.1');",
      'Migrate from ModPageSpeed 2.0: see /docs/migrating-to-2-1/.',
    ],
  },
  {
    id: 'l-mod_pagespeed-2-shorthand',
    // RULE (owner ruling, 2026-11-15): "mod_pagespeed 2" is not a product name
    // either. Write "mod_pagespeed", or "the current mod_pagespeed" where the
    // copy contrasts it with the 1.x line. The shorthand stays only where the
    // text is history or a record: dated blog posts, release notes, the 2026
    // rows of the project timelines, and the JSON-LD alternate name. Those
    // lines are listed one by one in scripts/product-name-shorthand-allow.json,
    // so a new use fails, in a blog post too.
    //
    // The second branch catches the shorthand assembled from the constants,
    // `${PRODUCT_NAME} 2` or its {{…}} template form.
    why: 'The product is mod_pagespeed (owner ruling, 2026-11-15). Write "mod_pagespeed", or "the current mod_pagespeed" to contrast with 1.x; an exact version (2.8.1) where the version is the point. Kept uses (history, records) are listed line by line in scripts/product-name-shorthand-allow.json.',
    re: new RegExp(
      [
        // The shorthand in prose: the name and a bare major "2", not the start
        // of a version ("2.8.1") or a longer number.
        /\bmod[_ ]pagespeed 2(?![.\d])/.source,
        // The shorthand assembled from the constants.
        /\bPRODUCT_(?:DISPLAY_)?NAME\}{1,2} 2(?![.\d])/.source,
      ].join('|'),
      'i',
    ),
    // The generated agent files repeat the docs and blog text verbatim; their
    // prose is scanned at its sources (the docs and blog bodies,
    // src/data/agent-files.mjs, the templates).
    exemptFile: (rel) => rel === 'public/llms.txt' || rel === 'public/llms-full.txt',
    exemptFileExpectation: ['public/llms-full.txt', 'public/llms.txt'],
    exemptLines: SHORTHAND_ALLOW_LINES,
    bad: 'mod_pagespeed 2 keeps the same directives and filter names.',
    good: 'The current mod_pagespeed keeps the same directives and filter names; 2.8.1 is the newest release.',
    variantsBad: [
      'Install mod_pagespeed 2 on Debian 12.',
      'The maintained line is <span class="whitespace-nowrap">mod_pagespeed 2</span>.',
      "q: 'Is mod_pagespeed 2 a drop-in replacement?'",
      'Its license transfers to mod_pagespeed 2, free of charge.',
      'MOD_PAGESPEED 2 is open source.',
      'const NAME = `${PRODUCT_NAME} 2`;',
      '{{PRODUCT_NAME}} 2 converges the lineage.',
    ],
    variantsGood: [
      'mod_pagespeed 2.8.1 is the current release.',
      'mod_pagespeed 2.1.0 converged the module line and the optimizer worker.',
      'mod_pagespeed 1.15 users upgrade in place.',
      'mod_pagespeed 20 times faster is not a claim we make.',
      'The current mod_pagespeed runs on Apache and nginx.',
    ],
  },
];
for (const rule of DENYLIST) if (!rule.crossSentence) rule.re = boundWindows(rule.re);

// ---------------------------------------------------------------------------
// FALSE-POSITIVE CORPUS — real, ACCURATE corrective/contrast phrasings an
// adversarial review surfaced. None of these may be flagged by ANY rule.
// (Machine-enforces the negation/contrast exemption design — §2.)
// ---------------------------------------------------------------------------
const FP_CORPUS = [
  'On RHEL 9 the signed yum repo is x86_64 only, while the apt repo also offers arm64 builds for Debian and Ubuntu.',
  'The yum repo serves x86_64 RPMs on RHEL 9; arm64 (aarch64) is available only through the apt channel on Debian/Ubuntu.',
  'Whereas the apt repo covers amd64 and arm64, the RHEL 9 yum repo is x86_64 only.',
  'We do not publish arm64 packages to the RHEL 9 yum repo; only x86_64 is signed there.',
  'On RHEL 9, the yum repo carries x86_64 packages; for arm64, build from source or use the apt-based images.',
  'For RHEL 9 on arm64, install the package manually rather than from the yum repo.',
  'Note: arm64 RPMs are not in the RHEL 9 yum repo — fetch them from the release archive instead.',
  'Older docs wrongly listed the RHEL 9 yum repo as amd64 + arm64; it has always been x86_64 only.',
  // Re-pointed with the rules they exercise: b1/b2/b3 now ask about the
  // optimizer worker, and the worker's packaging story is the opposite of the
  // one the retired engine had — it ships from the signed repository too.
  "Unlike the module's load_module line, the optimizer worker needs no nginx config.",
  'The optimizer worker does not use a load_module directive — it runs as its own service.',
  'There is no load_module line for the optimizer worker; it starts from its own unit file.',
  'Whereas the module loads into an existing nginx, the optimizer worker runs beside it.',
  'The optimizer worker is not installed as a dynamic module on an existing nginx; it runs as its own service.',
  'answer: the optimizer worker has no load_module directive because it runs as its own service.',
  'The optimizer worker is not a container-only component; it installs from the signed apt and yum repositories.',
  'The optimizer worker installs from apt and yum repos as well as from a container image.',
  'The module and the optimizer worker upgrade together from the signed repository, rather than on separate schedules.',
  'There is no v1.0.0 release of the 2.0 line; GA was v2.0.0.',
  'While the module ships from apt and yum repositories, the optimizer worker ships from that repository and as a container image.',
  // Added by the 2026-06-15 FP-probe-v2 (all accurate; none may flag):
  'A drop-in nginx image optimization module — install the module from signed apt/yum.', // the module IS a drop-in module
  'The drop-in nginx optimization module for Apache, nginx and IIS: mod_pagespeed.',
  'ModPageSpeed 2.0 skipped .NET 9: it targets the net8.0 and net10.0 LTS releases.',
  'ModPageSpeed 2.0.x runs on net10.0, which is newer than .NET 9.',
  'The el9 yum repo carries x86_64; arm64 ships from the apt repo only.',
  'On Debian 13 the 1.15 native module is built for nginx 1.26 (stable channel).', // trixie stock nginx IS 1.26.3
  'ModPageSpeed 2.0 supersedes the apt and yum packages of the legacy engine.',
  // Added by the 2026-06-15 FP-probe-v3 (accurate .NET-9 coexistence/era/"post-1.0"; none may flag):
  'ModPageSpeed 2.0.x runs on .NET 9 hosts too, even though the package targets net8.0 and net10.0.',
  'ModPageSpeed 2.0.x supports .NET 9 apps as a sidecar while the middleware itself targets net8.0/net10.0.',
  'ModPageSpeed 2.0 builds on .NET 9-era runtime improvements, shipping for net8.0 and net10.0.',
  'The post-1.0 cleanup of the Envoy shim landed in 2.0.16.',
  // Added by the f4 parenthetical-fence FP probe. All are ordinary, ACCURATE
  // release-note / blog / docs shapes that the untightened branch flagged:
  // version-prefixed release refs, markdown links, self-contained bracketed
  // clauses, and brackets that name BOTH product lines.
  'See the changelog entry (AVIF in 2.0.21) for the encoder bump.',
  'Read more about [AVIF in 2.0](/blog/avif-2/) and the 1.15 backport.',
  'Enable the four filters (AVIF is opt-in in 2.0 as well).',
  'Zero-copy serving is everywhere (zero-copy on 2.0.14+).',
  'Benchmarks below (AVIF on 2.0 vs 1.15).',
  'The cache is shared (variant-aware caching in 2.0 too).',
  'Docs: (zero-copy serving in 2.0 and 1.15).',
  'Full matrix (AVIF from 2.0.4 onward).',
];

// ENGINE-TAG CORPUS — correct v1.1.0+rN engine-tag refs (§5). The guard
// must never flag these (the 1.1 engine tags are real; iis-configuration.md uses
// v1.1.0+r11 twice). Machine-enforces §5 rather than relying on it being only
// structurally true today.
const ENGINE_TAG_CORPUS = [
  'Available from v1.1.0+r11 onward.',
  'AutoCreateCachePath shipped in v1.1.0+r11 (PR #174).',
  'See the v1.1.0+r3 manifest and the v1.1.0+r20 engine tag.',
];

// ---------------------------------------------------------------------------
// HISTORICAL DEFECT CORPUS — the REAL pre-fix lines that motivated rule f4,
// copied VERBATIM from the versions before commit 771866c2 ("re-anchor AVIF
// claims for mod_pagespeed 1.15"):
//   src/data/psi-mps-mapping.ts:250  src/data/psi-mps-mapping.ts:649
//   src/data/cwv-pillars.ts:157
// Every entry MUST be flagged by f4. A rule that misses its own motivating case
// is worse than no rule, so this is asserted rather than assumed — and asserted
// against the verbatim text, not a paraphrase that could be tuned to pass.
// ---------------------------------------------------------------------------
const HISTORICAL_DEFECTS: Array<{ site: string; line: string }> = [
  {
    site: 'src/data/psi-mps-mapping.ts:250 (pre-771866c2)',
    line: "      'ModPageSpeed transcodes JPEG/PNG/GIF to WebP (and AVIF on 2.0), recompresses with quality-aware encoders, resizes to the rendered display size, and strips EXIF/ICC metadata. Typical result: 40–70% smaller images at visually equivalent quality.',",
  },
  {
    site: 'src/data/psi-mps-mapping.ts:649 (pre-771866c2)',
    line: "      'JPEG/PNG/GIF transcoded to WebP (and AVIF on 2.0) when the client advertises support via `Accept`.',",
  },
  {
    site: 'src/data/cwv-pillars.ts:157 (pre-771866c2)',
    line: "        body: 'This attacks resource load duration. ModPageSpeed 2.0 transcodes images to WebP and AVIF based on the client\\'s <code>Accept</code> header and serves <a href=\"/blog/viewport-aware-image-optimization/\">viewport-aware responsive variants</a> (mobile/tablet/desktop, 1x/2x), so the hero downloads sooner. A single decode produces up to 37 cache variants, and every variant is verified against the original with SSIMULACRA2 before it is cached. mod_pagespeed 1.15 uses <code>recompress_images</code> + <code>convert_jpeg_to_webp</code>. Optimization is transparent: no URL rewrites, no markup changes, no build-pipeline changes.',",
  },
];

// ALLOWLIST — legit historical version refs that must always survive the scan.
const ALLOWLIST_SAMPLES = [
  'On releases before 2.0.14, the worker did not coordinate.',
  "You're on 2.0.14 or newer — dotnet list package should show it.",
  'From 2.0.14 the default is an auto-resolved per-process socket.',
  'Upgrade to 2.0.16+ and confirm.',
  'Bundled libpng 1.6.58; the Envoy 1.0.0 shim is internal.',
  'native module pinned per distro: stock nginx 1.18 to 1.26.3.',
  'Apache yum repo: AlmaLinux/RHEL 9 (x86_64; arm64 via direct download).',
  'mod_pagespeed 1.15 ships as signed apt and yum packages.',
  'The 2.0 line shipped as a Docker reverse proxy bundling nginx 1.30.2.',
];

// ---------------------------------------------------------------------------
// Flagging predicate: a line is flagged by a rule iff the regex matches AND the
// line is not corrective (negation/contrast). This is the single predicate used
// by BOTH the content scan and the matcher/corpus tests — no divergence.
// ---------------------------------------------------------------------------
// A COMMENT line, judged line-level (no parser) and LANGUAGE-AWARE. The
// language matters: in Markdown '#' opens a HEADING and '*' opens a BULLET —
// both are real published copy, and classifying them as comments would misread
// most of the prose in this scan surface.
//
// The .tmpl/.txt bucket is MARKDOWN-SHAPED, not shell-shaped: '#' there opens a
// HEADING too. Measured over public/llms.txt, public/llms-full.txt and
// scripts/llms-templates/*.tmpl, 114 of 114 '^#' lines are headings and ZERO
// are comments — llms-full.txt.tmpl even carries '## Variant-Aware Caching',
// which is rule f3's exact topic. So '#' is NOT a comment opener here.
//
// Nothing currently skips comment lines (the `notInComment` opt-out was tried
// and reverted — see the SCAN SURFACE note above). This classifier is kept, and
// unit-tested below, because getting it wrong is what made the opt-out look
// cheap: a rule that skips '#' lines in .tmpl/.txt goes blind on every heading.
const COMMENT_OPENER = {
  markdown: /^\s*<!--/, // ONLY the HTML comment form
  code: /^\s*(?:\/\/|\/\*|\*)/, // .ts / .mjs / .astro
  text: /^\s*\/\//, // .tmpl / .txt — '#' is a HEADING, not a comment
} as const;

function commentOpenerFor(file?: string): RegExp {
  if (!file) return COMMENT_OPENER.code; // bare-string unit tests
  const ext = path.extname(file).toLowerCase();
  if (ext === '.md' || ext === '.mdx') return COMMENT_OPENER.markdown;
  if (ext === '.ts' || ext === '.mjs' || ext === '.astro') return COMMENT_OPENER.code;
  return COMMENT_OPENER.text;
}

function isCommentLine(line: string, file?: string): boolean {
  return commentOpenerFor(file).test(line);
}

// ---------------------------------------------------------------------------
// CLAUSE SCOPING for BOTH exemptions (`exemptIf` and `negationExempt`).
//
//   Both used to be tested against the WHOLE LINE, with no requirement that the
//   exempting term govern the same clause as the match. That let an exempting
//   term anywhere in a sentence disarm a rule for a claim it has nothing to do
//   with:
//     "Migrating from 1.15? Install ModPageSpeed 2.0 from the signed apt
//      repository"  -> b3 disarmed by a "1.15" in the PREVIOUS sentence.
//     "Install ModPageSpeed 2.0 from the signed apt repository. Windows builds
//      are not on apt."  -> b3 disarmed by a TRUE denial in the NEXT sentence.
//   Now the exempting term must appear in the clause the match STARTS in. See
//   `exemptionMustNotDisarm` for the machine-enforced probes.
//
//   Boundary set: ; — ? ! ( and a sentence period. The period is guarded
//   against splitting inside a version number: "ModPageSpeed 2.0", "1.15" and
//   "v1.0.0" must stay intact, so a '.' between two digits is NOT a boundary.
//   '?' and '!' are included beyond a bare [.;—] because a question mark ends a
//   clause just as firmly as a period. '(' opens an aside that is its own
//   clause — "Install ModPageSpeed 2.0 from the apt repository (1.15 users: see
//   below)" is a 2.0-from-apt claim regardless of who the aside addresses.
// ---------------------------------------------------------------------------
function isClauseBoundary(line: string, i: number): boolean {
  const ch = line[i];
  // A ';' that closes an HTML entity is part of a WORD, not a clause end.
  // Page copy is full of them (&rarr; &mdash; &nbsp; &rsquo;), and splitting
  // there cuts a phrase in half: "Settings &rarr; ModPageSpeed" became the two
  // clauses "…Settings &rarr" and "ModPageSpeed…", so an exemption naming the
  // whole phrase never shared a clause with the token it licenses.
  if (ch === ';') return !/&(?:#\d+|[A-Za-z][A-Za-z0-9]*)$/.test(line.slice(0, i));
  if (ch === '—' || ch === '?' || ch === '!' || ch === '(') return true;
  if (ch !== '.') return false;
  // A '.' flanked by digits is a version separator, not a sentence end.
  return !(/\d/.test(line[i - 1] ?? '') && /\d/.test(line[i + 1] ?? ''));
}

function clauseRanges(line: string): Array<{ start: number; end: number }> {
  const ranges: Array<{ start: number; end: number }> = [];
  let start = 0;
  for (let i = 0; i < line.length; i++) {
    if (isClauseBoundary(line, i)) {
      ranges.push({ start, end: i });
      start = i + 1;
    }
  }
  ranges.push({ start, end: line.length });
  return ranges;
}

// The SINGLE clause range the match STARTS in.
//
// This deliberately does NOT union every clause the match overlaps. Several
// rule regexes use wide [\s\S]{0,80} / [^\n]{0,50} windows and so straddle a
// boundary; unioning re-imported the neighbouring clause's exempting tokens and
// re-opened the exact hole clause scoping exists to close:
//   "Install ModPageSpeed 2.0 from apt. 1.15 packages are signed too."
//     -> the b3 match runs past the period to reach "packages", dragging the
//        next sentence's "1.15" into scope and disarming itself.
// The claim's SUBJECT sits where the match begins, so that clause is the scope.
function clauseRangeOfMatch(line: string, start: number): { start: number; end: number } {
  const ranges = clauseRanges(line);
  const hit =
    ranges.find((r) => r.start <= start && start < r.end) ??
    // `start` landed exactly on a boundary character: attribute it to the
    // clause that boundary closes.
    [...ranges].reverse().find((r) => r.start <= start);
  return hit ?? { start: 0, end: line.length };
}

function clauseScopeOfMatch(line: string, start: number): string {
  const r = clauseRangeOfMatch(line, start);
  return line.slice(r.start, r.end);
}

// The COMMA-DELIMITED PART of that clause which contains the match start —
// the exemption scope for a rule that sets `exemptScope: 'comma-part'`.
//
// Same argument as clause scoping, one level finer: the claim's SUBJECT sits
// where the match begins, so the licence for that claim has to sit beside the
// subject, not in whatever got appended after the next comma. Commas INSIDE
// the match are not seams — a claim that spans one is still one claim — so the
// part is grown to the match start and closed at the first comma after it.
function commaPartOfMatch(line: string, start: number): string {
  const r = clauseRangeOfMatch(line, start);
  let from = r.start;
  let to = r.end;
  for (let i = r.start; i < r.end; i++) {
    if (line[i] !== ',') continue;
    if (i < start) from = i + 1;
    else {
      to = i;
      break;
    }
  }
  return line.slice(from, to);
}

// A per-rule global clone of the rule regex, so exec() can walk EVERY match on
// the line. Cached: recompiling per line per file is measurable at scan size.
const GLOBAL_RE = new Map<DenyRule, RegExp>();
function globalReFor(rule: DenyRule): RegExp {
  let re = GLOBAL_RE.get(rule);
  if (!re) {
    re = new RegExp(
      rule.re.source,
      rule.re.flags.includes('g') ? rule.re.flags : rule.re.flags + 'g',
    );
    GLOBAL_RE.set(rule, re);
  }
  return re;
}

// A line is flagged iff ANY match on it survives that match's own exemptions.
//
// Evaluating only the FIRST match was a silent miss: once match #1 landed in an
// exempt clause the rule stopped looking, so a second, unexempt claim on the
// same line was never tested —
//   "ModPageSpeed 2.0 works with the 1.15 apt repo; install ModPageSpeed 2.0
//    from the yum repository."
// (`file` is accepted for call-site symmetry with the scanner; no rule
// currently varies by file type.)
function flaggedMatches(rule: DenyRule, line: string): RegExpExecArray[] {
  const re = globalReFor(rule);
  re.lastIndex = 0;
  const out: RegExpExecArray[] = [];
  let m: RegExpExecArray | null;
  while ((m = re.exec(line)) !== null) {
    if (m[0] === '') {
      re.lastIndex++; // zero-width match: never stall
      continue;
    }
    const scope =
      rule.exemptScope === 'comma-part'
        ? commaPartOfMatch(line, m.index)
        : clauseScopeOfMatch(line, m.index);
    const exempt =
      (rule.negationExempt?.test(scope) ?? false) || (rule.exemptIf?.test(scope) ?? false);
    if (!exempt) out.push(m);
  }
  return out;
}
function isFlaggedByRule(rule: DenyRule, line: string, _file?: string): boolean {
  return flaggedMatches(rule, line).length > 0;
}
function rulesFlagging(line: string): DenyRule[] {
  return DENYLIST.filter((r) => isFlaggedByRule(r, line));
}

type Violation = { ruleId: string; file: string; location: string; line: number };

// ---------------------------------------------------------------------------
// BLOCK SCANNING. Markdown prose is hard-wrapped, so a claim can wrap across a
// physical line and a line-at-a-time scan never sees it. The scanner therefore
//   1. classifies every line (frontmatter / fenced code / prose),
//   2. joins consecutive PROSE lines of a markdown-shaped file into one block
//      (a single space replaces each line break, so a bounded [^\n]{0,N}
//      proximity window stays bounded and means the same thing it did on a
//      single line), and
//   3. runs the rules on the block, mapping a match back to its physical line
//      through an offset table so `file:line` reporting still works.
// Frontmatter and fenced code are classified and NEVER joined: they stay one
// block per physical line, scanned exactly as before. They are not skipped,
// because the frontmatter carries titles and descriptions (one rule keys on
// `title:`) and a fence can carry copy-pasteable claims; what joining must not
// do is glue a code sample or a YAML key onto the prose around it. Files that
// are not markdown-shaped (.astro, .ts, .mjs) are one block per line: their
// line breaks separate code, not words.
// ---------------------------------------------------------------------------
type LineKind = 'frontmatter' | 'fence' | 'prose';

function isMarkdownShaped(file?: string): boolean {
  return !!file && /\.(?:md|mdx|tmpl|txt)$/i.test(file);
}

function classifyLines(lines: string[], file?: string): LineKind[] {
  const kinds: LineKind[] = lines.map(() => 'prose');
  let i = 0;
  if (file && /\.mdx?$/i.test(file) && lines[0]?.trim() === '---') {
    kinds[0] = 'frontmatter';
    for (i = 1; i < lines.length; i++) {
      kinds[i] = 'frontmatter';
      if (lines[i].trim() === '---') {
        i++;
        break;
      }
    }
  }
  let fence: string | null = null;
  for (; i < lines.length; i++) {
    const f = /^\s*(```+|~~~+)/.exec(lines[i]);
    if (fence === null) {
      if (f) {
        fence = f[1][0];
        kinds[i] = 'fence';
      }
    } else {
      kinds[i] = 'fence';
      if (f && f[1][0] === fence) fence = null;
    }
  }
  return kinds;
}

// A line that ends the block before it and stands alone.
const STANDALONE_LINE = /^\s{0,3}#{1,6}(?:\s|$)|^\s*\||^\s*(?:-{3,}|\*{3,}|_{3,})\s*$|^\s*<!--/;
// Lines that end the block before them and open a new one (continuations join).
const LIST_MARKER = /^\s*(?:[-*+]|\d+[.)])\s/;
const QUOTE_MARKER = /^\s*>/;

type Block = { text: string; starts: number[]; lines: number[] };

function buildBlocks(text: string, file?: string): Block[] {
  const lines = text.split('\n');
  const blocks: Block[] = [];
  const single = (i: number) => blocks.push({ text: lines[i], starts: [0], lines: [i + 1] });
  if (!isMarkdownShaped(file)) {
    for (let i = 0; i < lines.length; i++) single(i);
    return blocks;
  }
  const kinds = classifyLines(lines, file);
  let cur: Block | null = null;
  let curQuote = false;
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (kinds[i] !== 'prose' || STANDALONE_LINE.test(line) || line.trim() === '') {
      cur = null;
      single(i);
      continue;
    }
    const quote = QUOTE_MARKER.test(line);
    if (cur === null || LIST_MARKER.test(line) || quote !== curQuote) {
      cur = { text: line, starts: [0], lines: [i + 1] };
      curQuote = quote;
      blocks.push(cur);
      continue;
    }
    const piece = (quote ? line.replace(QUOTE_MARKER, '') : line).trim();
    cur.starts.push(cur.text.length + 1);
    cur.lines.push(i + 1);
    cur.text += ' ' + piece;
  }
  return blocks;
}

function lineOfOffset(block: Block, index: number): number {
  let k = 0;
  for (let j = 0; j < block.starts.length; j++) if (block.starts[j] <= index) k = j;
  return block.lines[k];
}

// ---------------------------------------------------------------------------
// SHADOW TEXT. Markup and entities hide a claim from a prose regex:
//   "AVIF is <strong>ModPageSpeed 2.0</strong> only."  /  "AVIF&nbsp;is 2.0 only"
// shadowText() strips tags and decodes entities, and returns a map from every
// character of the shadow back to its index in the source, so a hit found in
// the shadow still reports the right physical line. Block-level tags (and <br>)
// become one space so two cells or two items never fuse into one word; inline
// tags vanish, so "<strong>2.0</strong>-only" stays "2.0-only".
// ---------------------------------------------------------------------------
const BLOCK_TAG =
  /^<\/?(?:p|br|li|ul|ol|dl|dt|dd|div|section|article|header|footer|tr|td|th|table|thead|tbody|h[1-6]|blockquote|pre|hr|details|summary|figure|figcaption)\b/i;
const NAMED_ENTITIES: Record<string, string> = {
  nbsp: ' ',
  amp: '&',
  lt: '<',
  gt: '>',
  quot: '"',
  apos: "'",
  mdash: '\u2014',
  ndash: '\u2013',
  rsquo: '\u2019',
  lsquo: '\u2018',
  rarr: '\u2192',
  hellip: '\u2026',
};

function decodeEntity(entity: string): string | null {
  const body = entity.slice(1, -1);
  if (/^#\d+$/.test(body)) return String.fromCodePoint(Number(body.slice(1)));
  if (/^#[xX][0-9a-fA-F]+$/.test(body)) return String.fromCodePoint(parseInt(body.slice(2), 16));
  return NAMED_ENTITIES[body] ?? null;
}

function shadowText(src: string): { text: string; map: number[] } {
  let text = '';
  const map: number[] = [];
  const push = (ch: string, at: number) => {
    text += ch;
    map.push(at);
  };
  for (let i = 0; i < src.length;) {
    const ch = src[i];
    if (ch === '<') {
      const tag = /^<\/?[A-Za-z][^<>]*>/.exec(src.slice(i));
      if (tag) {
        if (BLOCK_TAG.test(tag[0])) push(' ', i);
        i += tag[0].length;
        continue;
      }
    }
    if (ch === '&') {
      const ent = /^&(?:#\d+|#[xX][0-9a-fA-F]+|[A-Za-z][A-Za-z0-9]*);/.exec(src.slice(i));
      const decoded = ent ? decodeEntity(ent[0]) : null;
      if (ent && decoded !== null) {
        push(decoded, i);
        i += ent[0].length;
        continue;
      }
    }
    push(ch, i);
    i++;
  }
  return { text, map };
}

// One block's violations: the raw block, plus its shadow when markup or an
// entity is present. A hit is attributed to a physical line through the offset
// table, and a line is reported once per rule.
function scanBlockForRule(rule: DenyRule, block: Block): Array<{ line: number; matched: string }> {
  const hits: Array<{ line: number; matched: string }> = [];
  const seen = new Set<number>();
  const add = (index: number, matched: string) => {
    const line = lineOfOffset(block, index);
    if (seen.has(line)) return;
    seen.add(line);
    hits.push({ line, matched });
  };
  for (const m of flaggedMatches(rule, block.text)) add(m.index, m[0]);
  if (/[<&]/.test(block.text)) {
    const shadow = shadowText(block.text);
    if (shadow.text !== block.text) {
      for (const m of flaggedMatches(rule, shadow.text)) add(shadow.map[m.index], m[0]);
    }
  }
  return hits;
}

// The scan of one file's text, shared by the real scanner and the unit tests
// (so a test of wrapped or marked-up input runs the production path).
function scanTextForRule(rule: DenyRule, text: string, rel: string, file?: string): Violation[] {
  const violations: Violation[] = [];
  const physical = text.split('\n');
  for (const block of buildBlocks(text, file ?? rel)) {
    for (const hit of scanBlockForRule(rule, block)) {
      if (rule.exemptLines?.some((e) => e.file === rel && e.line === physical[hit.line - 1]))
        continue;
      violations.push({
        ruleId: rule.id,
        file: rel,
        location: `${rel}:${hit.line}: ${hit.matched}`,
        line: hit.line,
      });
    }
  }
  return violations;
}

function scanFilesForRule(rule: DenyRule, files: string[]): Violation[] {
  const violations: Violation[] = [];
  for (const file of files) {
    const rel = path.relative(WEBSITE_ROOT, file).split(path.sep).join('/');
    if (rule.exemptFile?.(rel)) continue;
    violations.push(...scanTextForRule(rule, readFileSync(file, 'utf8'), rel, file));
  }
  return violations;
}

function scanForRule(rule: DenyRule): Violation[] {
  return scanFilesForRule(rule, SCAN_FILES);
}

// What a rule's scan FAILS on: every violation, except those its `pending`
// ratchet covers exactly. Returns the failure lines and the number of hits
// tolerated as pending.
function unpendedViolations(rule: DenyRule): { failures: string[]; pendingHits: number } {
  const violations = scanForRule(rule);
  if (!rule.pending) {
    return { failures: violations.map((v) => `[${v.ruleId}] ${v.location}`), pendingHits: 0 };
  }
  const byFile = new Map<string, Violation[]>();
  for (const v of violations) byFile.set(v.file, [...(byFile.get(v.file) ?? []), v]);
  const failures: string[] = [];
  let pendingHits = 0;
  for (const [file, hits] of byFile) {
    const allowed = rule.pending[file] ?? 0;
    if (hits.length === allowed) {
      pendingHits += hits.length;
    } else if (hits.length > allowed) {
      failures.push(
        `[${rule.id}] ${file}: ${hits.length} hits, ${allowed} pending — new text must not add any:`,
        ...hits.map((v) => `    ${v.location}`),
      );
    } else {
      failures.push(
        `[${rule.id}] ${file}: ${hits.length} hits, ${allowed} pending — lower its count in scripts/retired-label-pending.json to ${hits.length}`,
      );
    }
  }
  for (const file of Object.keys(rule.pending)) {
    if (!byFile.has(file)) {
      failures.push(
        `[${rule.id}] ${file}: no hits left — remove it from scripts/retired-label-pending.json`,
      );
    }
  }
  return { failures, pendingHits };
}

// ---------------------------------------------------------------------------
// (3) CANONICAL-SOURCE consistency — the ground truth the denylist relies on.
// ---------------------------------------------------------------------------
describe('canonical sources hold ground-truth product facts', () => {
  it('product-facts.mjs SIDECAR_NGINX_VERSION === 1.30.7', () => {
    expect(facts.SIDECAR_NGINX_VERSION).toBe('1.30.7');
  });

  // The social-card generator rasterizes a wordmark onto all 72 cards, and a
  // card is the single highest-reach place the product is named: every share
  // of every post carries it. The script is not copy, so no SCAN_BUCKET sees
  // it, and the cards it writes are binaries no rule can read — a stale
  // wordmark there is invisible to every other check in this file.
  it('the social-card generator stamps the current product name', () => {
    const generator = readFileSync(
      path.join(WEBSITE_ROOT, 'scripts/generate-title-cards.mjs'),
      'utf8',
    );
    // The default card's eyebrow is the product-name constant, never a
    // literal (and never a version: owner ruling, 2026-10-20).
    expect(generator).toMatch(/\beyebrow: PRODUCT_DISPLAY_NAME,/);
    expect(generator).toContain(
      "import { PRODUCT_DISPLAY_NAME } from '../src/data/product-facts.mjs';",
    );
    expect(facts.PRODUCT_DISPLAY_NAME).toBe('mod_pagespeed');
    expect(generator).not.toMatch(/mod_pagespeed 2\.1|ModPageSpeed|MPS ?2\.0/);
  });

  it('ASPNETCORE_RIDS includes linux-arm64 (2.0 is multi-arch, not x86_64-only)', () => {
    expect(facts.ASPNETCORE_RIDS).toContain('linux-arm64');
    for (const rid of ['linux-x64', 'linux-arm64', 'osx-arm64', 'win-x64']) {
      expect(facts.ASPNETCORE_RIDS).toContain(rid);
    }
  });

  it('ASPNETCORE_TFMS is net8.0 + net10.0 (the 2.0 middleware runtime; NOT .NET 9)', () => {
    expect(facts.ASPNETCORE_TFMS).toEqual(['net8.0', 'net10.0']);
    expect(facts.ASPNETCORE_TFMS).not.toContain('net9.0');
  });

  it('NGINX_APT_ARCHES is amd64 + arm64', () => {
    expect(facts.NGINX_APT_ARCHES).toBe('amd64 + arm64');
  });

  it('nginx distro matrix matches the known per-distro stock-nginx pins', () => {
    const matrix = Object.fromEntries(
      (facts.NGINX_APT_DISTROS as Array<{ distro: string; nginx: string }>).map((d) => [
        d.distro,
        d.nginx,
      ]),
    );
    expect(matrix).toEqual({
      'Debian 12 bookworm': '1.22.1',
      'Debian 13 trixie': '1.26.3',
      'Ubuntu 22.04 jammy': '1.18.0',
      'Ubuntu 24.04 noble': '1.24.0',
      'Ubuntu 26.04 resolute': '1.28.3',
    });
  });

  it('2.0 manifest semver is plain SemVer (no +rN), derived as the scan baseline', () => {
    expect(manifest2_0.release.revision).toBeNull();
    expect(V2_SEMVER).toMatch(/^2\.0\.\d+$/);
    expect(manifest2_0.release.tag).toBe(`v${V2_SEMVER}`);
    expect(manifest2_0.release.tag).not.toBe('v1.0.0');
  });

  // Regression guard for the docker-tag staleness class (stale tags found in
  // installation-docker.md, deployment.mdx, web-bot-auth.md, and the
  // docs/[slug].astro HowTo JSON-LD): every pagespeed-worker/pagespeed-nginx
  // tag in the converged line's own install surfaces must equal the CURRENT
  // 2.1 manifest semver, derived at runtime, never hardcoded. Deliberately a
  // fixed file list, not a sitewide scan: historical blog posts legitimately
  // cite an older line's tag (e.g. run-with-docker-compose.md, migrating-from-1x.md)
  // and must not be flagged.
  it('docker image tags for the converged line match the 2.1 manifest semver', () => {
    expect(V2_1_SEMVER).toMatch(/^2\.[1-9]\d*\.\d+$/);
    const files = [
      path.join(CONTENT_DIR, 'docs/installation-docker.md'),
      path.join(CONTENT_DIR, 'docs/deployment.mdx'),
      path.join(CONTENT_DIR, 'docs/web-bot-auth.md'),
      // src/pages/docs/[slug].astro renders its tags with dockerTag() from
      // the manifest, so it carries no literal to check.
      path.join(WEBSITE_ROOT, 'public/docker-compose.yml'),
    ];
    // The tag is a literal semver ("...pagespeed-worker:2.1.0"), the same
    // wrapped in a shell env-var default
    // ("...pagespeed-worker:${PAGESPEED_VERSION:-2.1.0}"), or (in prose) one
    // immediately followed by punctuation ("pagespeed-worker:2.1.0),"). Match
    // the semver shape directly so trailing punctuation is never captured.
    const tagRe =
      /ghcr\.io\/we-amp\/pagespeed-(?:worker|nginx):(?:\$\{[A-Z_]+:-)?(\d+\.\d+\.\d+|latest)/g;
    for (const file of files) {
      const text = readFileSync(file, 'utf8');
      const tags = [...text.matchAll(tagRe)].map((m) => m[1]);
      expect(
        tags.length,
        `no pagespeed-worker/pagespeed-nginx tag found in ${file}`,
      ).toBeGreaterThan(0);
      for (const tag of tags) {
        // The worker and nginx images publish version tags only (`latest`
        // exists for the combined image alone), so `latest` here is a pull
        // that fails, not a rolling tag.
        expect(tag, `${file} names a :latest worker/nginx image, which is not published`).not.toBe(
          'latest',
        );
        expect(tag, `${file} pins a docker tag that disagrees with the 2.1 manifest`).toBe(
          V2_1_SEMVER,
        );
      }
    }

    // helm-deployment.mdx renders the image repository and tag in separate
    // Helm-values table cells, so the combined repo:tag form the regex above
    // matches never appears in its source — scan the image-tag default-value
    // cell directly instead. The chart takes ONE top-level `image.tag` for
    // both images (the per-image `worker.image.tag` / `nginx.image.tag` keys
    // are rejected by the chart); the cell states its effective default, the
    // chart appVersion, which must equal the manifest semver.
    const helmFile = path.join(CONTENT_DIR, 'docs/helm-deployment.mdx');
    const helmText = readFileSync(helmFile, 'utf8');
    const helmTagRe = /`(?:(?:worker|nginx)\.)?image\.tag`\s*\|\s*`(\d+\.\d+\.\d+)`/g;
    const helmTags = [...helmText.matchAll(helmTagRe)].map((m) => m[1]);
    expect(helmTags.length, `no image.tag default found in ${helmFile}`).toBeGreaterThan(0);
    for (const tag of helmTags) {
      expect(tag, `${helmFile} pins a Helm default tag that disagrees with the 2.1 manifest`).toBe(
        V2_1_SEMVER,
      );
    }

    // Both this file's "Upgrading" example and deployment.mdx's "Starting
    // the Stack" build command call <ImageTag line="..." /> inline, which
    // resolves the literal digits from the named release's manifest at
    // build time — the raw source never contains a "2.1.0"-shaped string
    // for these calls, only the `line` prop. Checking that prop directly is
    // equivalent to checking the rendered result, since ImageTag
    // deterministically maps line -> dockerTag(getRelease(line)). Both
    // files are themselves converged-line content, so every <ImageTag>
    // call in them must reference the current line.
    const imageTagLineRe = /<ImageTag\s+line="([^"]+)"/g;
    for (const file of [helmFile, path.join(CONTENT_DIR, 'docs/deployment.mdx')]) {
      const text = readFileSync(file, 'utf8');
      const imageTagLines = [...text.matchAll(imageTagLineRe)].map((m) => m[1]);
      expect(
        imageTagLines.length,
        `no <ImageTag line="..."/> usage found in ${file}`,
      ).toBeGreaterThan(0);
      for (const line of imageTagLines) {
        expect(
          line,
          `${file} calls <ImageTag line="${line}" /> instead of the converged line`,
        ).toBe(facts.CURRENT_LINE);
      }
    }
  });

  it('IMAGE_FORMAT_SUPPORT claims no Envoy port for ANY format (AVIF included)', () => {
    // An absent port is a port we do NOT claim. The 1.15 Envoy port is not
    // shipped, is excluded from CI, and its AVIF status is UNVERIFIED — so
    // Envoy must appear in no port list. Do NOT fix a failure here by relaxing
    // the assertion; remove Envoy from product-facts.mjs instead.
    type Row = { format: string; editions: Record<string, { ports: string[] }> };
    const rows = facts.IMAGE_FORMAT_SUPPORT as Row[];
    const offenders: string[] = [];
    for (const row of rows) {
      for (const [edition, { ports }] of Object.entries(row.editions)) {
        for (const port of ports) {
          if (/envoy/i.test(port)) offenders.push(`${row.format}/${edition}: ${port}`);
        }
      }
    }
    expect(offenders, offenders.length ? `\n${offenders.join('\n')}\n` : undefined).toEqual([]);
    // Belt and braces: AVIF specifically, via the public accessor.
    for (const edition of facts.editionsFor('AVIF')) {
      expect(facts.portsFor('AVIF', edition)).not.toContain('Envoy');
    }
  });

  it('IMAGE_FORMATS stays the flat list ["WebP","AVIF","SVG"] (public contract)', () => {
    expect(facts.IMAGE_FORMATS).toEqual(['WebP', 'AVIF', 'SVG']);
    // Now derived from the converged line; the value must not move.
    expect(facts.formatsFor(facts.CURRENT_LINE)).toEqual(facts.IMAGE_FORMATS);
  });

  it('AVIF is claimed on every line; SVG only where the worker runs (2.0 + 2.1)', () => {
    expect(facts.editionsFor('AVIF')).toEqual([facts.V1_LINE, facts.V2_LINE, facts.CURRENT_LINE]);
    expect(facts.editionsFor('WebP')).toEqual([facts.V1_LINE, facts.V2_LINE, facts.CURRENT_LINE]);
    expect(facts.editionsFor('SVG')).toEqual([facts.V2_LINE, facts.CURRENT_LINE]);
    // The copy helper renders the bare format list — one product, so no line
    // enumeration reaches rendered copy. The model above stays intact for the
    // non-copy consumers (editionsFor()/portsFor()).
    expect(facts.editionClause(['WebP', 'AVIF'])).toBe('WebP and AVIF');
    expect(facts.editionClause(['SVG'])).toBe('SVG');
    // An unknown format is skipped, never guessed.
    expect(facts.editionClause(['WebP', 'JXL'])).toBe('WebP');
    // …and no rendered clause may leak a line label or a port (founder
    // decision, mps2 #1000).
    for (const c of [
      facts.editionClause(),
      facts.editionClause(['WebP', 'AVIF']),
      facts.editionClause(['AVIF']),
    ]) {
      for (const line of [facts.V1_LINE, facts.V2_LINE, facts.CURRENT_LINE]) {
        expect(c).not.toContain(line);
      }
      for (const port of ['Apache', 'IIS', 'ASP.NET Core', 'Envoy']) {
        expect(c).not.toContain(port);
      }
    }
  });

  it('1.1 manifest tag === v{semver}+r{revision} (catches a "1.1.0 base" regression)', () => {
    const { semver, revision, tag } = manifest1_1.release;
    expect(revision).not.toBeNull();
    expect(tag).toBe(`v${semver}+r${revision}`);
    // Derived, not hardcoded: the manifest's semver must sit on the V1 line.
    expect(semver.startsWith(`${facts.V1_LINE}.`)).toBe(true);
  });

  // The docs-1.1 collection (formerly narrowed to a single archived
  // release-notes page) and its /1.1/docs/[slug] route are retired — the
  // converged history lives at docs/release-notes.mdx. Replaces the old
  // legacy-docs-retirement.test.ts, which asserted the collection held
  // exactly that one entry; there is no entry to hold now.
  it('the docs-1.1 collection no longer exists', () => {
    expect(existsSync(path.join(CONTENT_DIR, 'docs-1.1'))).toBe(false);
    expect(existsSync(path.join(WEBSITE_ROOT, 'src/pages/1.1/docs/[slug].astro'))).toBe(false);
  });
});

// ---------------------------------------------------------------------------
// (5) MATCHER UNIT TESTS — prove every regex + the exemption discriminate bad
//     vs good (incl. variants) without touching real content.
// ---------------------------------------------------------------------------
describe('denylist matchers discriminate bad vs good samples', () => {
  for (const rule of DENYLIST) {
    it(`[${rule.id}] flags its known-bad sample`, () => {
      expect(isFlaggedByRule(rule, rule.bad)).toBe(true);
    });
    it(`[${rule.id}] does NOT flag its known-good sample`, () => {
      expect(rulesFlagging(rule.good)).toEqual([]);
    });
    if (rule.variantsBad?.length) {
      it(`[${rule.id}] flags all bad variants (synonym/word-order coverage)`, () => {
        const missed = rule.variantsBad!.filter((s) => !isFlaggedByRule(rule, s));
        expect(
          missed,
          missed.length ? `\nnot flagged:\n  ${missed.join('\n  ')}\n` : undefined,
        ).toEqual([]);
      });
    }
    if (rule.variantsGood?.length) {
      it(`[${rule.id}] does NOT flag any good variant`, () => {
        const wrong = rule.variantsGood!.filter((s) => rulesFlagging(s).length > 0);
        expect(
          wrong,
          wrong.length ? `\nwrongly flagged:\n  ${wrong.join('\n  ')}\n` : undefined,
        ).toEqual([]);
      });
    }
  }

  // ADVERSARIAL: an exemption must not disarm a rule on a sentence that carries
  // the exemption term naturally but is FALSE.
  for (const rule of DENYLIST) {
    if (!rule.exemptionMustNotDisarm?.length) continue;
    it(`[${rule.id}] exemptions do NOT disarm it on false copy that carries an exemption term`, () => {
      const disarmed = rule.exemptionMustNotDisarm!.filter((s) => !isFlaggedByRule(rule, s));
      expect(
        disarmed,
        disarmed.length
          ? `\nexemption wrongly disarmed ${rule.id} on:\n  ${disarmed.join('\n  ')}\n`
          : undefined,
      ).toEqual([]);
    });
  }

  // A whole-file exemption cannot be probed with a sentence, so it is pinned
  // instead: the exact set of scanned files it removes from a rule's eyes is
  // asserted. Broadening the predicate, or a live copy surface drifting into
  // the exempted shape (a new release-notes-*.mdx that is really a landing
  // page, say), fails here rather than quietly going unguarded.
  for (const rule of DENYLIST) {
    if (!rule.exemptFile) continue;
    it(`[${rule.id}] the file exemption covers ONLY the archival surfaces it names`, () => {
      const covered = SCAN_FILES.map((f) =>
        path.relative(WEBSITE_ROOT, f).split(path.sep).join('/'),
      )
        .filter((rel) => rule.exemptFile!(rel))
        .sort();
      expect(covered).toEqual(rule.exemptFileExpectation ?? []);
    });
  }

  // An exact-line exemption names one line; if that line changes or moves to
  // another file, the entry goes stale and must fail rather than linger.
  for (const rule of DENYLIST) {
    if (!rule.exemptLines?.length) continue;
    it(`[${rule.id}] each exact-line exemption still matches exactly one line`, () => {
      for (const e of rule.exemptLines!) {
        const lines = readFileSync(path.join(WEBSITE_ROOT, e.file), 'utf8').split('\n');
        expect(lines.filter((l) => l === e.line).length, `${e.file}: ${e.line}`).toBe(1);
        expect(isFlaggedByRule(rule, e.line), `${e.line} no longer needs the exemption`).toBe(true);
      }
    });
  }

  // f4 must catch the two defects that motivated it, in their REAL wording.
  it('[f4] flags every historical pre-fix defect line verbatim', () => {
    const f4 = DENYLIST.find((r) => r.id.startsWith('f4-'))!;
    const missed = HISTORICAL_DEFECTS.filter((d) => !isFlaggedByRule(f4, d.line)).map(
      (d) => d.site,
    );
    expect(
      missed,
      missed.length
        ? `\nf4 MISSED its own motivating defects:\n  ${missed.join('\n  ')}\n`
        : undefined,
    ).toEqual([]);
  });

  // The comment classifier is LANGUAGE-AWARE; the .tmpl/.txt bucket is
  // markdown-shaped, so '#' there is a HEADING (114/114 measured), not a
  // comment. Getting this wrong is what made a comment opt-out look cheap.
  it('the comment classifier does not mistake Markdown headings for comments', () => {
    // '#' is a HEADING in every bucket that has one — never a comment.
    expect(
      isCommentLine('## Variant-Aware Caching', 'scripts/llms-templates/llms-full.txt.tmpl'),
    ).toBe(false);
    expect(isCommentLine('# ModPageSpeed', 'public/llms.txt')).toBe(false);
    expect(isCommentLine('## AVIF', 'src/content/blog/x.md')).toBe(false);
    expect(isCommentLine('- a bullet', 'src/content/blog/x.md')).toBe(false);
    // Real comment forms, per language.
    expect(isCommentLine('<!-- editorial note -->', 'src/content/blog/x.md')).toBe(true);
    expect(isCommentLine('// a note', 'src/data/product-facts.mjs')).toBe(true);
    expect(isCommentLine(' * jsdoc', 'src/data/psi-mps-mapping.ts')).toBe(true);
    expect(isCommentLine('// a note', 'public/llms.txt')).toBe(true);
    // Markdown's '//' is NOT a comment — it is ordinary text (and a URL tail).
    expect(isCommentLine('// not a comment in markdown', 'src/content/blog/x.md')).toBe(false);
  });

  it('no denylist rule fires on any allowlisted legit historical/dependency ref', () => {
    const wrongly = ALLOWLIST_SAMPLES.flatMap((s) => rulesFlagging(s).map((r) => `[${r.id}] ${s}`));
    expect(wrongly).toEqual([]);
  });
});

// ---------------------------------------------------------------------------
// (4) CORPUS TESTS — corrective copy and engine-tag refs must never be flagged.
// ---------------------------------------------------------------------------
describe('false-positive corpus: correct corrective/contrast copy is never flagged', () => {
  it('no rule flags any FP-corpus sample (negation/contrast exemption holds)', () => {
    const wrongly = FP_CORPUS.flatMap((s) => rulesFlagging(s).map((r) => `[${r.id}] ${s}`));
    expect(wrongly, wrongly.length ? `\n${wrongly.join('\n')}\n` : undefined).toEqual([]);
  });

  it('no rule flags a correct v1.1.0+rN engine-tag reference (§5)', () => {
    const wrongly = ENGINE_TAG_CORPUS.flatMap((s) => rulesFlagging(s).map((r) => `[${r.id}] ${s}`));
    expect(wrongly, wrongly.length ? `\n${wrongly.join('\n')}\n` : undefined).toEqual([]);
  });
});

// ---------------------------------------------------------------------------
// (1) THE GUARD — scan real content/pages; each rule must produce zero hits.
// ---------------------------------------------------------------------------
describe('content accuracy guard (anti-drift; PR #705 must not regress)', () => {
  // SCAN FLOORS — one per bucket. `walk()` swallows a missing directory and
  // returns [], so without these a renamed/moved tree turns the whole guard
  // green while scanning nothing.
  for (const bucket of SCAN_BUCKETS) {
    it(`scan floor: bucket "${bucket.name}" collects >= ${bucket.floor} files`, () => {
      expect(
        bucket.files.length,
        `bucket "${bucket.name}" collected ${bucket.files.length} files (floor ${bucket.floor}) — ` +
          'a path typo or moved tree silently degrades this guard to green',
      ).toBeGreaterThanOrEqual(bucket.floor);
    });

    // A bucket TOTAL is blind to losing one whole extension inside a mixed
    // bucket — see the SCAN_BUCKETS note. Assert each extension separately.
    for (const [ext, min] of Object.entries(bucket.extFloors ?? {})) {
      it(`scan floor: bucket "${bucket.name}" collects >= ${min} ${ext} files`, () => {
        const n = bucket.files.filter((f) => path.extname(f).toLowerCase() === ext).length;
        expect(
          n,
          `bucket "${bucket.name}" collected ${n} ${ext} files (floor ${min}) — the bucket ` +
            'total can stay green while an entire extension drops out of the scan',
        ).toBeGreaterThanOrEqual(min);
      });
    }
  }

  it('finds files to scan (extraction sanity)', () => {
    expect(SCAN_FILES.length).toBeGreaterThan(0);
    expect(CONTENT_FILES.length).toBeGreaterThan(0);
  });

  it('excluded paths are absent from the scan set', () => {
    const leaked = SCAN_FILES.filter((f) => isExcluded(f)).map((f) =>
      path.relative(WEBSITE_ROOT, f),
    );
    expect(leaked, leaked.length ? `\n${leaked.join('\n')}\n` : undefined).toEqual([]);
  });

  for (const rule of DENYLIST) {
    it(`[${rule.id}] no violations across the full scan surface`, () => {
      const { failures, pendingHits } = unpendedViolations(rule);
      if (pendingHits > 0) {
        console.warn(
          `[${rule.id}] ${pendingHits} pending hits in ${Object.keys(rule.pending ?? {}).length} files (scripts/retired-label-pending.json) — warnings until the rename removes them`,
        );
      }
      expect(
        failures,
        failures.length
          ? `\n${rule.id} — ${rule.why}\n` + failures.map((f) => `  ${f}`).join('\n') + '\n'
          : undefined,
      ).toEqual([]);
    });
  }

  it('aggregate: zero denylist violations across all rules', () => {
    const all = DENYLIST.flatMap((r) => unpendedViolations(r).failures);
    expect(all, all.length ? '\n' + all.map((f) => `  ${f}`).join('\n') + '\n' : undefined).toEqual(
      [],
    );
  });

  // The ratchet is only as good as its file: every pending entry names a
  // scanned file and a positive count.
  it('retired-label-pending.json lists only scanned files, with positive counts', () => {
    const scanned = new Set(
      SCAN_FILES.map((f) => path.relative(WEBSITE_ROOT, f).split(path.sep).join('/')),
    );
    const bad = Object.entries(RETIRED_LABEL_PENDING).filter(
      ([file, n]) => !scanned.has(file) || !Number.isInteger(n) || n < 1,
    );
    expect(bad).toEqual([]);
  });
});

// ---------------------------------------------------------------------------
// (6) CANARY FIXTURES — prove the SCAN PATH itself still works.
//
//   The matcher unit tests above call isFlaggedByRule() on string literals, so
//   they bypass the scanner entirely: the walker, the extension filter and the
//   line splitter are all unexercised by them. If walk() silently stopped
//   returning files, if an extension dropped out of a bucket, or if the line
//   splitter broke, every one of those tests would still pass and the guard
//   would be green while scanning nothing.
//
//   The canaries close that gap. test/fixtures/canary/<rule-id>.md each hold
//   that rule's `bad` sample on a known line, and live under test/** so they
//   are OUTSIDE SCAN_FILES (they would otherwise self-flag the real guard).
//   They are then scanned with the SAME walk() and the SAME scan function, and
//   each rule must produce exactly one hit at exactly the expected line.
// ---------------------------------------------------------------------------
const CANARY_DIR = path.join(WEBSITE_ROOT, 'test/fixtures/canary');
const CANARY_BAD_LINE = 5; // '---', title, '---', blank, then the bad sample
const CANARY_FILES = walk(CANARY_DIR, ['.md'], /* honorExclusions */ false);

// ---------------------------------------------------------------------------
// BLOCK SCANNING (issue: a wrapped false claim was invisible to the line scan).
// ---------------------------------------------------------------------------
// Wrap `sample` at one word boundary; a null return means the boundary is not a
// legitimate Markdown wrap point (the next word would open a list item, a
// heading, a quote or a table row, which ends the paragraph by design).
function wrapAt(sample: string, wordIndex: number): string | null {
  const words = sample.split(' ');
  if (wordIndex <= 0 || wordIndex >= words.length) return null;
  const next = words[wordIndex];
  if (/^(?:[-*+>|#]|\d+[.)]|```|~~~|<!--)/.test(next)) return null;
  return words.slice(0, wordIndex).join(' ') + '\n' + words.slice(wordIndex).join(' ');
}
function wrapPoints(sample: string): string[] {
  const out: string[] = [];
  for (let i = 1; i < sample.split(' ').length; i++) {
    const w = wrapAt(sample, i);
    if (w !== null) out.push(w);
  }
  return out;
}
const scanMd = (rule: DenyRule, text: string) =>
  scanTextForRule(rule, text, 'src/content/blog/sample.md', 'src/content/blog/sample.md');

describe('block scanning: classifier, joining and offset map', () => {
  const md = (text: string) => buildBlocks(text, 'a.md').map((b) => b.text);

  it('classifies frontmatter, fenced code and prose', () => {
    const lines = ['---', 'title: x', '---', 'prose', '```sh', 'apt install y', '```', 'more'];
    expect(classifyLines(lines, 'a.md')).toEqual([
      'frontmatter',
      'frontmatter',
      'frontmatter',
      'prose',
      'fence',
      'fence',
      'fence',
      'prose',
    ]);
    // A '---' that is not on line 1 is a thematic break, not frontmatter.
    expect(classifyLines(['intro', '---', 'x'], 'a.md')).toEqual(['prose', 'prose', 'prose']);
  });

  it('joins consecutive prose lines with one space and ends blocks at blank lines', () => {
    expect(md('AVIF is\n  ModPageSpeed 2.0\nonly.\n\nNext paragraph.')).toEqual([
      'AVIF is ModPageSpeed 2.0 only.',
      '',
      'Next paragraph.',
    ]);
  });

  it('ends a block at headings, table rows, list markers and blockquotes', () => {
    expect(md('one\n## Heading\ntwo\n| a | b |\nthree')).toEqual([
      'one',
      '## Heading',
      'two',
      '| a | b |',
      'three',
    ]);
    expect(md('- item one\n  wraps here\n- item two\n1. numbered\n2) other')).toEqual([
      '- item one wraps here',
      '- item two',
      '1. numbered',
      '2) other',
    ]);
    expect(md('before\n> quoted line\n> continues\nafter')).toEqual([
      'before',
      '> quoted line continues',
      'after',
    ]);
  });

  it('never joins frontmatter or fenced code to the prose around them', () => {
    expect(md('---\ntitle: A\ndescription: B\n---\ntext\n```\ncode a\ncode b\n```\ntail')).toEqual([
      '---',
      'title: A',
      'description: B',
      '---',
      'text',
      '```',
      'code a',
      'code b',
      '```',
      'tail',
    ]);
  });

  it('keeps one block per line outside markdown-shaped files (code breaks separate code)', () => {
    expect(buildBlocks('a\nb', 'x.astro').map((b) => b.text)).toEqual(['a', 'b']);
    expect(buildBlocks('a\nb', 'x.ts').map((b) => b.text)).toEqual(['a', 'b']);
    expect(buildBlocks('a\nb', 'x.txt').map((b) => b.text)).toEqual(['a b']);
  });

  it('maps a match offset back to its physical line', () => {
    const [block] = buildBlocks('alpha beta\ngamma delta\nepsilon', 'a.md');
    expect(lineOfOffset(block, block.text.indexOf('alpha'))).toBe(1);
    expect(lineOfOffset(block, block.text.indexOf('gamma'))).toBe(2);
    expect(lineOfOffset(block, block.text.indexOf('epsilon'))).toBe(3);
  });

  it('reports a wrapped claim on the line where the match starts', () => {
    const f1 = DENYLIST.find((r) => r.id.startsWith('f1-'))!;
    const hits = scanMd(f1, 'intro line\n\nThe encoder AVIF is\nModPageSpeed 2.0 only.\n');
    expect(hits.map((h) => h.line)).toEqual([3]);
  });
});

describe('block scanning: wrapped claims (the gap the line scanner had)', () => {
  // The motivating miss, verbatim: the old scanner saw "AVIF is" and
  // "ModPageSpeed 2.0 only." as two harmless lines.
  it('flags a claim wrapped across the line break that the per-line scan misses', () => {
    const f1 = DENYLIST.find((r) => r.id.startsWith('f1-'))!;
    const wrapped = 'The encoder AVIF is\nModPageSpeed 2.0 only.';
    expect(wrapped.split('\n').some((l) => isFlaggedByRule(f1, l))).toBe(false); // old behaviour
    expect(scanMd(f1, wrapped).length).toBe(1);
  });

  for (const rule of DENYLIST) {
    const bads = [rule.bad, ...(rule.variantsBad ?? [])];
    it(`[${rule.id}] flags every known-bad sample wrapped at every word boundary`, () => {
      const missed: string[] = [];
      for (const bad of bads) {
        for (const wrapped of wrapPoints(bad)) {
          if (scanMd(rule, wrapped).length === 0) missed.push(JSON.stringify(wrapped));
        }
      }
      expect(missed, missed.length ? `\nmissed:\n  ${missed.join('\n  ')}\n` : undefined).toEqual(
        [],
      );
    });

    const goods = [rule.good, ...(rule.variantsGood ?? [])];
    it(`[${rule.id}] does NOT flag any known-good sample wrapped at every word boundary`, () => {
      const wrong: string[] = [];
      for (const good of goods) {
        for (const wrapped of wrapPoints(good)) {
          for (const r of DENYLIST) {
            if (scanMd(r, wrapped).length > 0) wrong.push(`[${r.id}] ${JSON.stringify(wrapped)}`);
          }
        }
      }
      expect(
        wrong,
        wrong.length ? `\nwrongly flagged:\n  ${wrong.join('\n  ')}\n` : undefined,
      ).toEqual([]);
    });
  }

  // Joining must not turn two true sentences into one false claim: the
  // proximity windows stay bounded and clause-scoped on the joined block.
  it('the FP corpus and every known-good sample, joined into running paragraphs, flag nothing', () => {
    const goods = [...FP_CORPUS, ...DENYLIST.flatMap((r) => [r.good, ...(r.variantsGood ?? [])])];
    const wrong: string[] = [];
    for (let step = 1; step <= 3; step++) {
      for (let i = 0; i + 1 < goods.length; i += step) {
        // Two neighbours wrapped into one paragraph, as a writer would hard-wrap it.
        const paragraph = `${goods[i]}\n${goods[i + 1]}`;
        for (const r of DENYLIST) {
          // A crossSentence rule is paragraph-scoped by design (see the flag);
          // two unrelated samples glued into one paragraph are outside its contract.
          if (r.crossSentence) continue;
          const alone = [goods[i], goods[i + 1]].some((g) => isFlaggedByRule(r, g));
          if (!alone && scanMd(r, paragraph).length > 0) {
            wrong.push(`[${r.id}] ${JSON.stringify(paragraph)}`);
          }
        }
      }
    }
    expect(wrong, wrong.length ? `\n${wrong.join('\n')}\n` : undefined).toEqual([]);
  });

  // A real false positive the joined scan found in the shipped docs: a wrapped
  // paragraph whose "pagespeed-optimizer" is a config-file PATH, followed by a
  // true statement about the container images. Pins the path guard.
  it('a config path named pagespeed-optimizer is not the worker subject', () => {
    const b3 = DENYLIST.find((r) => r.id.startsWith('b3-'))!;
    const text =
      'then point the optimizer worker at it from `/etc/default/pagespeed-optimizer` -- the default\nbinary path exists only in the container images:';
    expect(scanMd(b3, text)).toEqual([]);
    // The worker as a subject is still caught across the same wrap.
    expect(
      scanMd(b3, 'The pagespeed-optimizer worker ships\nonly as a container image.').length,
    ).toBe(1);
  });

  it('frontmatter and fenced lines are classified, not glued to neighbouring prose', () => {
    const f1 = DENYLIST.find((r) => r.id.startsWith('f1-'))!;
    // "AVIF is" at the end of a fence must not join the prose "ModPageSpeed 2.0 only." after it.
    expect(scanMd(f1, '```\nAVIF is\n```\nModPageSpeed 2.0 only.')).toEqual([]);
    expect(scanMd(f1, '---\ntitle: AVIF is\n---\nModPageSpeed 2.0 only.')).toEqual([]);
  });
});

// ---------------------------------------------------------------------------
// MARKUP: shadow text strips tags and decodes entities, position-preserving.
// ---------------------------------------------------------------------------
describe('shadow text: markup and entities no longer hide a claim', () => {
  it('strips inline tags without fusing words, and block tags become one space', () => {
    expect(shadowText('AVIF is <strong>ModPageSpeed 2.0</strong> only.').text).toBe(
      'AVIF is ModPageSpeed 2.0 only.',
    );
    expect(shadowText('<td>AVIF</td><td>2.0</td>').text).toBe(' AVIF  2.0 ');
    expect(shadowText('a<br>b').text).toBe('a b');
    expect(shadowText('<em>2.0</em>-only').text).toBe('2.0-only');
  });

  it('decodes named and numeric entities and leaves unknown ones alone', () => {
    expect(shadowText('AVIF&nbsp;is 2.0&#8209;only &mdash; &x;').text).toBe(
      'AVIF is 2.0\u2011only \u2014 &x;',
    );
    expect(shadowText('a &lt;b&gt; &amp; c').text).toBe('a <b> & c');
    // A bare '<' that opens no tag is text.
    expect(shadowText('1 < 2 and AVIF').text).toBe('1 < 2 and AVIF');
  });

  it('keeps a map from every shadow character back to the source index', () => {
    const src = 'x <b>AVIF</b>&nbsp;y';
    const { text, map } = shadowText(src);
    expect(text).toBe('x AVIF y');
    expect(src[map[text.indexOf('A')]]).toBe('A');
    expect(map[text.indexOf('y')]).toBe(src.indexOf('y'));
    expect(src.slice(map[text.indexOf(' y')])).toBe('&nbsp;y');
    expect(map.length).toBe(text.length);
  });

  it('flags a claim split by tags or entities, on the right physical line', () => {
    const f1 = DENYLIST.find((r) => r.id.startsWith('f1-'))!;
    expect(isFlaggedByRule(f1, 'AVIF is <strong>ModPageSpeed 2.0</strong> only.')).toBe(false); // raw: blind
    const hits = scanMd(f1, 'intro\n\nAVIF is <strong>ModPageSpeed 2.0</strong>\nonly.');
    expect(hits.map((h) => h.line)).toEqual([3]);
    expect(scanMd(f1, 'AVIF&nbsp;is ModPageSpeed 2.0 only.').length).toBe(1);
    // The same markup around a TRUE sentence stays clear.
    expect(scanMd(f1, 'AVIF is <strong>available</strong> across 1.15 and 2.0.')).toEqual([]);
  });
});

// ---------------------------------------------------------------------------
// FACT ANCHORS (exclusivity rules must be tied to product-facts.mjs).
// ---------------------------------------------------------------------------
describe('exclusivity rules are anchored to a machine-checked fact', () => {
  // The rules that assert "X is NOT exclusive to 2.0". Adding a rule of this
  // shape means adding its id here, which is what forces an anchor.
  const ANCHORED_RULE_IDS = [
    'f1-avif-exclusive-to-2.0',
    'f2-1.15-does-not-ship-avif',
    'f3-zero-copy-and-variant-cache-are-not-2.0-only',
    'f4-implicit-exclusivity-by-scoping',
  ];
  const anchored = () => ANCHORED_RULE_IDS.map((id) => DENYLIST.find((r) => r.id === id)!);

  it('every exclusivity rule exists and names its anchor', () => {
    for (const id of ANCHORED_RULE_IDS) {
      const rule = DENYLIST.find((r) => r.id === id);
      expect(rule, `rule ${id} is missing`).toBeTruthy();
      expect(rule!.anchor, `rule ${id} must name an anchor`).toBeTruthy();
      expect(rule!.anchor!.describes.length).toBeGreaterThan(20);
    }
    // And nothing else claims an anchor without being on the list.
    const stray = DENYLIST.filter((r) => r.anchor && !ANCHORED_RULE_IDS.includes(r.id)).map(
      (r) => r.id,
    );
    expect(stray).toEqual([]);
  });

  it('every anchor holds against the current product-facts', () => {
    const broken = anchored()
      .filter((r) => !r.anchor!.holds(facts))
      .map((r) => `${r.id}: ${r.anchor!.describes}`);
    expect(
      broken,
      broken.length
        ? `\nthe fact behind these rules changed; re-verify each rule before editing the table:\n  ${broken.join('\n  ')}\n`
        : undefined,
    ).toEqual([]);
  });

  it('an anchor notices a changed fact (mutation probes)', () => {
    type Row = { format: string; editions: Record<string, unknown> };
    const dropEdition = (rows: Row[], key: string, edition: string): Row[] =>
      rows.map((r) => {
        if (r.format !== key) return r;
        const { [edition]: _gone, ...rest } = r.editions;
        return { ...r, editions: rest };
      });
    const base = facts as FactsView;
    const byId = (id: string) => DENYLIST.find((r) => r.id.startsWith(id))!;

    // AVIF drops off the 1.15 line: f1, f2 and f4 must go red.
    const noAvif115: FactsView = {
      ...base,
      IMAGE_FORMAT_SUPPORT: dropEdition(
        base.IMAGE_FORMAT_SUPPORT as Row[],
        'AVIF',
        base.V1_LINE,
      ) as FactsView['IMAGE_FORMAT_SUPPORT'],
    };
    for (const id of ['f1-', 'f2-', 'f4-']) expect(byId(id).anchor!.holds(noAvif115)).toBe(false);
    expect(byId('f3-').anchor!.holds(noAvif115)).toBe(true);

    // Zero-copy becomes 2.0-only: f3 and f4 must go red.
    const zeroCopy2Only: FactsView = {
      ...base,
      CAPABILITY_SUPPORT: (
        base.CAPABILITY_SUPPORT as Array<{ capability: string; editions: object }>
      ).map((r) =>
        r.capability === 'zero-copy-serving' ? { ...r, editions: { [base.V2_LINE]: {} } } : r,
      ) as FactsView['CAPABILITY_SUPPORT'],
    };
    for (const id of ['f3-', 'f4-']) expect(byId(id).anchor!.holds(zeroCopy2Only)).toBe(false);
    expect(byId('f1-').anchor!.holds(zeroCopy2Only)).toBe(true);

    // A capability row that disappears is a changed fact too.
    const noRows: FactsView = { ...base, CAPABILITY_SUPPORT: [] };
    expect(byId('f3-').anchor!.holds(noRows)).toBe(false);
  });

  it('the capability table matches the helper and never claims Envoy', () => {
    expect(facts.capabilityEditions('zero-copy-serving')).toEqual([facts.V1_LINE, facts.V2_LINE]);
    expect(facts.capabilityEditions('variant-aware-caching')).toEqual([
      facts.V1_LINE,
      facts.V2_LINE,
    ]);
    expect(facts.capabilityEditions('no-such-capability')).toEqual([]);
    expect(JSON.stringify(facts.CAPABILITY_SUPPORT)).not.toMatch(/envoy/i);
    expect(facts.V2_ONLY_PROPERTIES.length).toBeGreaterThan(0);
  });
});

// ---------------------------------------------------------------------------
// COMPUTED CLAIMS: the comparison pages build cells with editionClause(...) inside
// template literals, so a source scan sees the expression, not the sentence a
// reader gets. Evaluate the helper, render the sentence, run the rules on it.
// ---------------------------------------------------------------------------
type RenderedClaim = { page: string; line: number; source: string; rendered: string };

// Every template literal in `src` that interpolates editionClause([...]),
// rendered with the REAL helper. Any other interpolation must be a string
// export of product-facts.mjs (rendered too) or the render fails loudly, so a
// new computed expression cannot slip past unevaluated.
function renderEditionClauseTemplates(src: string, page: string): RenderedClaim[] {
  const out: RenderedClaim[] = [];
  const literal = /`((?:[^`\\]|\\.)*)`/g;
  let m: RegExpExecArray | null;
  while ((m = literal.exec(src)) !== null) {
    if (!m[1].includes('${editionClause(')) continue;
    const rendered = m[1].replace(/\$\{\s*([^}]+?)\s*\}/g, (_all, expr: string) => {
      const call = /^editionClause\(\s*(\[[^\]]*\])?\s*\)$/.exec(expr);
      if (call) {
        const formats = call[1] ? (JSON.parse(call[1].replace(/'/g, '"')) as string[]) : undefined;
        return facts.editionClause(formats);
      }
      const value = (facts as Record<string, unknown>)[expr];
      if (typeof value === 'string') return value;
      throw new Error(`${page}: cannot evaluate \${${expr}} in a computed claim`);
    });
    out.push({
      page,
      line: src.slice(0, m.index).split('\n').length,
      source: m[1],
      rendered,
    });
  }
  return out;
}

describe('computed claims: editionClause() cells on the comparison pages', () => {
  const VS_DIR = path.join(WEBSITE_ROOT, 'src/pages/alternatives');
  const vsPages = readdirSync(VS_DIR)
    .filter((n) => n.endsWith('.astro'))
    .sort();
  const claims = vsPages.flatMap((n) =>
    renderEditionClauseTemplates(
      readFileSync(path.join(VS_DIR, n), 'utf8'),
      `src/pages/alternatives/${n}`,
    ),
  );

  it('finds the computed cells on every vs page that uses the helper', () => {
    const usingHelper = vsPages.filter((n) =>
      readFileSync(path.join(VS_DIR, n), 'utf8').includes('editionClause('),
    );
    expect(usingHelper.length).toBeGreaterThanOrEqual(8);
    for (const n of usingHelper) {
      expect(
        claims.some((c) => c.page === `src/pages/alternatives/${n}`),
        `${n} calls editionClause() but no template literal was evaluated`,
      ).toBe(true);
    }
  });

  it('renders the sentence a reader sees (helper evaluated, entities decoded)', () => {
    const imgix = claims.find((c) => c.page.endsWith('/imgix.astro'))!;
    expect(shadowText(imgix.rendered).text).toBe(
      'WebP and AVIF \u2014 responsive variants at origin',
    );
  });

  it('no rule flags any rendered computed cell', () => {
    const wrong: string[] = [];
    for (const c of claims) {
      const text = shadowText(c.rendered).text;
      for (const r of DENYLIST) {
        if (isFlaggedByRule(r, c.rendered) || isFlaggedByRule(r, text)) {
          wrong.push(`[${r.id}] ${c.page}:${c.line}: ${text}`);
        }
      }
    }
    expect(wrong, wrong.length ? `\n${wrong.join('\n')}\n` : undefined).toEqual([]);
  });

  it('a helper that rendered a wrong fence WOULD be caught', () => {
    // Stand-in for a regression in the helper or the call site: the rendered
    // cell fences AVIF to 2.0. The rules must flag the rendered text.
    const f1 = DENYLIST.find((r) => r.id.startsWith('f1-'))!;
    const bad = renderEditionClauseTemplates(
      "const c = `${editionClause(['AVIF'])} is ModPageSpeed 2.0 only`;",
      'inline',
    );
    expect(bad.map((c) => c.rendered)).toEqual(['AVIF is ModPageSpeed 2.0 only']);
    expect(isFlaggedByRule(f1, bad[0].rendered)).toBe(true);
  });

  it('refuses an interpolation it cannot evaluate', () => {
    expect(() =>
      renderEditionClauseTemplates('const c = `${editionClause(["AVIF"])} ${mystery}`;', 'inline'),
    ).toThrow(/cannot evaluate/);
  });
});

describe('sentence-bounded windows', () => {
  it('boundWindows() keeps the length bound and stops at a sentence end, not at a version dot', () => {
    const re = boundWindows(/\bAVIF\b[^\n]{0,40}?\bonly\b/);
    expect(re.test('AVIF is shipped in 2.0 only')).toBe(true); // version dots do not end the window
    expect(re.test('AVIF is great. It is only here')).toBe(false); // a sentence end does
    expect(re.test('AVIF ' + 'x'.repeat(60) + ' only')).toBe(false); // still bounded
    expect(re.test('AVIF was in 2.0.\nit is only here')).toBe(false); // "0." before a space ends the sentence
  });

  it('the rules that opt out are exactly the paragraph-scoped ones', () => {
    expect(DENYLIST.filter((r) => r.crossSentence).map((r) => r.id.split('-')[0])).toEqual([
      'f4',
      'i',
    ]);
  });
});

describe('scan surface: the home page sections stay in scope', () => {
  const HOME_DIR = path.join(WEBSITE_ROOT, 'src/components/home');

  it('every home section file is scanned', () => {
    const onDisk = readdirSync(HOME_DIR)
      .filter((n) => n.endsWith('.astro'))
      .map((n) => path.join(HOME_DIR, n));
    expect(onDisk.length).toBeGreaterThan(0);
    const unscanned = onDisk.filter((f) => !SCAN_FILES.includes(f));
    expect(unscanned, unscanned.length ? `\n${unscanned.join('\n')}\n` : undefined).toEqual([]);
  });

  it('the exclusion covers the other components, not the home sections', () => {
    expect(isExcluded(path.join(HOME_DIR, 'Hero.astro'))).toBe(false);
    expect(isExcluded(path.join(WEBSITE_ROOT, 'src/components/CTAButton.astro'))).toBe(true);
    expect(isExcluded(path.join(WEBSITE_ROOT, 'src/components/instrument/Chip.astro'))).toBe(true);
  });

  it('the home page itself is still scanned', () => {
    expect(SCAN_FILES).toContain(path.join(WEBSITE_ROOT, 'src/pages/index.astro'));
  });
});

describe('canary fixtures: the scan path (walker, filter, splitter) still works', () => {
  it('the canary fixtures are NOT part of the real scan surface', () => {
    const leaked = SCAN_FILES.filter((f) => f.startsWith(CANARY_DIR));
    expect(leaked, leaked.length ? `\n${leaked.join('\n')}\n` : undefined).toEqual([]);
  });

  it('the walker collects one canary fixture per denylist rule', () => {
    expect(CANARY_FILES.length).toBe(DENYLIST.length);
    const missing = DENYLIST.map((r) => `${r.id}.md`).filter(
      (n) => !CANARY_FILES.some((f) => path.basename(f) === n),
    );
    expect(
      missing,
      missing.length ? `\nmissing fixtures:\n  ${missing.join('\n  ')}\n` : undefined,
    ).toEqual([]);
  });

  for (const rule of DENYLIST) {
    it(`[${rule.id}] the SCANNER finds its canary at line ${CANARY_BAD_LINE}`, () => {
      const fixture = CANARY_FILES.find((f) => path.basename(f) === `${rule.id}.md`);
      expect(fixture, `no canary fixture for ${rule.id}`).toBeTruthy();

      // The fixture must carry this rule's CURRENT bad sample verbatim, so the
      // fixture cannot drift away from the rule it is guarding.
      const lines = readFileSync(fixture!, 'utf8').split('\n');
      expect(lines[CANARY_BAD_LINE - 1]).toBe(rule.bad);

      // Scan it through the real scan function — not the string-literal path.
      const hits = scanFilesForRule(rule, [fixture!]);
      expect(
        hits.map((h) => h.line),
        `expected exactly one hit at line ${CANARY_BAD_LINE}, got:\n${hits
          .map((h) => `  ${h.location}`)
          .join('\n')}`,
      ).toEqual([CANARY_BAD_LINE]);
    });
  }
});

// ---------------------------------------------------------------------------
// Commercial copy rules (owner rulings, support-as-the-product).
//
// (1) Price amounts on /pricing/ and /support/ (and everything those pages
//     render from) are exactly the published euro amounts: 490, 1,900, 7,500
//     and 950. Any other currency amount, any dollar amount, any per-month
//     rate and any hour target other than the published 8 and 4 business
//     hours would publish a number nobody approved. Scanned RAW, as the
//     denylist is: a price in a comment drifts into copy next.
// (2) One commercial address (COMMERCIAL_EMAIL) everywhere commercial,
//     security@modpagespeed.com for disclosure only; the retired sales
//     address and the support alias stay off the site. Both literals are
//     assembled from parts so this file does not carry them either.
// ---------------------------------------------------------------------------
const RETIRED_SALES = ['sales', 'we-amp.com'].join('@');
const NOT_A_CONTACT = ['support', 'we-amp.com'].join('@');
const COMMERCIAL_PAGE_SOURCES = [
  'src/pages/pricing.astro',
  'src/pages/support.astro',
  'src/components/SupportTiers.astro',
  'src/data/faq-pricing.ts',
  'src/data/faq-support.ts',
].map((rel) => path.join(WEBSITE_ROOT, rel));

// A currency symbol or code next to a number, a per-period rate ("/yr",
// "/mo", "per year"), or an hours target ("7 business hours", "within 3
// hours"). Deliberately blind to the bands and step counts the pages do
// state ("up to 3 people", "within 3 business days").
const PRICE_AMOUNT =
  /[€$£]\s?\d|\b\d[\d.,]*\s?(?:EUR|USD|GBP|euros?|dollars?)\b|\d[\d.,]*\s*\/\s*(?:yr|year|mo|month)\b|\bper (?:year|month)\b|\b\d+\s*(?:business\s+)?hours?\b/i;
// The published amounts and targets, as they appear in the sources: euro
// amounts (490, 1,900, 7,500, 950) and the 8 and 4 business-hour targets.
const APPROVED_AMOUNT = /(?:€\s?(?:490|1,900|7,500|950)\b)|\b(?:8|4) business hours\b/;
const DISALLOWED_AMOUNT =
  /\$\s?\d|£\s?\d|\b\d[\d.,]*\s?(?:USD|GBP|dollars?)\b|\d[\d.,]*\s*\/\s*(?:mo|month)\b|\bper month\b/i;

function priceAmountHits(files: string[]): string[] {
  const hits: string[] = [];
  for (const file of files) {
    readFileSync(file, 'utf8')
      .split('\n')
      .forEach((line, i) => {
        const bad = line.match(DISALLOWED_AMOUNT);
        if (bad) hits.push(`${path.relative(WEBSITE_ROOT, file)}:${i + 1}: ${bad[0]}`);
        // Any other amount or hour target must be one of the published ones.
        const m = line.replace(new RegExp(APPROVED_AMOUNT, 'g'), '').match(PRICE_AMOUNT);
        if (m) hits.push(`${path.relative(WEBSITE_ROOT, file)}:${i + 1}: ${m[0]}`);
      });
  }
  return hits;
}

describe('commercial copy: only published amounts, one commercial address', () => {
  it('the price matcher flags amounts, rates and hour targets', () => {
    for (const bad of [
      'Standard support €123/yr',
      'from $4,567 a year',
      '6,789 EUR',
      'or €45/mo',
      'billed per year',
      'severity 1 within 7 business hours',
      'first response 3 hours',
    ]) {
      expect(PRICE_AMOUNT.test(bad), bad).toBe(true);
    }
    for (const good of [
      'up to 3 people',
      'within 3 business days',
      'Under 1 million pageviews a month',
      'early subscribers keep their quoted rate for three years',
      'SPDX 2.3 SBOM for the 2.1 line',
    ]) {
      expect(PRICE_AMOUNT.test(good), good).toBe(false);
    }
  });

  it('every commercial page source exists', () => {
    for (const file of COMMERCIAL_PAGE_SOURCES) expect(existsSync(file), file).toBe(true);
  });

  it('only the published amounts and hour targets on /pricing/ and /support/ sources', () => {
    const hits = priceAmountHits(COMMERCIAL_PAGE_SOURCES);
    expect(hits, hits.length ? `\n${hits.join('\n')}\n` : undefined).toEqual([]);
  });

  it('the support tier rows carry only the published euro prices and targets', () => {
    const rows = JSON.stringify(facts.SUPPORT_TIERS) + facts.PRICING_LINE;
    expect(rows).not.toMatch(DISALLOWED_AMOUNT);
    expect(facts.SUPPORT_TIERS.map((t) => t.prices.annualEur)).toEqual([490, 1900, 7500]);
    for (const tier of facts.SUPPORT_TIERS) expect(tier.prices.monthlyEur).toBeNull();
    expect(facts.AUDIT_PRICE_EUR).toBe(950);
  });

  it('no retired sales address anywhere on the scan surface', () => {
    const files = [...SCAN_FILES, ...COMMERCIAL_PAGE_SOURCES];
    const hits = files.filter((f) => readFileSync(f, 'utf8').includes(RETIRED_SALES));
    expect(
      hits.map((f) => path.relative(WEBSITE_ROOT, f)),
      'the retired sales address is back; use COMMERCIAL_EMAIL',
    ).toEqual([]);
  });

  it('no support alias anywhere on the scan surface', () => {
    const files = [...SCAN_FILES, ...COMMERCIAL_PAGE_SOURCES];
    const hits = files.filter((f) => readFileSync(f, 'utf8').includes(NOT_A_CONTACT));
    expect(
      hits.map((f) => path.relative(WEBSITE_ROOT, f)),
      'that address is not a contact channel; use COMMERCIAL_EMAIL',
    ).toEqual([]);
  });

  it('the commercial address is the single-sourced one', () => {
    expect(facts.COMMERCIAL_EMAIL).toBe('info@we-amp.com');
  });
});

// ---------------------------------------------------------------------------
// Single-sourced facts in page sources.
//
// The live-site count, its as-of month, the current release version and the
// support and audit prices live in product-facts.mjs. A page or component
// that types one of them out goes stale the next time the fact moves, while
// the pages that import it move with it. Scanned RAW, comments included, for
// the same reason as the denylist: a number in a comment drifts into copy.
// ---------------------------------------------------------------------------
const PAGE_SOURCE_DIRS = ['src/pages', 'src/components', 'src/layouts'].map((rel) =>
  path.join(WEBSITE_ROOT, rel),
);

function pageSourceFiles(): string[] {
  const out: string[] = [];
  const walk = (dir: string) => {
    for (const name of readdirSync(dir)) {
      const abs = path.join(dir, name);
      if (statSync(abs).isDirectory()) walk(abs);
      else if (/\.(?:astro|ts|tsx|mjs|js|md|mdx)$/.test(name)) out.push(abs);
    }
  };
  PAGE_SOURCE_DIRS.forEach(walk);
  return out;
}

// The rounded form the pages render (roundDownDisplay to the nearest 10,000).
function roundedLiveSites(): string {
  const sites = Number(facts.BUILTWITH_SITES.replace(/,/g, ''));
  return `${(Math.floor(sites / 10000) * 10000).toLocaleString('en-US')}+`;
}

function hardCodedFacts(): Array<{ label: string; literal: RegExp }> {
  const rounded = roundedLiveSites();
  const priceLiterals = [
    ...facts.SUPPORT_TIERS.map((t) => t.prices.annualEur),
    facts.AUDIT_PRICE_EUR,
  ].map((n) => facts.formatEur(n));
  return [
    { label: 'BUILTWITH_SITES', literal: new RegExp(escapeForRegExp(facts.BUILTWITH_SITES)) },
    { label: 'the rounded live-site count', literal: new RegExp(escapeForRegExp(rounded)) },
    {
      label: 'BUILTWITH_AS_OF next to BuiltWith',
      literal: new RegExp(`BuiltWith[^\\n]{0,40}${escapeForRegExp(facts.BUILTWITH_AS_OF)}`),
    },
    {
      label: 'MODULE_PACKAGE_VERSION',
      literal: new RegExp(`(?<![\\d.])${escapeForRegExp(facts.MODULE_PACKAGE_VERSION)}(?![\\d])`),
    },
    ...priceLiterals.map((p) => ({
      label: `the published price ${p}`,
      literal: new RegExp(`${escapeForRegExp(p)}(?!\\d|[.,]\\d)`),
    })),
  ];
}

function hardCodedFactHits(files: string[]): string[] {
  const rules = hardCodedFacts();
  const hits: string[] = [];
  for (const file of files) {
    readFileSync(file, 'utf8')
      .split('\n')
      .forEach((line, i) => {
        for (const r of rules) {
          if (r.literal.test(line)) {
            hits.push(`${path.relative(WEBSITE_ROOT, file)}:${i + 1}: ${r.label}`);
          }
        }
      });
  }
  return hits;
}

describe('single-sourced facts: page sources import them, never type them out', () => {
  it('the matcher flags typed-out copies and passes the imported forms', () => {
    const rules = hardCodedFacts();
    const flagged = (line: string) => rules.some((r) => r.literal.test(line));
    for (const bad of [
      `<span>${facts.BUILTWITH_SITES} live sites</span>`,
      `the core behind ${roundedLiveSites()} live sites`,
      `BuiltWith, ${facts.BUILTWITH_AS_OF}`,
      `Install ${facts.MODULE_PACKAGE_VERSION} today`,
      `Support costs ${facts.formatEur(facts.SUPPORT_TIERS[0].prices.annualEur)} a year`,
      `Support costs ${facts.formatEur(facts.SUPPORT_TIERS[0].prices.annualEur)}, or more`,
    ]) {
      expect(flagged(bad), bad).toBe(true);
    }
    for (const good of [
      '<span>{builtwithLiveRounded} live sites</span>',
      'BuiltWith, {BUILTWITH_AS_OF}',
      'Hacker News in May 2026',
      `version ${facts.MODULE_PACKAGE_VERSION}0`,
      'up to 3 people',
    ]) {
      expect(flagged(good), good).toBe(false);
    }
  });

  it('no page, component or layout source types out a single-sourced fact', () => {
    const hits = hardCodedFactHits(pageSourceFiles());
    expect(
      hits,
      hits.length ? `\nimport these from product-facts.mjs:\n${hits.join('\n')}\n` : undefined,
    ).toEqual([]);
  });
});
