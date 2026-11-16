// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The /pricing/ FAQ: buying questions (what is free, what support covers,
// which tier, how to buy, VAT, what happens after payment or a lapse, what
// happens to existing licenses). Rendered visibly and as FAQPage JSON-LD from
// the same source. Technical questions live in the docs; the detail of each
// tier lives on /support/.

import type { FaqEntry } from './faq-shared';
import {
  COMMERCIAL_EMAIL,
  LICENSE_CLAUSE,
  LICENSING_TERMS_URL,
  SOURCE_PUBLICATION,
} from './product-facts.mjs';
import { VERSION_MAP } from './version-map.mjs';

const CURRENT = VERSION_MAP[0];

const link = (href: string, text: string) =>
  `<a href="${href}" class="text-interactive hover:text-interactive-hover underline">${text}</a>`;

export const faqPricing: FaqEntry[] = [
  {
    q: 'Is the software really free?',
    a: `Yes. mod_pagespeed is ${LICENSE_CLAUSE}: free to install and run, in development and in production, on any number of servers. The rule packs we publish are free too, and the same for everyone: the edge-SEO pack is available now (${link('/packs/', 'rule packs')}), and the packs we have planned will be free when they ship. Support is optional.`,
  },
  {
    q: 'What if I never subscribe?',
    a: 'You run the same software from the same signed public repositories as everyone else. Running without a subscription is a first-class choice.',
  },
  {
    q: 'What does support cover?',
    a: 'Configuration, upgrades and incidents for the people at your organization who can open tickets, for mod_pagespeed on Apache, nginx, IIS, Docker and ASP.NET Core. It does not cover changes to your own application code or site content; that is the audit or a consulting engagement.',
  },
  {
    q: 'Which tier do I need?',
    a: `Standard is a ticket and email channel with a first-response target, for one organization and one person at your organization who can open tickets. Choose Priority when you need a next-business-day first response, vulnerability notifications timed to the EU Cyber Resilience Act reporting windows, or up to 3 people at your organization who can open tickets. Choose Enterprise for response targets per agreement and people at your organization as agreed. ${link('/support/', 'What each tier includes')}.`,
  },
  {
    q: 'Which version is current?',
    a: `mod_pagespeed ${CURRENT.release}, released ${CURRENT.date}. From 2.5.0 on, the packages, the container images, the NuGet package and the IIS installer all carry the release number; the Helm chart keeps its own chart version (${CURRENT.helmChart}). ${link('/docs/release-notes/#version-map', 'The version map')} lists every release.`,
  },
  {
    q: 'How do I buy?',
    a: 'Standard and Priority are bought at checkout. FastSpring, our reseller and merchant of record, takes the payment, charges the VAT that applies and issues the invoice. There is no separate invoice by email from us. Enterprise runs on a master agreement with purchase order and invoice.',
  },
  {
    q: 'Do I pay VAT?',
    a: 'Prices on this page exclude VAT. At checkout, FastSpring charges the VAT that applies to you; a business in the EU can enter its VAT number there. On an invoice from We-Amp B.V. (Enterprise): customers in the Netherlands are charged 21% BTW; for business customers elsewhere in the EU with a valid VAT number, VAT is reverse charged (BTW verlegd, Art. 6 Wet OB 1968 / Art. 44 Directive 2006/112/EC); business customers outside the EU, including the United Kingdom, are outside the scope of Dutch VAT.',
  },
  {
    q: 'What happens after I pay?',
    a: "A reply from an engineer within your tier's response target, with any questions about your setup. The onboarding call and your ticket channel follow within five business days.",
  },
  {
    q: 'What happens if my subscription lapses?',
    a: `Nothing happens to your deployment. The software is yours under the ${SOURCE_PUBLICATION.license}: it keeps running and optimizing, and you keep upgrading from the public repositories.`,
  },
  {
    q: 'Will it break my site?',
    a: 'Transforms are deliberately conservative, and the design falls back to your original content when an optimization step fails. Test on staging, then disable any path, URL pattern or transform type with one line of config.',
  },
  {
    q: 'What if We-Amp disappears?',
    a: `Your deployment keeps running. The software is ${LICENSE_CLAUSE} and there is no kill switch in the request path, so the code stays yours to build, patch and fork.`,
  },
  {
    q: 'I have an IISpeed or mod_pagespeed 1.x license. What now?',
    a: `Your license moves to mod_pagespeed at no cost, and paid terms are honored to expiry. Existing customers keep their current rate when they move to a support subscription. Request the transfer through the ${link('/contact/?topic=iispeed-transfer', 'license-transfer form')}; other questions go to ${link(`mailto:${COMMERCIAL_EMAIL}`, COMMERCIAL_EMAIL)}.`,
  },
  {
    q: 'Where do the support terms live?',
    a: `At ${link(LICENSING_TERMS_URL, 'we-amp.com/licensing/')}. Subscriptions are governed by those support terms together with the ${link('/terms/', 'terms of service')}.`,
  },
];
