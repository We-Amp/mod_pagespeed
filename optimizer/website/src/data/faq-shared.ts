// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The FAQ entry shape shared by the page FAQs (faq-pricing.ts,
// faq-support.ts and the inline FAQs on other pages). Answers may contain
// HTML; JSON-LD callers pipe them through stripHtml() to get plain text for
// the Schema.org Answer.text field, so the structured data says exactly what
// the visible answer says.

export interface FaqEntry {
  q: string;
  a: string;
}

export function stripHtml(html: string): string {
  return html.replace(/<[^>]+>/g, '');
}
