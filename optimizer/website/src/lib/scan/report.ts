// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The downloadable Markdown report: what the page showed, as plain text.
// Pure; the caller turns the string into a file.

import { RISK_LENSES, lensMeasured } from './status';

type Report = Record<string, unknown> & { grade?: string; score?: number };

const LENS_NAME: Record<string, string> = {
  preConsentLeak: 'Trackers before consent',
  scriptInventory: 'Scripts on the page',
  seoDefects: 'SEO defects',
  responseExposure: 'Errors and versions the server reveals',
};

/** The four Risk & SEO states, in panel order; a lens that is off or missing has no row. */
export function riskStates(report: Report | null): Array<{ name: string; state: string }> {
  const rows: Array<{ name: string; state: string }> = [];
  if (!report) return rows;
  for (const lens of RISK_LENSES) {
    const v = report[lens.key] as { status?: string; verdict?: string } | undefined;
    if (!v || v.status === 'disabled') continue;
    rows.push({
      name: LENS_NAME[lens.key],
      state: !lensMeasured(v)
        ? 'Not measured'
        : v.verdict === lens.flagged
          ? 'Attention'
          : 'Nothing flagged',
    });
  }
  return rows;
}

const SCORE = (n: number | null | undefined) => (typeof n === 'number' ? `${n}/100` : 'n/a');

export function buildReportMarkdown(input: {
  url: string;
  when: string;
  mobile: number | null;
  desktop: number | null;
  report: Report | null;
  /** Failing audit lines, when the speed pillar supplies them. */
  audits?: string[];
}): string {
  const { report } = input;
  const lines = [
    '# Scan report',
    '',
    `Analyzed: ${input.url}`,
    `When: ${input.when}`,
    `Speed (PageSpeed Insights): mobile ${SCORE(input.mobile)}, desktop ${SCORE(input.desktop)}`,
    report && report.grade
      ? `AI readability: grade ${report.grade} (${SCORE(report.score)})`
      : 'AI readability: not measured',
  ];
  const risk = riskStates(report);
  if (risk.length) {
    lines.push('', '## Risk and SEO', '', ...risk.map((r) => `- ${r.name}: ${r.state}`));
  }
  if (input.audits) {
    lines.push(
      '',
      '## Failing audits',
      '',
      ...(input.audits.length ? input.audits : ['- None flagged']),
    );
  }
  lines.push('');
  return lines.join('\n');
}
