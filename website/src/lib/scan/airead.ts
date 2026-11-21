// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Pure formatting for the AI readability panel: the grade, the five category
// rows (same arithmetic and wording as the original result page), the token
// line, the raw-HTML Markdown, the robots.txt line and the "Also found"
// sentences. No DOM here.

import {
  agentPassCtaApplies,
  complianceFixCtaApplies,
  markdownOriginCtaApplies,
  tollboothCtaApplies,
} from './demand.mjs';
import { serverOffersApply } from './fit';

interface Category {
  score: number;
  max: number;
  flag?: string;
  verdict?: string;
}
interface Report {
  url?: string;
  grade?: string;
  score?: number;
  categories?: Record<string, Category>;
  diff?: {
    staticCleanTokens?: number;
    renderedCleanTokens?: number;
    staticMarkdown?: string;
  };
  agentVerifiability?: any; // eslint-disable-line @typescript-eslint/no-explicit-any
  signedAgentVerification?: any; // eslint-disable-line @typescript-eslint/no-explicit-any
  accessibility?: any; // eslint-disable-line @typescript-eslint/no-explicit-any
  agentSignals?: any; // eslint-disable-line @typescript-eslint/no-explicit-any
  access?: any; // eslint-disable-line @typescript-eslint/no-explicit-any
  platform?: unknown;
}

export interface CategoryRow {
  name: string;
  score: number;
  max: number;
  /** Bar width in percent. */
  pct: number;
  /** The flag chip text, or an empty string. */
  flag: string;
  verdict: string;
  /** A factual line under the verdict; empty when there is none or it replaced the verdict. */
  note: string;
  /** True when the verdict is the scan's reading of robots.txt. */
  fromRules: boolean;
}

export interface AireadModel {
  grade: string;
  score: number;
  /** strong, mixed, weak or poor. */
  word: string;
  url: string;
  categories: CategoryRow[];
  tokenLine: string;
  rawMarkdown: string;
  alsoFound: {
    wedge: 'tollbooth' | 'agentpass' | 'compliancefix' | 'markdown-origin';
    text: string;
    link?: { label: string; href: string };
  }[];
}

const WORDS: Record<string, string> = {
  A: 'strong',
  B: 'strong',
  C: 'mixed',
  D: 'weak',
  F: 'poor',
};

export const TOKEN_CAPTION =
  'Most AI crawlers read only the raw HTML. When the raw count is far below the count after JavaScript, those crawlers see little of your content.';

/** The category the robots.txt reading belongs to. */
export const CRAWLER_CATEGORY = 'Crawler access';
const MAX_NAMED_CRAWLERS = 6;

const names = (v: unknown): string[] =>
  Array.isArray(v) ? v.filter((x): x is string => typeof x === 'string') : [];

/**
 * What the site's robots.txt says about the AI crawlers the scan checks, from
 * the scan's reading of the file for the site root. Empty when the result has
 * no such reading (an older cached result).
 */
export function crawlerRulesLine(r: Report): string {
  const cr = r.access?.crawlerRules;
  if (!cr || typeof cr !== 'object') return '';
  const tail = cr.truncated === true ? ' Part of the file was not read.' : '';
  if (cr.parsed !== true) {
    if (cr.reason === 'missing') {
      return 'The scan could not read a robots.txt file for this site.' + tail;
    }
    if (cr.reason === 'empty') return 'robots.txt holds no rules, so it blocks no crawler.' + tail;
    if (cr.reason === 'not-robots') {
      return 'The address /robots.txt does not return a robots.txt file.' + tail;
    }
    return '';
  }
  const trainingBlocked = names(cr.training?.blocked);
  const answerBlocked = names(cr.answer?.blocked);
  const allowed = names(cr.training?.allowed).length + names(cr.answer?.allowed).length;
  const training = trainingBlocked.length + names(cr.training?.allowed).length;
  const answer = answerBlocked.length + names(cr.answer?.allowed).length;
  if (cr.wildcardBlocksAll === true) {
    // A crawler allowed here has a rule of its own that lets it in.
    return (
      'robots.txt blocks every crawler that has no rule of its own' +
      (allowed > 0
        ? '; ' +
          (allowed === 1
            ? '1 AI crawler has its own rule and may'
            : allowed + ' AI crawlers have their own rule and may') +
          ' fetch the home page (/).'
        : '.') +
      tail
    );
  }
  if (training + answer === 0) return '';
  const blocked = [...new Set([...trainingBlocked, ...answerBlocked])];
  if (blocked.length === 0) {
    return (
      `robots.txt lets all ${training + answer} AI crawlers we check fetch the home page (/). Rules for other paths are not read.` +
      tail
    );
  }
  const shown = blocked.slice(0, MAX_NAMED_CRAWLERS).join(', ');
  const more = blocked.length - MAX_NAMED_CRAWLERS;
  return (
    `robots.txt blocks ${trainingBlocked.length} of ${training} AI crawlers we check for training data and ${answerBlocked.length} of ${answer} that fetch pages to answer questions from the home page (/): ${shown}${more > 0 ? ` and ${more} more` : ''}.` +
    tail
  );
}

/**
 * Whether robots.txt keeps any AI crawler the scan checks from the home page:
 * true or false from the scan's reading of the file, null when the result has
 * no such reading.
 */
