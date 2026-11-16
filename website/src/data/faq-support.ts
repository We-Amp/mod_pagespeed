// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The /support/ page FAQ: what a subscription covers and how the signed
// artifacts are verified. Rendered visibly on the page and emitted as FAQPage JSON-LD from the
// same entries, so the structured data never says more than the page does.
// Answers may contain HTML; JSON-LD callers pipe them through stripHtml().
// Prices and first-response targets live in product-facts.mjs and on /pricing/.

import type { FaqEntry } from './faq-shared';
import { COMMERCIAL_EMAIL, SOURCE_PUBLICATION } from './product-facts.mjs';

const link = (href: string, text: string) =>
  `<a href="${href}" class="text-interactive hover:text-interactive-hover underline">${text}</a>`;

export const faqSupport: FaqEntry[] = [
  {
    q: 'Does a subscription change what the software does?',
    a: `No. Every install runs the same software under the ${SOURCE_PUBLICATION.license}, with every optimization enabled by configuration, not by a plan. A subscription changes who answers when you need help; the signed packages and the ${link('/packs/', 'rule packs')} are free and the same for everyone. Writing rules for your own sites is consulting work.`,
  },
  {
    q: 'Who answers a ticket?',
    a: 'The engineers who build mod_pagespeed. There is no first-line desk in front of them: the person reading your ticket can read the code path behind it.',
  },
  {
    q: 'When do you answer?',
    a: 'On CET business days. There is no 24x7 desk. First-response targets are per tier: Standard within 2 business days, Priority next business day, and severity 1 (production down) within 8 business hours on Priority and 4 business hours on Enterprise, with other Enterprise severities per agreement.',
  },
  {
    q: 'How do security updates reach me?',
    a: `Everyone gets security fixes at the same time, as regular releases through the channel they installed from, listed under Security in the ${link('/docs/release-notes/', 'release notes')}. Subscribers get advance notice of a planned security release: the date, the severity, the affected versions and the impact class. When a vulnerability is already public or being exploited, the notice goes out with the release. Priority and Enterprise subscribers also get vulnerability notifications timed to the EU Cyber Resilience Act reporting windows. Every release from 2.6.0 on carries its SPDX SBOM and VEX document, and both are also in the source tree.`,
  },
  {
    q: 'Does the number of servers I run change the price?',
    a: 'No. A subscription covers people and response, not installations: who at your organization can open tickets and how fast we answer. The software, updates and fixes are the same and free on every server, there is no metering, and the software never reports a count.',
  },
  {
    q: 'What is signed today?',
    a: `The apt and yum repositories are GPG-signed with the repository signing key. From 2.5.0 on, each mod_pagespeed GitHub release carries one signature, SHA256SUMS.asc, made with a separate release signing key over the SHA256SUMS file that lists every release file; the release files carry no per-file .asc signatures. The Windows installer is Authenticode-signed by We-Amp B.V. The optimizer worker's release assets on GitHub carry no signature; every one is listed in SHA256SUMS. The container images are cosign-signed and carry an SBOM attestation and build provenance. Every release from 2.6.0 on carries its SPDX SBOM and VEX document, and both are also in the source tree. Everything is the same software under the ${SOURCE_PUBLICATION.license}. See ${link('#hardened-builds', 'signed packages and images')}.`,
  },
  {
    q: 'Can I verify the standard packages without a subscription?',
    a: `Yes. The apt and yum repositories are GPG-signed, every release asset is listed in SHA256SUMS, the mod_pagespeed releases sign that file from 2.5.0 on, and the SPDX SBOM and OpenVEX file are published in the source tree. The ${link('#verify', 'recipe')} on this page checks both keys, the signature and the checksums.`,
  },
  {
    q: 'What happens if a subscription lapses?',
    a: `Nothing happens to your deployment. The software keeps running and you keep upgrading from the public channels. Write to ${link(`mailto:${COMMERCIAL_EMAIL}`, COMMERCIAL_EMAIL)} when you want to pick it up again.`,
  },
];