export function crawlersBlocked(r: Report): boolean | null {
  const cr = r.access?.crawlerRules;
  if (!cr || typeof cr !== 'object' || cr.parsed !== true) return null;
  return (
    cr.wildcardBlocksAll === true ||
    names(cr.training?.blocked).length + names(cr.answer?.blocked).length > 0
  );
}

/**
 * The reason sentences for the gated topics, worded as on the original page.
 * On a hosted platform each sentence keeps its finding and drops the part
 * about a server the owner runs.
 */
export function alsoFoundSentences(r: Report): AireadModel['alsoFound'] {
  const out: AireadModel['alsoFound'] = [];
  const offers = serverOffersApply(r as Record<string, unknown>);
  const av = r.agentVerifiability;
  const ax = r.accessibility;
  const sa = r.signedAgentVerification;
  if (tollboothCtaApplies(av)) {
    // How much of the page is in the raw HTML, then who may fetch it. The
    // crawler clause comes from the scan's reading of robots.txt, the same
    // source as the line on the crawler access row; a result without that
    // reading says AI crawlers are allowed and names no count.
    const pct = Number(av.detail.exposurePct);
    const blocked = crawlersBlocked(r);
    const text =
      (pct >= 100 ? 'All' : 'About ' + av.detail.exposurePct + '%') +
      ' of this page’s content is in the raw HTML' +
      (blocked === null
        ? ', and AI crawlers are allowed to read it'
        : blocked
          ? ''
          : ', and robots.txt lets AI crawlers fetch it') +
      (offers ? ', but your origin can’t tell a real signed agent from an impostor.' : '.');
    out.push({ wedge: 'tollbooth', text });
  }
  if (agentPassCtaApplies(sa)) {
    out.push({
      wedge: 'agentpass',
      text:
        sa.classification === 'verifying'
          ? 'Your origin answered our signed test request differently from one with a corrupted signature, which is consistent with checking agent signatures.'
          : 'Your origin already treats signed-agent requests differently from unsigned ones.',
    });
  }
  if (complianceFixCtaApplies(ax)) {
    const flagged =
      'Automated checks flagged ' +
      (ax.detail.total === 1
        ? '1 accessibility issue'
        : ax.detail.total + ' accessibility issues') +
      ' on this page';
    out.push({
      wedge: 'compliancefix',
      text: offers
        ? flagged +
          '; ' +
          (ax.detail.fixableInline === 1 ? '1 looks' : ax.detail.fixableInline + ' look') +
          ' like the kind a self-hosted optimizer can fix on your own servers, with no app changes. We don’t issue compliance certificates.'
        : flagged + '. We don’t issue compliance certificates.',
    });
  }
  const as = r.agentSignals;
  if (as && as.status === 'ok' && as.markdown) {
    if (markdownOriginCtaApplies(as)) {
      out.push(
        offers
          ? {
              wedge: 'markdown-origin',
              text: 'This page did not return Markdown when asked for it (Accept: text/markdown). The mod_pagespeed optimizer worker can serve a rendered Markdown copy at the same URL, on your own server. It is experimental and off by default. The module alone never renders Markdown; with AgentOptimize on it only adds Vary: Accept.',
              link: { label: 'How it works', href: '/docs/agent-optimize/' },
            }
          : {
              wedge: 'markdown-origin',
              text: 'This page did not return Markdown when asked for it (Accept: text/markdown).',
            },
      );
    } else if (as.markdown.negotiated === true) {
      out.push({ wedge: 'markdown-origin', text: 'This page returns Markdown on request.' });
    }
  }
  return out;
}

function crawlerRow(
  name: string,
  verdict: string,
  r: Report,
): Pick<CategoryRow, 'verdict' | 'note' | 'fromRules'> {
  if (name !== CRAWLER_CATEGORY) return { verdict, note: '', fromRules: false };
  const line = crawlerRulesLine(r);
  // Whenever the scan has something to say about robots.txt, that line stands in for the
  // older verdict sentence, so the row never shows two readings that disagree.
  return line
    ? { verdict: line, note: '', fromRules: true }
    : { verdict, note: '', fromRules: false };
}

export function formatAiread(r: Report): AireadModel {
  const categories = Object.entries(r.categories ?? {}).map(([name, c]) => ({
    name,
    score: c.score,
    max: c.max,
    pct: Math.round((c.score / c.max) * 100),
    flag:
      c.flag === 'csr-gap' ? 'invisible without JS' : c.flag === 'thin' ? 'no content found' : '',
    // On the crawler access row the scan's reading of robots.txt, when there
    // is one, takes the place of the verdict sentence, so the row says one
    // thing about robots.txt. The score is not touched.
    ...crawlerRow(name, c.verdict ?? '', r),
  }));
  const d = r.diff ?? {};
  const tokens = (n: number | undefined) => '~' + (n ?? 0).toLocaleString();
  return {
    grade: String(r.grade),
    score: Number(r.score),
    word: WORDS[String(r.grade)] || '',
    url: r.url ?? '',
    categories,
    tokenLine: `Readable text in raw HTML: ${tokens(d.staticCleanTokens)} tokens · after JavaScript: ${tokens(d.renderedCleanTokens)} tokens`,
    rawMarkdown: d.staticMarkdown && d.staticMarkdown.trim() ? d.staticMarkdown : '',
    alsoFound: alsoFoundSentences(r),
  };
}
